/*-------------------------------------------------------------------------
 *
 * vfdw_modify.h
 *		The FdwRoutine modify callbacks: validate, resolve and buffer.
 *
 * This module performs NO I/O beyond acquiring the pooled connection at
 * BeginForeignModify, which exists so that an unreachable host, a bad
 * password, a rejected certificate or an ACL denial is reported at the DML
 * statement rather than at COMMIT. Every byte a statement produces goes into
 * the write buffer; nothing reaches Valkey until the pre-commit flush.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_modify.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_MODIFY_H
#define VFDW_MODIFY_H

#include "vfdw.h"

#include "commands/explain.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#endif
#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"

#include "vfdw_map.h"
#include "vfdw_val.h"

/*
 * This module's own fdw_private slots. Deliberately its own numbering: the
 * scan's VFDW_PRIV_* live in a different list on a different plan node, and
 * sharing the constants would make a mistake in one silently readable as the
 * other.
 */
typedef enum VfdwModPriv
{
	/*
	 * Exactly two Strings, in {key, member} order, either of which may be the
	 * empty string when the shape wants no junk column for that role. Always
	 * two, so that position carries the role and no reader has to guess which
	 * of the two a one-element list held.
	 */
	VFDW_MOD_PRIV_JUNK_NAMES = 0,

	/*
	 * An IntList of assigned attnums. Only the planner can derive it:
	 * updatedCols is long gone by the time the buffer is flushed.
	 */
	VFDW_MOD_PRIV_TARGET_ATTRS,

	VFDW_MOD_PRIV_TABLETYPE,	/* Integer: VfdwTableType, for EXPLAIN */
	VFDW_MOD_PRIV_KEYSOURCE,	/* String: "key column" | "singleton_key" */
	VFDW_MOD_PRIV_NSLOTS
} VfdwModPriv;

typedef struct VfdwModifyState
{
	Relation	rel;			/* borrowed from rinfo */
	VfdwTableMap *map;			/* in stmt_cxt */
	CmdType		operation;
	MemoryContext stmt_cxt;
	VfdwValCtx	vc;

	/* In the subplan targetlist; InvalidAttrNumber when the shape wants none. */
	AttrNumber	junk_key;
	AttrNumber	junk_member;

	/*
	 * The whole old row, for DELETE ... RETURNING: a deferred write has no
	 * remote RETURNING to read the deleted row back from, so without this the
	 * clause returns a tuple of NULLs with the row count right.
	 */
	AttrNumber	junk_wholerow;

	/*
	 * In the WRITE BUFFER context, shared by every op this statement makes,
	 * and built on FIRST USE rather than at Begin.
	 *
	 * The lifetime has to be the buffer's: the operations carrying it are read
	 * after EndForeignModify, when the statement's own context is gone.
	 * Nothing on the flush path consumes it yet - valkey_fdw_test_wbuf_targets
	 * is what holds it to its meaning. But paying for it at Begin charged
	 * every DML statement against write_max_bytes whether or not it buffered
	 * anything - an UPDATE matching no rows, or a statement refused before its
	 * first row, left a bitmap behind for the rest of the transaction. That is
	 * a bitmap charged against write_max_bytes by statements that
	 * buffered no rows at all. The planned list is kept instead and converted when the
	 * first operation actually needs it.
	 */
	const Bitmapset *target_attrs;
	List	   *target_attnos;
} VfdwModifyState;

extern List *vfdw_modify_plan(PlannerInfo *root, ModifyTable *plan,
							  Index resultRelation, int subplan_index);
extern void vfdw_modify_begin(ModifyTableState *mtstate, ResultRelInfo *rinfo,
							  List *fdw_private, int subplan_index, int eflags);
extern TupleTableSlot *vfdw_modify_insert(EState *estate, ResultRelInfo *rinfo,
										  TupleTableSlot *slot,
										  TupleTableSlot *planSlot);
extern TupleTableSlot *vfdw_modify_update(EState *estate, ResultRelInfo *rinfo,
										  TupleTableSlot *slot,
										  TupleTableSlot *planSlot);
extern TupleTableSlot *vfdw_modify_delete(EState *estate, ResultRelInfo *rinfo,
										  TupleTableSlot *slot,
										  TupleTableSlot *planSlot);
extern void vfdw_modify_end(EState *estate, ResultRelInfo *rinfo);
extern void vfdw_modify_explain(ModifyTableState *mtstate, ResultRelInfo *rinfo,
								List *fdw_private, int subplan_index,
								ExplainState *es);

/*
 * Acquire the pooled connection, prove the server usable, bind the unit, and
 * give the lease straight back. Shared with src/vfdw_copy.c, which has to do
 * the same thing without a plan to read.
 */
extern void vfdw_modify_connect(VfdwModifyState *st, Relation rel);

/*
 * COPY FROM, tuple routing and batch insert: the entry points that reach
 * ExecForeignInsert with no plan and no fdw_private to read, so they resolve
 * the relation from rinfo and enforce the write gates for themselves.
 */
extern void vfdw_modify_begin_insert(ModifyTableState *mtstate,
									 ResultRelInfo *rinfo);
extern void vfdw_modify_end_insert(EState *estate, ResultRelInfo *rinfo);
extern int	vfdw_modify_batch_size(ResultRelInfo *rinfo);
extern TupleTableSlot **vfdw_modify_batch_insert(EState *estate,
												 ResultRelInfo *rinfo,
												 TupleTableSlot **slots,
												 TupleTableSlot **planSlots,
												 int *numSlots);

#endif							/* VFDW_MODIFY_H */
