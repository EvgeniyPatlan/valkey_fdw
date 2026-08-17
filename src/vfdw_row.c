/*-------------------------------------------------------------------------
 *
 * vfdw_row.c
 *		Turning a Valkey reply into a PostgreSQL tuple.
 *
 * Split from the scan itself so that paging and strategy selection stay
 * separable from value decoding - the two change for entirely different
 * reasons, and the decoding half is where the protocol version shows
 * through.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_row.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_row.h"

#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "mb/pg_wchar.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#include "vfdw_cmd.h"
#include "vfdw_ttl.h"

/*
 * One FmgrInfo per attribute, plus the per-attribute domain cache.
 *
 * A packed column's values arrive one element at a time, so the function
 * cached for it is the ELEMENT's input function: the array itself is built
 * from Datums by construct_md_array and never passes through array_in, and
 * caching array_in here would leave the elements with no input function at
 * all.
 */
void
vfdw_row_ctx_init(VfdwRowCtx *ctx, VfdwTableMap *map, MemoryContext cxt)
{
	int			i;

	ctx->map = map;
	ctx->infuncs = MemoryContextAlloc(cxt, sizeof(FmgrInfo) * Max(map->natts, 1));
	ctx->domain_extra = MemoryContextAllocZero(cxt,
											   sizeof(void *) * Max(map->natts, 1));
	ctx->cache_cxt = cxt;
	ctx->cur_elem = 0;
	ctx->ttl_ms = map->nttl > 0
		? MemoryContextAllocZero(cxt, sizeof(int64) * map->nttl)
		: NULL;

	for (i = 0; i < map->natts; i++)
	{
		const VfdwColumn *col = &map->cols[i];

		if (col->attnum == InvalidAttrNumber)
			continue;

		fmgr_info_cxt(col->packed != NULL ? col->packed->col.typinput
					  : col->typinput, &ctx->infuncs[i], cxt);
	}
}

/*
 * Bytes from Valkey to a Datum of the column's declared type.
 *
 * Lengths travel with the data all the way in (invariant I3): nothing here
 * treats the bytes as a C string, so a value containing a NUL arrives whole.
 */
Datum
vfdw_row_datum_from_bytes(VfdwRowCtx *ctx, const VfdwColumn *col,
						  const char *data, size_t len)
{
	int			idx = col->attnum - 1;

	if (col->is_binary)
	{
		bytea	   *b = (bytea *) palloc(VARHDRSZ + len);
		Datum		d;

		SET_VARSIZE(b, VARHDRSZ + len);
		memcpy(VARDATA(b), data, len);
		d = PointerGetDatum(b);

		/*
		 * A domain over bytea is binary too, so it never reaches its own
		 * input function and would otherwise carry its CHECK constraints in
		 * name only. The cached DomainIOData is what keeps this from costing
		 * a syscache lookup and an expression compile per value.
		 */
		if (col->is_domain)
			domain_check(d, false, col->typid,
						 ctx->domain_extra != NULL ? &ctx->domain_extra[idx] : NULL,
						 ctx->cache_cxt);

		return d;
	}

	/*
	 * Anything not declared bytea becomes a text representation, so it has to
	 * be valid in the server encoding. Passing these bytes straight into a
	 * text datum lets invalidly-encoded data into the database.
	 *
	 * There is deliberately no mirror of this check in the write direction:
	 * outbound bytes were produced by the server itself and are already in
	 * the server encoding, so verifying them would verify the server against
	 * itself (src/vfdw_val.c states the argument in full).
	 */
	pg_verifymbstr(data, (int) len, false);

	return InputFunctionCall(&ctx->infuncs[idx], pnstrdup(data, len),
							 col->typioparam, col->typmod);
}

/*
 * Store one value into a slot attribute, honouring the column's type.
 */
static void
vfdw_scan_store(VfdwRowCtx *ctx, TupleTableSlot *slot,
				const VfdwColumn *col, const char *data, size_t len)
{
	int			idx = col->attnum - 1;

	/*
	 * The read side can use a null pointer as its NULL signal because
	 * libvalkey never hands back a NULL str for a zero-length bulk. The write
	 * side has no such guarantee and must not copy this: see VfdwValue.
	 */
	if (data == NULL)
	{
		slot->tts_isnull[idx] = true;
		return;
	}

	slot->tts_values[idx] = vfdw_row_datum_from_bytes(ctx, col, data, len);
	slot->tts_isnull[idx] = false;
}

/*
 * The whole collection, as one array, for a packed table.
 *
 * WHAT THE ARRAY HOLDS, per table type, and why:
 *
 * A hash is its reply in the reply's own order - field, value, field, value.
 * That flat alternation is what makes hstore(value) reconstruct the hash, and
 * reconstructing a hash whose fields were not known when the table was defined
 * is the whole reason this shape exists. It is the same order under both
 * protocol versions: libvalkey lays a RESP3 map out as 2n alternating children
 * exactly as RESP2 sends them, which is the same fact vfdw_scan_hash_lookup
 * relies on to walk a hash in pairs without asking which protocol it got.
 *
 * A list and a set are their members, in the order the server gave them.
 *
 * A zset is its members and NOT its scores: vfdw_scan_value_command drops
 * WITHSCORES for a packed table, so the reply is a flat array of members under
 * either protocol. Packing scores as well would make the array's shape depend
 * on the transport rather than on the data, since WITHSCORES answers RESP2
 * with member and score alternating and RESP3 with a nested pair per member.
 *
 * Every element goes through vfdw_row_datum_from_bytes, so a text element is
 * verified against the server encoding by the one route every other text value
 * takes, and a bytea element keeps bytes that route would reject (I3).
 */
static void
vfdw_row_store_packed(VfdwRowCtx *ctx, TupleTableSlot *slot,
					  const VfdwColumn *col, const valkeyReply *reply)
{
	const VfdwPackedElem *elem = col->packed;
	int			idx = col->attnum - 1;
	int			n = (int) reply->elements;
	Datum	   *values = palloc(sizeof(Datum) * Max(n, 1));
	bool	   *nulls = palloc0(sizeof(bool) * Max(n, 1));
	int			dims[1];
	int			lbs[1];
	int			i;

	for (i = 0; i < n; i++)
	{
		const valkeyReply *e = vfdw_reply_child(reply, (size_t) i);

		/*
		 * A null element becomes a NULL element rather than being dropped:
		 * dropping it would shift every later member down one position, and
		 * for a hash that turns the field,value alternation into a mapping of
		 * each field onto the next field's value. libvalkey never leaves str
		 * NULL for a zero-length bulk, so this is an element that carried no
		 * bytes at all rather than an empty one.
		 */
		if (e->str == NULL)
		{
			nulls[i] = true;
			continue;
		}

		values[i] = vfdw_row_datum_from_bytes(ctx, &elem->col, e->str, e->len);
	}

	dims[0] = n;
	lbs[0] = 1;
	slot->tts_values[idx] =
		PointerGetDatum(construct_md_array(values, nulls, 1, dims, lbs,
										   elem->col.typid, elem->typlen,
										   elem->typbyval, elem->typalign));
	slot->tts_isnull[idx] = false;
}

/*
 * Find a field's value within a hash reply.
 *
 * RESP3 answers HGETALL with a map, RESP2 with a flat alternating array. Both
 * are pairs; only the container type differs.
 *
 * The pair walk indexes element[] from the server's own element count, so
 * every child is taken through vfdw_reply_child, whose guard is what keeps an
 * element the reply declared and did not carry from becoming a heap overflow.
 */
static bool
vfdw_scan_hash_lookup(const valkeyReply *reply, const char *field,
					  const char **data, size_t *len)
{
	size_t		i;
	size_t		flen = strlen(field);

	for (i = 0; i + 1 < reply->elements; i += 2)
	{
		const valkeyReply *k = vfdw_reply_child(reply, i);
		const valkeyReply *v = vfdw_reply_child(reply, i + 1);

		/*
		 * A field name with no bytes behind it cannot be the one asked for,
		 * and comparing it would read the NULL libvalkey left there.
		 */
		if (k->str == NULL)
			continue;

		if (k->len == flen && memcmp(k->str, field, flen) == 0)
		{
			*data = v->str;
			*len = v->len;
			return true;
		}
	}
	return false;
}

/*
 * Locate the member, and for a zset its score, for one row of a collection
 * reply.
 *
 * The two protocol versions disagree about the shape here. RESP2 answers
 * ZRANGE ... WITHSCORES with a flat array alternating member and score;
 * RESP3 answers with an array of two-element pairs. Reading one layout as
 * the other yields the wrong number of rows with null members, so the shape
 * is detected rather than assumed.
 */
static void vfdw_row_fill_column(VfdwRowCtx *ctx, TupleTableSlot *slot,
								 const VfdwColumn *col, const char *key,
								 size_t keylen, valkeyReply *reply);

static bool
vfdw_scan_member_at(const valkeyReply *reply, int idx, bool want_score,
					const valkeyReply **member, const valkeyReply **score)
{
	const valkeyReply *first;

	if (reply->elements == 0 || idx >= (int) reply->elements)
		return false;

	first = vfdw_reply_child(reply, 0);

	if (want_score && first->type == VALKEY_REPLY_ARRAY)
	{
		/* RESP3: one pair per element. */
		const valkeyReply *pair = vfdw_reply_child(reply, (size_t) idx);

		if (pair->type != VALKEY_REPLY_ARRAY || pair->elements < 2)
			return false;
		*member = vfdw_reply_child(pair, 0);
		*score = vfdw_reply_child(pair, 1);
		return true;
	}

	if (want_score)
	{
		/* RESP2: member and score alternate. */
		if (idx + 1 >= (int) reply->elements)
			return false;
		*member = vfdw_reply_child(reply, (size_t) idx);
		*score = vfdw_reply_child(reply, (size_t) idx + 1);
		return true;
	}

	*member = vfdw_reply_child(reply, (size_t) idx);
	*score = NULL;
	return true;
}

/*
 * How far the element index advances per row.
 *
 * One for a list or set, and for a RESP3 zset whose elements are already
 * pairs; two for a RESP2 zset, where a row spans a member and its score.
 */
int
vfdw_scan_member_stride(VfdwTableType type, const valkeyReply *reply)
{
	if (type != VFDW_TABLE_ZSET)
		return 1;
	if (reply->elements > 0 &&
		vfdw_reply_child(reply, 0)->type == VALKEY_REPLY_ARRAY)
		return 1;
	return 2;
}

/*
 * One row per member, or one row per key?
 *
 * The MAP decides, not the table type. A packed table over a list, a set or a
 * zset is the same table type as a column-mapped one and answers with one row
 * per KEY, the members packed into its array column; taking the answer from
 * the type gives it one row per member instead, each carrying a copy of the
 * whole collection, and the row count is then the member count rather than the
 * key count.
 */
bool
vfdw_scan_is_multirow(const VfdwTableMap *map)
{
	if (map->legacy_value)
		return false;

	return map->tabletype == VFDW_TABLE_LIST ||
		map->tabletype == VFDW_TABLE_SET ||
		map->tabletype == VFDW_TABLE_ZSET;
}

/*
 * Fill a slot from one key and its value reply.
 */
void
vfdw_scan_fill(VfdwRowCtx *ctx, TupleTableSlot *slot,
			   const char *key, size_t keylen, valkeyReply *reply)
{
	VfdwTableMap *map = ctx->map;
	int			i;

	ExecClearTuple(slot);
	for (i = 0; i < map->natts; i++)
		slot->tts_isnull[i] = true;

	for (i = 0; i < map->natts; i++)
		vfdw_row_fill_column(ctx, slot, &map->cols[i], key, keylen, reply);

	ExecStoreVirtualTuple(slot);
}

/*
 * A ttl column, from the expiries copied out before the value reply arrived.
 *
 * Left NULL when the server had no duration to report, which vfdw_ttl_datum
 * decides: a field with no expiry and a field that is not there are both
 * absences rather than durations, and a row whose field is missing has its
 * other columns NULL for the same reason.
 */
static void
vfdw_row_store_ttl(VfdwRowCtx *ctx, TupleTableSlot *slot,
				   const VfdwColumn *col)
{
	Datum		value;

	/*
	 * ttl_ms is NULL only when the map says there are no ttl columns, and then
	 * this is unreachable. Tested anyway: the cost is one branch per ttl column
	 * per row, and what it buys is that a path which builds a tuple without
	 * having asked for expiries reports NULL rather than dereferencing NULL.
	 */
	if (ctx->ttl_ms == NULL || !vfdw_ttl_datum(ctx->ttl_ms[col->ttl_slot], &value))
		return;

	slot->tts_values[col->attnum - 1] = value;
	slot->tts_isnull[col->attnum - 1] = false;
}

/*
 * A zset member's score, which is the second half of the pair the member came
 * from and so is found the same way rather than by a parallel index.
 */
static void
vfdw_row_store_score(VfdwRowCtx *ctx, TupleTableSlot *slot,
					 const VfdwColumn *col, valkeyReply *reply)
{
	const valkeyReply *m = NULL;
	const valkeyReply *sc = NULL;

	if (vfdw_scan_member_at(reply, ctx->cur_elem, true, &m, &sc) &&
		sc != NULL && sc->str != NULL)
		vfdw_scan_store(ctx, slot, col, sc->str, sc->len);
}

/*
 * A list member's index within its list, ZERO-BASED.
 *
 * Zero because that is how every Valkey verb taking a list index counts:
 * LINDEX, LSET and LRANGE all read 0 as the head. Numbering from 1 to match
 * SQL array subscripts would make `WHERE pos = 1` mean the second member to
 * the server and the first to the reader.
 *
 * cur_elem IS the index rather than a count kept beside it. A list has a
 * stride of one, so the element being emitted and its position are the same
 * number, and a second counter would be a second thing to keep in step.
 */
static void
vfdw_row_store_position(VfdwRowCtx *ctx, TupleTableSlot *slot,
						const VfdwColumn *col)
{
	slot->tts_values[col->attnum - 1] =
		(getBaseType(col->typid) == INT8OID)
		? Int64GetDatum((int64) ctx->cur_elem)
		: Int32GetDatum(ctx->cur_elem);
	slot->tts_isnull[col->attnum - 1] = false;
}

/*
 * One hash field, from whichever of the two replies this table asks for.
 *
 * HGETALL carries the field names beside the values and is walked in pairs;
 * HMGET carries only values, in the order the fields were asked for, so the
 * column's own slot is the answer. Which shape arrived is not guessed from the
 * reply - map->hmget is what asked for it, and vfdw_scan_cmd.c reads the same
 * flag when it builds the command.
 */
static void
vfdw_row_store_field(VfdwRowCtx *ctx, TupleTableSlot *slot,
					 const VfdwColumn *col, valkeyReply *reply)
{
	const char *data = NULL;
	size_t		len = 0;

	if (reply->type != VALKEY_REPLY_MAP && reply->type != VALKEY_REPLY_ARRAY)
		return;

	if (ctx->map->hmget)
	{
		const valkeyReply *e;

		if ((size_t) col->field_slot >= reply->elements)
			return;

		e = vfdw_reply_child(reply, (size_t) col->field_slot);
		if (e != NULL && e->str != NULL)
			vfdw_scan_store(ctx, slot, col, e->str, e->len);
		return;
	}

	if (vfdw_scan_hash_lookup(reply, col->field, &data, &len))
		vfdw_scan_store(ctx, slot, col, data, len);
}

static void
vfdw_row_fill_column(VfdwRowCtx *ctx, TupleTableSlot *slot,
					 const VfdwColumn *col, const char *key, size_t keylen,
					 valkeyReply *reply)
{
	VfdwTableMap *map = ctx->map;

	switch (col->kind)
	{
		case VFDW_COL_KEY:
			vfdw_scan_store(ctx, slot, col, key, keylen);
			break;

		case VFDW_COL_VALUE:
			if (reply->type == VALKEY_REPLY_STRING)
				vfdw_scan_store(ctx, slot, col, reply->str, reply->len);
			break;

		case VFDW_COL_FIELD:
			vfdw_row_store_field(ctx, slot, col, reply);
			break;

		case VFDW_COL_LEGACY_VALUE:
			vfdw_row_store_packed(ctx, slot, col, reply);
			break;

		case VFDW_COL_MEMBER:
		{
			const valkeyReply *m = NULL;
			const valkeyReply *sc = NULL;

			if (vfdw_scan_member_at(reply, ctx->cur_elem,
									map->tabletype == VFDW_TABLE_ZSET,
									&m, &sc))
				vfdw_scan_store(ctx, slot, col, m->str, m->len);
			break;
		}

		case VFDW_COL_SCORE:
			vfdw_row_store_score(ctx, slot, col, reply);
			break;

		case VFDW_COL_TTL:
			vfdw_row_store_ttl(ctx, slot, col);
			break;

		case VFDW_COL_POSITION:
			vfdw_row_store_position(ctx, slot, col);
			break;

		default:
			/* distance arrives with the phase that fills it. */
			break;
	}
}
