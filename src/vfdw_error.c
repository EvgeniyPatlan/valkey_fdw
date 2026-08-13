/*-------------------------------------------------------------------------
 *
 * vfdw_error.c
 *		Error classification for valkey_fdw.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_error.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_error.h"

#include "mb/pg_wchar.h"
#include "utils/builtins.h"

int
vfdw_errcode_for_context(const valkeyContext *conn)
{
	switch (conn->err)
	{
		case VALKEY_ERR_IO:
		case VALKEY_ERR_EOF:
			return ERRCODE_CONNECTION_FAILURE;
		case VALKEY_ERR_PROTOCOL:
			return ERRCODE_PROTOCOL_VIOLATION;
		case VALKEY_ERR_TIMEOUT:
			return ERRCODE_CONNECTION_FAILURE;
		case VALKEY_ERR_OOM:
			return ERRCODE_OUT_OF_MEMORY;
		default:
			return ERRCODE_FDW_ERROR;
	}
}

/*
 * Server error replies carry a machine-readable prefix. Translating it gives
 * callers a code worth branching on: a wrong-type key is a schema problem, a
 * WRONGPASS is a credentials problem, and LOADING is transient. Collapsing
 * all three into one generic FDW error throws that away.
 */
static const struct
{
	const char *prefix;
	int			sqlerrcode;
}			vfdw_reply_errors[] =
{
	{"WRONGTYPE", ERRCODE_DATATYPE_MISMATCH},
	{"WRONGPASS", ERRCODE_INVALID_PASSWORD},
	{"NOAUTH", ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION},
	{"NOPERM", ERRCODE_INSUFFICIENT_PRIVILEGE},
	{"NOPROTO", ERRCODE_FEATURE_NOT_SUPPORTED},
	{"LOADING", ERRCODE_CANNOT_CONNECT_NOW},
	{"MASTERDOWN", ERRCODE_CANNOT_CONNECT_NOW},
	{"CLUSTERDOWN", ERRCODE_CANNOT_CONNECT_NOW},
	{"BUSY", ERRCODE_LOCK_NOT_AVAILABLE},
	{"OOM", ERRCODE_OUT_OF_MEMORY},
	{"READONLY", ERRCODE_READ_ONLY_SQL_TRANSACTION},
	{"MISCONF", ERRCODE_CONFIG_FILE_ERROR},
	{"CROSSSLOT", ERRCODE_FEATURE_NOT_SUPPORTED},
	{"EXECABORT", ERRCODE_TRANSACTION_ROLLBACK},
	{NULL, 0}
};

int
vfdw_errcode_for_reply(const char *errstr)
{
	int			i;

	if (errstr == NULL)
		return ERRCODE_FDW_ERROR;

	for (i = 0; vfdw_reply_errors[i].prefix != NULL; i++)
	{
		size_t		len = strlen(vfdw_reply_errors[i].prefix);

		if (strncmp(errstr, vfdw_reply_errors[i].prefix, len) == 0 &&
			(errstr[len] == ' ' || errstr[len] == '\0'))
			return vfdw_reply_errors[i].sqlerrcode;
	}

	return ERRCODE_FDW_ERROR;
}

bool
vfdw_reply_is_error(const valkeyReply *reply)
{
	return reply != NULL && reply->type == VALKEY_REPLY_ERROR;
}

bool
vfdw_reply_has_prefix(const valkeyReply *reply, const char *prefix)
{
	size_t		len;

	if (!vfdw_reply_is_error(reply) || reply->str == NULL)
		return false;

	len = strlen(prefix);
	if (reply->len < len)
		return false;

	if (strncmp(reply->str, prefix, len) != 0)
		return false;

	/*
	 * ON A WORD BOUNDARY, for the same reason vfdw_errcode_for_reply matches
	 * on one. A raw prefix test answers true for a code that merely starts
	 * with another - "WRONGTYPEX" for "WRONGTYPE" - and the two callers that
	 * matter disagree about what that costs. The classifier would give such a
	 * body a generic SQLSTATE while the scan's skippable rule dropped its row
	 * without a word, so the looser of the two tests was the one deciding
	 * whether data went missing.
	 *
	 * Valkey separates the code from the message with a space, and a code with
	 * no message at all is the whole body.
	 */
	return reply->len == len || reply->str[len] == ' ';
}

/*
 * A byte range as text an error is allowed to carry (invariant I2).
 *
 * An ERROR is one of the few places a value reaches both the client and the
 * server log without passing through the read path's pg_verifymbstr, and where
 * client_encoding differs from the server encoding the conversion runs DURING
 * error reporting. So the bytes are checked against the database encoding here
 * and named by their length when they fail - whoever produced them. A reply
 * body Valkey sent is one source. A key or a member this wrapper formatted
 * itself is another: a bytea key column carries arbitrary bytes by design,
 * which is the documented reason it exists, so two inserts of '\xff' in one
 * transaction would otherwise put a raw 0xff into a UTF-8 database's log.
 *
 * One function for that question, because a second copy of it would be free to
 * disagree with this one: two spellings of the same rule is how an option name
 * with a trailing space came to have a conflict check nothing ever reached.
 *
 * vfdw_val_printable in src/vfdw_score.c is not that second copy and must not
 * be folded into this. It answers a narrower question - whether to echo text
 * that has NOT yet been proved to be a number - and is deliberately stricter
 * than the encoding: printable ASCII, at most 64 bytes. Being shown at all and
 * being worth showing are different tests, and the stricter one is the one
 * that may not be relaxed into this.
 */
char *
vfdw_safe_text(const char *bytes, size_t len)
{
	/*
	 * pnstrdup(NULL, 0) is undefined by the standard even though glibc
	 * tolerates it, and a caller holding nothing wants an empty string rather
	 * than a crash on a path that is already reporting a failure.
	 */
	if (bytes == NULL || len == 0)
		return pstrdup("");

	if (pg_verify_mbstr(GetDatabaseEncoding(), bytes, (int) len, true))
		return pnstrdup(bytes, len);

	return psprintf("a " UINT64_FORMAT "-byte value that is not valid in "
					"encoding \"%s\"",
					(uint64) len, GetDatabaseEncodingName());
}

char *
vfdw_error_reply_text(const valkeyReply *reply, int *sqlerrcode)
{
	char	   *raw;

	/*
	 * A reply with no body. pnstrdup(NULL, 0) is undefined by the standard
	 * even though glibc tolerates it, and "(no message)" is a more useful
	 * errdetail than an empty one.
	 *
	 * THE LENGTH TEST IS NOT REDUNDANT, and leaving it out meant this branch
	 * never achieved the second of those two aims. libvalkey ALLOCATES an
	 * empty string for an error frame carrying no text - `-\r\n` gives str
	 * non-NULL with len 0 - so the NULL test alone let an empty message
	 * through to be copied faithfully, and the user got a bare "DETAIL:"
	 * with nothing after it. That was found by fabricating the frame the
	 * register had recorded as impossible to produce; the register's reason
	 * for calling it unreachable was that no real server sends one, which
	 * was true and beside the point.
	 */
	if (reply == NULL || reply->str == NULL || reply->len == 0)
	{
		if (sqlerrcode != NULL)
			*sqlerrcode = ERRCODE_FDW_ERROR;
		return pstrdup("(no message)");
	}

	/*
	 * Classified on the raw bytes, before any substitution: the prefix that
	 * decides the SQLSTATE is ASCII and sits at the front, so a message whose
	 * tail is unreportable still yields WRONGTYPE rather than a generic code.
	 */
	raw = pnstrdup(reply->str, reply->len);
	if (sqlerrcode != NULL)
		*sqlerrcode = vfdw_errcode_for_reply(raw);

	return vfdw_safe_text(reply->str, reply->len);
}

void
vfdw_error_from_context(valkeyContext *conn, const char *what)
{
	/*
	 * Copy before reporting. errstr lives inside the context; a later change
	 * that releases the connection anywhere on this path would otherwise turn
	 * this into a read of freed memory.
	 */
	char	   *detail = pstrdup(conn->errstr);
	int			sqlerrcode = vfdw_errcode_for_context(conn);

	/* The connection is not released here. See invariant I1. */
	vfdw_ereport(ERROR, sqlerrcode, what, detail);
	pg_unreachable();
}

void
vfdw_error_from_reply(const valkeyReply *reply, const char *what)
{
	char	   *detail;
	int			sqlerrcode;

	Assert(vfdw_reply_is_error(reply));

	/*
	 * A reply body is length-counted and not necessarily NUL-terminated in
	 * the way a C string is, so exactly reply->len bytes are copied rather
	 * than trusting strlen. The same discipline applies to every value read
	 * from Valkey (invariant I3).
	 */
	detail = vfdw_error_reply_text(reply, &sqlerrcode);

	vfdw_ereport(ERROR, sqlerrcode, what, detail);
	pg_unreachable();
}

void
vfdw_error_from_reply_free(valkeyReply *reply, const char *what)
{
	char	   *detail;
	int			sqlerrcode;

	Assert(vfdw_reply_is_error(reply));

	/* Copy... */
	detail = vfdw_error_reply_text(reply, &sqlerrcode);

	/* ...then release, while the copy is safely ours... */
	freeReplyObject(reply);

	/* ...and only then report, which never returns. */
	vfdw_ereport(ERROR, sqlerrcode, what, detail);
	pg_unreachable();
}
