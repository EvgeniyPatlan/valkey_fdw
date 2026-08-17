/*-------------------------------------------------------------------------
 *
 * vfdw_render.c
 *		Turning one tuple slot into the bytes an operation carries.
 *
 * The order the columns are rendered in is load-bearing and belongs to the
 * callers, which is where it is stated: vfdw_modify_insert and its siblings
 * call key, score, member, payload in that order - cheapest refusals first,
 * so the common ones leave nothing retained in the write buffer.
 *
 * Split out of vfdw_modify.c at the 800-line gate. The seam is that nothing
 * here appends, folds or touches a connection; it only produces bytes.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_render.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_modify.h"

#include "utils/memutils.h"
#include "utils/rel.h"

#include "vfdw_render.h"

#include "utils/array.h"
#include "vfdw_val.h"
#include "vfdw_wbuf.h"

/*
 * Copy a hash field name into the buffer context.
 *
 * col->field points into the VfdwTableMap, which dies with stmt_cxt at
 * EndForeignModify, long before the pre-commit flush reads it. It is a
 * catalog option string of ours and NUL-terminated, so strlen on it is
 * correct (invariant I3 is about Valkey-owned bytes).
 */
const char *
vfdw_modify_retain_name(const char *field, size_t *namelen)
{
	MemoryContext dest = vfdw_wbuf_context();
	size_t		len = strlen(field);
	char	   *copy = MemoryContextAlloc(dest, len + 1);

	memcpy(copy, field, len + 1);
	*namelen = len;
	return copy;
}

/*
 * Every mapped hash field of the row, NULLs included.
 *
 * A NULL field is recorded rather than dropped: at flush time it is an HDEL,
 * and a field the statement did not map is simply not in this array. A row
 * whose every mapped field is NULL is refused for INSERT only; the reason it
 * is not refused for UPDATE is with the check below.
 */
void
vfdw_modify_render_fields(VfdwModifyState *st, TupleTableSlot *slot,
						  VfdwWriteOp *op)
{
	const VfdwTableMap *map = st->map;
	VfdwWriteArg *args;
	int			n = 0;
	int			nonnull = 0;
	int			i;

	if (map->nfields == 0)
		return;

	args = (VfdwWriteArg *) MemoryContextAllocZero(vfdw_wbuf_context(),
												   sizeof(VfdwWriteArg) *
												   map->nfields);

	for (i = 0; i < map->natts; i++)
	{
		const VfdwColumn *col = &map->cols[i];
		Datum		d;
		bool		isnull;
		VfdwValue	v;

		if (col->kind != VFDW_COL_FIELD)
			continue;

		d = slot_getattr(slot, col->attnum, &isnull);
		vfdw_val_from_slot(&st->vc, map, col, d, isnull, &v);

		args[n].name = vfdw_modify_retain_name(col->field, &args[n].namelen);
		args[n].isnull = v.isnull;
		args[n].data = v.data;
		args[n].len = v.len;
		if (!v.isnull)
			nonnull++;
		n++;
	}

	/*
	 * The all-fields-null rule is right for INSERT and wrong for UPDATE, so
	 * it is scoped to INSERT here rather than applied to both.
	 *
	 * For INSERT it is right: a row that sets none of the mapped fields would
	 * create nothing at all, so reporting one row inserted would be a lie.
	 *
	 * For UPDATE it is not. Clearing every mapped field is a legitimate thing
	 * to ask for - it means HDEL those fields - and refusing it leaves no way
	 * to express it. If the table maps every field the key then disappears,
	 * which is not a special case this code needs to handle: README's
	 * boundary already states that emptying a row and deleting it are the
	 * same physical outcome, because Valkey has no "exists but empty" state.
	 */
	if (nonnull == 0 && op->kind == VFDW_OP_INSERT)
		vfdw_val_error_all_fields_null(st->rel);

	op->fields = args;
	op->nfields = n;
}

/*
 * The expiries a row sets, one entry per ttl column.
 *
 * Rendered in the same walk order the read path assigns slots in, so a table
 * with two ttl columns writes each field's own duration; and rendered from the
 * COLUMN's field name rather than from whatever fields[] ended up holding,
 * because a ttl column may name a field this statement did not otherwise
 * touch.
 */
void
vfdw_modify_render_ttls(VfdwModifyState *st, TupleTableSlot *slot,
						VfdwWriteOp *op)
{
	const VfdwTableMap *map = st->map;
	VfdwWriteArg *args;
	int			n = 0;
	int			i;

	if (map->nttl == 0)
		return;

	args = (VfdwWriteArg *) MemoryContextAllocZero(vfdw_wbuf_context(),
												   sizeof(VfdwWriteArg) *
												   map->nttl);

	for (i = 0; i < map->natts; i++)
	{
		const VfdwColumn *col = &map->cols[i];
		Datum		d;
		bool		isnull;
		VfdwValue	v;

		if (col->kind != VFDW_COL_TTL)
			continue;

		d = slot_getattr(slot, col->attnum, &isnull);
		vfdw_val_from_slot(&st->vc, map, col, d, isnull, &v);

		args[n].name = vfdw_modify_retain_name(col->field, &args[n].namelen);
		args[n].isnull = v.isnull;
		args[n].data = v.data;
		args[n].len = v.len;
		n++;
	}

	op->ttls = args;
	op->nttls = n;
}

/*
 * Refuse a row that names a member's position; see the position arm of
 * vfdw_val_from_slot for why one cannot be written.
 */
static void
vfdw_modify_check_position(VfdwModifyState *st, TupleTableSlot *slot)
{
	const VfdwTableMap *map = st->map;
	int			i;

	for (i = 0; i < map->natts; i++)
	{
		const VfdwColumn *col = &map->cols[i];
		Datum		d;
		bool		isnull;
		VfdwValue	v;

		if (col->kind != VFDW_COL_POSITION)
			continue;

		d = slot_getattr(slot, col->attnum, &isnull);
		vfdw_val_from_slot(&st->vc, map, col, d, isnull, &v);
	}
}

/*
 * Every element of a packed row's array, as the bytes the key should hold.
 *
 * ELEMENT BY ELEMENT THROUGH vfdw_val_render, which is the same route each
 * scalar column's value takes - so an element is checked against the server
 * encoding when its type is text and kept verbatim when it is bytea (I3),
 * exactly as the read direction does in reverse.
 *
 * A NULL ELEMENT IS REFUSED rather than skipped or written as empty. Skipping
 * it would silently shorten the collection, and there is no byte string that
 * means "absent" to Valkey - a member is present or it is not. The read
 * direction produces a NULL element only for a reply element that carried no
 * bytes, which a server does not send for a member.
 *
 * A NULL ARRAY is a different statement and is left to the fold: it means the
 * key should hold nothing, which is a deletion.
 */
/* The one packed column of a packed table, or NULL. */
static const VfdwColumn *
vfdw_modify_packed_column(const VfdwTableMap *map)
{
	int			i;

	for (i = 0; i < map->natts; i++)
	{
		if (map->cols[i].kind == VFDW_COL_LEGACY_VALUE)
			return &map->cols[i];
	}
	return NULL;
}

/*
 * One element of a packed array, as the bytes it should become.
 *
 * Through vfdw_val_render, the route every scalar column's value takes, so an
 * element of a text array is checked against the server encoding and one of a
 * bytea array is kept verbatim (I3) - the exact inverse of how the read
 * direction built it.
 *
 * A NULL ELEMENT IS REFUSED rather than skipped or written empty. Skipping
 * would silently shorten the collection, and no byte string means "absent" to
 * Valkey: a member is there or it is not.
 */
static void
vfdw_modify_pack_element(VfdwModifyState *st, const VfdwColumn *col,
						 Datum elem, bool isnull, VfdwWriteArg *out, int i)
{
	VfdwValue	v;

	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("a packed value may not contain NULL elements"),
				 errdetail("Element %d of the array is NULL.", i + 1),
				 errhint("Valkey has no absent member: remove the element "
						 "instead of nulling it.")));

	vfdw_val_render(&st->vc, &col->packed->col, elem, false, &v);
	out->data = v.data;
	out->len = v.len;
}

/*
 * A packed hash's array is field and value alternating - what the read
 * direction produces and what hstore(value) reconstructs - so an odd number of
 * elements names a field with no value.
 *
 * Refused at the STATEMENT rather than in the fold, which runs at pre-commit
 * where the message could no longer point at the row that caused it.
 */
static void
vfdw_modify_check_pairs(const VfdwTableMap *map, int n)
{
	if (map->tabletype != VFDW_TABLE_HASH || (n % 2) == 0)
		return;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("a packed hash needs an even number of elements"),
			 errdetail("The array has %d, so its last field has no value.", n),
			 errhint("The array alternates field and value, which is the shape "
					 "a read of this table returns.")));
}

void
vfdw_modify_render_packed(VfdwModifyState *st, TupleTableSlot *slot,
						  VfdwWriteOp *op)
{
	const VfdwTableMap *map = st->map;
	const VfdwColumn *col;
	VfdwWriteArg *args;
	Datum		d;
	bool		isnull;
	ArrayType  *arr;
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	int			i;

	col = vfdw_modify_packed_column(map);
	if (col == NULL)
		return;

	d = slot_getattr(slot, col->attnum, &isnull);
	if (isnull)
	{
		op->packed = NULL;
		op->npacked = 0;
		op->has_value = true;	/* the column was assigned; it said "nothing" */
		return;
	}

	arr = DatumGetArrayTypeP(d);
	if (ARR_NDIM(arr) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a packed value must be a one-dimensional array"),
				 errdetail("This one has %d dimensions.", ARR_NDIM(arr))));

	deconstruct_array(arr, col->packed->col.typid, col->packed->typlen,
					  col->packed->typbyval, col->packed->typalign,
					  &elems, &nulls, &n);

	args = (VfdwWriteArg *) MemoryContextAllocZero(vfdw_wbuf_context(),
												   sizeof(VfdwWriteArg) *
												   Max(n, 1));

	for (i = 0; i < n; i++)
		vfdw_modify_pack_element(st, col, elems[i], nulls[i], &args[i], i);

	vfdw_modify_check_pairs(map, n);

	op->packed = args;
	op->npacked = n;
	op->has_value = true;
}

void
vfdw_modify_render_payload(VfdwModifyState *st, TupleTableSlot *slot,
						   VfdwWriteOp *op)
{
	const VfdwTableMap *map = st->map;
	const VfdwColumn *col;
	Datum		d;
	bool		isnull;
	VfdwValue	v;

	/*
	 * A packed row is the key's whole contents, whatever collection type it
	 * is, so it is rendered before the per-type paths rather than inside one
	 * of them.
	 */
	if (map->legacy_value)
	{
		vfdw_modify_render_packed(st, slot, op);
		return;
	}

	if (map->tabletype == VFDW_TABLE_HASH)
	{
		vfdw_modify_render_fields(st, slot, op);
		vfdw_modify_render_ttls(st, slot, op);
		return;
	}

	/*
	 * A position column contributes nothing to the wire and so would never be
	 * looked at - which is exactly why it is looked at here.
	 *
	 * The member and the score are what a list or a zset write is made of, and
	 * a column that is neither is simply not read. For a position column that
	 * silence is wrong in the one direction that matters: a row naming a
	 * position would be accepted, the position discarded, and the member
	 * appended somewhere else entirely, with nothing said. Reading it now is
	 * what turns that into the refusal vfdw_val_from_slot already carries.
	 */
	vfdw_modify_check_position(st, slot);

	if (map->valueattno == InvalidAttrNumber)
		return;

	col = &map->cols[map->valueattno - 1];
	d = slot_getattr(slot, map->valueattno, &isnull);
	vfdw_val_from_slot(&st->vc, map, col, d, isnull, &v);

	op->value = v.data;
	op->valuelen = v.len;
	op->has_value = true;
}
