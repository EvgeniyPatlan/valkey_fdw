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

void
vfdw_modify_render_payload(VfdwModifyState *st, TupleTableSlot *slot,
						   VfdwWriteOp *op)
{
	const VfdwTableMap *map = st->map;
	const VfdwColumn *col;
	Datum		d;
	bool		isnull;
	VfdwValue	v;

	if (map->tabletype == VFDW_TABLE_HASH)
	{
		vfdw_modify_render_fields(st, slot, op);
		vfdw_modify_render_ttls(st, slot, op);
		return;
	}

	if (map->valueattno == InvalidAttrNumber)
		return;

	col = &map->cols[map->valueattno - 1];
	d = slot_getattr(slot, map->valueattno, &isnull);
	vfdw_val_from_slot(&st->vc, map, col, d, isnull, &v);

	op->value = v.data;
	op->valuelen = v.len;
	op->has_value = true;
}
