/*-------------------------------------------------------------------------
 *
 * vfdw_script.h
 *		The server-side program, its identity, and how its replies are read.
 *
 * A transaction's writes are applied by one Lua script executed once. That is
 * what makes the unit atomic without MULTI: the whole program runs as a single
 * server-side execution during which no other client's command can interleave,
 * so the check and the write it guards cannot be separated by the network.
 *
 * TWO PHASES, AND WHY THE ORDER IS THE POINT
 *
 * Phase 1 runs every check and writes nothing. Phase 2 writes and checks
 * nothing. A script that interleaved them could refuse the fourth plan after
 * applying the first three, which is the partially-applied outcome this design
 * exists to make unreachable. Phase 1 has a single exit: the first failing
 * check returns a sentinel and nothing has been written.
 *
 * NO USER DATA IS EVER CONCATENATED INTO SCRIPT TEXT.
 *
 * The program is one compile-time literal. Keys arrive in KEYS, everything
 * else in ARGV, both length-prefixed by RESP - so there is no delimiter, no
 * escaping, and no NUL problem. A script assembled per transaction would be a
 * new script every time, defeating EVALSHA, and would put user bytes where
 * Lua parses them.
 *
 * THE SHEBANG MATTERS TWICE
 *
 * With `#!lua` the server enforces the declared-key rule: a shebang-less
 * script is granted SCRIPT_ALLOW_CROSS_SLOT and will happily write undeclared
 * same-node keys right up until a slot migrates. It also makes an out-of-
 * memory condition refuse the whole script up front rather than only before
 * the first write.
 *
 * THE WRITE VERBS ARE LITERAL, AND A LINT DEPENDS ON IT
 *
 * Phase 2 dispatches through a table whose values are closures naming their
 * verb literally - SET=function(k,a) return server.pcall('SET', k, a[1]) end.
 * A script that took its verb from ARGV would be shorter and would make the
 * W1 marker lint vacuous: you cannot grep for write verbs that are not there.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_script.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_SCRIPT_H
#define VFDW_SCRIPT_H

#include "vfdw.h"

#include "vfdw_cmd.h"

/*
 * The four outcomes phase 1 can report, plus the two that are not sentinels.
 *
 * These are the classification the flush turns into a SQLSTATE; the mapping
 * lives with the flush (S6) rather than here, because "what does this mean to
 * SQL" is a different question from "what did the server say".
 */
typedef enum VfdwScriptVerdict
{
	VFDW_SCRIPT_UNKNOWN = 0,	/* not one of ours; the caller classifies it */
	VFDW_SCRIPT_OK,				/* VFDW1 OK <n> */
	VFDW_SCRIPT_CONFLICT,		/* VFDW1 CONFLICT - 23505 */
	VFDW_SCRIPT_MISSING,		/* VFDW1 MISSING  - 40001 */
	VFDW_SCRIPT_WRONGTYPE,		/* VFDW1 WRONGTYPE - 42804 */
	VFDW_SCRIPT_BADPROTO,		/* VFDW1 BADPROTO - XX000, our own bug */
	VFDW_SCRIPT_INTERNAL		/* VFDW1 INTERNAL - XX000, partially applied */
} VfdwScriptVerdict;

/* The program, and the lowercase hex SHA1 the server will know it by. */
extern const char *vfdw_script_text(void);
extern const char *vfdw_script_sha1(void);

/*
 * Classify one reply body.
 *
 * Takes (str, len) rather than a NUL-terminated string: a reply is Valkey's
 * memory with its own length, and invariant I3 says the length travels with
 * it, which is why this does not go through vfdw_reply_has_prefix: that one
 * takes a valkeyReply, and the body classified here has often been copied out
 * of one already. Both match ON A WORD BOUNDARY, because without it
 * "VFDW1 CONFLICTX" would classify as CONFLICT and report a unique violation
 * for something else entirely.
 *
 * *detail, when non-NULL, receives a palloc'd copy of the text after the
 * code, or NULL when there is none. Copied because the caller's reply is
 * batch-owned and the very next take invalidates it.
 */
extern VfdwScriptVerdict vfdw_script_classify(const char *str, size_t len,
											  char **detail);

/*
 * Append EVALSHA <sha> <numkeys> <keys...> <argv...> for the current ledger.
 *
 * The command must already be initialised; the flush owns it, because it also
 * owns the batch the command goes into. mcxt is scratch for the key table and
 * may be the flush's own short-lived context - nothing here outlives the call
 * except the bytes it points at, which belong to the write buffer.
 */
extern void vfdw_script_encode(VfdwCmd *cmd, MemoryContext mcxt);

/* The name of a verdict, for messages and for the suite. */
extern const char *vfdw_script_verdict_name(VfdwScriptVerdict v);

#endif							/* VFDW_SCRIPT_H */
