/*-------------------------------------------------------------------------
 *
 * vfdw_rowid.h
 *		Row identity per table shape: junk column names, injection, resolution.
 *
 * AddForeignUpdateTargets injects resjunk Vars over REAL table columns, named
 * with the attnum embedded - vfdw_key_a3, vfdw_member_a2 - mirroring core's
 * ctidN convention. The attnum is not decoration: two valkey tables attached
 * as partitions of one parent may carry their key column at different
 * attnums, and add_row_identity_var raises "conflicting uses of row-identity
 * name" when one name maps to two different Vars. That collision is the only
 * behavioural, mutation-testable property of the naming scheme, because
 * AddForeignUpdateTargets and BeginForeignModify necessarily use the same
 * builder and a test comparing one against the other would be a tautology.
 *
 * The reserved names tableoid, ctid, ctidN, wholerow and wholerowN are never
 * used. Core additionally adds its OWN wholerow junk column for foreign-table
 * UPDATE, and for DELETE when row triggers exist; we never read
 * ri_RowIdAttNo, which on a foreign table points at that wholerow and is
 * InvalidAttrNumber for a trigger-free DELETE.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_rowid.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_ROWID_H
#define VFDW_ROWID_H

#include "vfdw.h"

#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"
#include "utils/relcache.h"

#include "vfdw_map.h"

/*
 * Which junk columns a shape needs, and the real columns they are Vars over.
 *
 * want_key is false whenever the table has a singleton_key, even if it also
 * declares a key 'true' column: the singleton's identity is the option, and a
 * junk Var over a column filled with a constant would let a row name a key
 * the table does not own. keyattno may then legitimately be
 * InvalidAttrNumber.
 */
typedef struct VfdwRowId
{
	bool		want_key;
	AttrNumber	keyattno;
	bool		want_member;
	AttrNumber	memberattno;
} VfdwRowId;

extern void vfdw_rowid_shape(const VfdwTableMap *map, CmdType operation,
							 VfdwRowId *out);

/* psprintf("vfdw_%s_a%d", role, attno): "vfdw_key_a3", "vfdw_member_a2". */
extern char *vfdw_rowid_junk_name(const char *role, AttrNumber attno);

extern void vfdw_rowid_add_update_targets(PlannerInfo *root, Index rtindex,
										  RangeTblEntry *target_rte,
										  Relation target_relation);

/*
 * Resolve the junk names carried in fdw_private against the subplan's
 * targetlist. The names come from the plan so that they are derived once, not
 * twice; a second derivation is what a tautological test compares against.
 *
 * An empty name means the shape wants no junk column for that role, and
 * leaves the corresponding attno InvalidAttrNumber.
 */
extern void vfdw_rowid_resolve(ModifyTableState *mtstate, const List *junk_names,
							   AttrNumber *junk_key, AttrNumber *junk_member);

extern void vfdw_rowid_read(TupleTableSlot *planSlot, AttrNumber junkattno,
							Datum *value, bool *isnull);

extern AttrNumber vfdw_rowid_find_wholerow(ModifyTableState *mtstate);

#endif							/* VFDW_ROWID_H */
