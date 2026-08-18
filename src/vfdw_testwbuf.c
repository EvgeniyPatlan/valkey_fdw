/*-------------------------------------------------------------------------
 *
 * vfdw_testwbuf.c
 *		SQL entry points that observe the deferred write buffer.
 *
 * A file of its own rather than an extension of src/vfdw_testfuncs.c, which
 * is already 596 lines against a hard 800-line gate.
 *
 * Three rules govern the dump, and each is a defect it would otherwise have:
 *
 *	- Nothing here allocates in the write-buffer context. The tuplestore and
 *	  every bytea it builds go in CurrentMemoryContext, because allocating in
 *	  the buffer would inflate the very number valkey_fdw_test_wbuf_stats
 *	  reports.
 *	- tuple_probe renders the first non-dropped attribute out of the STORED
 *	  HeapTuple, using the relation's current tupledesc. That read-back is what
 *	  turns "we called ExecCopySlotHeapTuple" from a claim into an observable.
 *	- Keys, members and field values are bytea, never text. An embedded NUL and
 *	  an invalidly encoded byte both vanish through text, and those are exactly
 *	  what the write path must carry.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_testwbuf.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "lib/stringinfo.h"

#include "vfdw_flush.h"
#include "vfdw_ledger.h"
#include "vfdw_opcode.h"
#include "vfdw_scan.h"
#include "vfdw_script.h"
#include "vfdw_overlay.h"
#include "vfdw_wbuf.h"

PG_FUNCTION_INFO_V1(valkey_fdw_test_wbuf_stats);
PG_FUNCTION_INFO_V1(valkey_fdw_test_wbuf_dump);
PG_FUNCTION_INFO_V1(valkey_fdw_test_wbuf_fields);
PG_FUNCTION_INFO_V1(valkey_fdw_test_wbuf_targets);
PG_FUNCTION_INFO_V1(valkey_fdw_test_flush_stats);

#define VFDW_WBUF_STATS_NCOLS	9
#define VFDW_WBUF_DUMP_NCOLS	15
#define VFDW_WBUF_FIELDS_NCOLS	4
#define VFDW_WBUF_TARGETS_NCOLS	3
#define VFDW_FLUSH_STATS_NCOLS	4

/* Bytes with an explicit length, never strlen: invariant I3 by habit. */
static bytea *
vfdw_testwbuf_bytea(const char *data, size_t len)
{
	bytea	   *out = (bytea *) palloc(VARHDRSZ + len);

	SET_VARSIZE(out, VARHDRSZ + len);
	if (len > 0)
		memcpy(VARDATA(out), data, len);
	return out;
}

static const char *
vfdw_testwbuf_kind_name(VfdwOpKind kind)
{
	switch (kind)
	{
		case VFDW_OP_INSERT:
			return "insert";
		case VFDW_OP_UPDATE:
			return "update";
		case VFDW_OP_DELETE:
			return "delete";
	}
	return "unknown";
}

Datum
valkey_fdw_test_wbuf_stats(PG_FUNCTION_ARGS)
{
	const VfdwWriteUnit *unit = vfdw_wbuf_get_unit();
	Datum		values[VFDW_WBUF_STATS_NCOLS];
	bool		nulls[VFDW_WBUF_STATS_NCOLS];
	TupleDesc	tupdesc;
	int			i;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "valkey_fdw_test_wbuf_stats: return type is not a record");
	tupdesc = BlessTupleDesc(tupdesc);

	for (i = 0; i < VFDW_WBUF_STATS_NCOLS; i++)
		nulls[i] = false;

	values[0] = Int32GetDatum(vfdw_wbuf_live_ops());
	values[1] = Int64GetDatum((int64) vfdw_wbuf_alloc_bytes());
	values[2] = Int64GetDatum((int64) vfdw_wbuf_generation());
	values[3] = BoolGetDatum(unit->bound);

	/*
	 * Everything below is meaningless when nothing is bound, so it is
	 * reported as NULL rather than as a stale value from the last binding.
	 */
	if (!unit->bound)
	{
		for (i = 4; i < VFDW_WBUF_STATS_NCOLS; i++)
			nulls[i] = true;
	}
	else
	{
		values[4] = ObjectIdGetDatum(unit->serverid);
		values[5] = ObjectIdGetDatum(unit->umid);
		values[6] = Int32GetDatum(unit->database);
		values[7] = Int32GetDatum(unit->max_ops);
		values[8] = Int64GetDatum(unit->max_bytes);
	}

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * Render the operation's stored HeapTuple's first non-dropped attribute.
 *
 * Read through the relation's current tupledesc, in the CALLER's context, so
 * a tuple that was borrowed rather than copied shows up here as the wrong
 * row, as garbage, or as an ASan report - instead of as a claim nobody
 * checked.
 */
static Datum
vfdw_testwbuf_probe(const VfdwWriteOp *op, bool *isnull)
{
	Relation	rel;
	TupleDesc	tupdesc;
	Datum		result = (Datum) 0;
	int			i;

	*isnull = true;
	if (op->newtup == NULL)
		return result;

	rel = table_open(op->relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
		Datum		d;
		bool		attnull;

		if (attr->attisdropped)
			continue;

		d = heap_getattr(op->newtup, attr->attnum, tupdesc, &attnull);
		if (!attnull)
		{
			Oid			outfunc;
			bool		varlena;

			getTypeOutputInfo(attr->atttypid, &outfunc, &varlena);
			result = CStringGetTextDatum(OidOutputFunctionCall(outfunc, d));
			*isnull = false;
		}
		break;
	}

	table_close(rel, AccessShareLock);
	return result;
}

static void
vfdw_testwbuf_dump_one(ReturnSetInfo *rsinfo, const VfdwWriteOp *op, int ordinal)
{
	Datum		values[VFDW_WBUF_DUMP_NCOLS];
	bool		nulls[VFDW_WBUF_DUMP_NCOLS];
	int			i;

	for (i = 0; i < VFDW_WBUF_DUMP_NCOLS; i++)
		nulls[i] = false;

	values[0] = Int32GetDatum(ordinal);
	values[1] = ObjectIdGetDatum(op->relid);
	values[2] = CStringGetTextDatum(vfdw_testwbuf_kind_name(op->kind));
	values[3] = Int32GetDatum(op->level);
	values[4] = Int64GetDatum((int64) op->cid);
	values[5] = Int32GetDatum((int) op->hashslot);

	values[6] = PointerGetDatum(vfdw_testwbuf_bytea(op->key, op->keylen));
	if (op->oldkey != NULL)
		values[7] = PointerGetDatum(vfdw_testwbuf_bytea(op->oldkey,
														op->oldkeylen));
	else
		nulls[7] = true;
	if (op->has_member)
		values[8] = PointerGetDatum(vfdw_testwbuf_bytea(op->member,
														op->memberlen));
	else
		nulls[8] = true;

	/*
	 * oldmember, value and score exist here because the buffer is the only
	 * place they live until S6 sends them. Without a column apiece, stubbing
	 * any of the three to an empty string changes nothing a suite can see -
	 * measured, not assumed: all three survived a mutation pass unnoticed.
	 */
	if (op->has_oldmember)
		values[9] = PointerGetDatum(vfdw_testwbuf_bytea(op->oldmember,
														op->oldmemberlen));
	else
		nulls[9] = true;
	if (op->has_value)
		values[10] = PointerGetDatum(vfdw_testwbuf_bytea(op->value,
														 op->valuelen));
	else
		nulls[10] = true;
	if (op->has_score)
		values[11] = PointerGetDatum(vfdw_testwbuf_bytea(op->score,
														 op->scorelen));
	else
		nulls[11] = true;

	values[12] = Int32GetDatum(op->nfields);
	values[13] = Int32GetDatum(op->newtup != NULL ?
							   HeapTupleHeaderGetNatts(op->newtup->t_data) : 0);
	values[14] = vfdw_testwbuf_probe(op, &nulls[14]);

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

Datum
valkey_fdw_test_wbuf_dump(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const VfdwWriteOp *op;
	int			ordinal = 1;

	InitMaterializedSRF(fcinfo, 0);

	for (op = vfdw_wbuf_first(); op != NULL; op = op->next)
		vfdw_testwbuf_dump_one(rsinfo, op, ordinal++);

	return (Datum) 0;
}

/*
 * Hash field names and values, per operation.
 *
 * Separate from the dump because a field name outliving the statement that
 * produced it is the property under test, and reading it back is the only way
 * to see that it was copied rather than pointed at.
 */
Datum
valkey_fdw_test_wbuf_fields(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const VfdwWriteOp *op;
	int			ordinal = 1;

	InitMaterializedSRF(fcinfo, 0);

	for (op = vfdw_wbuf_first(); op != NULL; op = op->next, ordinal++)
	{
		int			i;

		for (i = 0; i < op->nfields; i++)
		{
			const VfdwWriteArg *arg = &op->fields[i];
			Datum		values[VFDW_WBUF_FIELDS_NCOLS];
			bool		nulls[VFDW_WBUF_FIELDS_NCOLS] = {false, false, false,
			false};

			values[0] = Int32GetDatum(ordinal);
			values[1] = Int32GetDatum(i + 1);
			values[2] = PointerGetDatum(vfdw_testwbuf_bytea(arg->name,
															arg->namelen));
			if (arg->isnull)
				nulls[3] = true;
			else
				values[3] = PointerGetDatum(vfdw_testwbuf_bytea(arg->data,
																arg->len));

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values,
								 nulls);
		}
	}

	return (Datum) 0;
}

/*
 * valkey_fdw_test_wbuf_targets() -> (ordinal, kind, attnum)
 *
 * Which columns each buffered operation was planned against.
 *
 * vfdw_modify_target_attrs decides this at plan time and had NO OBSERVABLE
 * ANYWHERE - not in the buffer dump, not in EXPLAIN, not in fdw_private
 * printing. Making it return NIL was run and left every suite green, which
 * is thirty-five lines whose removal nothing would notice.
 *
 * The distinction it draws is the one worth asserting. An INSERT targets
 * every non-dropped, non-generated column, because every one of them is
 * being written. An UPDATE targets only the columns the statement actually
 * assigned, taken from the RTE's updatedCols - so a two-column table updated
 * in one column must show one attnum here and not two. Collapsing the two
 * cases would make an UPDATE rewrite fields it was never asked to touch.
 *
 * Attribute numbers are reported as they are stored: real 1-based attnums,
 * not the FirstLowInvalidHeapAttributeNumber-offset form that updatedCols
 * uses, because that offset is undone when the list is built.
 */
Datum
valkey_fdw_test_wbuf_targets(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const VfdwWriteOp *op;
	int			ordinal = 1;

	InitMaterializedSRF(fcinfo, 0);

	for (op = vfdw_wbuf_first(); op != NULL; op = op->next, ordinal++)
	{
		int			attno = -1;

		/*
		 * bms_next_member answers -2 for a NULL set, so an operation with no
		 * targets contributes no rows rather than crashing - which is what a
		 * regressed vfdw_modify_target_attrs would produce, and it must be
		 * visible as missing rows rather than as an error.
		 */
		while ((attno = bms_next_member(op->target_attrs, attno)) >= 0)
		{
			Datum		values[VFDW_WBUF_TARGETS_NCOLS];
			bool		nulls[VFDW_WBUF_TARGETS_NCOLS] = {false, false, false};

			values[0] = Int32GetDatum(ordinal);
			values[1] = CStringGetTextDatum(vfdw_testwbuf_kind_name(op->kind));
			values[2] = Int32GetDatum(attno);

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values,
								 nulls);
		}
	}

	return (Datum) 0;
}

/*
 * valkey_fdw_test_overlay_stats() -> how the read-your-own-writes index moved
 *
 * The property a bulk write has to hold is that its scan does not throw the
 * index away per row. That is a COUNT rather than a duration: asserting the
 * wall clock would be asserting something about this machine, and the same
 * quadratic would pass on a fast one.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_overlay_stats);

Datum
valkey_fdw_test_overlay_stats(PG_FUNCTION_ARGS)
{
	Datum		values[2];
	bool		nulls[2] = {false, false};
	TupleDesc	tupdesc;
	uint64		rebuilds;
	uint64		extends;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "valkey_fdw_test_overlay_stats: return type is not a record");
	tupdesc = BlessTupleDesc(tupdesc);

	vfdw_overlay_index_stats(&rebuilds, &extends);
	values[0] = Int64GetDatum((int64) rebuilds);
	values[1] = Int64GetDatum((int64) extends);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * valkey_fdw_test_leak_stats() -> the two pairings that must stay balanced
 *
 * A scan's batch contexts against the resets a rescan does instead of creating
 * another, and the flush's batches opened against those it closed. Both were
 * fixed with no test, because neither leak fails anything: the query answers,
 * the transaction commits, and the cost is memory nobody counted. These count
 * it.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_leak_stats);

Datum
valkey_fdw_test_leak_stats(PG_FUNCTION_ARGS)
{
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};
	TupleDesc	tupdesc;
	uint64		contexts;
	uint64		resets;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "valkey_fdw_test_leak_stats: return type is not a record");
	tupdesc = BlessTupleDesc(tupdesc);

	vfdw_scan_batch_stats(&contexts, &resets);
	values[0] = Int64GetDatum((int64) contexts);
	values[1] = Int64GetDatum((int64) resets);
	values[2] = Int64GetDatum((int64) vfdw_flush_batches_opened());
	values[3] = Int64GetDatum((int64) vfdw_flush_batches_closed());

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

Datum
valkey_fdw_test_flush_stats(PG_FUNCTION_ARGS)
{
	Datum		values[VFDW_FLUSH_STATS_NCOLS];
	bool		nulls[VFDW_FLUSH_STATS_NCOLS] = {false, false, false, false};
	TupleDesc	tupdesc;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "valkey_fdw_test_flush_stats: return type is not a record");
	tupdesc = BlessTupleDesc(tupdesc);

	values[0] = Int64GetDatum((int64) vfdw_flush_calls());
	values[1] = Int64GetDatum((int64) vfdw_flush_empty_returns());
	values[2] = Int64GetDatum((int64) vfdw_flush_flushes());
	values[3] = Int64GetDatum((int64) vfdw_flush_retries());

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * valkey_fdw_test_ledger() -> the folded plan, one row per check and action
 *
 * Rendered as names and bytea, never as opcode numbers: a recorded file full
 * of small integers survives a renumbering of the enum unchanged, because
 * every number moves together, and the S5 script would then be decoding the
 * wrong opcode for the right name.
 *
 * step_no is per plan and restarts at 1 for the actions, so the check list
 * and the action list are each independently ordered in the output. Ordering
 * is the property that matters most here - ZREM before ZADD on a member
 * rename is the difference between a rename and a deletion.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_ledger);

#define VFDW_LEDGER_NCOLS	9

static void
vfdw_testwbuf_ledger_row(ReturnSetInfo *rsinfo, const VfdwKeyPlan *plan,
						 int planno, const char *kind, int stepno,
						 const char *opname, const VfdwLedgerStep *step)
{
	Datum		values[VFDW_LEDGER_NCOLS];
	bool		nulls[VFDW_LEDGER_NCOLS];
	int			i;

	for (i = 0; i < VFDW_LEDGER_NCOLS; i++)
		nulls[i] = false;

	values[0] = Int32GetDatum(planno);
	values[1] = PointerGetDatum(vfdw_testwbuf_bytea(plan->key, plan->keylen));
	values[2] = CStringGetTextDatum(vfdw_tabletype_name(plan->ttype));
	values[3] = CStringGetTextDatum(vfdw_ledger_require_name(plan->require));
	values[4] = CStringGetTextDatum(vfdw_ledger_state_name(plan->state));
	values[5] = CStringGetTextDatum(kind);
	values[6] = Int32GetDatum(stepno);
	values[7] = CStringGetTextDatum(opname);

	if (step != NULL && step->nargs > 0)
	{
		StringInfoData buf;
		int			a;

		initStringInfo(&buf);
		for (a = 0; a < step->nargs; a++)
		{
			if (a > 0)
				appendStringInfoChar(&buf, '|');
			appendBinaryStringInfo(&buf, step->args[a].data,
								   (int) step->args[a].len);
		}
		values[8] = PointerGetDatum(vfdw_testwbuf_bytea(buf.data, buf.len));
		pfree(buf.data);
	}
	else
		nulls[8] = true;

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

Datum
valkey_fdw_test_ledger(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const VfdwKeyPlan *plan;
	int			planno = 1;

	InitMaterializedSRF(fcinfo, 0);

	for (plan = vfdw_ledger_first(); plan != NULL; plan = plan->next, planno++)
	{
		const VfdwLedgerStep *s;
		int			n;

		/*
		 * A plan with no steps at all still gets a row. Without it a fold
		 * that dropped every action would render as an absent plan and read
		 * as "this key was never touched", which is the opposite of the
		 * truth and the easiest kind of wrong answer to believe.
		 */
		if (plan->checks == NULL && plan->actions == NULL)
			vfdw_testwbuf_ledger_row(rsinfo, plan, planno, "empty", 0,
									 "NONE", NULL);

		for (s = plan->checks, n = 1; s != NULL; s = s->next, n++)
			vfdw_testwbuf_ledger_row(rsinfo, plan, planno, "check", n,
									 vfdw_ledger_check_name(s->op), s);

		for (s = plan->actions, n = 1; s != NULL; s = s->next, n++)
			vfdw_testwbuf_ledger_row(rsinfo, plan, planno, "action", n,
									 vfdw_ledger_action_name(s->op), s);
	}

	return (Datum) 0;
}

/*
 * Script diagnostics.
 *
 * The script's text is exposed so the suite can parse its two Lua dispatch
 * tables and compare them against the C enums. That comparison is the whole
 * point: the enum and the tables are two independent spellings of one
 * vocabulary, and nothing but a test can hold them together. Reading the
 * text from SQL keeps the parity check honest - a C-side check would compare
 * the enum against itself.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_script);
PG_FUNCTION_INFO_V1(valkey_fdw_test_script_sha1);
PG_FUNCTION_INFO_V1(valkey_fdw_test_script_classify);
PG_FUNCTION_INFO_V1(valkey_fdw_test_opcodes);

Datum
valkey_fdw_test_script(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(vfdw_script_text()));
}

Datum
valkey_fdw_test_script_sha1(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(vfdw_script_sha1()));
}

/*
 * Takes bytea, not text. A reply is bytes with a length, and the boundary
 * cases this exists to pin - a code at the very end of the buffer, a code
 * followed by a letter - are exactly the ones a NUL-terminated round trip
 * would blur.
 */
Datum
valkey_fdw_test_script_classify(PG_FUNCTION_ARGS)
{
	bytea	   *in = PG_GETARG_BYTEA_PP(0);
	char	   *detail = NULL;
	VfdwScriptVerdict v;
	Datum		values[2];
	bool		nulls[2] = {false, false};
	TupleDesc	tupdesc;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "valkey_fdw_test_script_classify: return type is not a record");
	tupdesc = BlessTupleDesc(tupdesc);

	v = vfdw_script_classify(VARDATA_ANY(in), VARSIZE_ANY_EXHDR(in), &detail);

	values[0] = CStringGetTextDatum(vfdw_script_verdict_name(v));
	if (detail != NULL)
		values[1] = CStringGetTextDatum(detail);
	else
		nulls[1] = true;

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* Every opcode the C side defines, by kind, number and name. */
Datum
valkey_fdw_test_opcodes(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			i;

	InitMaterializedSRF(fcinfo, 0);

	for (i = 0; i < vfdw_check_op_count(); i++)
	{
		Datum		v[3];
		bool		n[3] = {false, false, false};

		v[0] = CStringGetTextDatum("check");
		v[1] = Int32GetDatum(i);
		v[2] = CStringGetTextDatum(vfdw_ledger_check_name(i));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, n);
	}
	for (i = 0; i < vfdw_action_op_count(); i++)
	{
		Datum		v[3];
		bool		n[3] = {false, false, false};

		v[0] = CStringGetTextDatum("action");
		v[1] = Int32GetDatum(i);
		v[2] = CStringGetTextDatum(vfdw_ledger_action_name(i));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, n);
	}

	return (Datum) 0;
}

/*
 * valkey_fdw_test_script_program() -> the encoded EVALSHA, one row per element
 *
 * The golden vector. The encoder is the only writer of this format and the Lua
 * program the only reader, so a change to either that the other does not match
 * produces a runtime BADPROTO from inside a commit - the worst place to
 * discover it. Recorded here, the same change is a diff.
 *
 * The SHA is rendered as a fixed placeholder rather than as itself: it moves
 * whenever the script text changes, including for a comment, and a golden
 * vector that churns on every edit stops being read. script.sql asserts the
 * real digest separately, against the server.
 */
PG_FUNCTION_INFO_V1(valkey_fdw_test_script_program);

Datum
valkey_fdw_test_script_program(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext scratch;
	VfdwCmd		cmd;
	int			i;

	InitMaterializedSRF(fcinfo, 0);

	scratch = AllocSetContextCreate(CurrentMemoryContext,
									"valkey_fdw script program probe",
									ALLOCSET_SMALL_SIZES);
	vfdw_cmd_init(&cmd, scratch, 32);
	vfdw_script_encode(&cmd, scratch);

	for (i = 0; i < cmd.argc; i++)
	{
		Datum		v[3];
		bool		n[3] = {false, false, false};

		v[0] = Int32GetDatum(i);
		v[1] = CStringGetTextDatum(i == 1 ? "<sha1>" : "arg");
		if (i == 1)
			v[2] = PointerGetDatum(vfdw_testwbuf_bytea("<sha1>", 6));
		else
			v[2] = PointerGetDatum(vfdw_testwbuf_bytea(cmd.argv[i],
													   cmd.arglens[i]));

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, v, n);
	}

	MemoryContextDelete(scratch);
	return (Datum) 0;
}
