/*
 * Copyright (c) 2019 Mindaugas Rasiukevicius <rmind at noxt eu>
 * All rights reserved.
 *
 * Use is subject to license terms, as specified in the LICENSE file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <err.h>

#include <sqlite3.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "rvault.h"
#include "storage.h"
#include "sys.h"
#include "sdb.h"

#if SQLITE_VERSION_NUMBER < 3023000
#error need sqlite 3.23 or newer
#endif

///////////////////////////////////////////////////////////////////////////////

typedef struct {
	int		fd;
	sqlite3 *	db;
} sdb_t;

static int
sdb_init(sqlite3 *db)
{
	static const char *sdb_init_q =
	    "CREATE TABLE IF NOT EXISTS sdb ("
	    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
	    "  key VARCHAR UNIQUE,"
	    "  val VARCHAR UNIQUE"
	    ");"
	    "CREATE INDEX IF NOT EXISTS sdb_key_idx ON sdb (key);";

	app_log(LOG_DEBUG, "%s: initializing database", __func__);

	if (sqlite3_exec(db, sdb_init_q, NULL, NULL, NULL) != SQLITE_OK) {
		app_log(LOG_CRIT, "sqlite3_exec: %s", sqlite3_errmsg(db));
		return -1;
	}
	return 0;
}

static sdb_t *
sdb_open(rvault_t *vault)
{
	sdb_t *sdb = NULL;
	sqlite3 *db = NULL;
	void *buf = NULL;
	size_t len = 0;
	ssize_t flen;
	char *fpath;
	int fd;

	/*
	 * Open the SDB file, decrypt and load the data into a buffer.
	 */
	if (asprintf(&fpath, "%s/%s", vault->base_path, SDB_META_FILE) == -1) {
		return NULL;
	}
	fd = open(fpath, O_CREAT | O_RDWR, 0600);
	free(fpath);
	if (fd == -1) {
		return NULL;
	}
	if ((flen = fs_file_size(fd)) == -1) {
		goto out;
	}
	if (flen && (buf = storage_read_data(vault, fd, flen, &len)) == NULL) {
		goto out;
	}

	/*
	 * Open an in-memory SQLite database and:
	 * a) Import the stored database,
	 * b) Initialize a fresh one.
	 */
	if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
		goto out;
	}
	if (buf) {
		void *db_buf;

		app_log(LOG_DEBUG, "%s: loading the database", __func__);
		if ((db_buf = sqlite3_malloc64(len)) == NULL) {
			goto out;
		}
		memcpy(db_buf, buf, len);
		sbuffer_free(buf, len);

		/*
		 * Note: if sqlite3_deserialize() fails, it will free the
		 * database buffer, so no need to sqlite3_free().
		 */
		if (sqlite3_deserialize(db, "main", db_buf, len, len,
		    SQLITE_DESERIALIZE_FREEONCLOSE |
		    SQLITE_DESERIALIZE_RESIZEABLE) != SQLITE_OK) {
			app_log(LOG_CRIT, "%s: database loading failed %s",
			    __func__, sqlite3_errmsg(db));
			goto out;
		}
	} else if (sdb_init(db) == -1) {
		goto out;
	}

	if ((sdb = calloc(1, sizeof(sdb_t))) == NULL) {
		goto out;
	}
	sdb->db = db;
	sdb->fd = fd;
	return sdb;
out:
	if (db) {
		sqlite3_close(db);
	}
	close(fd);
	free(sdb);
	return NULL;
}

static int
sdb_sync(rvault_t *vault, sdb_t *sdb)
{
	sqlite3_int64 len;
	unsigned char *buf;
	int ret;

	if ((buf = sqlite3_serialize(sdb->db, "main", &len, 0)) == NULL) {
		return -1;
	}
	ret = storage_write_data(vault, sdb->fd, buf, len);
	sqlite3_free(buf);
	return ret;
}

static void
sdb_close(sdb_t *sdb)
{
	sqlite3_close(sdb->db);
	close(sdb->fd);
	free(sdb);
}

///////////////////////////////////////////////////////////////////////////////

static int
sdb_query(sdb_t *sdb, const char *query, const char *k, const char *v)
{
	sqlite3_stmt *stmt = NULL;
	int ret = -1;

	if (sqlite3_prepare_v2(sdb->db, query, -1, &stmt, NULL) != SQLITE_OK)
		goto out;
	if (k && sqlite3_bind_text(stmt, 1, k, -1, SQLITE_STATIC) != SQLITE_OK)
		goto out;
	if (v && sqlite3_bind_text(stmt, 2, v, -1, SQLITE_STATIC) != SQLITE_OK)
		goto out;

	while (sqlite3_step(stmt) != SQLITE_DONE) {
		const unsigned ncols = sqlite3_column_count(stmt);

		for (unsigned i = 0; i < ncols; i++) {
			if (sqlite3_column_type(stmt, i)) {
				printf("%s\n", sqlite3_column_text(stmt, i));
			}
		}
	}
	ret = 0;
out:
	if (ret) {
		app_log(LOG_ERR, "%s: %s", __func__, sqlite3_errmsg(sdb->db));
	}
	if (stmt) {
		sqlite3_finalize(stmt);
	}
	return ret;
}

///////////////////////////////////////////////////////////////////////////////

static const struct {
	const char *	cmd;
	unsigned	params;
	const char *	query;
} sdb_cmds[] = {
	{ "LS",  0, "SELECT key FROM sdb ORDER BY key" },
	{ "GET", 1, "SELECT val FROM sdb WHERE key = ?" },
	{ "SET", 2, "INSERT OR REPLACE INTO sdb (key, val) VALUES (?, ?)" },
	{ "DEL", 1, "DELETE FROM sdb WHERE key = ?" },
};

static int
sdb_exec_cmd(sdb_t *sdb, char *line)
{
	char *tokens[2] = { NULL, NULL };
	unsigned n;

	if ((n = str_tokenize(line, tokens, __arraycount(tokens))) < 1) {
		return -1;
	}
	for (unsigned i = 0; i < __arraycount(sdb_cmds); i++) {
		char *key, *secret;
		int ret;

		if (strcasecmp(sdb_cmds[i].cmd, tokens[0]) != 0) {
			continue;
		}

		key = (sdb_cmds[i].params >= 1) ? tokens[1] : NULL;
		secret = (sdb_cmds[i].params >= 2) ? getpass("Secret:") : NULL;
		ret = sdb_query(sdb, sdb_cmds[i].query, key, secret);

		if (secret) {
			crypto_memzero(secret, strlen(secret));
			secret = NULL; // diagnostic
		}
		return ret;
	}
	return -1;
}

static char *
cmd_generator(const char *text, const int state)
{
	static unsigned cmd_iter_idx;
	static size_t text_len;

	if (!state) {
		cmd_iter_idx = 0;
		text_len = strlen(text);
	}
	while (cmd_iter_idx <__arraycount(sdb_cmds)) {
		const char *cmd = sdb_cmds[cmd_iter_idx++].cmd;
		if (strncasecmp(cmd, text, text_len) == 0) {
			return strdup(cmd);
		}
	}
	cmd_iter_idx = 0;
	return NULL;
}

static char **
cmd_completion(const char *text, const int start, const int end)
{
	(void)start; (void)end;

	/* Note: disable default of path completion. */
	rl_attempted_completion_over = 1;
#if 0
	if (start && rl_line_buffer[end - 1] == ' ') {
		return rl_completion_matches(text, secret_name_generator);
	}
#endif
	return rl_completion_matches(text, cmd_generator);
}

static void
sdb_usage(void)
{
	printf(
	    "Invalid command.\n"
	    "\n"
	    "Usage:\n"
	    "  LS		list secrets\n"
	    "  GET <name>	get the secret value\n"
	    "  SET <name>	set the secret value\n"
	    "  DEL <name>	delete the secret\n"
	    "\n"
	    "Note: names must not have white spaces.\n"
	);
}

void
sdb_cli(rvault_t *vault, int argc, char **argv)
{
	sdb_t *sdb;
	char *line;

	if ((sdb = sdb_open(vault)) == NULL) {
		err(EXIT_FAILURE, "could not open the database");
	}
	rl_attempted_completion_function = cmd_completion;
	while ((line = readline("> ")) != NULL) {
		if (sdb_exec_cmd(sdb, line) == 0) {
			sdb_sync(vault, sdb);
		} else {
			sdb_usage();
		}
		crypto_memzero(line, strlen(line));
		free(line);
	}
	sdb_close(sdb);

	(void)argc; (void)argv;
}