/*-------------------------------------------------------------------------
 *
 * valkey_fdw.c
 *		Foreign data wrapper for Valkey: registration and entry points.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/valkey_fdw.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw.h"

#include <valkey/valkey.h>

#include "access/reloptions.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#if PG_VERSION_NUM >= 180000
/*
 * PostgreSQL 18 split explain.h: the ExplainState struct moved to
 * explain_state.h and the ExplainProperty* reporting functions to
 * explain_format.h. Before 18 both live in explain.h.
 */
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#endif
#include "foreign/fdwapi.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "parser/parsetree.h"
#include "optimizer/restrictinfo.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "vfdw_estimate.h"
#include "vfdw_map.h"
#include "vfdw_modify.h"
#include "vfdw_rowid.h"
#include "vfdw_scan.h"
#include "vfdw_option.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(valkey_fdw_handler);
PG_FUNCTION_INFO_V1(valkey_fdw_validator);
PG_FUNCTION_INFO_V1(valkey_fdw_version);
PG_FUNCTION_INFO_V1(valkey_fdw_libvalkey_version);
PG_FUNCTION_INFO_V1(valkey_fdw_options);

/*
 * The scan-side callbacks that stayed in this file: planning, EXPLAIN,
 * ANALYZE and the updatability answer. Everything the write path needs lives
 * in src/vfdw_modify.c and src/vfdw_rowid.c, which this file only registers.
 */
static void vfdwGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel,
								  Oid foreigntableid);
static void vfdwGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel,
								Oid foreigntableid);
static ForeignScan *vfdwGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel,
									   Oid foreigntableid,
									   ForeignPath *best_path, List *tlist,
									   List *scan_clauses, Plan *outer_plan);
static void vfdwExplainForeignScan(ForeignScanState *node, ExplainState *es);
static bool vfdwAnalyzeForeignTable(Relation relation,
									AcquireSampleRowsFunc *func,
									BlockNumber *totalpages);
static int	vfdwIsForeignRelUpdatable(Relation rel);

/*
 * Rows assumed for a foreign table nobody has run ANALYZE on.
 *
 * There is no cheap way to count a keyspace, so some number has to be
 * invented. Keeping it a round constant rather than a heuristic makes it
 * obvious in an EXPLAIN that it was invented, and ANALYZE replaces it with a
 * measurement.
 */
#define VFDW_DEFAULT_TUPLES 1000.0

void
vfdw_ereport(int elevel, int sqlerrcode, const char *msg, const char *detail)
{
	/*
	 * detail is deliberately funnelled through "%s". It originates from
	 * Valkey and must never be treated as a format string. See invariant I2.
	 */
	if (detail != NULL)
		ereport(elevel,
				(errcode(sqlerrcode),
				 errmsg("%s", msg),
				 errdetail("%s", detail)));
	else
		ereport(elevel,
				(errcode(sqlerrcode),
				 errmsg("%s", msg)));
}

Datum
valkey_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *routine = makeNode(FdwRoutine);

	routine->GetForeignRelSize = vfdwGetForeignRelSize;
	routine->GetForeignPaths = vfdwGetForeignPaths;
	routine->GetForeignPlan = vfdwGetForeignPlan;
	routine->BeginForeignScan = vfdw_scan_begin;
	routine->IterateForeignScan = vfdw_scan_next;
	routine->ReScanForeignScan = vfdw_scan_rescan;
	routine->EndForeignScan = vfdw_scan_end;
	routine->ExplainForeignScan = vfdwExplainForeignScan;
	routine->AnalyzeForeignTable = vfdwAnalyzeForeignTable;
	routine->IsForeignRelUpdatable = vfdwIsForeignRelUpdatable;

	routine->AddForeignUpdateTargets = vfdw_rowid_add_update_targets;
	routine->PlanForeignModify = vfdw_modify_plan;
	routine->BeginForeignModify = vfdw_modify_begin;
	routine->ExecForeignInsert = vfdw_modify_insert;
	routine->ExecForeignUpdate = vfdw_modify_update;
	routine->ExecForeignDelete = vfdw_modify_delete;
	routine->EndForeignModify = vfdw_modify_end;

	/*
	 * COPY FROM and tuple routing reach ExecForeignInsert through these, not
	 * through BeginForeignModify. Registering BeginForeignInsert is what makes
	 * CopyFrom pass a state at all; while it was absent the wrapper had to
	 * refuse rather than dereference a NULL ri_FdwState.
	 */
	routine->BeginForeignInsert = vfdw_modify_begin_insert;
	routine->EndForeignInsert = vfdw_modify_end_insert;
	routine->ExecForeignBatchInsert = vfdw_modify_batch_insert;
	routine->GetForeignModifyBatchSize = vfdw_modify_batch_size;
	routine->ExplainForeignModify = vfdw_modify_explain;

	PG_RETURN_POINTER(routine);
}

/*
 * Build the "valid options in this context" hint from the same table the
 * lookup used, so the hint can never drift from what is actually accepted.
 */
static char *
vfdw_option_hint(Oid context)
{
	const VfdwOptionDef *def;
	StringInfoData buf;

	initStringInfo(&buf);
	for (def = vfdw_options; def->name != NULL; def++)
	{
		if (def->context != context)
			continue;
		appendStringInfo(&buf, "%s%s", buf.len > 0 ? ", " : "", def->name);
	}

	return buf.len > 0 ? buf.data : pstrdup("<none>");
}

/*
 * The three key-discovery options are alternatives, and this is the only
 * place that can say so at DDL time.
 *
 * A table's whole option list arrives here at once, which is what makes the
 * check possible - unlike a cross-column rule, where the validator sees one
 * column's options and nothing else. Checking here rather than only in
 * vfdw_map_read_table_options closes two holes at once: the table could
 * previously be CREATEd and only fail at its first SELECT, and
 * vfdw_map_writability, which reads options and never builds the map,
 * reported it as fully updatable in information_schema.
 */
static void
vfdw_check_key_options(List *options)
{
	static const char *const exclusive[] = {"singleton_key", "keyprefix",
	"keyset", NULL};
	StringInfoData named;
	ListCell   *lc;
	int			count = 0;
	int			i;

	initStringInfo(&named);

	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		for (i = 0; exclusive[i] != NULL; i++)
		{
			if (strcmp(def->defname, exclusive[i]) != 0)
				continue;
			appendStringInfo(&named, "%s\"%s\"", count > 0 ? ", " : "",
							 exclusive[i]);
			count++;
		}
	}

	if (count > 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a table may name only one key-discovery option"),
				 errdetail("This table names %s.", named.data),
				 errhint("singleton_key, keyprefix and keyset are "
						 "alternatives; keep exactly one.")));
}

/*
 * A packed collection over a string, refused here for the reason the check
 * above is here: the rule is about two options together, so no per-option
 * validator can see it.
 *
 * A string holds one value and has no members. An array of it would always
 * have one element and its length would say nothing, the mapped shape already
 * reads that value, and the packed one would give a string table a second
 * answer to "is this key absent" - GET's nil, or an empty container.
 *
 * vfdw_map_resolve_packed refuses it too, and both are wanted: this one
 * catches the definition, that one catches a catalogue row nothing validated.
 */
static void
vfdw_check_shape_options(List *options)
{
	const char *tabletype = NULL;
	bool		legacy = false;
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "tabletype") == 0)
			tabletype = defGetString(def);
		else if (strcmp(def->defname, "legacy_value") == 0)
			legacy = defGetBoolean(def);
	}

	if (legacy && tabletype != NULL && strcmp(tabletype, "string") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("legacy_value cannot be combined with tabletype \"string\""),
				 errdetail("A string holds one value, not a collection."),
				 errhint("Drop legacy_value to read it as a single column, or "
						 "name a collection table type.")));
}

Datum
valkey_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			context = PG_GETARG_OID(1);
	ListCell   *lc;
	List	   *seen = NIL;

	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		const VfdwOptionDef *odef;
		ListCell   *lc2;

		odef = vfdw_find_option(def->defname, context);
		if (odef == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
					 errhint("Valid options in this context are: %s",
							 vfdw_option_hint(context))));

		/*
		 * Duplicate detection is uniform rather than per-option, so it cannot
		 * be defeated by a value that happens to be falsy - a per-option
		 * check tests the value it stored rather than whether it stored one,
		 * so "database '0'" may be given twice.
		 */
		foreach(lc2, seen)
		{
			if (strcmp((char *) lfirst(lc2), def->defname) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("option \"%s\" specified more than once",
								def->defname)));
		}
		seen = lappend(seen, pstrdup(def->defname));

		vfdw_validate_option(odef, defGetString(def));
	}

	/*
	 * Last, so that an unknown or malformed option is reported before a
	 * combination rule fires on options that may themselves be nonsense.
	 */
	if (context == ForeignTableRelationId)
	{
		vfdw_check_key_options(options);
		vfdw_check_shape_options(options);
	}

	PG_RETURN_VOID();
}

Datum
valkey_fdw_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(VFDW_CODE_VERSION);
}

/*
 * Report the libvalkey the module is actually running against.
 *
 * Compile-time macros are a fallback: they describe what we built against,
 * which can differ from what is loaded. Verification item V7 tracks whether
 * libvalkey grows a runtime accessor; if it does, this switches to it.
 */
Datum
valkey_fdw_libvalkey_version(PG_FUNCTION_ARGS)
{
#if defined(LIBVALKEY_VERSION_MAJOR) && defined(LIBVALKEY_VERSION_MINOR) && defined(LIBVALKEY_VERSION_PATCH)
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%d.%d.%d",
											  LIBVALKEY_VERSION_MAJOR,
											  LIBVALKEY_VERSION_MINOR,
											  LIBVALKEY_VERSION_PATCH)));
#elif defined(LIBVALKEY_VERSION)
	PG_RETURN_TEXT_P(cstring_to_text(LIBVALKEY_VERSION));
#else
	PG_RETURN_TEXT_P(cstring_to_text("unknown"));
#endif
}

/*
 * Name an option's type for SQL.
 *
 * Lives beside the SRF rather than inside it so the row-building loop stays
 * short enough to read in one go. The fallback is not dead code: it is what a
 * kind added to the table and not taught to this mapping produces, and
 * "unknown" in a diagnostic view is a great deal easier to notice than an
 * uninitialised pointer.
 */
static const char *
vfdw_option_kind_label(VfdwOptKind kind)
{
	switch (kind)
	{
		case VFDW_OPT_STRING:
			return "string";
		case VFDW_OPT_INT:
			return "integer";
		case VFDW_OPT_BOOL:
			return "boolean";
		case VFDW_OPT_ENUM:
			return "enum";
		case VFDW_OPT_PATH:
			return "path";
	}

	return "unknown";
}

/*
 * Expose the option table to SQL.
 *
 * This is what lets the test suite assert that every option the code knows
 * about is reachable and documented, rather than trusting that a strcmp
 * chain somewhere matches this list. requires_superuser is here for the same
 * reason sensitive is: an access rule users can only discover by being
 * refused is not a documented rule.
 */
Datum
valkey_fdw_options(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const VfdwOptionDef *def;

	InitMaterializedSRF(fcinfo, 0);

	for (def = vfdw_options; def->name != NULL; def++)
	{
		/*
		 * As wide as the OUT list in valkey_fdw--0.1.sql, which is where
		 * InitMaterializedSRF got the descriptor tuplestore_putvalues reads
		 * these against. A column added there and not here is not a compile
		 * error; it is a read off the end of this frame that usually returns
		 * stack garbage rather than crashing.
		 */
		Datum		values[6];
		bool		nulls[6] = {false, false, false, false, false, false};
		const char *ctx;

		if (def->context == ForeignServerRelationId)
			ctx = "server";
		else if (def->context == UserMappingRelationId)
			ctx = "user_mapping";
		else if (def->context == ForeignTableRelationId)
			ctx = "table";
		else if (def->context == AttributeRelationId)
			ctx = "column";
		else
			ctx = "unknown";

		values[0] = CStringGetTextDatum(def->name);
		values[1] = CStringGetTextDatum(ctx);
		values[2] = CStringGetTextDatum(vfdw_option_kind_label(def->kind));
		if (def->defval != NULL)
			values[3] = CStringGetTextDatum(def->defval);
		else
			nulls[3] = true;
		values[4] = BoolGetDatum(def->sensitive);
		values[5] = BoolGetDatum(def->priv != VFDW_OPTPRIV_NONE);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

/* ---------------------------------------------------------------------
 * Planning.
 * --------------------------------------------------------------------- */

static void
vfdwGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	/*
	 * Resolve the table's shape before anything else. A column with no source
	 * in Valkey is a definition error, and finding it here means it is
	 * reported once, at plan time, rather than becoming an out-of-bounds read
	 * per row in the executor.
	 */
	baserel->fdw_private = vfdw_map_build_for_relid(foreigntableid);

	/*
	 * reltuples is -1 until someone runs ANALYZE, so a table nobody has
	 * measured gets the placeholder. Selectivity is then left to the planner's
	 * own machinery rather than estimated here: it already knows what a key
	 * equality is worth, and hand-rolling that is how an FDW ends up claiming
	 * one row for a clause that matches thousands.
	 */
	if (baserel->tuples < 0)
	{
		/*
		 * Asked of the server, for the two shapes it can answer in one
		 * command. Everything else keeps the placeholder, which is why this
		 * does not turn planning into a round trip for every table.
		 */
		double		rows = vfdw_estimate_rows(foreigntableid,
											  (VfdwTableMap *) baserel->fdw_private);

		baserel->tuples = rows >= 0 ? rows : VFDW_DEFAULT_TUPLES;
	}

	set_baserel_size_estimates(root, baserel);
}

static void
vfdwGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	Cost		startup_cost;
	Cost		total_cost;

	(void) foreigntableid;

	/*
	 * One round trip to get going, then one per page of keys plus the usual
	 * per-tuple cost. Crude, but it is at least shaped like what the scan
	 * actually does; real numbers arrive with AnalyzeForeignTable.
	 */
	startup_cost = 10;
	total_cost = startup_cost + baserel->rows * cpu_tuple_cost;

	add_path(baserel, (Path *)
			 create_foreignscan_path(root, baserel,
									 NULL,	/* default pathtarget */
									 baserel->rows,
#if PG_VERSION_NUM >= 180000
									 0, /* no disabled nodes */
#endif
									 startup_cost,
									 total_cost,
									 NIL,	/* no pathkeys */
									 NULL,	/* no outer rel */
									 NULL,	/* no extra plan */
#if PG_VERSION_NUM >= 170000
									 NIL,	/* no fdw_restrictinfo */
#endif
									 NIL));	/* no fdw_private */
}

static ForeignScan *
vfdwGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid,
				   ForeignPath *best_path, List *tlist, List *scan_clauses,
				   Plan *outer_plan)
{
	List	   *fdw_private;

	(void) foreigntableid;
	(void) best_path;

	/*
	 * Every restriction clause stays with the scan node for the executor to
	 * apply, including the key equality that becomes a single fetch. Valkey
	 * answers by exact key, so the recheck costs nothing, and leaving it in
	 * place means a mistake in the access-path choice cannot return rows that
	 * do not satisfy the query.
	 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	fdw_private = vfdw_scan_plan(root, baserel,
								 (VfdwTableMap *) baserel->fdw_private,
								 scan_clauses);

	return make_foreignscan(tlist, scan_clauses, baserel->relid,
							NIL,	/* no expressions to evaluate */
							fdw_private,
							NIL,	/* no custom tlist */
							NIL,	/* no remote quals */
							outer_plan);
}

/*
 * Report how the scan reaches Valkey.
 *
 * The strategy comes from the plan rather than from execution state, so it is
 * visible on a plain EXPLAIN of a query that never runs. The skipped count is
 * a measurement and only exists under ANALYZE: it is how many keys the scan
 * discovered and then found gone or holding another type, which is otherwise
 * invisible - the rows simply are not there.
 */
static void
vfdwExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
	List	   *private = plan->fdw_private;
	VfdwScanStrategy strategy;

	ExplainPropertyText("Valkey Strategy",
						vfdw_scan_strategy_name(private), es);

	/*
	 * How much of the keyspace the scan will walk, or how many keys it will
	 * fetch outright. Without these a narrowed scan and a full one look
	 * identical from the outside: same rows, very different cost.
	 */
	strategy = (VfdwScanStrategy) intVal(list_nth(private, VFDW_PRIV_STRATEGY));
	if (strategy == VFDW_SCAN_KEYSPACE)
	{
		const char *pattern = vfdw_plan_pattern(private);

		if (pattern != NULL)
			ExplainPropertyText("Valkey Match", pattern, es);
	}
	else if (strategy == VFDW_SCAN_KEYS)
		ExplainPropertyInteger("Valkey Keys", NULL,
							   list_length(vfdw_plan_keys(private)), es);

	if (es->analyze)
	{
		ExplainPropertyInteger("Valkey Keys Skipped", NULL,
							   vfdw_scan_skipped(node), es);

		/*
		 * How many SCAN round trips the page size bought. Reported because it
		 * is the only consequence of scan_count anything outside the wrapper
		 * can observe: the same rows come back in the same order at every
		 * page size, so without this the option's only test is that its value
		 * parses.
		 */
		if (strategy == VFDW_SCAN_KEYSPACE)
			ExplainPropertyInteger("Valkey Scan Round Trips", NULL,
								   vfdw_scan_pages(node), es);
	}
}

/*
 * Which commands a table would accept.
 *
 * Without this callback core reports every foreign table as read-only, so an
 * INSERT is refused with a message about foreign tables in general rather than
 * about this one. Answering here means a table that genuinely cannot be
 * written - readonly, a shape with no row identity, one whose reader is not
 * implemented - says so through information_schema and pg_relation_is_updatable
 * as well as through the error.
 *
 * Must not raise and must not build the map: the rewriter calls this for every
 * foreign table it touches, and information_schema.tables calls it for every
 * relation in the database.
 */
static int
vfdwIsForeignRelUpdatable(Relation rel)
{
	return vfdw_map_writability_for_relid(RelationGetRelid(rel));
}

/*
 * Accept ANALYZE and hand back the sampler, so a table's row estimate is one
 * somebody measured rather than one invented from the key count.
 *
 * totalpages is zero because Valkey has no pages to count and inventing a
 * number here would only feed a fabricated relpages back into the planner.
 * The row count that matters comes from the sample scan, which counts every
 * row it reads.
 */
static bool
vfdwAnalyzeForeignTable(Relation relation, AcquireSampleRowsFunc *func,
						BlockNumber *totalpages)
{
	(void) relation;

	*func = vfdw_scan_acquire_sample_rows;
	*totalpages = 0;
	return true;
}

