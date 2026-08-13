/*-------------------------------------------------------------------------
 *
 * vfdw_error.h
 *		Error classification for valkey_fdw.
 *
 * Two invariants are enforced here rather than left to callers:
 *
 *	I1	Nothing is released on the way out of an error path. A reply is freed
 *		by the reset callback on the batch's memory context, a connection by
 *		the pool's transaction-end callback; freeing either inline before an
 *		ereport is how an error path in connection handling becomes a
 *		use-after-free.
 *
 *	I2	Nothing reaches error-reporting code unless it is known valid in the
 *		database encoding, whoever produced it. Bytes are copied into palloc'd
 *		memory with their length, checked by vfdw_safe_text, and passed as data
 *		through a "%s", never as a format string.
 *
 * I2 is stated about bytes rather than about ownership. Valkey echoes the
 * client's own
 * arguments back inside some error messages ("unknown command 'X', with args
 * beginning with..."), and this wrapper deliberately sends binary-safe keys,
 * so a server error can carry bytes that are not valid in the database
 * encoding. A key or member the wrapper formats is the same problem arriving
 * from the other side: a bytea key column carries arbitrary bytes by design,
 * and the write program's own error text has the key concatenated into it. An
 * ERROR is one of the few places a value reaches both the client and the
 * server log without passing through the read path's pg_verifymbstr, and where
 * client_encoding differs from the server encoding the conversion runs during
 * error reporting. So every such byte range is checked and replaced by a
 * description of its length when it fails, rather than being trusted because
 * error text "is ASCII in practice". A reply with no body at all is named as
 * such rather than handed to pnstrdup(NULL, 0), which two of the four sites
 * this file replaced guarded against and two did not.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_error.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_ERROR_H
#define VFDW_ERROR_H

#include "vfdw.h"

#include <valkey/valkey.h>

/*
 * Map a libvalkey context error (VALKEY_ERR_*) to a PostgreSQL error code.
 */
extern int	vfdw_errcode_for_context(const valkeyContext *conn);

/*
 * Map a Valkey server error reply to a PostgreSQL error code, by its prefix:
 * WRONGTYPE, NOAUTH, WRONGPASS, NOPERM, LOADING, OOM and friends each mean
 * something specific and deserve a code a client can branch on.
 */
extern int	vfdw_errcode_for_reply(const char *errstr);

/*
 * Raise from a context error. Does not touch the connection: it belongs to the
 * pool, whose transaction-end callback closes it because the error left it
 * mid-conversation.
 */
VFDW_NORETURN extern void vfdw_error_from_context(valkeyContext *conn, const char *what) VFDW_NORETURN_TAIL;

/*
 * Raise from a VALKEY_REPLY_ERROR. Does not free the reply.
 */
VFDW_NORETURN extern void vfdw_error_from_reply(const valkeyReply *reply, const char *what) VFDW_NORETURN_TAIL;

/*
 * Raise from a VALKEY_REPLY_ERROR, taking ownership of the reply.
 *
 * Copies the message, releases the reply, then reports - in that order. The
 * ordering is the entire point: reporting first never returns, so the reply
 * would leak, and releasing first would leave the message pointing into freed
 * memory. Encapsulating it here means no caller has to get it right.
 */
VFDW_NORETURN extern void vfdw_error_from_reply_free(valkeyReply *reply, const char *what) VFDW_NORETURN_TAIL;

/*
 * True when the reply is a server error. Callers that handle particular
 * errors themselves - MOVED, ASK, NOSCRIPT - test the prefix before deciding
 * to raise.
 */
extern bool vfdw_reply_is_error(const valkeyReply *reply);
extern bool vfdw_reply_has_prefix(const valkeyReply *reply, const char *prefix);

/*
 * A byte range as text an error may carry: a palloc'd NUL-terminated copy when
 * the bytes are valid in the database encoding, and "a N-byte value that is
 * not valid in encoding X" when they are not.
 *
 * This is where I2 is enforced, so every site that reports bytes calls it -
 * a reply body, a key or member built from a bytea column, the write program's
 * own message. The result is a C string and goes through "%s"; a length-
 * counted "%.*s" straight from the source bytes bypasses the check, which is
 * the mistake this exists to make hard.
 */
extern char *vfdw_safe_text(const char *bytes, size_t len);

/*
 * An error reply's body as errdetail text, plus the SQLSTATE its prefix
 * implies.
 *
 * One function so that the copy (I2), the NULL-body guard and the encoding
 * check happen together and cannot be half-applied. The SQLSTATE is derived
 * from the RAW bytes and the text from the checked ones, so a message whose
 * tail is unprintable still classifies on its prefix.
 */
extern char *vfdw_error_reply_text(const valkeyReply *reply, int *sqlerrcode);

#endif							/* VFDW_ERROR_H */
