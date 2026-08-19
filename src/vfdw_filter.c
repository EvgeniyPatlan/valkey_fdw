/*-------------------------------------------------------------------------
 *
 * vfdw_filter.c
 *		A WHERE clause compiled into valkey-search's query language, or not
 *		accepted at all.
 *
 * THE RULE, AND WHY IT IS ABSOLUTE. valkey-search applies a filter BEFORE the
 * nearest-neighbour search - the 6.1 spike measured it, and vfilter.sql keeps
 * the measurement - so the k rows a search returns are the k nearest of the
 * FILTERED set. A clause this file cannot compile must therefore make the
 * whole query fail, because applying it above the scan instead subtracts rows
 * from a set that was already cut to k: the answer is fewer than k rows, and
 * they are not the k nearest matching ones.
 *
 * The clauses stay in the plan as rechecks, exactly as they do for the
 * ordinary key path. That is safe here only because every compiled term means
 * EXACTLY what its clause means; a term that were merely close - a superset -
 * would make the recheck do the subtracting. Every rule below exists to keep
 * that exactness, and each one was measured rather than reasoned about.
 *
 * WHY NUMERIC AND NOT TAG. `@n:[lo hi]` is exactly a pair of comparisons: the
 * ends are inclusive, `(` makes either end exclusive, the infinities are
 * spellable, and `[1 1]` is equality. A TAG field is tokenised on its
 * separator, so a document whose field holds "a,b" is indexed as two tags and
 * `@tg:{a}` matches it - while SQL `tg = 'a'` compares the whole field and
 * does not. That filter is a superset, which is the direction that cannot be
 * recovered from, so tag equality is refused. The sound fix is to over-fetch
 * and re-rank until k rows survive the recheck, which needs the quals
 * evaluated inside the scan and is a different algorithm.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_filter.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_filter.h"

#include <math.h>

#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/lsyscache.h"

bool
vfdw_filter_name_is_safe(const char *name)
{
	const char *p;

	if (name == NULL || *name == '\0')
		return false;

	/*
	 * A conservative set, not an escape function. valkey-search's query
	 * grammar is not this tree's to model, and a name that needs escaping is
	 * a name a user can change - where a wrong escape is a query that asks
	 * something else and says nothing about it.
	 */
	for (p = name; *p != '\0'; p++)
	{
		if (*p >= 'a' && *p <= 'z')
			continue;
		if (*p >= 'A' && *p <= 'Z')
			continue;
		if (*p >= '0' && *p <= '9')
			continue;
		if (*p == '_' || *p == '-' || *p == '.')
			continue;
		return false;
	}

	return true;
}

/*
 * Is this column type one whose values survive being a valkey-search NUMERIC?
 *
 * A NUMERIC attribute is a double. int2, int4, float4 and float8 all convert
 * to one and back without loss, so a comparison the index makes is the
 * comparison SQL would make.
 *
 * int8 and numeric are REFUSED, and not out of caution. A bigint beyond 2^53
 * is rounded on the way into the index, so two distinct values become one: a
 * row the qual excludes can match the filter, the recheck drops it, and the
 * answer is short by one. The bound being small does not help, because it is
 * the stored values that are rounded.
 */
static bool
vfdw_filter_type_is_exact(Oid typid)
{
	switch (getBaseType(typid))
	{
		case INT2OID:
		case INT4OID:
		case FLOAT4OID:
		case FLOAT8OID:
			return true;
		default:
			return false;
	}
}

/*
 * The numeric field this Var names, or NULL.
 *
 * Bounds-checked before the array is indexed, for the reason vfdw_plan.c's key
 * check gives: a whole-row or system Var carries an attnum that is zero or
 * negative.
 */
static const char *
vfdw_filter_numeric_field(Node *node, RelOptInfo *baserel, VfdwTableMap *map)
{
	Var		   *var;
	VfdwColumn *col;

	node = (Node *) strip_implicit_coercions((Node *) node);

	if (node == NULL || !IsA(node, Var))
		return NULL;

	var = (Var *) node;
	if (var->varno != (int) baserel->relid || var->varlevelsup != 0)
		return NULL;
	if (var->varattno < 1 || var->varattno > map->natts)
		return NULL;

	col = &map->cols[var->varattno - 1];
	if (col->kind != VFDW_COL_FIELD || col->index_type != VFDW_INDEX_NUMERIC)
		return NULL;
	if (!vfdw_filter_type_is_exact(col->typid))
		return NULL;
	if (!vfdw_filter_name_is_safe(col->field))
		return NULL;

	return col->field;
}

/*
 * The tag field this Var names, or NULL.
 *
 * Unlike the numeric one, the COLUMN's type is not constrained: a tag is
 * compared as text by the index whatever PostgreSQL calls it, and the
 * comparison this pushes is deliberately inexact anyway.
 */
static const char *
vfdw_filter_tag_field(Node *node, RelOptInfo *baserel, VfdwTableMap *map)
{
	Var		   *var;
	VfdwColumn *col;

	node = (Node *) strip_implicit_coercions((Node *) node);

	if (node == NULL || !IsA(node, Var))
		return NULL;

	var = (Var *) node;
	if (var->varno != (int) baserel->relid || var->varlevelsup != 0)
		return NULL;
	if (var->varattno < 1 || var->varattno > map->natts)
		return NULL;

	col = &map->cols[var->varattno - 1];
	if (col->kind != VFDW_COL_FIELD || col->index_type != VFDW_INDEX_TAG)
		return NULL;
	if (!vfdw_filter_name_is_safe(col->field))
		return NULL;

	return col->field;
}

/*
 * Which end of a range this operator bounds, from its name.
 *
 * By NAME, for the reason vfdw_knn.c recognises its own operators that way:
 * these are catalog OIDs. The five are the ones a range can express exactly;
 * <> cannot, because the query language's negation would have to exclude a
 * point and the spike did not establish that it can.
 */
/* Is this operator "=", by name? See vfdw_filter_op_range for why by name. */
static bool
vfdw_filter_op_is_equality(Oid opno)
{
	char	   *name = get_opname(opno);
	bool		ok;

	if (name == NULL)
		return false;

	ok = strcmp(name, "=") == 0;
	pfree(name);
	return ok;
}

static bool
vfdw_filter_op_range(Oid opno, bool var_on_left, VfdwFilterTerm *term)
{
	char	   *name = get_opname(opno);
	bool		ok = true;

	if (name == NULL)
		return false;

	term->both = false;
	term->exclusive = false;

	if (strcmp(name, "=") == 0)
		term->both = true;
	else if (strcmp(name, "<") == 0)
	{
		term->lower = !var_on_left;
		term->exclusive = true;
	}
	else if (strcmp(name, "<=") == 0)
		term->lower = !var_on_left;
	else if (strcmp(name, ">") == 0)
	{
		term->lower = var_on_left;
		term->exclusive = true;
	}
	else if (strcmp(name, ">=") == 0)
		term->lower = var_on_left;
	else
		ok = false;

	pfree(name);
	return ok;
}

/*
 * A TAG equality, which is a superset of its clause rather than the clause.
 *
 * Equality only: a range over tags is not a thing the query language has, and
 * a tag is compared as text by the index whatever PostgreSQL calls the column.
 * See VfdwFilterTerm.tag for why a superset is sound here and was not before.
 */
static VfdwFilterTerm *
vfdw_filter_tag_term(PlannerInfo *root, RelOptInfo *baserel, VfdwTableMap *map,
					 OpExpr *op)
{
	VfdwFilterTerm *term;
	const char *field;
	Node	   *bound;

	field = vfdw_filter_tag_field(linitial(op->args), baserel, map);
	bound = (Node *) lsecond(op->args);

	if (field == NULL)
	{
		field = vfdw_filter_tag_field(lsecond(op->args), baserel, map);
		bound = (Node *) linitial(op->args);
	}
	if (field == NULL)
		return NULL;
	if (bms_is_member(baserel->relid, pull_varnos(root, bound)))
		return NULL;
	if (!vfdw_filter_op_is_equality(op->opno))
		return NULL;

	term = (VfdwFilterTerm *) palloc0(sizeof(VfdwFilterTerm));
	term->field = field;
	term->bound = (Expr *) bound;
	term->tag = true;
	return term;
}

/*
 * One clause, compiled or refused.
 *
 * The bound must not mention this relation. A row-dependent bound would be a
 * different filter per row, which one query string cannot express and which
 * nothing here would notice: it would build one filter from whatever the
 * expression evaluated to once.
 */
static VfdwFilterTerm *
vfdw_filter_one(PlannerInfo *root, RelOptInfo *baserel, VfdwTableMap *map,
				Expr *clause)
{
	VfdwFilterTerm *term;
	OpExpr	   *op;
	const char *field;
	Node	   *bound;
	bool		var_on_left = true;

	if (!IsA(clause, OpExpr))
		return NULL;

	op = (OpExpr *) clause;
	if (list_length(op->args) != 2 || op->opresulttype != BOOLOID)
		return NULL;

	field = vfdw_filter_numeric_field(linitial(op->args), baserel, map);
	bound = (Node *) lsecond(op->args);

	if (field == NULL)
	{
		field = vfdw_filter_numeric_field(lsecond(op->args), baserel, map);
		bound = (Node *) linitial(op->args);
		var_on_left = false;
	}

	if (field == NULL)
		return vfdw_filter_tag_term(root, baserel, map, op);

	if (bms_is_member(baserel->relid, pull_varnos(root, bound)))
		return NULL;
	if (!vfdw_filter_type_is_exact(exprType(bound)))
		return NULL;

	term = (VfdwFilterTerm *) palloc0(sizeof(VfdwFilterTerm));
	term->field = field;
	term->bound = (Expr *) bound;

	if (!vfdw_filter_op_range(op->opno, var_on_left, term))
		return NULL;

	return term;
}

List *
vfdw_filter_compile(PlannerInfo *root, RelOptInfo *baserel, VfdwTableMap *map,
					List *clauses, const char **why)
{
	List	   *terms = NIL;
	ListCell   *lc;

	*why = NULL;

	foreach(lc, clauses)
	{
		Expr	   *clause = (Expr *) lfirst(lc);
		VfdwFilterTerm *term;

		term = vfdw_filter_one(root, baserel, map, clause);
		if (term == NULL)
		{
			*why = "a WHERE clause that cannot be compiled into the search "
				"itself, and applying it afterwards would remove rows the "
				"search already counted towards k";
			return NULL;
		}

		terms = lappend(terms, term);
	}

	return terms;
}

/*
 * A bound as the query language spells it.
 *
 * Converted by type rather than through a function lookup, because the four
 * accepted types each convert to a double exactly and the conversion is one
 * line each. float8out then prints the digits PostgreSQL would print, so the
 * number the index compares against is the number the qual named.
 *
 * The infinities get the language's own spelling, which the spike confirmed
 * it accepts. NaN has none and needs none: a comparison against NaN is false
 * for every row, so the caller is told there are no rows rather than being
 * handed a filter that cannot mean that.
 */
static bool
vfdw_filter_bound_text(Datum value, Oid typid, StringInfo out)
{
	float8		f;

	switch (getBaseType(typid))
	{
		case INT2OID:
			f = (float8) DatumGetInt16(value);
			break;
		case INT4OID:
			f = (float8) DatumGetInt32(value);
			break;
		case FLOAT4OID:
			f = (float8) DatumGetFloat4(value);
			break;
		case FLOAT8OID:
			f = DatumGetFloat8(value);
			break;
		default:

			/*
			 * Unreachable: vfdw_filter_type_is_exact accepted this type at
			 * plan time, and it accepts exactly the four above. Loud rather
			 * than a default conversion, because a fifth type added to one
			 * list and not the other would otherwise be compared as whatever
			 * its Datum happens to look like.
			 */
			elog(ERROR, "valkey_fdw: filter bound of unexpected type %u",
				 typid);
	}

	if (isnan(f))
		return false;

	if (isinf(f))
		appendStringInfoString(out, f > 0 ? "+inf" : "-inf");
	else
		appendStringInfoString(out, DatumGetCString(
			DirectFunctionCall1(float8out, Float8GetDatum(f))));

	return true;
}

/*
 * A tag term: `@field:{value}`.
 *
 * THE VALUE GOES INTO THE QUERY STRING, which is the same surface a field name
 * is - and worse, because this one can come from a Param and therefore from a
 * user. A value carrying the query language's punctuation would add terms the
 * query never contained.
 *
 * Refused rather than escaped, for the reason the field-name check gives:
 * valkey-search's grammar is not this tree's to model. But refusing the QUERY
 * would be wrong here - the term is only an optimisation. A term that cannot
 * be spelled is simply NOT PUSHED, and the scan's own recheck still gives the
 * right rows; it just reads more of the index to find them.
 *
 * The separator is the same case. A value containing it can never match as one
 * tag - vfilter.sql measured `@tg:{a,b}` matching nothing at all - so pushing
 * it would return too few rows. Omitted for the same reason and with the same
 * outcome.
 *
 * Returns false only when the value is NULL, which makes the whole query
 * unsatisfiable and is the caller's cue to send nothing.
 */
static bool
vfdw_filter_render_tag(VfdwFilterTerm *term, Datum value, ExprState *state,
					   StringInfo out)
{
	Oid			typid = exprType((Node *) state->expr);
	Oid			outfunc;
	bool		varlena;
	char	   *text;
	const char *p;

	getTypeOutputInfo(typid, &outfunc, &varlena);
	text = OidOutputFunctionCall(outfunc, value);

	for (p = text; *p != '\0'; p++)
	{
		if (*p >= 'a' && *p <= 'z')
			continue;
		if (*p >= 'A' && *p <= 'Z')
			continue;
		if (*p >= '0' && *p <= '9')
			continue;
		if (*p == '_' || *p == '-' || *p == '.' || *p == ' ')
			continue;

		/* Not spellable. Not pushed; the recheck still answers. */
		pfree(text);
		return true;
	}

	appendStringInfo(out, "@%s:{%s} ", term->field, text);
	pfree(text);
	return true;
}

/*
 * A numeric term: `@field:[lo hi]`, with `(` for an exclusive end.
 */
static bool
vfdw_filter_render_range(VfdwFilterTerm *term, Datum value, ExprState *state,
						 StringInfo out)
{
	StringInfoData num;

	initStringInfo(&num);
	if (!vfdw_filter_bound_text(value, exprType((Node *) state->expr), &num))
		return false;

	/*
	 * Equality against an infinity is refused rather than spelled. Both ends
	 * would be the language's own "+inf", and whether that matches a field
	 * whose text is "inf" is a question the spike did not put - so it is not
	 * one this file answers by guessing.
	 */
	if (term->both && num.data[0] != '-' && num.data[1] == 'i')
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot search for a field equal to infinity"),
				 errdetail("The search's filter language spells the "
						   "infinities as range ends, and whether one matches "
						   "a stored value is not established.")));

	appendStringInfo(out, "@%s:[", term->field);

	if (term->both)
		appendStringInfo(out, "%s %s", num.data, num.data);
	else if (term->lower)
		appendStringInfo(out, "%s%s +inf",
						 term->exclusive ? "(" : "", num.data);
	else
		appendStringInfo(out, "-inf %s%s",
						 term->exclusive ? "(" : "", num.data);

	appendStringInfoString(out, "] ");
	pfree(num.data);
	return true;
}

bool
vfdw_filter_render(List *terms, ExprContext *econtext, List *bound_states,
				   StringInfo out)
{
	ListCell   *lc;
	ListCell   *sc;

	forboth(lc, terms, sc, bound_states)
	{
		VfdwFilterTerm *term = (VfdwFilterTerm *) lfirst(lc);
		ExprState  *state = (ExprState *) lfirst(sc);
		Datum		value;
		bool		isnull;

		value = ExecEvalExpr(state, econtext, &isnull);

		/*
		 * A NULL bound makes the comparison NULL for every row, so the query
		 * has no rows. Reported rather than rendered: there is no filter that
		 * means "nothing", and inventing an empty range would be a filter
		 * whose emptiness depended on the values in the index.
		 */
		if (isnull)
			return false;

		if (term->tag)
		{
			if (!vfdw_filter_render_tag(term, value, state, out))
				return false;
			continue;
		}

		if (!vfdw_filter_render_range(term, value, state, out))
			return false;
	}

	return true;
}

/*
 * The terms as Nodes, and their bounds as a list of expressions.
 *
 * Two lists rather than one, because they travel by different routes and have
 * to: a bound may be a Param, so it belongs in the plan's fdw_exprs where
 * setrefs.c renumbers it, while the field name and the range shape are data
 * and belong in fdw_private. Pairing them again on the other side is by
 * POSITION, which is why both are built by this one walk.
 */
List *
vfdw_filter_encode(List *terms, List **bounds)
{
	List	   *out = NIL;
	ListCell   *lc;

	foreach(lc, terms)
	{
		VfdwFilterTerm *term = (VfdwFilterTerm *) lfirst(lc);
		int			flags = 0;

		if (term->lower)
			flags |= VFDW_FILTER_LOWER;
		if (term->exclusive)
			flags |= VFDW_FILTER_EXCLUSIVE;
		if (term->both)
			flags |= VFDW_FILTER_BOTH;
		if (term->tag)
			flags |= VFDW_FILTER_TAG;

		out = lappend(out, list_make2(makeString(pstrdup(term->field)),
									  makeInteger(flags)));
		*bounds = lappend(*bounds, term->bound);
	}

	return out;
}

/*
 * The inverse, without the bounds: those arrive as compiled ExprStates and are
 * paired back on by position. The struct's bound field stays NULL here, and
 * vfdw_filter_render reads the type from the state's own expression rather
 * than from it - one source for that type, on the side that has the value.
 */
List *
vfdw_filter_decode(List *encoded)
{
	List	   *out = NIL;
	ListCell   *lc;

	foreach(lc, encoded)
	{
		List	   *pair = (List *) lfirst(lc);
		VfdwFilterTerm *term = (VfdwFilterTerm *) palloc0(sizeof(VfdwFilterTerm));
		int			flags = intVal(lsecond(pair));

		term->field = strVal((String *) linitial(pair));
		term->lower = (flags & VFDW_FILTER_LOWER) != 0;
		term->exclusive = (flags & VFDW_FILTER_EXCLUSIVE) != 0;
		term->both = (flags & VFDW_FILTER_BOTH) != 0;
		term->tag = (flags & VFDW_FILTER_TAG) != 0;

		out = lappend(out, term);
	}

	return out;
}

/*
 * The filter as EXPLAIN shows it, with the bounds written as `$`.
 *
 * The values are not plan-time facts - a bound may be a Param - so what a plan
 * can show is the SHAPE: which field, which end, and whether the end is
 * exclusive. That is what a reader has to check against their WHERE clause,
 * and it is the only place the pushdown is visible at all: a filter pushed
 * wrongly returns k rows in order and looks exactly like one pushed rightly.
 */
char *
vfdw_filter_describe(List *terms)
{
	StringInfoData buf;
	ListCell   *lc;

	initStringInfo(&buf);

	foreach(lc, terms)
	{
		VfdwFilterTerm *term = (VfdwFilterTerm *) lfirst(lc);

		if (buf.len > 0)
			appendStringInfoChar(&buf, ' ');

		if (term->tag)
			appendStringInfo(&buf, "@%s:{$}", term->field);
		else if (term->both)
			appendStringInfo(&buf, "@%s:[$ $]", term->field);
		else if (term->lower)
			appendStringInfo(&buf, "@%s:[%s$ +inf]", term->field,
							 term->exclusive ? "(" : "");
		else
			appendStringInfo(&buf, "@%s:[-inf %s$]", term->field,
							 term->exclusive ? "(" : "");
	}

	return buf.data;
}

bool
vfdw_filter_is_exact(List *terms)
{
	ListCell   *lc;

	foreach(lc, terms)
	{
		if (((VfdwFilterTerm *) lfirst(lc))->tag)
			return false;
	}

	return true;
}
