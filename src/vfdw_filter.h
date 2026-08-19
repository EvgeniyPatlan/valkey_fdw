/*-------------------------------------------------------------------------
 *
 * vfdw_filter.h
 *		A WHERE clause compiled into valkey-search's query language, or not
 *		accepted at all.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_filter.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_FILTER_H
#define VFDW_FILTER_H

#include "vfdw.h"

#include "lib/stringinfo.h"
#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"

#include "vfdw_map.h"

/*
 * One comparison, as a numeric range with one bound open.
 *
 * A range rather than an operator, because that is what the query language
 * has: `@f:[lo hi]`, with `(` marking a bound exclusive. Equality is the
 * degenerate range and needs no separate case, which the spike confirmed
 * rather than assumed - `@n:[1 1]` matches exactly the documents whose field
 * is 1.
 *
 * bound is the expression the comparison names. It is evaluated when the scan
 * runs, not now, for the same reason the query vector is: it may be a Param,
 * and a generic plan carries the expression rather than the value.
 */
typedef struct VfdwFilterTerm
{
	const char *field;			/* the indexed attribute */
	Expr	   *bound;			/* the value compared against */
	bool		lower;			/* bound is the LOW end of the range */
	bool		exclusive;		/* the bound itself does not match */
	bool		both;			/* equality: the bound is both ends */

	/*
	 * A TAG term rather than a numeric range, and INEXACT.
	 *
	 * A TAG field is tokenised on its separator, so `@tg:{a}` matches a
	 * document whose field holds "a,b" while SQL `tg = 'a'` does not. The
	 * filter is therefore a SUPERSET of the qual, and a superset is only
	 * sound if the scan rechecks and fetches more when too few rows survive -
	 * which is what src/vfdw_search.c's over-fetch loop is for.
	 *
	 * Measured, not assumed: test/regress/sql/vfilter.sql.
	 */
	bool		tag;
} VfdwFilterTerm;

/*
 * Compile every restriction clause on this relation, or fail.
 *
 * Returns NULL when any clause cannot be compiled - and the caller must then
 * refuse the whole query rather than let the executor apply what is left.
 * THAT IS THE WHOLE POINT OF THIS FILE: a filter applied only above the scan
 * removes rows from a set the server already reduced to k, so the answer is
 * fewer than k rows and not the k nearest matching ones.
 *
 * The compiled terms are kept as a List of VfdwFilterTerm, and the clauses
 * they came from stay in the plan as rechecks. Rechecking is safe here
 * precisely because each term means exactly what its clause means; a term that
 * were merely close would make the recheck subtract rows.
 *
 * why is set to the reason nothing was compiled, for the refusal message.
 */
extern List *vfdw_filter_compile(PlannerInfo *root, RelOptInfo *baserel,
								 VfdwTableMap *map, List *clauses,
								 const char **why);

/* True when every compiled term means EXACTLY what its clause means. */
extern bool vfdw_filter_is_exact(List *terms);

/*
 * Is this name safe to put in a query string?
 *
 * A field name is part of the query, so a name carrying the language's own
 * punctuation changes what is asked - `@num:[1 1] @tg:{a}` written as one name
 * is accepted by the server and silently adds a filter. Measured in
 * test/regress/sql/vfilter.sql.
 */
extern bool vfdw_filter_name_is_safe(const char *name);

/*
 * Render the terms into a query prefix, evaluating each bound.
 *
 * Returns false when a bound evaluates to NULL, which means the comparison is
 * NULL for every row and the query has no rows at all - so the caller skips
 * the search rather than sending a filter that cannot be written.
 */
extern bool vfdw_filter_render(List *terms, ExprContext *econtext,
							   List *bound_states, StringInfo out);

/*
 * The range shape, as it travels in the plan.
 *
 * A bitmask rather than three Integer nodes, because the three are read
 * together or not at all and a plan that carried two of them would be a plan
 * with half a range in it.
 */
#define VFDW_FILTER_LOWER		0x01
#define VFDW_FILTER_EXCLUSIVE	0x02
#define VFDW_FILTER_BOTH		0x04
#define VFDW_FILTER_TAG			0x08

/*
 * Split the terms into what fdw_private carries and what fdw_exprs carries,
 * and put them back together.
 *
 * The two halves are paired by POSITION on the way back, so both are built by
 * one walk in one order - the same reason a ttl column's reply slot is
 * assigned where the ttl columns are counted.
 */
extern List *vfdw_filter_encode(List *terms, List **bounds);
extern List *vfdw_filter_decode(List *encoded);

/* The filter's shape, for EXPLAIN, with the bounds written as `$`. */
extern char *vfdw_filter_describe(List *terms);

#endif							/* VFDW_FILTER_H */
