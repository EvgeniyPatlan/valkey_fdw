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

#include "executor/executor.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"

#include "vfdw_cmd.h"

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

bool
vfdw_scan_is_multirow(VfdwTableType type)
{
	return type == VFDW_TABLE_LIST || type == VFDW_TABLE_SET ||
		type == VFDW_TABLE_ZSET;
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

static void
vfdw_row_fill_column(VfdwRowCtx *ctx, TupleTableSlot *slot,
					 const VfdwColumn *col, const char *key, size_t keylen,
					 valkeyReply *reply)
{
	VfdwTableMap *map = ctx->map;
	const char *data = NULL;
	size_t		len = 0;

	{
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
				if (reply->type == VALKEY_REPLY_MAP ||
					reply->type == VALKEY_REPLY_ARRAY)
				{
					if (vfdw_scan_hash_lookup(reply, col->field, &data, &len))
						vfdw_scan_store(ctx, slot, col, data, len);
				}
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
			{
				const valkeyReply *m = NULL;
				const valkeyReply *sc = NULL;

				if (vfdw_scan_member_at(reply, ctx->cur_elem, true, &m, &sc) &&
					sc != NULL && sc->str != NULL)
					vfdw_scan_store(ctx, slot, col, sc->str, sc->len);
				break;
			}

			default:
				/* Remaining kinds arrive with the write and search phases. */
				break;
		}
	}
}
