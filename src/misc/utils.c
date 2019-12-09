/*
 * Copyright (c) 2019 Mindaugas Rasiukevicius <rmind at noxt eu>
 * All rights reserved.
 *
 * Use is subject to license terms, as specified in the LICENSE file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>

#include "utils.h"

#define	BUF_SIZE		(1024)
#define	BUF_GROW_SIZE		(1024)

/*
 * write_wrapped_hex: print the binary buffer data in hex blocks.
 */
ssize_t
hex_write_wrapped(FILE *stream, const void *buf, size_t len)
{
	const uint8_t *b = buf;
	unsigned long n = 0;
	size_t nbytes = 0;
	int ret;

	/*
	 * Settings:
	 * - Block of 4 hex characters (2 bytes) and a space.
	 * - Up to 15 blocks per row, so they fit in 76 columns.
	 */
	while (len >= 2) {
		const char endb = (++n % 15 == 0) ? '\n' : ' ';

		ret = fprintf(stream, "%02x%02x%c", b[0], b[1], endb);
		if (ret < 0) {
			return -1;
		}
		nbytes += ret;
		len -= 2;
		b += 2;
	}
	if (len) {
		if ((ret = fprintf(stream, "%02x", *b)) < 0) {
			return -1;
		}
		nbytes += ret;
	}
	fflush(stream);
	return nbytes;
}

/*
 * read_arbitrary_hex: consume arbitrary hex text i.e. any characters in
 * the range of 0..F (either lower or upper case).
 */
void *
hex_read_arbitrary(FILE *stream, size_t *outlen)
{
	size_t alloc_len = 0, nbytes = 0;
	uint8_t *buf = NULL;

	while (!feof(stream)) {
		uint8_t tmpbuf[BUF_SIZE];
		bool hf = false;
		size_t len;

		/*
		 * Grow the buffer as we go.
		 */
		if ((alloc_len - nbytes) < BUF_SIZE) {
			void *nbuf;

			alloc_len += BUF_GROW_SIZE;
			if ((nbuf = calloc(1, alloc_len)) == NULL) {
				free(buf);
				return NULL;
			}
			if (buf) {
				memcpy(nbuf, buf, nbytes);
				free(buf);
			}
			buf = nbuf;
		}

		/*
		 * Consume a block of data.  Read any hex characters
		 * and push bytes.
		 */
		if ((len = fread(tmpbuf, 1, BUF_SIZE, stream)) == 0) {
			break;
		}
		for (unsigned i = 0; i < len; i++) {
			unsigned char halfb = tolower((unsigned char)tmpbuf[i]);

			if (!isxdigit(halfb)) {
				continue;
			}
			halfb -= (halfb >= 'a') ? ('a' - 10) : '0';
			if (hf) {
				const size_t prev = nbytes - 1;
				buf[prev] = (buf[prev] << 4) | halfb;
			} else {
				buf[nbytes] = halfb;
				nbytes++;
			}
			hf = !hf;
		}
	}
	*outlen = nbytes;
	return buf;
}
