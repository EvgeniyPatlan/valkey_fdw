/*-------------------------------------------------------------------------
 *
 * vfdw_slot.c
 *		Hash-tag extraction and the CRC-16 cluster slot.
 *
 * Split from src/vfdw_val.c because these two functions are pure, total and
 * non-raising, which lets a suite pin them against the published vectors with
 * no server and no connection in the way.
 *
 * Their answer decides two things. vfdw_cluster_route turns a slot into the
 * node that owns it, and vfdw_flush_cluster_slot refuses a transaction whose
 * keys do not all agree on one - so a wrong slot here misroutes a command, or
 * lets a cross-slot write through to be refused from inside the script after
 * part of it has applied.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_slot.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_val.h"

/*
 * CRC-16/XMODEM: width 16, poly 0x1021, init 0x0000, no input reflection, no
 * output reflection, no final xor. Published check value: crc16("123456789")
 * == 0x31C3, which pins all four parameters in one number.
 *
 * The bitwise form rather than the 256-entry lookup table Valkey ships. The
 * table would be transcribed data whose only proof of correctness is that it
 * matches the source it was copied from; this is an independent
 * implementation whose agreement with the server is a test result. If a
 * profile ever shows the difference, the table can be generated from this
 * loop at first use rather than pasted in.
 *
 * The bytes are read through unsigned char. Valkey's own table lookup masks
 * with & 0x00FF for the same reason: plain char is signed on x86-64 Linux and
 * on ARM, and every byte >= 0x80 would otherwise sign-extend. That bug is
 * invisible to any ASCII-only test, i.e. to almost every test one would write.
 */
uint16
vfdw_val_crc16(const char *buf, size_t len)
{
	const unsigned char *p = (const unsigned char *) buf;
	uint16		crc = 0;
	size_t		i;
	int			bit;

	for (i = 0; i < len; i++)
	{
		crc ^= (uint16) ((uint16) p[i] << 8);

		for (bit = 0; bit < 8; bit++)
			crc = (crc & 0x8000)
				? (uint16) ((uint16) (crc << 1) ^ 0x1021)
				: (uint16) (crc << 1);
	}

	return crc;
}

/*
 * The substring a key's slot is computed over, exactly as keyHashSlot does it.
 *
 * Scan forward for the first '{'. If there is none, the whole key. Otherwise
 * scan forward from the byte AFTER it for the first '}'. If there is none, or
 * if it sits immediately after the '{', the whole key. Otherwise the bytes
 * strictly between them.
 *
 * No brace balancing, no nesting, no backtracking, and no second candidate
 * pair. Every "improvement" - matching the outer pair of '{a{b}c}', taking the
 * last '}', retrying after a degenerate '{}' - puts a key on a different node
 * than the server puts it, which in cluster mode means writing to a slot we
 * did not declare. Searching for '}' from index 0 rather than from s+1 is the
 * subtler one: on 'a}b{c' it finds a closer to the LEFT of the opener and
 * computes a negative length.
 *
 * memchr with explicit lengths, never strchr: a NUL is an ordinary byte here.
 * It is a legal member of a hash tag, it does not end the key, and it does not
 * end the tag.
 */
void
vfdw_val_hashtag(const char *key, size_t keylen, const char **tag,
				 size_t *taglen)
{
	const char *open;
	const char *close;
	size_t		after;

	*tag = key;
	*taglen = keylen;

	if (keylen == 0)
		return;					/* memchr(NULL, c, 0) is not worth relying on */

	open = memchr(key, '{', keylen);
	if (open == NULL)
		return;

	after = keylen - (size_t) (open - key) - 1;
	close = memchr(open + 1, '}', after);
	if (close == NULL || close == open + 1)
		return;

	*tag = open + 1;
	*taglen = (size_t) (close - open) - 1;
}

/*
 * The mask rather than a modulo, to match the server byte for byte. For this
 * function the two are exactly equivalent - 16384 is 2^14 and the accumulator
 * is uint16, so it is never negative - and they would diverge only if the CRC
 * were held in a signed type that went negative, where C's % yields a negative
 * remainder and an out-of-range slot.
 *
 * Deliberately NOT the read path's SCAN MATCH pattern logic. Valkey has a
 * separate patternHashSlot() with different rules: a pattern containing '*',
 * '?', '[' or '\' before the closing brace routes to all slots. A glob and a
 * literal key with the same bytes can route differently, so sharing one helper
 * between reads and writes would eventually send a SCAN to one node when it
 * needed every primary.
 */
uint16
vfdw_val_slot(const char *key, size_t keylen)
{
	const char *tag;
	size_t		taglen;

	vfdw_val_hashtag(key, keylen, &tag, &taglen);

	return (uint16) (vfdw_val_crc16(tag, taglen) & 0x3FFF);
}
