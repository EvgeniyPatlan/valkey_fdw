/*-------------------------------------------------------------------------
 *
 * vfdw_rowid.c
 *		Row identity per table shape.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_rowid.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_rowid.h"

#include "vfdw_refuse.h"

#include "access/table.h"
#include "catalog/pg_foreign_table.h"
#include "executor/executor.h"
#include "foreign/foreign.h"
#include "nodes/makefuncs.h"
/*
 * Named explicitly: add_row_identity_var is NOT reached transitively through
 * optimizer/planmain.h, and the build failure that follows from assuming it is
 * looks like a missing prototype for a function that plainly exists.
 */
#include "optimizer/appendinfo.h"
#include "utils/builtins.h"
#include "utils/rel.h"

/*
 * Row identity per table shape, as code:
 *
 *		string, hash					the key
 *		list, set, zset					(key, member)
 *		any shape with singleton_key	the option supplies the key, so no
 *										key junk is wanted; the collection
 *										types still want their member
 *
 * Exhaustive over VfdwTableType, and deliberately without a default arm: one
 * would let the next table type added to the enum acquire whatever identity
 * rule happened to be written last, silently. A missing arm is a compiler
 * warning; a wrong identity is a row updated in the wrong place.
 */
void
vfdw_rowid_shape(const VfdwTableMap *map, CmdType operation, VfdwRowId *out)
{
	bool		singleton = (map->singleton_key != NULL);

	out->want_key = false;
	out->keyattno = map->keyattno;
	out->want_member = false;
	out->memberattno = map->memberattno;

	/*
	 * A PACKED ROW IS THE KEY, whatever collection is inside it. Its array is
	 * the key's whole contents rather than one member of them, so the key
	 * identifies the row completely and there is no member to name - which is
	 * also why a packed table can be written at all: the objection to writing
	 * one was that an array of members says nothing about WHICH member a write
	 * means, and a write that replaces all of them names none.
	 */
	if (map->legacy_value)
	{
		out->want_key = !singleton;
	}
	else
	{
		switch (map->tabletype)
		{
			case VFDW_TABLE_STRING:
			case VFDW_TABLE_HASH:
				out->want_key = !singleton;
				break;

			case VFDW_TABLE_LIST:
			case VFDW_TABLE_SET:
			case VFDW_TABLE_ZSET:
				out->want_key = !singleton;
				out->want_member = true;
				break;
		}
	}

	if (out->want_key && out->keyattno == InvalidAttrNumber)
		elog(ERROR, "valkey_fdw: table has no key column to identify rows by");
	if (out->want_member && out->memberattno == InvalidAttrNumber)
		elog(ERROR, "valkey_fdw: table has no member column to identify rows by");
}

char *
vfdw_rowid_junk_name(const char *role, AttrNumber attno)
{
	return psprintf("vfdw_%s_a%d", role, (int) attno);
}

/*
 * Inject one junk Var over a real column.
 *
 * add_row_identity_var asserts varno == rtindex, varlevelsup == 0 and
 * varnullingrels == NULL, all of which makeVar with these arguments
 * satisfies.
 */
static void
vfdw_rowid_add_one(PlannerInfo *root, Index rtindex, Relation rel,
				   const char *role, AttrNumber attno)
{
	Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(rel), attno - 1);
	Var		   *var;

	var = makeVar(rtindex, attno, attr->atttypid, attr->atttypmod,
				  attr->attcollation, 0);

	add_row_identity_var(root, var, rtindex,
						 vfdw_rowid_junk_name(role, attno));
}

/*
 * Never called for INSERT: core does not call this callback for one, so there
 * is no CMD_INSERT branch to get wrong.
 *
 * Building the map here is safe and deliberate. This is a plan-time path,
 * unlike IsForeignRelUpdatable, so a shape the reader refuses is refused for
 * its own reason rather than with a message about writes.
 */
void
vfdw_rowid_add_update_targets(PlannerInfo *root, Index rtindex,
							  RangeTblEntry *target_rte,
							  Relation target_relation)
{
	Oid			relid = RelationGetRelid(target_relation);
	VfdwTableMap *map = vfdw_map_build(target_relation, GetForeignTable(relid));
	VfdwRowId	rowid;

	/*
	 * Asked here, before the shape is taken, because this callback runs ahead
	 * of PlanForeignModify and vfdw_rowid_shape has no answer for a table that
	 * cannot be written. A packed collection is the case: it names no member
	 * column, so the shape would demand one and reach its own internal error -
	 * an XX000 about row identity, for a table whose real answer is that this
	 * shape reads and does not write. The refusal that says so lives one
	 * callback later, and nothing reaches it.
	 */
	vfdw_refuse_unwritable(relid, map, root->parse->commandType);

	vfdw_rowid_shape(map, root->parse->commandType, &rowid);

	if (rowid.want_key)
		vfdw_rowid_add_one(root, rtindex, target_relation, "key",
						   rowid.keyattno);
	if (rowid.want_member)
		vfdw_rowid_add_one(root, rtindex, target_relation, "member",
						   rowid.memberattno);

	/*
	 * A DELETE has no new row, so without this its RETURNING clause gets an
	 * all-null tuple: core stores one, the row count is right, and every
	 * column is NULL. That is a plausible empty result, which is the failure
	 * mode this project refuses everywhere else.
	 *
	 * Core adds a wholerow junk column of its own for a foreign table only
	 * when row triggers exist, and a deferred write has no remote RETURNING to
	 * fetch the old row from - so the row has to be captured locally, at scan
	 * time, by asking for it here.
	 *
	 * Asked for unconditionally on DELETE rather than only when RETURNING is
	 * present: root->parse->returningList is the *query's* list, and a DELETE
	 * inside a CTE or a rule can acquire one later in planning. One extra
	 * whole-row Var on a statement that does not use it costs a projection
	 * core was already doing.
	 */
	if (root->parse->commandType == CMD_DELETE)
	{
		Var		   *var = makeVar(rtindex, InvalidAttrNumber,
								  RelationGetForm(target_relation)->reltype,
								  -1, InvalidOid, 0);

		add_row_identity_var(root, var, rtindex, "wholerow");
	}
}

/*
 * The wholerow junk column, or InvalidAttrNumber when the plan has none.
 *
 * Unlike the identity columns this is NOT an error to be missing: a plan
 * shape that never asked for it is a plan that will not use it, and the
 * delete callback falls back to the slot core gave it.
 */
AttrNumber
vfdw_rowid_find_wholerow(ModifyTableState *mtstate)
{
	Plan	   *subplan = outerPlanState(mtstate)->plan;

	return ExecFindJunkAttributeInTlist(subplan->targetlist, "wholerow");
}

/*
 * Follows postgres_fdw's precedent exactly, including the elog: a junk column
 * the plan promised and the executor cannot find is an internal
 * inconsistency, not a user error.
 */
static AttrNumber
vfdw_rowid_find(ModifyTableState *mtstate, const char *name)
{
	Plan	   *subplan = outerPlanState(mtstate)->plan;
	AttrNumber	attno;

	if (name == NULL || name[0] == '\0')
		return InvalidAttrNumber;

	attno = ExecFindJunkAttributeInTlist(subplan->targetlist, name);
	if (!AttributeNumberIsValid(attno))
		elog(ERROR, "could not find junk column \"%s\"", name);

	return attno;
}

void
vfdw_rowid_resolve(ModifyTableState *mtstate, const List *junk_names,
				   AttrNumber *junk_key, AttrNumber *junk_member)
{
	*junk_key = InvalidAttrNumber;
	*junk_member = InvalidAttrNumber;

	if (junk_names == NIL)
		return;

	Assert(list_length(junk_names) == 2);
	*junk_key = vfdw_rowid_find(mtstate, strVal(list_nth(junk_names, 0)));
	*junk_member = vfdw_rowid_find(mtstate, strVal(list_nth(junk_names, 1)));
}

/*
 * A NULL row identity means the scan produced a row it cannot name, which is
 * an internal inconsistency rather than a user error - so it is an elog, not
 * an ereport with a SQLSTATE an application could branch on.
 */
void
vfdw_rowid_read(TupleTableSlot *planSlot, AttrNumber junkattno,
				Datum *value, bool *isnull)
{
	*value = slot_getattr(planSlot, junkattno, isnull);

	if (*isnull)
		elog(ERROR, "valkey_fdw: row identity column %d is null",
			 (int) junkattno);
}
