/*-------------------------------------------------------------------------
 *
 * vfdw_cmd.c
 *		Binary-safe command construction and pipelined execution.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_cmd.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_cmd.h"

#include "utils/builtins.h"
#include "utils/memutils.h"

#include "vfdw_error.h"
#include "vfdw_io.h"

struct VfdwBatch
{
	VfdwConn   *vconn;
	valkeyContext *conn;
	TimestampTz deadline;

	int			max_inflight;	/* pipeline_batch from the server options */
	int			queued;			/* appended to the output buffer, not sent */
	int			outstanding;	/* sent, reply not yet taken */
	int64		flushes;		/* pushes of the output buffer to the server */

	/* Owned; released on the next take, at end, or by the reset callback. */
	valkeyReply *current;

	MemoryContextCallback cb;
	bool		cb_registered;
};

/* ---------------------------------------------------------------------
 * Command construction
 * --------------------------------------------------------------------- */

void
vfdw_cmd_init(VfdwCmd *cmd, MemoryContext mctx, int capacity)
{
	Assert(capacity > 0);

	cmd->mctx = mctx;
	cmd->capacity = capacity;
	cmd->argc = 0;
	cmd->argv = (const char **)
		MemoryContextAlloc(mctx, sizeof(char *) * capacity);
	cmd->arglens = (size_t *)
		MemoryContextAlloc(mctx, sizeof(size_t) * capacity);
}

void
vfdw_cmd_reset(VfdwCmd *cmd)
{
	cmd->argc = 0;
}

/*
 * Grow both vectors together. Keeping them in one function is deliberate:
 * an argument whose length lives in a different-sized array than its pointer
 * is the shape of every off-by-one in this kind of code.
 */
static void
vfdw_cmd_ensure(VfdwCmd *cmd, int needed)
{
	int			newcap;

	if (needed <= cmd->capacity)
		return;

	newcap = cmd->capacity * 2;
	while (newcap < needed)
		newcap *= 2;

	cmd->argv = (const char **)
		repalloc(cmd->argv, sizeof(char *) * newcap);
	cmd->arglens = (size_t *)
		repalloc(cmd->arglens, sizeof(size_t) * newcap);
	cmd->capacity = newcap;
}

void
vfdw_cmd_add_bytes(VfdwCmd *cmd, const char *value, size_t len)
{
	vfdw_cmd_ensure(cmd, cmd->argc + 1);
	cmd->argv[cmd->argc] = value;
	cmd->arglens[cmd->argc] = len;
	cmd->argc++;
}

void
vfdw_cmd_add_cstr(VfdwCmd *cmd, const char *value)
{
	/*
	 * strlen is correct here and only here: this overload is for our own
	 * literals and NUL-terminated strings. Anything originating from Valkey
	 * or from a Datum must go through vfdw_cmd_add_bytes with a real length.
	 */
	vfdw_cmd_add_bytes(cmd, value, strlen(value));
}

void
vfdw_cmd_add_int(VfdwCmd *cmd, int64 value)
{
	char	   *buf = MemoryContextAlloc(cmd->mctx, 24);
	int			len;

	len = snprintf(buf, 24, INT64_FORMAT, value);
	vfdw_cmd_add_bytes(cmd, buf, (size_t) len);
}

/* ---------------------------------------------------------------------
 * Batching
 * --------------------------------------------------------------------- */

/*
 * Release the reply the batch is holding when its context goes away.
 *
 * This is why no caller needs a PG_TRY: an error unwinding past the batch
 * resets the context, which runs this, which returns the reply to libvalkey.
 * The connection itself is left flagged mid-conversation, so the transaction
 * callback discards it rather than handing on a stream at an unknown offset.
 */
static void
vfdw_batch_cleanup(void *arg)
{
	VfdwBatch  *batch = (VfdwBatch *) arg;

	if (batch->current != NULL)
	{
		freeReplyObject(batch->current);
		batch->current = NULL;
	}
}

VfdwBatch *
vfdw_batch_begin(VfdwConn *vconn, MemoryContext mctx)
{
	VfdwBatch  *batch = MemoryContextAllocZero(mctx, sizeof(VfdwBatch));
	const VfdwServerOptions *opts = vfdw_conn_options(vconn);

	batch->vconn = vconn;
	batch->conn = vfdw_conn_context(vconn);
	batch->deadline = vfdw_io_deadline(opts->command_timeout_ms);
	batch->max_inflight = opts->pipeline_batch;

	batch->cb.func = vfdw_batch_cleanup;
	batch->cb.arg = batch;
	MemoryContextRegisterResetCallback(mctx, &batch->cb);
	batch->cb_registered = true;

	vfdw_conn_begin(vconn);

	return batch;
}

static void
vfdw_batch_flush(VfdwBatch *batch)
{
	if (batch->queued == 0)
		return;

	vfdw_io_flush(batch->conn, batch->deadline);
	batch->outstanding += batch->queued;
	batch->queued = 0;
	batch->flushes++;
}

int64
vfdw_batch_flushes(const VfdwBatch *batch)
{
	return batch->flushes;
}

void
vfdw_batch_add(VfdwBatch *batch, const VfdwCmd *cmd)
{
	Assert(cmd->argc > 0);

	if (valkeyAppendCommandArgv(batch->conn, cmd->argc, cmd->argv,
								cmd->arglens) != VALKEY_OK)
		vfdw_error_from_context(batch->conn,
								"could not queue command for Valkey");

	batch->queued++;

	/*
	 * Bound how much can be in flight. Without this a scan over a large
	 * keyspace would queue every command before sending any, holding the
	 * whole request set in the output buffer and the whole reply set in the
	 * reader.
	 */
	if (batch->queued >= batch->max_inflight)
		vfdw_batch_flush(batch);
}

int
vfdw_batch_pending(const VfdwBatch *batch)
{
	return batch->queued + batch->outstanding;
}

valkeyReply *
vfdw_batch_next(VfdwBatch *batch)
{
	/*
	 * Release the previous reply first. Callers hold it only until their next
	 * call, which means they never free one and cannot leak it by raising
	 * partway through handling it.
	 */
	if (batch->current != NULL)
	{
		freeReplyObject(batch->current);
		batch->current = NULL;
	}

	/* Anything still buffered has to go out before a reply can arrive. */
	vfdw_batch_flush(batch);

	if (batch->outstanding == 0)
		return NULL;

	batch->current = vfdw_io_get_reply(batch->conn, batch->deadline);
	batch->outstanding--;

	return batch->current;
}

void
vfdw_batch_end(VfdwBatch *batch)
{
	/*
	 * Replies for commands nobody read would otherwise sit in the stream and
	 * be mistaken for the next statement's, so drain them before declaring
	 * the connection clean.
	 */
	while (vfdw_batch_pending(batch) > 0)
		(void) vfdw_batch_next(batch);

	if (batch->current != NULL)
	{
		freeReplyObject(batch->current);
		batch->current = NULL;
	}

	vfdw_conn_end(batch->vconn);
}

/* ---------------------------------------------------------------------
 * Reply shape
 * --------------------------------------------------------------------- */

const char *
vfdw_reply_type_name(int type)
{
	switch (type)
	{
		case VALKEY_REPLY_STRING:	return "string";
		case VALKEY_REPLY_ARRAY:	return "array";
		case VALKEY_REPLY_INTEGER:	return "integer";
		case VALKEY_REPLY_NIL:		return "nil";
		case VALKEY_REPLY_STATUS:	return "status";
		case VALKEY_REPLY_ERROR:	return "error";
		case VALKEY_REPLY_DOUBLE:	return "double";
		case VALKEY_REPLY_BOOL:		return "boolean";
		case VALKEY_REPLY_MAP:		return "map";
		case VALKEY_REPLY_SET:		return "set";
		case VALKEY_REPLY_PUSH:		return "push";
		case VALKEY_REPLY_VERB:		return "verbatim";
		case VALKEY_REPLY_BIGNUM:	return "bignum";
		case VALKEY_REPLY_ATTR:		return "attribute";
		default:					return "unknown";
	}
}

const valkeyReply *
vfdw_reply_child(const valkeyReply *reply, size_t i)
{
	if (reply->element == NULL || i >= reply->elements ||
		reply->element[i] == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("a Valkey aggregate reply is missing an element it "
						"declared")));

	return reply->element[i];
}

void
vfdw_reply_expect(valkeyReply *reply, int allowed, const char *what)
{
	if (reply == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("%s", what),
				 errdetail("No reply was available.")));

	if (vfdw_reply_is_error(reply))
	{
		/*
		 * The batch owns this reply, so the message is copied here and the
		 * reply is left for the batch to release - unlike the standalone
		 * path, this must not free it. The copy, the NULL-body guard and the
		 * encoding check are one call so this site cannot answer any of the
		 * three differently from src/vfdw_error.c, which is how it came to
		 * lack the NULL guard its sibling had.
		 */
		int			sqlerrcode;
		char	   *detail = vfdw_error_reply_text(reply, &sqlerrcode);

		vfdw_ereport(ERROR, sqlerrcode, what, detail);
	}

	if ((allowed & VFDW_RTYPE(reply->type)) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
				 errmsg("%s", what),
				 errdetail("Valkey answered with a %s reply, which is not a "
						   "shape this command can return.",
						   vfdw_reply_type_name(reply->type))));
}
