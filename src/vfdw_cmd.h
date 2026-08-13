/*-------------------------------------------------------------------------
 *
 * vfdw_cmd.h
 *		Binary-safe command construction and pipelined execution.
 *
 * Two jobs, both otherwise done by hand at every call site.
 *
 * Construction: arguments are accumulated with their lengths, never as C
 * strings, and issued through valkeyAppendCommandArgv. A key or value
 * containing a NUL byte therefore travels whole rather than being truncated
 * at the first zero (invariant I3).
 *
 * Execution: commands are queued and flushed in batches, so fetching N
 * values costs ceil(N / pipeline_batch) round trips instead of N. A round
 * trip per row is the single largest cost in the read path, and batching is
 * the only thing that removes it.
 *
 * A batch registers a reset callback on the memory context it is created in,
 * so an error unwinding past it releases the reply it holds without any
 * caller writing cleanup code on an error path (invariant I1).
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_cmd.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_CMD_H
#define VFDW_CMD_H

#include "vfdw.h"

#include <valkey/valkey.h>

#include "utils/palloc.h"

#include "vfdw_conn.h"

/*
 * One command under construction. Arguments are appended in order; the
 * argument vector and its lengths grow together so they cannot drift apart.
 */
typedef struct VfdwCmd
{
	const char **argv;
	size_t	   *arglens;
	int			argc;
	int			capacity;
	MemoryContext mctx;
} VfdwCmd;

extern void vfdw_cmd_init(VfdwCmd *cmd, MemoryContext mctx, int capacity);
extern void vfdw_cmd_reset(VfdwCmd *cmd);

/* For string literals and NUL-terminated text we own. */
extern void vfdw_cmd_add_cstr(VfdwCmd *cmd, const char *value);

/* For anything from Valkey, from a bytea, or from a text Datum. */
extern void vfdw_cmd_add_bytes(VfdwCmd *cmd, const char *value, size_t len);

extern void vfdw_cmd_add_int(VfdwCmd *cmd, int64 value);

/*
 * A pipelined batch of commands against one connection.
 */
typedef struct VfdwBatch VfdwBatch;

extern VfdwBatch *vfdw_batch_begin(VfdwConn *vconn, MemoryContext mctx);

/* Queue a command. Flushes automatically once pipeline_batch is reached. */
extern void vfdw_batch_add(VfdwBatch *batch, const VfdwCmd *cmd);

/*
 * Take the next reply, in the order the commands were queued.
 *
 * The batch owns the returned reply: it stays valid until the next call to
 * vfdw_batch_next or vfdw_batch_end. Callers therefore never free replies,
 * and cannot leak one by raising in between.
 */
extern valkeyReply *vfdw_batch_next(VfdwBatch *batch);

/* Number of queued commands whose replies have not been taken yet. */
extern int	vfdw_batch_pending(const VfdwBatch *batch);

/*
 * How many times the output buffer has been pushed to the server.
 *
 * The only externally observable consequence of the pipeline_batch option: a
 * probe that reports how many values matched produces the same number at
 * every depth, so it cannot tell a batch that honours the configured bound
 * from one that ignores it.
 */
extern int64 vfdw_batch_flushes(const VfdwBatch *batch);

extern void vfdw_batch_end(VfdwBatch *batch);

/*
 * Insist a reply has one of the expected types, naming the command if not.
 *
 * The mask is built from VFDW_RTYPE(). An error reply is reported with the
 * server's own message; a merely unexpected shape says so explicitly rather
 * than being dereferenced on the assumption it is what we asked for.
 */
#define VFDW_RTYPE(t)	(1 << (t))

extern void vfdw_reply_expect(valkeyReply *reply, int allowed, const char *what);

/*
 * The RESP name for a reply type, for a message that has to say what arrived.
 *
 * vfdw_reply_expect validates only the TOP level of a reply, so a caller that
 * walks into an array needs this to report an element that is not the shape
 * it is about to read as.
 */
extern const char *vfdw_reply_type_name(int type);

/*
 * Element i of an aggregate, or an error.
 *
 * Every walk over a reply the server shaped goes through this: the hash
 * lookup, the zset pair decode and the cluster shard parse all index
 * element[] with arithmetic derived from a count the server supplied, and a
 * frame declaring more elements than it carries would be read past its end.
 * A heap overflow of exactly that shape takes the backend down mid-query, and
 * it is the far end rather than this code that decides when it happens.
 *
 * elements is therefore read before element is indexed, and a NULL child is
 * refused rather than dereferenced.
 */
extern const valkeyReply *vfdw_reply_child(const valkeyReply *reply, size_t i);

#endif							/* VFDW_CMD_H */
