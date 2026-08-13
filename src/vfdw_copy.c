/*-------------------------------------------------------------------------
 *
 * vfdw_copy.c
 *		COPY FROM, tuple routing, and batch insert.
 *
 * These reach ExecForeignInsert through BeginForeignInsert rather than
 * BeginForeignModify, and they bring neither a plan nor fdw_private - so
 * everything the modify path reads from the plan has to be derived from the
 * relation instead. That is sound only because this path is INSERT: there are
 * no junk row-identity columns, and every mapped column is assigned by
 * definition. Both facts are restated where the derivation happens, because
 * neither holds for the general modify path.
 *
 * Split out of vfdw_modify.c when that file crossed the 800-line gate. The
 * seam is the entry point: everything here is reached without a plan.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_copy.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_modify.h"

#include "foreign/foreign.h"
#include "nodes/bitmapset.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "vfdw_refuse.h"
#include "vfdw_val.h"
#include "vfdw_wbuf.h"

/* ---------------------------------------------------------------------
 * COPY FROM, tuple routing, and batch insert (S8)
 * --------------------------------------------------------------------- */

/*
 * The insert-only entry point.
 *
 * COPY FROM and tuple routing into a partition both arrive here rather than
 * through BeginForeignModify, and they bring neither fdw_private nor a
 * subplan: there is no plan node behind them. So everything BeginForeignModify
 * reads from the plan has to be derived from the relation instead - which is
 * sound only because this path is INSERT, where there are no junk row-identity
 * columns and every mapped column is assigned by definition.
 *
 * Registering it is what makes COPY work at all. While it was unregistered,
 * CopyFrom reached ExecForeignInsert with ri_FdwState NULL and the wrapper had
 * to refuse rather than dereference it.
 */
void
vfdw_modify_begin_insert(ModifyTableState *mtstate, ResultRelInfo *rinfo)
{
	Relation	rel = rinfo->ri_RelationDesc;
	VfdwModifyState *st;
	MemoryContext stmt_cxt;
	MemoryContext old;
	int			i;

	/*
	 * mtstate is NULL for COPY FROM and non-NULL for tuple routing, so the
	 * parent context is taken from it only when it is there. Dereferencing it
	 * unconditionally crashes on the commoner of the two paths.
	 */
	stmt_cxt = AllocSetContextCreate(mtstate != NULL
									 ? mtstate->ps.state->es_query_cxt
									 : CurrentMemoryContext,
									 "valkey_fdw copy",
									 ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(stmt_cxt);

	st = (VfdwModifyState *) MemoryContextAllocZero(stmt_cxt,
													sizeof(VfdwModifyState));
	st->stmt_cxt = stmt_cxt;
	st->rel = rel;
	st->operation = CMD_INSERT;
	st->map = vfdw_map_build(rel, GetForeignTable(RelationGetRelid(rel)));

	vfdw_refuse_unwritable(RelationGetRelid(rel), st->map, CMD_INSERT);
	vfdw_val_ctx_init(&st->vc, rel, st->map, stmt_cxt, vfdw_wbuf_context());

	MemoryContextSwitchTo(old);

	/*
	 * No junk columns: an INSERT has no old row to identify. Left explicit
	 * rather than relying on the zeroed allocation, because InvalidAttrNumber
	 * happens to be 0 and a reader should not have to know that.
	 */
	st->junk_key = InvalidAttrNumber;
	st->junk_member = InvalidAttrNumber;
	st->junk_wholerow = InvalidAttrNumber;

	/*
	 * Every mapped attribute is assigned. BeginForeignModify takes this from
	 * the plan's target list; COPY has no plan, and its rule is simply that
	 * all columns are supplied.
	 */
	old = MemoryContextSwitchTo(vfdw_wbuf_context());
	for (i = 0; i < st->map->natts; i++)
	{
		if (st->map->cols[i].attnum != InvalidAttrNumber)
			st->target_attrs = bms_add_member((Bitmapset *) st->target_attrs,
											  st->map->cols[i].attnum);
	}
	MemoryContextSwitchTo(old);

	vfdw_modify_connect(st, rel);

	rinfo->ri_FdwState = st;
}

void
vfdw_modify_end_insert(EState *estate, ResultRelInfo *rinfo)
{
	VfdwModifyState *st = (VfdwModifyState *) rinfo->ri_FdwState;

	if (st == NULL)
		return;

	MemoryContextDelete(st->stmt_cxt);
	rinfo->ri_FdwState = NULL;
}

/*
 * How many rows core may hand over at once.
 *
 * Every row is buffered in memory and nothing reaches the wire until
 * pre-commit, so a batch saves callback overhead and no round trips - which
 * is why this is a modest constant rather than something derived from
 * pipeline_batch, whose whole purpose is bounding commands in flight.
 *
 * 1 when the statement needs per-row work core cannot batch: RETURNING has to
 * project each row, and an AFTER ROW trigger has to see each one. Reporting
 * more in either case makes core skip rows silently.
 */
int
vfdw_modify_batch_size(ResultRelInfo *rinfo)
{
	if (rinfo->ri_projectReturning != NULL ||
		(rinfo->ri_TrigDesc != NULL &&
		 (rinfo->ri_TrigDesc->trig_insert_after_row ||
		  rinfo->ri_TrigDesc->trig_insert_before_row)))
		return 1;

	return 100;
}

/*
 * Buffer a batch.
 *
 * Each row goes through the same single-row path, so there is exactly one
 * place that renders a row and one place that appends it - a batch loop with
 * its own copy of that logic is how the two drift and a COPY starts writing
 * something an INSERT would not.
 *
 * *numSlots is not reduced: every row is accepted or the statement raises, so
 * a short return would mean silently dropping rows.
 */
TupleTableSlot **
vfdw_modify_batch_insert(EState *estate, ResultRelInfo *rinfo,
						 TupleTableSlot **slots, TupleTableSlot **planSlots,
						 int *numSlots)
{
	int			i;

	for (i = 0; i < *numSlots; i++)
		(void) vfdw_modify_insert(estate, rinfo, slots[i],
								  planSlots != NULL ? planSlots[i] : NULL);

	return slots;
}
