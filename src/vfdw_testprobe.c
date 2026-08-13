/*-------------------------------------------------------------------------
 *
 * vfdw_testprobe.c
 *		Keyspace probes: what actually landed in Valkey, as SQL rows.
 *
 * THIS ENTRY POINT RETURNS AN ERROR REPLY AS DATA. Its sibling
 * valkey_fdw_test_keys, in src/vfdw_testkeys.c, RAISES on one. The divergence
 * is deliberate: a probe exists to
 * describe whatever came back, and an error is one of the things that can come
 * back, so raising would throw away the answer the caller asked for. A key
 * dump has no row shape to describe an error with, and answering one as a row
 * of NULLs would be indistinguishable from an empty keyspace.
 *
 * What was wrong was not the contracts, it was that nothing said so - and two
 * sibling entry points with opposite error contracts is how a suite author
 * writes a wrong assertion and believes it. Both are asserted: probe.sql for
 * the returning half, probe_acl.sql for both side by side, on a user that can
 * authenticate and do nothing else.
 *
 * Split from src/vfdw_testfuncs.c for the same reason src/vfdw_testval.c was:
 * that file is already most of the way to the 800-line gate and these answer a
 * different question.
 *
 * These are how a write suite asserts its result. A write checked by reading
 * the same table back through the FDW proves only that the wrapper's two
 * halves agree with each other - and once the overlay lands, a scan does not
 * even reach the server for a row the current transaction wrote. So the
 * assertion has to come from somewhere else, and this is it.
 *
 * They go through the pool (vfdw_get_connection) rather than shelling out to
 * valkey-cli, which decides three things at once:
 *
 *	- they inherit AUTH, TLS, the logical database and the configured timeouts
 *	  from the server options and user mapping, so they work on the tls, acl
 *	  and cluster topologies where a plaintext valkey-cli either cannot connect
 *	  or connects as nobody;
 *	- arguments are a bytea vector rather than a shell command line, so a key
 *	  or value containing a space, a quote, a tab, a newline or a NUL is
 *	  ordinary rather than impossible;
 *	- a failure is a failure. A shell fixture that ends in "; echo ok" reports
 *	  success for every command, and a Valkey error arrives on stdout with exit
 *	  status 0, so even without the echo it would come back looking like a
 *	  value.
 *
 * WHAT THIS CANNOT SEE, stated plainly because the alternative is a suite that
 * believes it proves more than it does. The probe shares vfdw_conn.c,
 * vfdw_cmd.c, vfdw_io.c and vfdw_error.c with the code under test. A defect
 * confined to those four files is invisible here BY CONSTRUCTION, and it is
 * invisible SYMMETRICALLY: a bug that truncates an outgoing argument truncates
 * it identically for the probe's write and the probe's read, so a round trip
 * through the probe agrees with itself and passes. Two rules follow, and both
 * are binding on every assertion written against this file:
 *
 *	1. No keyspace assertion may be a probe-write / probe-read round trip on
 *	   its own. Every one must include at least one number the server computed
 *	   and the probe cannot echo: STRLEN, HLEN, SCARD, ZCARD, LLEN, EXISTS,
 *	   TYPE, LPOS.
 *	2. A green suite built on this says nothing about the transport. The
 *	   transport is tested by io.sql and fault.sql, which reach a raw socket
 *	   through valkey_fdw_test_ping and valkey_fdw_test_binary.
 *
 * Nesting is ONE LEVEL DEEP. An element that is itself an aggregate is
 * reported as its own type and element count, and its children are not
 * emitted. Anything needing depth gets a purpose-built entry point, which is
 * what valkey_fdw_test_keys is: SCAN's reply is a cursor beside a nested list,
 * driven to zero in a loop, and putting that in the caller would force the
 * nested case into this contract and a WHILE loop into every suite that wants
 * a key dump.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_testprobe.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw.h"

#include "catalog/pg_type.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "vfdw_cmd.h"
#include "vfdw_conn.h"
#include "vfdw_error.h"
#include "vfdw_io.h"
#include "vfdw_test_common.h"
#include "vfdw_testprobe_internal.h"

PG_FUNCTION_INFO_V1(valkey_fdw_test_probe);
PG_FUNCTION_INFO_V1(valkey_fdw_test_probe_pairs);

/* Columns of the probe's result row. */
#define VFDW_PROBE_NCOLS	6



/*
 * The typed columns one reply value contributes to a row.
 */
typedef struct VfdwProbeCols
{
	const char *type;
	bytea	   *keypart;
	bytea	   *valpart;
	bool		has_num;
	int64		num;
	bool		has_dbl;
	double		dbl;
} VfdwProbeCols;

/* ---------------------------------------------------------------------
 * Bytes
 * --------------------------------------------------------------------- */

/*
 * Copy exactly len bytes out of a reply.
 *
 * Never strlen: a Valkey string is a byte string and may contain a NUL, so
 * anything that measures it as a C string reports a shorter value than the one
 * that is actually stored (invariant I3).
 */
bytea *
vfdw_probe_bytea(const char *data, size_t len)
{
	bytea	   *result;

	if (data == NULL)
	{
		/*
		 * A length with no data behind it is libvalkey disagreeing with
		 * itself. Reading it would be a heap overflow, so say so instead.
		 */
		if (len != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_ERROR),
					 errmsg("a Valkey reply declared a length with no bytes")));
		return NULL;
	}

	result = (bytea *) palloc(VARHDRSZ + len);
	SET_VARSIZE(result, VARHDRSZ + len);
	memcpy(VARDATA(result), data, len);
	return result;
}

/* ---------------------------------------------------------------------
 * Reply shape
 * --------------------------------------------------------------------- */

bool
vfdw_probe_is_aggregate(int type)
{
	return type == VALKEY_REPLY_ARRAY ||
		type == VALKEY_REPLY_SET ||
		type == VALKEY_REPLY_MAP ||
		type == VALKEY_REPLY_PUSH ||
		type == VALKEY_REPLY_ATTR;
}

/*
 * valkey_fdw_test_probe_child_guard() -> text
 *
 * Exercise vfdw_reply_child's arity guard, which nothing arriving over the
 * wire can reach.
 *
 * The guard is not a probe helper: it stands wherever a reply the server
 * shaped is indexed, which is the scan's hash and zset decoding, the cluster
 * shard parse and these entry points alike. It is called here with a reply
 * built by hand because that is the only caller that can make it fire.
 *
 * THE REGISTER RECORDED THIS AS UNREACHABLE FOR THE WRONG REASON - that
 * libvalkey leaves reply->str NULL for every aggregate, which is true and
 * concerns a different function. The real reason is that libvalkey's reader
 * never HANDS UP a short aggregate: a frame declaring more elements than it
 * carries is incomplete, so the reader waits for the rest and the connection
 * closing yields EOF rather than a truncated array. The fault proxy cannot
 * fabricate one either, for the same reason.
 *
 * So the guard is not defence against a server, it is defence against a
 * libvalkey whose framing changes. That is worth keeping and worth
 * executing, and a hand-built reply is the only thing that executes it.
 * Calling it with a well-formed reply first is what makes the second call a
 * test of the guard rather than of the call itself.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_probe_child_guard);
Datum
valkey_fdw_test_probe_child_guard(PG_FUNCTION_ARGS)
{
	valkeyReply child = {0};
	valkeyReply *children[2];
	valkeyReply parent = {0};

	child.type = VALKEY_REPLY_INTEGER;
	child.integer = 1;
	children[0] = &child;
	children[1] = NULL;

	parent.type = VALKEY_REPLY_ARRAY;
	parent.elements = 2;
	parent.element = children;

	/* Element 0 is present, so this must return rather than raise. */
	if (vfdw_reply_child(&parent, 0) != &child)
		elog(ERROR, "guard rejected an element that is present");

	/* Element 1 was declared and is absent. This must raise. */
	(void) vfdw_reply_child(&parent, 1);

	elog(ERROR, "guard admitted a missing element");
	PG_RETURN_NULL();
}

/*
 * Reduce one reply value to the columns that describe it.
 *
 * An aggregate reports its element count and nothing else; its children are
 * the caller's business.
 */
static void
vfdw_probe_columns(const valkeyReply *reply, VfdwProbeCols *out)
{
	memset(out, 0, sizeof(*out));
	out->type = vfdw_reply_type_name(reply->type);

	switch (reply->type)
	{
		case VALKEY_REPLY_INTEGER:
			out->has_num = true;
			out->num = (int64) reply->integer;
			break;

		case VALKEY_REPLY_BOOL:
			out->has_num = true;
			out->num = reply->integer != 0 ? 1 : 0;
			break;

		case VALKEY_REPLY_DOUBLE:

			/*
			 * libvalkey fills str for a double in addition to dval, so both
			 * the exact binary64 value and the server's own rendering of it
			 * are observable. A score assertion wants the former.
			 */
			out->has_dbl = true;
			out->dbl = reply->dval;
			out->valpart = vfdw_probe_bytea(reply->str, reply->len);
			break;

		case VALKEY_REPLY_VERB:
			/* vtype is a fixed 4-byte field; bound the read to it. */
			out->keypart = vfdw_probe_bytea(reply->vtype,
											strnlen(reply->vtype, 3));
			out->valpart = vfdw_probe_bytea(reply->str, reply->len);
			break;

		case VALKEY_REPLY_STRING:
		case VALKEY_REPLY_STATUS:
		case VALKEY_REPLY_ERROR:
		case VALKEY_REPLY_BIGNUM:
			out->valpart = vfdw_probe_bytea(reply->str, reply->len);
			break;

		case VALKEY_REPLY_NIL:
			break;

		default:
			if (vfdw_probe_is_aggregate(reply->type))
			{
				out->has_num = true;
				out->num = (int64) reply->elements;
			}
			break;
	}
}

/* ---------------------------------------------------------------------
 * Emission
 * --------------------------------------------------------------------- */

static void
vfdw_probe_row(ReturnSetInfo *rsinfo, const char *type, int ordinal,
			   const VfdwProbeCols *cols, bytea *keypart)
{
	Datum		values[VFDW_PROBE_NCOLS];
	bool		nulls[VFDW_PROBE_NCOLS];

	values[0] = CStringGetTextDatum(type);
	nulls[0] = false;

	values[1] = Int32GetDatum(ordinal);
	nulls[1] = false;

	values[2] = PointerGetDatum(keypart);
	nulls[2] = (keypart == NULL);

	values[3] = PointerGetDatum(cols->valpart);
	nulls[3] = (cols->valpart == NULL);

	values[4] = Int64GetDatum(cols->num);
	nulls[4] = !cols->has_num;

	values[5] = Float8GetDatum(cols->dbl);
	nulls[5] = !cols->has_dbl;

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * The element rows of an aggregate, at ordinals 1..N.
 *
 * Under `pairs` two consecutive elements become one row: the first supplies
 * key_part and the second supplies everything else. That is what makes a RESP3
 * MAP and a RESP2 flat ARRAY render identically, which is the only reason an
 * HGETALL or ZRANGE ... WITHSCORES assertion can be protocol-independent.
 */
static void
vfdw_probe_elements(ReturnSetInfo *rsinfo, const valkeyReply *reply, bool pairs)
{
	size_t		i = 0;
	int			ordinal = 0;

	while (i < reply->elements)
	{
		VfdwProbeCols cols;
		bytea	   *keypart;

		/*
		 * Invariant I6: every wait is interruptible, and so is every loop
		 * long enough to be worth waiting on. A million-element LRANGE
		 * materialises here, and without this the backend ignores a cancel
		 * until the whole reply has been turned into rows.
		 */
		CHECK_FOR_INTERRUPTS();

		vfdw_probe_columns(vfdw_reply_child(reply, i), &cols);
		i++;

		if (!pairs)
			keypart = cols.keypart;
		else
		{
			keypart = cols.valpart;
			vfdw_probe_columns(vfdw_reply_child(reply, i), &cols);
			i++;
		}

		vfdw_probe_row(rsinfo, cols.type, ++ordinal, &cols, keypart);
	}
}

/*
 * Every reply emits at least one row, at ordinal 0.
 *
 * That is the whole answer to "what did the probe say". A nil is one row
 * saying 'nil'; an empty array is one row saying 'array' with num = 0; and
 * because neither is zero rows and neither is SQL NULL, "the probe returned
 * nothing" stays a third, visibly different outcome instead of a synonym for
 * either.
 */
static void
vfdw_probe_render(ReturnSetInfo *rsinfo, const valkeyReply *reply, bool pairs)
{
	VfdwProbeCols cols;
	bool		aggregate = vfdw_probe_is_aggregate(reply->type);

	vfdw_probe_columns(reply, &cols);

	if (aggregate && pairs)
	{
		if ((reply->elements % 2) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
					 errmsg("a paired reply must have an even number of "
							"elements")));

		cols.type = "map";
		cols.num = (int64) (reply->elements / 2);
	}

	vfdw_probe_row(rsinfo, cols.type, 0, &cols, cols.keypart);

	if (aggregate)
		vfdw_probe_elements(rsinfo, reply, pairs);
}

/* ---------------------------------------------------------------------
 * Arguments and the pooled connection
 * --------------------------------------------------------------------- */

/*
 * The pooled connection for this server, positioned on this database.
 *
 * The database is a required argument and is not defaulted. A table on
 * database '3' writes where a default-database probe cannot see it, so a
 * probe that silently assumed 0 would make that assertion vacuous.
 */
VfdwConn *
vfdw_probe_connect(const char *servername, int database)
{
	ForeignServer *server;
	UserMapping *user;
	VfdwConn   *vconn;

	if (database < 0 || database > 15)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("database must be between 0 and 15")));

	server = vfdw_test_server(servername);
	user = GetUserMapping(GetUserId(), server->serverid);

	/*
	 * Cluster-aware, because a probe IS node-scoped by definition: it runs
	 * one command against one node and reports what came back. That is the
	 * caveat already recorded above about a key dump on a cluster reporting
	 * only the slots its node serves, and it is the reason a probe is the
	 * right tool for asking a cluster a question about itself - including
	 * whether our routing agrees with the server's.
	 */
	vconn = vfdw_get_connection_cluster(server, user);
	vfdw_conn_select_db(vconn, database);

	return vconn;
}

/*
 * Turn the variadic bytea[] into a command.
 *
 * A NULL element does not trip STRICT - and STRICT on a set-returning function
 * would answer a NULL argument with ZERO ROWS, which is exactly the "the
 * helper quietly did nothing" outcome this whole file exists to make
 * impossible. So the array is deconstructed and every element is checked.
 */
static void
vfdw_probe_args(FunctionCallInfo fcinfo, VfdwCmd *cmd)
{
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(2);
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	int			i;

	deconstruct_array(arr, BYTEAOID, -1, false, TYPALIGN_INT,
					  &elems, &nulls, &nelems);

	if (nelems == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a probe needs at least a command verb")));

	for (i = 0; i < nelems; i++)
	{
		bytea	   *arg;

		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("a probe argument must not be NULL")));

		arg = DatumGetByteaPP(elems[i]);
		vfdw_cmd_add_bytes(cmd, VARDATA_ANY(arg), VARSIZE_ANY_EXHDR(arg));
	}
}

/*
 * Run one command and render its reply.
 *
 * A Valkey ERROR reply is rendered as data, never raised: a helper that raised
 * on WRONGTYPE could not be used to assert that a key holds the wrong type,
 * and the fault suites have to read the keyspace after a fault has fired. A
 * connection, AUTH, TLS, deadline or cancellation failure still raises through
 * the ordinary machinery, because a probe that reported "could not connect" as
 * data would let a test that never reached the server report a clean keyspace.
 */
static Datum
vfdw_probe_run(FunctionCallInfo fcinfo, bool pairs)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	VfdwConn   *vconn;
	VfdwBatch  *batch;
	MemoryContext mctx;
	VfdwCmd		cmd;
	valkeyReply *reply;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("a probe argument must not be NULL")));

	InitMaterializedSRF(fcinfo, 0);

	vconn = vfdw_probe_connect(text_to_cstring(PG_GETARG_TEXT_PP(0)),
							   PG_GETARG_INT32(1));

	mctx = AllocSetContextCreate(CurrentMemoryContext, "vfdw probe",
								 ALLOCSET_SMALL_SIZES);

	/*
	 * The command is built before the batch opens, so a caller bug - a NULL
	 * argument, no verb - costs nothing. Raising after vfdw_batch_begin would
	 * leave the connection flagged mid-conversation, and the transaction
	 * callback would then discard and reopen a connection that never had a
	 * byte sent on it.
	 */
	vfdw_cmd_init(&cmd, mctx, 8);
	vfdw_probe_args(fcinfo, &cmd);

	batch = vfdw_batch_begin(vconn, mctx);
	vfdw_batch_add(batch, &cmd);

	reply = vfdw_batch_next(batch);
	if (reply == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("Valkey sent no reply to a probe command")));

	vfdw_probe_render(rsinfo, reply, pairs);

	/* Success path only: on an error path the context unwind does it (I1). */
	vfdw_batch_end(batch);
	vfdw_release_connection(vconn);
	MemoryContextDelete(mctx);

	return (Datum) 0;
}

Datum
valkey_fdw_test_probe(PG_FUNCTION_ARGS)
{
	return vfdw_probe_run(fcinfo, false);
}

Datum
valkey_fdw_test_probe_pairs(PG_FUNCTION_ARGS)
{
	return vfdw_probe_run(fcinfo, true);
}
