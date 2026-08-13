/*-------------------------------------------------------------------------
 *
 * vfdw_modify.c
 *		The FdwRoutine modify callbacks: validate, resolve and buffer.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_modify.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_modify.h"

#include "utils/typcache.h"

#include "vfdw_ledger.h"
#include "vfdw_render.h"

#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_foreign_table.h"
#include "executor/executor.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "vfdw_conn.h"
#include "vfdw_refuse.h"
#include "vfdw_rowid.h"
#include "vfdw_wbuf.h"

StaticAssertDecl(VFDW_MOD_PRIV_NSLOTS == 4,
				 "fdw_private slot count and list_make4 disagree");

/* ---------------------------------------------------------------------
 * PlanForeignModify
 * --------------------------------------------------------------------- */

/*
 * Which attributes the statement assigned.
 *
 * Only the planner can know it - updatedCols is gone by the time the buffer is
 * flushed - so it is carried on every operation the statement buffers. Nothing
 * on the flush path reads it yet; valkey_fdw_test_wbuf_targets asserts what it
 * holds, so an INSERT targeting every writable column and an UPDATE targeting
 * only what it assigned cannot quietly collapse into each other.
 *
 * A partition's child RTE carries perminfoindex 0 - permissions are checked
 * on the parent - so there is no per-child updatedCols to read. The fallback
 * is every assignable attribute, a superset, which is the safe direction for
 * a consumer that asks "what did this statement NOT assign".
 */
static List *
vfdw_modify_target_attrs(PlannerInfo *root, RangeTblEntry *rte, Relation rel,
						 CmdType operation)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	List	   *result = NIL;
	int			i;

	if (operation == CMD_UPDATE && rte->perminfoindex != 0)
	{
		RTEPermissionInfo *perminfo;
		int			col = -1;

		perminfo = getRTEPermissionInfo(root->parse->rteperminfos, rte);
		while ((col = bms_next_member(perminfo->updatedCols, col)) >= 0)
		{
			AttrNumber	attno = col + FirstLowInvalidHeapAttributeNumber;

			if (attno <= 0)		/* a whole-row reference names no column */
				continue;
			result = lappend_int(result, (int) attno);
		}
		return result;
	}

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

		if (attr->attisdropped || attr->attgenerated)
			continue;
		result = lappend_int(result, (int) attr->attnum);
	}

	return result;
}

/*
 * Junk names travel in fdw_private rather than being rebuilt at Begin: it
 * removes the second derivation entirely, and it puts them where
 * ExplainForeignModify can print them, which is the only place a recorded
 * expected file can see them.
 */
static List *
vfdw_modify_junk_names(const VfdwTableMap *map, CmdType operation)
{
	VfdwRowId	rowid;

	if (operation == CMD_INSERT)
		return list_make2(makeString(pstrdup("")), makeString(pstrdup("")));

	vfdw_rowid_shape(map, operation, &rowid);

	return list_make2(makeString(rowid.want_key
								 ? vfdw_rowid_junk_name("key", rowid.keyattno)
								 : pstrdup("")),
					  makeString(rowid.want_member
								 ? vfdw_rowid_junk_name("member",
														rowid.memberattno)
								 : pstrdup("")));
}

/*
 * Building the map here is deliberate and stays: this is a plan-time path, so
 * a shape whose reader is not implemented is refused for its own reason
 * rather than with a message about writes.
 */
List *
vfdw_modify_plan(PlannerInfo *root, ModifyTable *plan, Index resultRelation,
				 int subplan_index)
{
	RangeTblEntry *rte = planner_rt_fetch(resultRelation, root);
	VfdwTableMap *map = vfdw_map_build_for_relid(rte->relid);
	Relation	rel;
	List	   *priv;

	vfdw_refuse_unwritable(rte->relid, map, plan->operation);
	vfdw_refuse_on_conflict(plan);

	/* Core already holds a lock on every relation it is planning. */
	rel = table_open(rte->relid, NoLock);

	priv = list_make4(vfdw_modify_junk_names(map, plan->operation),
					  vfdw_modify_target_attrs(root, rte, rel, plan->operation),
					  makeInteger((int) map->tabletype),
					  makeString(pstrdup(map->singleton_key != NULL
										 ? "singleton_key" : "key column")));

	table_close(rel, NoLock);

	return priv;
}

/* ---------------------------------------------------------------------
 * BeginForeignModify
 * --------------------------------------------------------------------- */

static const Bitmapset *
vfdw_modify_attr_bitmap(List *attnos)
{
	MemoryContext old = MemoryContextSwitchTo(vfdw_wbuf_context());
	Bitmapset  *bms = NULL;
	ListCell   *lc;

	foreach(lc, attnos)
		bms = bms_add_member(bms, lfirst_int(lc));

	MemoryContextSwitchTo(old);
	return bms;
}

/*
 * Prove the server is reachable and usable now, then give the lease straight
 * back.
 *
 * The scan holds its lease because it owns a reply stream. This callback owns
 * none: the write path issues no Valkey command outside vfdw_flush_pre_commit,
 * so there is no reply stream here and nothing a second reader could
 * interleave with. Holding it would instead cost one of VFDW_MAX_CONN_SLOTS
 * per DML statement in a transaction, so an INSERT routed into 33 foreign
 * partitions would fail with "too many concurrent Valkey scans on one server"
 * - a message wrong in both nouns. The flush re-acquires from the pool after
 * proving quiescence and does not care which slot this touched.
 *
 * vfdw_conn_select_db is deliberately NOT called. The rule is that no command
 * this module issues reaches the wire before the flush, and acquiring is not
 * issuing: AUTH and HELLO happen inside vfdw_get_connection on a cold
 * connection and are part of taking one out of the pool. A SELECT would be
 * this callback's own command, and the flush re-issues it anyway on whichever
 * slot it takes. Do not "fix" the asymmetry with the scan.
 *
 * prefer_replica and the two caps are read from the options this acquisition
 * actually used, so a plan cached before an ALTER SERVER cannot disagree with
 * them. Binding after the refusal, rather than before, keeps a refused
 * statement from leaving a unit bound behind it.
 *
 * The same is true of the verdict this acquisition carries: whether the server
 * will run the write program at all was settled when the connection was
 * opened, and this is the first moment of the write at which it can be
 * reported. Reported here rather than at the flush, because by the flush the
 * user has been told their transaction is being applied.
 */
void
vfdw_modify_connect(VfdwModifyState *st, Relation rel)
{
	ForeignTable *table = GetForeignTable(RelationGetRelid(rel));
	ForeignServer *server = GetForeignServer(table->serverid);
	UserMapping *user = GetUserMapping(GetUserId(), table->serverid);
	/*
	 * Cluster-aware: this only proves the server is reachable and gives the
	 * lease straight back. The write itself goes through the flush, which
	 * routes to the primary owning the unit's slot and refuses a unit that
	 * spans slots - so admitting a cluster here admits nothing unrouted.
	 */
	VfdwConn   *vconn = vfdw_get_connection_cluster(server, user);
	const VfdwServerOptions *opts = vfdw_conn_options(vconn);
	const char *refusal;
	size_t		refusallen = 0;

	if (opts->prefer_replica)
		vfdw_refuse_prefer_replica(server);

	refusal = vfdw_conn_write_refusal(vconn, &refusallen);
	if (refusal != NULL)
		vfdw_refuse_no_write_program(server, refusal, refusallen);

	vfdw_wbuf_bind(rel, table->serverid, user->umid, GetUserId(),
				   st->map->database, opts->write_max_ops,
				   (int64) opts->write_max_bytes);

	vfdw_release_connection(vconn);
}

/*
 * Core calls this unconditionally and passes eflags down. Without the early
 * return a plain EXPLAIN of an INSERT would open a connection and bind the
 * transaction's unit, so EXPLAIN of an insert into a second database would
 * raise 0A000.
 *
 * There is no VfdwConn * in VfdwModifyState, and that absence is the visible
 * consequence of releasing the lease in vfdw_modify_connect.
 */
void
vfdw_modify_begin(ModifyTableState *mtstate, ResultRelInfo *rinfo,
				  List *fdw_private, int subplan_index, int eflags)
{
	EState	   *estate = mtstate->ps.state;
	Relation	rel = rinfo->ri_RelationDesc;
	VfdwModifyState *st;
	MemoryContext stmt_cxt;
	MemoryContext old;

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	stmt_cxt = AllocSetContextCreate(estate->es_query_cxt,
									 "valkey_fdw modify",
									 ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(stmt_cxt);

	st = (VfdwModifyState *) MemoryContextAllocZero(stmt_cxt,
													sizeof(VfdwModifyState));
	st->stmt_cxt = stmt_cxt;
	st->rel = rel;
	st->operation = mtstate->operation;

	/*
	 * ri_RelationDesc, never ri_RangeTableIndex: the latter is 0 for a routed
	 * partition.
	 */
	st->map = vfdw_map_build(rel, GetForeignTable(RelationGetRelid(rel)));
	vfdw_refuse_unwritable(RelationGetRelid(rel), st->map, st->operation);

	/*
	 * Output functions are resolved once per statement inside here; there is
	 * no getTypeOutputInfo call anywhere on this path, because typoutput and
	 * typisvarlena live on VfdwColumn beside the inbound pair.
	 */
	vfdw_val_ctx_init(&st->vc, rel, st->map, stmt_cxt, vfdw_wbuf_context());

	MemoryContextSwitchTo(old);

	vfdw_rowid_resolve(mtstate,
					   (List *) list_nth(fdw_private, VFDW_MOD_PRIV_JUNK_NAMES),
					   &st->junk_key, &st->junk_member);
	st->junk_wholerow = vfdw_rowid_find_wholerow(mtstate);
	st->target_attrs = NULL;
	st->target_attnos = (List *) list_nth(fdw_private,
										  VFDW_MOD_PRIV_TARGET_ATTRS);

	vfdw_modify_connect(st, rel);

	rinfo->ri_FdwState = st;
}

/* ---------------------------------------------------------------------
 * Rendering one row
 * --------------------------------------------------------------------- */

/*
 * The key, from the row.
 *
 * col is resolved only after keyattno has been tested, because on a singleton
 * table keyattno may legitimately be InvalidAttrNumber and unconditional
 * indexing here is a read of cols[-1]. vfdw_val_build_key accepts col == NULL
 * on such a table and answers with the option's value.
 */
static void
vfdw_modify_render_key(VfdwModifyState *st, TupleTableSlot *slot,
					   VfdwWriteOp *op)
{
	const VfdwTableMap *map = st->map;
	const VfdwColumn *col = NULL;
	Datum		d = (Datum) 0;
	bool		isnull = true;
	VfdwValue	v;

	if (map->keyattno != InvalidAttrNumber)
	{
		col = &map->cols[map->keyattno - 1];
		d = slot_getattr(slot, map->keyattno, &isnull);
	}

	vfdw_val_build_key(&st->vc, map, col, d, isnull, &v);
	op->key = v.data;
	op->keylen = v.len;
}

static void
vfdw_modify_render_score(VfdwModifyState *st, TupleTableSlot *slot,
						 VfdwWriteOp *op)
{
	const VfdwTableMap *map = st->map;
	const VfdwColumn *col;
	Datum		d;
	bool		isnull;
	VfdwValue	v;

	if (map->scoreattno == InvalidAttrNumber)
		return;

	col = &map->cols[map->scoreattno - 1];
	d = slot_getattr(slot, map->scoreattno, &isnull);
	vfdw_val_from_slot(&st->vc, map, col, d, isnull, &v);

	op->score = v.data;
	op->scorelen = v.len;
	op->has_score = true;
}

static void
vfdw_modify_render_member(VfdwModifyState *st, TupleTableSlot *slot,
						  VfdwWriteOp *op)
{
	const VfdwTableMap *map = st->map;
	const VfdwColumn *col;
	Datum		d;
	bool		isnull;
	VfdwValue	v;

	if (map->memberattno == InvalidAttrNumber)
		return;

	col = &map->cols[map->memberattno - 1];
	d = slot_getattr(slot, map->memberattno, &isnull);
	vfdw_val_from_slot(&st->vc, map, col, d, isnull, &v);

	op->member = v.data;
	op->memberlen = v.len;
	op->has_member = true;
}

/*
 * The whole new row, copied so it outlives the per-tuple context.
 *
 * The slot's own storage dies at the end of the callback, long before the
 * pre-commit flush; src/vfdw_overlay.c is the consumer, and reads this tuple
 * to show a scan the writes its own transaction has not sent yet.
 */
static HeapTuple
vfdw_modify_copy_tuple(TupleTableSlot *slot)
{
	MemoryContext old = MemoryContextSwitchTo(vfdw_wbuf_context());
	HeapTuple	tup = ExecCopySlotHeapTuple(slot);

	MemoryContextSwitchTo(old);
	return tup;
}

static VfdwWriteOp *
vfdw_modify_op_begin(VfdwModifyState *st, VfdwOpKind kind)
{
	VfdwWriteOp *op;

	/* At the top of the callback, not the bottom: see vfdw_val_ctx_reset. */
	vfdw_val_ctx_reset(&st->vc);

	op = vfdw_wbuf_op_new(st->rel);
	op->kind = kind;
	op->relid = RelationGetRelid(st->rel);
	op->tabletype = st->map->tabletype;
	if (st->target_attrs == NULL)
		st->target_attrs = vfdw_modify_attr_bitmap(st->target_attnos);
	op->target_attrs = st->target_attrs;

	/*
	 * Retained into the buffer, not pointed at: st->map dies with the
	 * statement, and the ledger reads this from an abort callback long after.
	 */
	if (st->map->keyset != NULL)
		op->keyset = vfdw_modify_retain_name(st->map->keyset, &op->keysetlen);

	return op;
}

/*
 * Put the deleted row into the slot RETURNING will project.
 *
 * The wholerow junk column is requested by vfdw_rowid_add_update_targets for
 * every DELETE. If it is absent - a plan shape that never asked for it - the
 * slot is left as core gave it, which is the old all-null behaviour rather
 * than a crash.
 */
static void
vfdw_modify_fill_deleted(VfdwModifyState *st, TupleTableSlot *slot,
						 TupleTableSlot *planSlot)
{
	Datum		d;
	bool		isnull;
	HeapTupleHeader tuphdr;
	HeapTupleData tup;

	if (planSlot == NULL || slot == NULL ||
		st->junk_wholerow == InvalidAttrNumber)
		return;

	d = ExecGetJunkAttribute(planSlot, st->junk_wholerow, &isnull);
	if (isnull)
		return;

	tuphdr = DatumGetHeapTupleHeader(d);

	/*
	 * Deformed into the slot's own arrays rather than stored as a tuple.
	 * ExecForceStoreHeapTuple was the first attempt and it crashed the
	 * backend: the returning slot's descriptor is not necessarily the one the
	 * wholerow datum was built against, and a tuple stored under a mismatched
	 * descriptor is read at the wrong offsets. Deforming asks the datum for
	 * its own descriptor and copies value by value, so a mismatch is a
	 * missing column rather than a wild pointer.
	 */
	{
		Oid			typ = HeapTupleHeaderGetTypeId(tuphdr);
		int32		typmod = HeapTupleHeaderGetTypMod(tuphdr);
		TupleDesc	srcdesc = lookup_rowtype_tupdesc(typ, typmod);
		int			natts = Min(srcdesc->natts, slot->tts_tupleDescriptor->natts);
		int			i;

		tup.t_len = HeapTupleHeaderGetDatumLength(tuphdr);
		ItemPointerSetInvalid(&(tup.t_self));
		tup.t_tableOid = InvalidOid;
		tup.t_data = tuphdr;

		ExecClearTuple(slot);
		heap_deform_tuple(&tup, srcdesc, slot->tts_values, slot->tts_isnull);

		/* Anything the returning slot has beyond the old row reads as NULL. */
		for (i = natts; i < slot->tts_tupleDescriptor->natts; i++)
			slot->tts_isnull[i] = true;

		ExecStoreVirtualTuple(slot);
		ReleaseTupleDesc(srcdesc);
	}
}

/* ---------------------------------------------------------------------
 * ExecForeignInsert / Update / Delete
 * --------------------------------------------------------------------- */

/*
 * planSlot is named and never dereferenced, structurally rather than by a
 * null check: COPY FROM and tuple routing reach this function through
 * BeginForeignInsert with no plan slot at all, and ExecForeignBatchInsert
 * passes a NULL planSlots array outright. Not touching it is what keeps those
 * entry points from needing a null check on every line that follows.
 *
 * The return is always non-NULL. Returning NULL means "no row" to core:
 * es_processed is not incremented, AFTER triggers do not fire and RETURNING
 * produces nothing.
 */
TupleTableSlot *
vfdw_modify_insert(EState *estate, ResultRelInfo *rinfo, TupleTableSlot *slot,
				   TupleTableSlot *planSlot)
{
	VfdwModifyState *st = (VfdwModifyState *) rinfo->ri_FdwState;
	VfdwWriteOp *op;

	if (st == NULL)
		vfdw_refuse_no_modify_state();

	op = vfdw_modify_op_begin(st, VFDW_OP_INSERT);

	/* Cheapest refusals first, so the common ones leave nothing behind. */
	vfdw_modify_render_key(st, slot, op);
	vfdw_modify_render_score(st, slot, op);
	vfdw_modify_render_member(st, slot, op);
	vfdw_modify_render_payload(st, slot, op);

	op->hashslot = vfdw_val_slot(op->key, op->keylen);
	op->newtup = vfdw_modify_copy_tuple(slot);

	vfdw_wbuf_append(op, st->rel);

	/*
	 * Folded after the append, so that this incremental path and
	 * vfdw_ledger_rebuild_from_buffer walk the same sequence and must produce
	 * the same ledger - which wbuf.sql asserts rather than assumes. It may
	 * raise 23505; the operation is already linked, and the abort that
	 * follows either resets the buffer or truncates and re-folds it.
	 */
	vfdw_ledger_note(op, st->rel);
	return slot;
}

/*
 * The old identity, from the junk columns of planSlot.
 *
 * Rendered with vfdw_val_render rather than vfdw_val_build_key: these bytes
 * came out of the table's own scan, so re-validating them against the table's
 * key rules could only ever fire on a scan that was already wrong, and would
 * report it as a user error.
 */
static void
vfdw_modify_read_old(VfdwModifyState *st, TupleTableSlot *planSlot,
					 VfdwWriteOp *op)
{
	const VfdwTableMap *map = st->map;
	Datum		d;
	bool		isnull;
	VfdwValue	v;

	if (st->junk_key != InvalidAttrNumber)
	{
		vfdw_rowid_read(planSlot, st->junk_key, &d, &isnull);
		vfdw_val_render(&st->vc, &map->cols[map->keyattno - 1], d, isnull, &v);
		op->oldkey = v.data;
		op->oldkeylen = v.len;
	}
	else
	{
		/* Singleton: identity is the option, so the key cannot be renamed. */
		op->oldkey = op->key;
		op->oldkeylen = op->keylen;
	}

	if (st->junk_member == InvalidAttrNumber)
		return;

	vfdw_rowid_read(planSlot, st->junk_member, &d, &isnull);
	vfdw_val_render(&st->vc, &map->cols[map->memberattno - 1], d, isnull, &v);
	op->oldmember = v.data;
	op->oldmemberlen = v.len;
	op->has_oldmember = true;
}

TupleTableSlot *
vfdw_modify_update(EState *estate, ResultRelInfo *rinfo, TupleTableSlot *slot,
				   TupleTableSlot *planSlot)
{
	VfdwModifyState *st = (VfdwModifyState *) rinfo->ri_FdwState;
	VfdwWriteOp *op;

	if (st == NULL)
		vfdw_refuse_no_modify_state();

	/*
	 * Unreachable: vfdw_map_writability gives a list table an INSERT|DELETE
	 * mask, so an UPDATE is refused at plan time and again at Begin. Loud
	 * rather than a silent wrong write if that ever stops being true.
	 */
	if (st->map->tabletype == VFDW_TABLE_LIST)
		elog(ERROR, "valkey_fdw: UPDATE reached the list write path");

	op = vfdw_modify_op_begin(st, VFDW_OP_UPDATE);

	vfdw_modify_render_key(st, slot, op);
	vfdw_modify_render_score(st, slot, op);
	vfdw_modify_render_member(st, slot, op);
	vfdw_modify_render_payload(st, slot, op);

	/*
	 * The complete new row comes from slot, which core has already rebuilt;
	 * only the identity comes from planSlot. Both key and oldkey are stored,
	 * because when they differ the ledger folds them into a RENAME rather than
	 * a delete and recreate, which would destroy whatever the table does not
	 * map.
	 */
	vfdw_modify_read_old(st, planSlot, op);

	op->hashslot = vfdw_val_slot(op->key, op->keylen);
	op->newtup = vfdw_modify_copy_tuple(slot);

	vfdw_wbuf_append(op, st->rel);

	/*
	 * Folded after the append, so that this incremental path and
	 * vfdw_ledger_rebuild_from_buffer walk the same sequence and must produce
	 * the same ledger - which wbuf.sql asserts rather than assumes. It may
	 * raise 23505; the operation is already linked, and the abort that
	 * follows either resets the buffer or truncates and re-folds it.
	 */
	vfdw_ledger_note(op, st->rel);
	return slot;
}

/*
 * Identity only: no payload, no score, no HeapTuple. What is deleted is named
 * by the junk columns of planSlot, and nothing is sent until the flush.
 *
 * slot is the returning slot, and it is filled here rather than left for core
 * to store an all-null tuple into. A deferred write has no remote RETURNING
 * to fetch the old row from, so the row comes from the wholerow junk column
 * vfdw_rowid_add_update_targets asks for on every DELETE. Left to core, a
 * DELETE ... RETURNING would report the right row count with every column
 * NULL - a plausible empty result rather than an error.
 */
TupleTableSlot *
vfdw_modify_delete(EState *estate, ResultRelInfo *rinfo, TupleTableSlot *slot,
				   TupleTableSlot *planSlot)
{
	VfdwModifyState *st = (VfdwModifyState *) rinfo->ri_FdwState;
	const VfdwTableMap *map;
	VfdwWriteOp *op;
	Datum		d;
	bool		isnull;
	VfdwValue	v;

	if (st == NULL)
		vfdw_refuse_no_modify_state();

	map = st->map;
	op = vfdw_modify_op_begin(st, VFDW_OP_DELETE);

	if (st->junk_key != InvalidAttrNumber)
	{
		vfdw_rowid_read(planSlot, st->junk_key, &d, &isnull);
		vfdw_val_render(&st->vc, &map->cols[map->keyattno - 1], d, isnull, &v);
	}
	else
		vfdw_val_build_key(&st->vc, map, NULL, (Datum) 0, true, &v);

	op->key = v.data;
	op->keylen = v.len;

	if (st->junk_member != InvalidAttrNumber)
	{
		vfdw_rowid_read(planSlot, st->junk_member, &d, &isnull);
		vfdw_val_render(&st->vc, &map->cols[map->memberattno - 1], d, isnull,
						&v);
		op->member = v.data;
		op->memberlen = v.len;
		op->has_member = true;
	}

	op->hashslot = vfdw_val_slot(op->key, op->keylen);

	vfdw_wbuf_append(op, st->rel);

	/*
	 * Folded after the append, so that this incremental path and
	 * vfdw_ledger_rebuild_from_buffer walk the same sequence and must produce
	 * the same ledger - which wbuf.sql asserts rather than assumes. It may
	 * raise 23505; the operation is already linked, and the abort that
	 * follows either resets the buffer or truncates and re-folds it.
	 */
	vfdw_ledger_note(op, st->rel);

	/*
	 * Fill the returning slot from the wholerow junk column, so
	 * DELETE ... RETURNING returns the row that was deleted rather than a
	 * tuple of NULLs. Core hands this callback an empty slot and stores
	 * whatever is left in it.
	 */
	vfdw_modify_fill_deleted(st, slot, planSlot);
	return slot;
}

/* ---------------------------------------------------------------------
 * EndForeignModify and ExplainForeignModify
 * --------------------------------------------------------------------- */

void
vfdw_modify_end(EState *estate, ResultRelInfo *rinfo)
{
	VfdwModifyState *st = (VfdwModifyState *) rinfo->ri_FdwState;

	if (st == NULL)
		return;					/* EXPLAIN-only: Begin returned early */

	/*
	 * Per-statement state only. The buffer outlives it, and every byte an
	 * operation points at is in the buffer context rather than in here.
	 * Deleting rather than leaving it to es_query_cxt is what makes a
	 * borrowed pointer into this context detectable instead of merely
	 * incorrect.
	 *
	 * No connection release: the lease was returned in vfdw_modify_connect.
	 */
	MemoryContextDelete(st->stmt_cxt);
	rinfo->ri_FdwState = NULL;
}

/*
 * Prints from fdw_private only, because there is no fdw_state under an
 * EXPLAIN without ANALYZE.
 *
 * It deliberately does not print the command shape, and its absence is a
 * decision rather than an omission: there is no per-statement shape to print.
 * The ledger folds the whole transaction into one plan per key and the script
 * encoder renders that at pre-commit, so what a single statement contributes
 * is not separable, and an EXPLAIN without ANALYZE has buffered no rows at
 * all. Printing a guess would describe a command the flush never sends.
 */
void
vfdw_modify_explain(ModifyTableState *mtstate, ResultRelInfo *rinfo,
					List *fdw_private, int subplan_index, ExplainState *es)
{
	List	   *junk;
	const char *keyname;
	const char *membername;

	if (fdw_private == NIL)
		return;

	ExplainPropertyText("Valkey Write",
						"deferred, one atomic unit, flushed at commit", es);
	ExplainPropertyText("Valkey Table Type",
						vfdw_tabletype_name((VfdwTableType)
											intVal(list_nth(fdw_private,
															VFDW_MOD_PRIV_TABLETYPE))),
						es);
	ExplainPropertyText("Valkey Key Source",
						strVal(list_nth(fdw_private,
										VFDW_MOD_PRIV_KEYSOURCE)), es);

	if (!es->verbose)
		return;

	junk = (List *) list_nth(fdw_private, VFDW_MOD_PRIV_JUNK_NAMES);
	keyname = strVal(list_nth(junk, 0));
	membername = strVal(list_nth(junk, 1));

	if (keyname[0] != '\0')
		ExplainPropertyText("Valkey Row Identity", keyname, es);
	if (membername[0] != '\0')
		ExplainPropertyText("Valkey Row Identity Member", membername, es);
}
