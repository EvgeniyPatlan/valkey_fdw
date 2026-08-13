/*
 * valkey_fdw_test_keys - dump the keys a server holds, with their types.
 *
 * THIS ENTRY POINT RAISES ON AN ERROR REPLY. Its sibling
 * valkey_fdw_test_probe RETURNS one as data, and the difference is
 * deliberate: a probe describes whatever came back, including an error, while
 * a key dump has no row shape to describe one with. A dump that answered an
 * error as a row of NULLs would be indistinguishable from an empty keyspace,
 * which is the assertion a suite author most wants to trust.
 *
 * The contract is stated here, and in the sibling, because two entry points
 * with opposite error contracts and nothing saying so is how a wrong
 * assertion gets written and believed. On the acl topology a user without
 * SCAN permission takes this path.
 */
#include "postgres.h"

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
#include "vfdw_testprobe_internal.h"

PG_FUNCTION_INFO_V1(valkey_fdw_test_keys);

/*
 * Most keys a single key dump will collect.
 *
 * A probe that hung on a keyspace someone forgot to scope would be reported as
 * a flaky suite rather than as the mistake it is, so it stops and says so.
 */
#define VFDW_PROBE_KEY_CAP	10000

/*
 * How many TYPE lookups may be in flight at once.
 *
 * Matches the pipeline_batch default. Queueing all of them and reading none
 * would fill the server's output buffer while we are still filling ours, and
 * vfdw_io_flush waits only for writability today.
 */
#define VFDW_PROBE_TYPE_CHUNK	256

/* ---------------------------------------------------------------------
 * Key dump
 * --------------------------------------------------------------------- */

static void
vfdw_probe_keys_add(VfdwProbeKeys *acc, const char *data, size_t len)
{
	if (acc->nkeys >= VFDW_PROBE_KEY_CAP)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many keys match this probe prefix"),
				 errdetail("The dump stops at %d keys.", VFDW_PROBE_KEY_CAP)));

	if (acc->nkeys >= acc->capacity)
	{
		acc->capacity *= 2;
		acc->keys = (bytea **) repalloc(acc->keys,
										sizeof(bytea *) * acc->capacity);
	}

	acc->keys[acc->nkeys++] = vfdw_probe_bytea(data, len);
}

/* Byte order, with length as the tie-break: keys are binary, not text. */
static int
vfdw_probe_key_cmp(const void *a, const void *b)
{
	bytea	   *ka = *(bytea *const *) a;
	bytea	   *kb = *(bytea *const *) b;
	size_t		la = VARSIZE_ANY_EXHDR(ka);
	size_t		lb = VARSIZE_ANY_EXHDR(kb);
	int			cmp = memcmp(VARDATA_ANY(ka), VARDATA_ANY(kb), Min(la, lb));

	if (cmp != 0)
		return cmp;
	if (la == lb)
		return 0;
	return la < lb ? -1 : 1;
}

/*
 * SCAN may return the same key on more than one page, so a dump that did not
 * dedupe would report a key twice for reasons that have nothing to do with the
 * keyspace.
 */
static void
vfdw_probe_keys_dedupe(VfdwProbeKeys *acc)
{
	int			i;
	int			out = 0;

	if (acc->nkeys <= 1)
		return;

	qsort(acc->keys, acc->nkeys, sizeof(bytea *), vfdw_probe_key_cmp);

	for (i = 1; i < acc->nkeys; i++)
		if (vfdw_probe_key_cmp(&acc->keys[out], &acc->keys[i]) != 0)
			acc->keys[++out] = acc->keys[i];

	acc->nkeys = out + 1;
}

/*
 * The SCAN MATCH pattern for a literal prefix.
 *
 * The prefix is bytes, not a glob, so its metacharacters must match only
 * themselves. Without this a dump scoped to "p5:g*lob:" would report an
 * entirely different set of keys than its call site says.
 */
static char *
vfdw_probe_match(bytea *prefix, size_t *outlen)
{
	const char *src = VARDATA_ANY(prefix);
	size_t		len = VARSIZE_ANY_EXHDR(prefix);
	char	   *result = palloc(len * 2 + 2);
	char	   *dst = result;
	size_t		i;

	for (i = 0; i < len; i++)
	{
		char		c = src[i];

		if (c == '*' || c == '?' || c == '[' || c == ']' || c == '\\')
			*dst++ = '\\';
		*dst++ = c;
	}
	*dst++ = '*';

	*outlen = (size_t) (dst - result);
	return result;
}

/*
 * One SCAN page. Returns the next cursor, or NULL once the traversal is done.
 *
 * An empty page is normal - SCAN may return nothing while still having more to
 * visit - so only a cursor of "0" ends the loop (invariant I5).
 */
static char *
vfdw_probe_scan_page(VfdwBatch *batch, MemoryContext mctx, VfdwProbeKeys *acc,
					 const char *cursor, const char *match, size_t matchlen)
{
	VfdwCmd		cmd;
	valkeyReply *reply;
	const valkeyReply *cur;
	const valkeyReply *keys;
	char	   *next;
	size_t		i;

	vfdw_cmd_init(&cmd, mctx, 6);
	vfdw_cmd_add_cstr(&cmd, "SCAN");
	vfdw_cmd_add_cstr(&cmd, cursor);
	vfdw_cmd_add_cstr(&cmd, "MATCH");
	vfdw_cmd_add_bytes(&cmd, match, matchlen);
	vfdw_cmd_add_cstr(&cmd, "COUNT");
	vfdw_cmd_add_int(&cmd, VFDW_PROBE_SCAN_COUNT);
	vfdw_batch_add(batch, &cmd);

	reply = vfdw_batch_next(batch);
	vfdw_reply_expect(reply, VFDW_RTYPE(VALKEY_REPLY_ARRAY), "SCAN failed");

	cur = vfdw_probe_child(reply, 0);
	keys = vfdw_probe_child(reply, 1);

	if (cur->type != VALKEY_REPLY_STRING || !vfdw_probe_is_aggregate(keys->type))
		ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
				 errmsg("SCAN did not answer with a cursor and a key list")));

	for (i = 0; i < keys->elements; i++)
	{
		const valkeyReply *k = vfdw_probe_child(keys, i);

		if (k->type != VALKEY_REPLY_STRING)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
					 errmsg("SCAN returned a key that is not a string")));

		vfdw_probe_keys_add(acc, k->str, k->len);
	}

	if (cur->len == 1 && cur->str[0] == '0')
		return NULL;

	next = MemoryContextAlloc(mctx, cur->len + 1);
	memcpy(next, cur->str, cur->len);
	next[cur->len] = '\0';
	return next;
}

static void
vfdw_probe_type_queue(VfdwBatch *batch, MemoryContext mctx,
					  const VfdwProbeKeys *acc, int start, int count)
{
	VfdwCmd		cmd;
	int			i;

	vfdw_cmd_init(&cmd, mctx, 2);

	for (i = start; i < start + count; i++)
	{
		vfdw_cmd_reset(&cmd);
		vfdw_cmd_add_cstr(&cmd, "TYPE");
		vfdw_cmd_add_bytes(&cmd, VARDATA_ANY(acc->keys[i]),
						   VARSIZE_ANY_EXHDR(acc->keys[i]));
		vfdw_batch_add(batch, &cmd);
	}
}

/*
 * TYPE is the cheapest proof both that a key is gone ('none') and that a write
 * created the Valkey type it claimed to, which is why the dump carries it
 * rather than being a bare list of keys.
 */
static void
vfdw_probe_type_emit(ReturnSetInfo *rsinfo, VfdwBatch *batch,
					 const VfdwProbeKeys *acc, int start, int count)
{
	int			i;

	for (i = start; i < start + count; i++)
	{
		valkeyReply *reply;
		Datum		values[VFDW_KEYS_NCOLS];
		bool		nulls[VFDW_KEYS_NCOLS] = {false, false};

		CHECK_FOR_INTERRUPTS();

		reply = vfdw_batch_next(batch);

		vfdw_reply_expect(reply, VFDW_RTYPE(VALKEY_REPLY_STATUS),
						  "TYPE failed");

		values[0] = PointerGetDatum(acc->keys[i]);
		values[1] = PointerGetDatum(cstring_to_text_with_len(reply->str,
															reply->len));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
}

/*
 * valkey_fdw_test_keys(server_name, database, prefix) -> (key, keytype)
 *
 * Every key under a literal prefix, with its Valkey type, driven to cursor 0.
 *
 * This is what a ROLLBACK gate compares: the sorted (key, keytype) list before
 * a transaction against the same list after it. A count is not enough - it is
 * accidentally satisfied when one key appears and another disappears, which is
 * exactly the shape a half-applied flush has.
 */
Datum
valkey_fdw_test_keys(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	VfdwProbeKeys acc;
	VfdwConn   *vconn;
	VfdwBatch  *batch;
	MemoryContext mctx;
	char	   *match;
	const char *cursor;
	size_t		matchlen;
	int			start;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("a probe argument must not be NULL")));

	InitMaterializedSRF(fcinfo, 0);

	match = vfdw_probe_match(PG_GETARG_BYTEA_PP(2), &matchlen);
	acc.capacity = 64;
	acc.nkeys = 0;
	acc.keys = (bytea **) palloc(sizeof(bytea *) * acc.capacity);

	vconn = vfdw_probe_connect(text_to_cstring(PG_GETARG_TEXT_PP(0)),
							   PG_GETARG_INT32(1));

	mctx = AllocSetContextCreate(CurrentMemoryContext, "vfdw probe keys",
								 ALLOCSET_SMALL_SIZES);
	batch = vfdw_batch_begin(vconn, mctx);

	cursor = "0";
	do
	{
		CHECK_FOR_INTERRUPTS();
		cursor = vfdw_probe_scan_page(batch, mctx, &acc, cursor, match, matchlen);
	} while (cursor != NULL);

	vfdw_probe_keys_dedupe(&acc);

	for (start = 0; start < acc.nkeys; start += VFDW_PROBE_TYPE_CHUNK)
	{
		int			count = Min(VFDW_PROBE_TYPE_CHUNK, acc.nkeys - start);

		vfdw_probe_type_queue(batch, mctx, &acc, start, count);
		vfdw_probe_type_emit(rsinfo, batch, &acc, start, count);
	}

	vfdw_batch_end(batch);
	vfdw_release_connection(vconn);
	MemoryContextDelete(mctx);

	return (Datum) 0;
}
