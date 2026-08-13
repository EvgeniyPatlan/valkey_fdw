/*-------------------------------------------------------------------------
 *
 * vfdw_testfuncs.c
 *		SQL entry points that exercise the connection and I/O layers directly.
 *
 * The layers below the planner are built before any FDW callback can use
 * them. Exposing them to SQL means the fault-injection, timeout and
 * cancellation suites can run against the real code from the first commit,
 * instead of waiting for a scan path to exist. Error handling that nothing
 * ever executes is error handling that is broken and not yet known to be.
 *
 * These functions are diagnostic, not part of the FDW's supported surface.
 * The ones that take a host and a port connect directly and are not pooled:
 * the raw context belongs to the one call, which closes it in a PG_FINALLY,
 * so an error raised inside the block leaves no socket behind. That is local
 * to this file and does not weaken invariant I1, which is about the pooled
 * connections and the replies the rest of the extension holds.
 *
 * Who may call them follows the ARGUMENT rather than the function. The three
 * that take a host and a port are superuser-only: an address is not a catalog
 * object, so there is no grant that describes it and nothing weaker to check.
 * The ones that take a server name resolve it through vfdw_test_server, which
 * demands USAGE on what the name resolved to.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_testfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw.h"

#include "funcapi.h"
#include "utils/builtins.h"

#include "access/htup_details.h"
#include "utils/memutils.h"
#include "foreign/foreign.h"
#include "miscadmin.h"

#include "vfdw_cmd.h"
#include "vfdw_conn.h"
#include "vfdw_error.h"
#include "vfdw_io.h"
#include "vfdw_test_common.h"

PG_FUNCTION_INFO_V1(valkey_fdw_test_ping);
PG_FUNCTION_INFO_V1(valkey_fdw_test_reply_ceiling);
PG_FUNCTION_INFO_V1(valkey_fdw_test_pipeline);
PG_FUNCTION_INFO_V1(valkey_fdw_test_binary);
PG_FUNCTION_INFO_V1(valkey_fdw_test_block);

/*
 * Send one command built from an explicit argument vector.
 *
 * Every argument carries its own length. Nothing here calls strlen on data
 * bound for Valkey, so a value containing a NUL byte is transmitted whole
 * rather than silently truncated at the first zero (invariant I3).
 */
static void
vfdw_test_append(valkeyContext *conn, int argc, const char **argv,
				 const size_t *arglens)
{
	if (valkeyAppendCommandArgv(conn, argc, argv, arglens) != VALKEY_OK)
		vfdw_error_from_context(conn, "could not queue command for Valkey");
}

/*
 * Round trip one command and hand back its reply.
 */
static valkeyReply *
vfdw_test_roundtrip(valkeyContext *conn, TimestampTz deadline,
					int argc, const char **argv, const size_t *arglens)
{
	vfdw_test_append(conn, argc, argv, arglens);
	vfdw_io_flush(conn, deadline);
	return vfdw_io_get_reply(conn, deadline);
}

/*
 * valkey_fdw_test_ping(host, port, connect_timeout_ms, command_timeout_ms)
 *
 * Proves a connection can be opened, driven and closed without any blocking
 * libvalkey call.
 */
Datum
valkey_fdw_test_ping(PG_FUNCTION_ARGS)
{
	char	   *host;
	int			port;
	int			connect_ms;
	int			command_ms;
	valkeyContext *volatile conn = NULL;
	text	   *volatile result = NULL;

	vfdw_test_require_superuser();

	host = text_to_cstring(PG_GETARG_TEXT_PP(0));
	port = PG_GETARG_INT32(1);
	connect_ms = PG_GETARG_INT32(2);
	command_ms = PG_GETARG_INT32(3);

	PG_TRY();
	{
		TimestampTz deadline;
		const char *argv[] = {"PING"};
		const size_t arglens[] = {4};
		valkeyReply *reply;

		conn = vfdw_io_connect(host, port, NULL, connect_ms);
		vfdw_io_set_limits(conn, 16 * 1024 * 1024, 1024 * 1024);

		deadline = vfdw_io_deadline(command_ms);
		reply = vfdw_test_roundtrip(conn, deadline, 1, argv, arglens);

		if (vfdw_reply_is_error(reply))
			vfdw_error_from_reply_free(reply, "PING failed");

		result = cstring_to_text_with_len(reply->str, reply->len);
		freeReplyObject(reply);
	}
	PG_FINALLY();
	{
		if (conn != NULL)
			valkeyFree(conn);
	}
	PG_END_TRY();

	PG_RETURN_TEXT_P(result);
}

/*
 * Queue n SETs through a batch and consume their acknowledgements.
 */
static void
vfdw_test_pipeline_set(VfdwBatch *batch, MemoryContext mctx, int n)
{
	VfdwCmd		cmd;
	int			i;

	vfdw_cmd_init(&cmd, mctx, 4);

	for (i = 0; i < n; i++)
	{
		vfdw_cmd_reset(&cmd);
		vfdw_cmd_add_cstr(&cmd, "SET");
		vfdw_cmd_add_cstr(&cmd, psprintf("vfdw:test:pipe:%d", i));
		vfdw_cmd_add_int(&cmd, i);
		vfdw_batch_add(batch, &cmd);
	}

	for (i = 0; i < n; i++)
		vfdw_reply_expect(vfdw_batch_next(batch), VFDW_RTYPE(VALKEY_REPLY_STATUS),
						  "SET failed");
}

/*
 * Queue n GETs and count how many replies carry the value their own request
 * asked for. A mismatch means replies were paired with the wrong requests,
 * which is the failure that makes pipelining unsafe.
 */
static int64
vfdw_test_pipeline_get(VfdwBatch *batch, MemoryContext mctx, int n)
{
	VfdwCmd		cmd;
	int64		matched = 0;
	int			i;

	vfdw_cmd_init(&cmd, mctx, 4);

	for (i = 0; i < n; i++)
	{
		vfdw_cmd_reset(&cmd);
		vfdw_cmd_add_cstr(&cmd, "GET");
		vfdw_cmd_add_cstr(&cmd, psprintf("vfdw:test:pipe:%d", i));
		vfdw_batch_add(batch, &cmd);
	}

	for (i = 0; i < n; i++)
	{
		valkeyReply *reply = vfdw_batch_next(batch);
		char	   *expected = psprintf("%d", i);

		vfdw_reply_expect(reply, VFDW_RTYPE(VALKEY_REPLY_STRING), "GET failed");

		if (reply->len == strlen(expected) &&
			memcmp(reply->str, expected, reply->len) == 0)
			matched++;
	}

	return matched;
}

/*
 * Remove what the probe wrote.
 *
 * A test that leaves keys behind turns the next run's keyspace into a
 * function of this one, and a suite that scans the keyspace then measures
 * history rather than its own fixture.
 */
static void
vfdw_test_pipeline_del(VfdwBatch *batch, MemoryContext mctx, int n)
{
	VfdwCmd		cmd;
	int			i;

	vfdw_cmd_init(&cmd, mctx, 4);

	for (i = 0; i < n; i++)
	{
		vfdw_cmd_reset(&cmd);
		vfdw_cmd_add_cstr(&cmd, "DEL");
		vfdw_cmd_add_cstr(&cmd, psprintf("vfdw:test:pipe:%d", i));
		vfdw_batch_add(batch, &cmd);
	}

	for (i = 0; i < n; i++)
		vfdw_reply_expect(vfdw_batch_next(batch),
						  VFDW_RTYPE(VALKEY_REPLY_INTEGER), "DEL failed");
}

/*
 * valkey_fdw_test_pipeline(host, port, n, command_timeout_ms)
 *
 * Queues n SETs, then n GETs, and verifies the replies come back in request
 * order. Returns the number of values that matched. This is the primitive the
 * scan path is built on: one round trip per batch instead of one per row.
 */
Datum
valkey_fdw_test_pipeline(PG_FUNCTION_ARGS)
{
	char	   *servername = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			n = PG_GETARG_INT32(1);
	ForeignServer *server = vfdw_test_server(servername);
	UserMapping *user = GetUserMapping(GetUserId(), server->serverid);
	VfdwConn   *vconn;
	VfdwBatch  *batch;
	MemoryContext mctx;
	int64		matched;

	if (n < 0 || n > 1000000)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("pipeline depth must be between 0 and 1000000")));

	/*
	 * A context of its own so the batch's reset callback runs when this
	 * finishes, whether it finishes normally or by raising.
	 */
	mctx = AllocSetContextCreate(CurrentMemoryContext, "vfdw pipeline test",
								 ALLOCSET_DEFAULT_SIZES);

	vconn = vfdw_get_connection(server, user);
	batch = vfdw_batch_begin(vconn, mctx);

	vfdw_test_pipeline_set(batch, mctx, n);
	matched = vfdw_test_pipeline_get(batch, mctx, n);
	vfdw_test_pipeline_del(batch, mctx, n);

	vfdw_batch_end(batch);
	vfdw_release_connection(vconn);
	MemoryContextDelete(mctx);

	PG_RETURN_INT64(matched);
}

/*
 * Store payload under key, read it back, and return the bytes verbatim.
 */
static bytea *
vfdw_test_set_get(valkeyContext *conn, TimestampTz deadline, const char *key,
				  const char *payload, size_t payload_len)
{
	const char *argv[3];
	size_t		arglens[3];
	valkeyReply *reply;
	bytea	   *result;

	argv[0] = "SET";
	arglens[0] = 3;
	argv[1] = key;
	arglens[1] = strlen(key);
	argv[2] = payload;
	arglens[2] = payload_len;

	freeReplyObject(vfdw_test_roundtrip(conn, deadline, 3, argv, arglens));

	argv[0] = "GET";
	arglens[0] = 3;
	reply = vfdw_test_roundtrip(conn, deadline, 2, argv, arglens);

	if (reply->type != VALKEY_REPLY_STRING)
	{
		freeReplyObject(reply);
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("expected a string reply from GET")));
	}

	/*
	 * Copy exactly reply->len bytes. Anything that treats reply->str as a C
	 * string truncates at the first NUL, which is the whole defect this
	 * function exists to detect (invariant I3).
	 */
	result = (bytea *) palloc(VARHDRSZ + reply->len);
	SET_VARSIZE(result, VARHDRSZ + reply->len);
	memcpy(VARDATA(result), reply->str, reply->len);
	freeReplyObject(reply);

	return result;
}

/*
 * valkey_fdw_test_binary(host, port, payload)
 *
 * Stores payload and reads it back, returning exactly the bytes Valkey
 * returned. A bytea round trip through this function is how the suite proves
 * embedded NULs and non-UTF8 bytes survive: handing reply->str to a C string
 * API truncates both at the first zero.
 */
Datum
valkey_fdw_test_binary(PG_FUNCTION_ARGS)
{
	char	   *host;
	int			port;
	bytea	   *payload;
	const char *payload_data;
	size_t		payload_len;
	valkeyContext *volatile conn = NULL;
	bytea	   *volatile result = NULL;

	vfdw_test_require_superuser();

	host = text_to_cstring(PG_GETARG_TEXT_PP(0));
	port = PG_GETARG_INT32(1);
	payload = PG_GETARG_BYTEA_PP(2);
	payload_data = VARDATA_ANY(payload);
	payload_len = VARSIZE_ANY_EXHDR(payload);

	PG_TRY();
	{
		TimestampTz deadline;

		conn = vfdw_io_connect(host, port, NULL, 5000);
		deadline = vfdw_io_deadline(30000);
		vfdw_io_set_limits(conn, 16 * 1024 * 1024, 1024 * 1024);

		result = vfdw_test_set_get(conn, deadline, "vfdw:test:binary",
								   payload_data, payload_len);
	}
	PG_FINALLY();
	{
		if (conn != NULL)
			valkeyFree(conn);
	}
	PG_END_TRY();

	PG_RETURN_BYTEA_P(result);
}

/*
 * valkey_fdw_test_block(host, port, block_seconds, command_timeout_ms)
 *
 * Issues a BLPOP on a key that never receives a push, so the server holds the
 * reply for block_seconds. Two suites use it:
 *
 *   - with a short command_timeout_ms, the deadline must fire;
 *   - with a long one, pg_cancel_backend() must interrupt the wait promptly.
 *
 * BLPOP blocks only this client, so it cannot wedge the shared test server
 * the way DEBUG SLEEP would.
 */
Datum
valkey_fdw_test_block(PG_FUNCTION_ARGS)
{
	char	   *host;
	int			port;
	int			block_seconds;
	int			command_ms;
	valkeyContext *volatile conn = NULL;
	text	   *volatile result = NULL;

	vfdw_test_require_superuser();

	host = text_to_cstring(PG_GETARG_TEXT_PP(0));
	port = PG_GETARG_INT32(1);
	block_seconds = PG_GETARG_INT32(2);
	command_ms = PG_GETARG_INT32(3);

	PG_TRY();
	{
		TimestampTz deadline;
		char		secs[32];
		const char *argv[3];
		size_t		arglens[3];
		valkeyReply *reply;

		conn = vfdw_io_connect(host, port, NULL, 5000);
		deadline = vfdw_io_deadline(command_ms);
		snprintf(secs, sizeof(secs), "%d", block_seconds);

		argv[0] = "BLPOP";
		arglens[0] = 5;
		argv[1] = "vfdw:test:never-pushed";
		arglens[1] = strlen(argv[1]);
		argv[2] = secs;
		arglens[2] = strlen(secs);

		reply = vfdw_test_roundtrip(conn, deadline, 3, argv, arglens);
		result = cstring_to_text(reply->type == VALKEY_REPLY_NIL
								 ? "timed out on the server"
								 : "unexpected reply");
		freeReplyObject(reply);
	}
	PG_FINALLY();
	{
		if (conn != NULL)
			valkeyFree(conn);
	}
	PG_END_TRY();

	PG_RETURN_TEXT_P(result);
}

/*
 * valkey_fdw_test_pooled_ping(server_name)
 *
 * PINGs through the pooled connection for the named server. Repeated calls
 * must reuse one connection: that is what valkey_fdw_test_pool_stats()
 * asserts, and it is the whole difference between a pool and a fresh connect
 * per statement.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_pooled_ping);
Datum
valkey_fdw_test_pooled_ping(PG_FUNCTION_ARGS)
{
	char	   *servername = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ForeignServer *server = vfdw_test_server(servername);
	UserMapping *user = GetUserMapping(GetUserId(), server->serverid);
	VfdwConn   *vconn = vfdw_get_connection(server, user);
	valkeyContext *conn = vfdw_conn_context(vconn);
	const VfdwServerOptions *opts = vfdw_conn_options(vconn);
	TimestampTz deadline = vfdw_io_deadline(opts->command_timeout_ms);
	const char *argv[] = {"PING"};
	const size_t arglens[] = {4};
	valkeyReply *reply;
	text	   *result;

	vfdw_conn_begin(vconn);

	if (valkeyAppendCommandArgv(conn, 1, argv, arglens) != VALKEY_OK)
		vfdw_error_from_context(conn, "could not queue PING");
	vfdw_io_flush(conn, deadline);
	reply = vfdw_io_get_reply(conn, deadline);

	if (vfdw_reply_is_error(reply))
		vfdw_error_from_reply_free(reply, "PING failed");

	result = cstring_to_text_with_len(reply->str, reply->len);
	freeReplyObject(reply);

	vfdw_conn_end(vconn);
	vfdw_release_connection(vconn);

	PG_RETURN_TEXT_P(result);
}

/*
 * valkey_fdw_test_pool_stats() -> (open, opened_total, leased)
 *
 * open          connections currently held by the pool
 * opened_total  connect() calls made since backend start
 * leased        slots currently held by a reader
 *
 * The second is the interesting one: if it grows once per statement, pooling
 * is not working.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_pool_stats);
Datum
valkey_fdw_test_pool_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	values[0] = Int32GetDatum(vfdw_conn_pool_size());
	values[1] = Int64GetDatum(vfdw_conn_open_count());
	values[2] = Int32GetDatum(vfdw_conn_leased_count());

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * valkey_fdw_test_resp3(server_name) -> boolean
 *
 * Which protocol the pooled connection to this server actually negotiated.
 *
 * This exists because the RESP2 fallback cannot otherwise be asserted. Both
 * protocols decode to the SAME rows - that is the whole point of the fallback
 * - so a suite that runs a scan with RESP3 refused and checks the rows would
 * pass identically whether the fallback ran or whether the refusal never
 * arrived. It would be an assertion that cannot fail, which is the trap this
 * project keeps finding in its own tests.
 *
 * Reporting the flag makes the protocol itself the assertion, and the rows a
 * separate one about decoding.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_resp3);
Datum
valkey_fdw_test_resp3(PG_FUNCTION_ARGS)
{
	char	   *servername = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ForeignServer *server = vfdw_test_server(servername);
	UserMapping *user = GetUserMapping(GetUserId(), server->serverid);
	VfdwConn   *vconn = vfdw_get_connection(server, user);

	PG_RETURN_BOOL(vfdw_conn_is_resp3(vconn));
}

/*
 * valkey_fdw_test_poke(server_name, key bytea, value bytea) -> bigint
 *
 * Write one key through the pooled connection, or delete it when value is
 * NULL. Returns what the server answered: 1 for a SET, and for a DEL the
 * number of keys removed.
 *
 * This exists so a suite can build its own fixture without shelling out to
 * valkey-cli. The floor suite needs exactly that: it runs on topologies where
 * no fixture helper is available, and without a key of its own its only scan
 * assertion is "returns nothing", which a scan path that returned nothing
 * under every circumstance would also satisfy.
 *
 * The key is bytea and carries its length, so a probe cannot be the reason a
 * NUL-bearing key looks fine.
 *
 * This is a convenience over the same layers valkey_fdw_test_probe reaches -
 * probe(srv, 0, 'SET', k, v) is the same write - and is kept because smoke.sql
 * and leak.sql use it and smoke.sql is the floor suite on cluster and search.
 * One difference matters: poke never calls vfdw_conn_select_db, so it writes
 * to whatever database the pooled connection happens to be on. That is fine
 * for its callers, which are all on database 0, and makes it unusable for any
 * assertion about the database option. Use the probe there.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_poke);
Datum
valkey_fdw_test_poke(PG_FUNCTION_ARGS)
{
	char	   *servername = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ForeignServer *server = vfdw_test_server(servername);
	UserMapping *user = GetUserMapping(GetUserId(), server->serverid);
	bytea	   *key = PG_GETARG_BYTEA_PP(1);
	VfdwConn   *vconn;
	VfdwBatch  *batch;
	MemoryContext mctx;
	VfdwCmd		cmd;
	valkeyReply *reply;
	int64		answer;

	mctx = AllocSetContextCreate(CurrentMemoryContext, "vfdw poke",
								 ALLOCSET_SMALL_SIZES);

	vconn = vfdw_get_connection(server, user);
	batch = vfdw_batch_begin(vconn, mctx);

	vfdw_cmd_init(&cmd, mctx, 4);
	if (PG_ARGISNULL(2))
	{
		vfdw_cmd_add_cstr(&cmd, "DEL");
		vfdw_cmd_add_bytes(&cmd, VARDATA_ANY(key), VARSIZE_ANY_EXHDR(key));
	}
	else
	{
		bytea	   *value = PG_GETARG_BYTEA_PP(2);

		vfdw_cmd_add_cstr(&cmd, "SET");
		vfdw_cmd_add_bytes(&cmd, VARDATA_ANY(key), VARSIZE_ANY_EXHDR(key));
		vfdw_cmd_add_bytes(&cmd, VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value));
	}
	vfdw_batch_add(batch, &cmd);

	reply = vfdw_batch_next(batch);
	vfdw_reply_expect(reply,
					  VFDW_RTYPE(VALKEY_REPLY_STATUS) |
					  VFDW_RTYPE(VALKEY_REPLY_INTEGER),
					  "poke failed");
	answer = reply->type == VALKEY_REPLY_INTEGER ? reply->integer : 1;

	vfdw_batch_end(batch);
	vfdw_release_connection(vconn);
	MemoryContextDelete(mctx);

	PG_RETURN_INT64(answer);
}

/*
 * valkey_fdw_test_batch_flushes(server_name, depth) -> bigint
 *
 * How many times the batch had to push its output buffer to the server while
 * queueing `depth` commands. This is the only observable consequence of
 * pipeline_batch: a suite that asserts only "all n values matched" produces
 * the same number at every depth, so deleting the line that reads the option
 * and hardcoding any bound would not change its output.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_batch_flushes);
Datum
valkey_fdw_test_batch_flushes(PG_FUNCTION_ARGS)
{
	char	   *servername = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			n = PG_GETARG_INT32(1);
	ForeignServer *server = vfdw_test_server(servername);
	UserMapping *user = GetUserMapping(GetUserId(), server->serverid);
	VfdwConn   *vconn;
	VfdwBatch  *batch;
	MemoryContext mctx;
	VfdwCmd		cmd;
	int64		flushes;
	int			i;

	if (n < 0 || n > 1000000)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("pipeline depth must be between 0 and 1000000")));

	mctx = AllocSetContextCreate(CurrentMemoryContext, "vfdw flush count",
								 ALLOCSET_DEFAULT_SIZES);

	vconn = vfdw_get_connection(server, user);
	batch = vfdw_batch_begin(vconn, mctx);

	vfdw_cmd_init(&cmd, mctx, 4);
	for (i = 0; i < n; i++)
	{
		vfdw_cmd_reset(&cmd);
		vfdw_cmd_add_cstr(&cmd, "PING");
		vfdw_batch_add(batch, &cmd);
	}

	/*
	 * Counted before the drain, so the answer describes the queueing that the
	 * option governs and not the final flush every batch performs.
	 */
	flushes = vfdw_batch_flushes(batch);

	vfdw_batch_end(batch);
	vfdw_release_connection(vconn);
	MemoryContextDelete(mctx);

	PG_RETURN_INT64(flushes);
}

/*
 * Lower the libvalkey allocation ceiling, so a suite can watch it refuse.
 *
 * The ceiling is fixed in the shipped code and nothing a statement can reach
 * may move it - a per-server option let an unprivileged caller lower it for
 * the whole backend, which is argued in src/vfdw_io.c. A bound no statement
 * can reach is also a bound no statement can prove, and an assertion that
 * cannot fail is what this tree refuses to ship, so the lever exists and is
 * confined to this extension, which is superuser-only. Zero restores the
 * built-in value.
 *
 * Process-global and NOT reset at end of statement, exactly like the thing it
 * moves: a suite that lowers it is responsible for putting it back, and
 * io.sql does.
 */
Datum
valkey_fdw_test_reply_ceiling(PG_FUNCTION_ARGS)
{
	int64		bytes;

	vfdw_test_require_superuser();
	bytes = PG_GETARG_INT64(0);

	vfdw_io_set_alloc_ceiling_for_test(bytes);

	PG_RETURN_INT64(bytes);
}
