/*-------------------------------------------------------------------------
 *
 * vfdw_plan.c
 *		Choosing how a scan will reach Valkey.
 *
 * Split out from vfdw_scan.c: deciding the access path is planner work, done
 * once per query, and it shares nothing with the executor's page and reply
 * bookkeeping. The two meet only through what is encoded in the plan's
 * fdw_private.
 *
 * Every narrowing decided here is an access-path choice and nothing more.
 * The restriction clauses stay in the plan, so a mistake in this file can
 * cost a round trip but cannot let through a row the query did not ask for -
 * with one exception, which is why scoping exists below. A table confined to
 * a keyprefix must never fetch a key outside it, because the key column would
 * then be filled with a name that satisfies the very recheck meant to
 * exclude it.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_plan.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_scan.h"

#include "vfdw_filter.h"
#include "vfdw_knn.h"

#include "catalog/pg_operator_d.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/restrictinfo.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

/*
 * Above this many keys a fetch list stops being a shortcut: walking the
 * keyspace once beats pipelining an unbounded number of point reads. The
 * bound also keeps the duplicate check from dominating planning.
 */
#define VFDW_MAX_KEY_LIST 1000

/*
 * Is this the key column of the relation being planned?
 *
 * Written the obvious way, this check indexes the tuple descriptor with an
 * unvalidated varattno, so a whole-row or system Var reads out of bounds, and
 * which relation the Var belongs to is never checked at all.
 *
 * The IsA(node, Var) test also excludes a domain-typed key column, because
 * the parser wraps such a Var in a RelabelType or CoerceToDomain. That is the
 * right answer - such a table plans as a keyspace scan and returns the right
 * row - but it is currently reached by accident rather than by rule. Anyone
 * widening this with strip_implicit_coercions() must first check that
 * vfdw_key_column_pushable still refuses what it should: a domain over bytea
 * is binary in both directions, and it would become reachable here.
 */
static bool
vfdw_is_key_var(RelOptInfo *baserel, VfdwTableMap *map, Node *node)
{
	Var		   *var;

	if (!IsA(node, Var))
		return false;
	var = (Var *) node;

	if (var->varno != (int) baserel->relid || var->varlevelsup != 0)
		return false;
	if (var->varattno <= 0 || var->varattno > map->natts)
		return false;

	return var->varattno == map->keyattno;
}

/* The type's own equality, rather than a hardcoded pg_proc OID. */
static bool
vfdw_is_equality(Oid opno, Oid vartype)
{
	TypeCacheEntry *typentry = lookup_type_cache(vartype, TYPECACHE_EQ_OPR);

	return typentry->eq_opr == opno;
}

/*
 * Under a non-deterministic collation two different byte strings compare
 * equal, so the key the constant names is not the only key that satisfies the
 * clause. Fetching just that one silently drops the others - and worse, the
 * answer then depends on whether the constant folded: 'WHERE k = ''NDQ2''' got
 * a point lookup and no rows while 'WHERE k = (SELECT ''NDQ2''::text)' got a
 * keyspace scan and the row. The LIKE path has refused this since it was
 * written; the equality path had the same hole and no guard.
 */
static bool
vfdw_collation_is_deterministic(Oid collid)
{
	return !OidIsValid(collid) || get_collation_isdeterministic(collid);
}

/*
 * May a key restriction on this table become a fetch of a named key at all?
 *
 * The pushdown replaces "walk the keyspace and keep the keys whose decoded
 * value equals the constant" with "fetch out(constant)". That is sound only
 * when out(constant) is the ONLY byte string the read path decodes to the
 * constant - and the read path decodes a key with the column's INPUT
 * function. So the requirement is out(in(K)) == K for every K the table might
 * hold, which is a property of the type, not of the constant.
 *
 * Two families fail it, and the second is the reason a round-trip check on
 * the constant is not the fix:
 *
 *	bytea. The read path does not use bytea_in at all - it stores the raw
 *	bytes verbatim - while a plan key rendered with byteaout is the 13-byte
 *	hex TEXT '\x6e756c3a6f6b'. The two disagree about which bytes the column
 *	names, so the lookup fetches a key that does not exist, or, on a keyprefix
 *	table, is dropped by vfdw_keys_refine and the scan ends with an empty key
 *	list and zero rows. Refusing the pushdown here sends the qual to the
 *	keyspace walk, where the executor compares bytea to bytea and gets it
 *	right. Teaching the pushdown to carry (bytes, len) through fdw_private -
 *	which also means fdw_private can no longer hold keys as String, since a
 *	bytea key may contain NUL, and vfdw_scan_set_keys can no longer take their
 *	length with strlen - is an optimisation for later, deliberately not done
 *	here.
 *
 *	Types whose output canonicalises: int4, numeric, timestamp, json, bpchar.
 *	Key '007' with an int4 key column is returned by a full scan as k = 7, and
 *	'WHERE k = 7' would fetch the key named '7', which does not exist.
 *	Checking in(out(c)) == c does NOT catch this - in('7') is 7, so the check
 *	passes and the row is still lost. The condition is about keys the table
 *	holds, which plan time cannot enumerate, so the only sound test available
 *	is on the type.
 *
 * text and varchar are named explicitly rather than derived, because there is
 * no catalog property that means "the output function reproduces the input
 * bytes": typcategory 'S' includes bpchar, which pads. A domain over text
 * would qualify and cannot arrive here anyway - the parser wraps the Var in a
 * RelabelType, so vfdw_is_key_var already refuses it (see the note there).
 *
 * This is a whitelist, and src/vfdw_val.c argues at length against one. The
 * argument there is about RENDERING a Datum, where a whitelist would refuse
 * user-defined types for no reason and would be a second answer to a question
 * the type itself answers. This asks something different - whether a decoding
 * is injective - which the type system does not expose, and getting it wrong
 * loses rows rather than costing a round trip.
 */
static bool
vfdw_key_column_pushable(VfdwTableMap *map)
{
	const VfdwColumn *col;
	Oid			basetype;

	if (map->keyattno == InvalidAttrNumber || map->keyattno > map->natts)
		return false;

	col = &map->cols[map->keyattno - 1];

	/*
	 * The whitelist below is the whole gate. An explicit
	 * `if (col->is_binary) return false;` stood here and could not fail:
	 * is_binary means getBaseType(typid) == BYTEAOID (src/vfdw_map.c), and the
	 * return admits only TEXTOID and VARCHAROID, so bytea was already refused
	 * one line later. Removing it changed no suite's output - which is the
	 * problem with it, not a licence to leave it: a guard in the qual path
	 * that cannot change an answer reads as protection and provides none.
	 *
	 * The gate that remains does bite. Adding BYTEAOID to the return turns two
	 * of scan.sql's bytea vectors from a keyspace scan into a 0-key fetch and
	 * loses their rows.
	 */
	basetype = getBaseType(col->typid);
	return basetype == TEXTOID || basetype == VARCHAROID;
}

/* Render a datum as the key name it stands for. */
static String *
vfdw_key_from_datum(Oid typid, Datum value)
{
	Oid			outfunc;
	bool		isvarlena;

	getTypeOutputInfo(typid, &outfunc, &isvarlena);
	return makeString(OidOutputFunctionCall(outfunc, value));
}

/*
 * "key = 'literal'", in either argument order.
 */
static List *
vfdw_keys_from_opexpr(RelOptInfo *baserel, VfdwTableMap *map, OpExpr *op)
{
	Node	   *left;
	Node	   *right;
	Const	   *cnst;

	if (list_length(op->args) != 2)
		return NIL;

	left = (Node *) linitial(op->args);
	right = (Node *) lsecond(op->args);

	/* The commuted form restricts just as much, so accept it too. */
	if (IsA(right, Var) && IsA(left, Const))
	{
		Node	   *tmp = left;

		left = right;
		right = tmp;
	}

	if (!vfdw_is_key_var(baserel, map, left) || !IsA(right, Const))
		return NIL;

	cnst = (Const *) right;
	if (cnst->constisnull || cnst->consttype != ((Var *) left)->vartype)
		return NIL;
	if (!vfdw_is_equality(op->opno, cnst->consttype))
		return NIL;
	if (!vfdw_collation_is_deterministic(op->inputcollid))
		return NIL;

	return list_make1(vfdw_key_from_datum(cnst->consttype, cnst->constvalue));
}

/*
 * "key IN (...)", which the parser hands over as "key = ANY (array)".
 *
 * VFDW_MAX_KEY_LIST is applied to the array's own declared element count and
 * not to deconstruct_array's output. That call allocates a Datum and a bool
 * for every element and copies each one out, so a bound tested afterwards has
 * the planner materialise an IN list of a million constants for the sole
 * purpose of throwing it away - paying the whole cost of the shortcut in
 * order to refuse it. The array header already carries the answer.
 */
static List *
vfdw_keys_from_saop(RelOptInfo *baserel, VfdwTableMap *map,
					ScalarArrayOpExpr *saop)
{
	Const	   *cnst;
	ArrayType  *arr;
	Oid			elemtype;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	Datum	   *values;
	bool	   *nulls;
	int			nelems;
	int			i;
	List	   *keys = NIL;

	/*
	 * ANY is a union of equalities and so becomes a list of keys. ALL is a
	 * conjunction, which one key satisfies only in degenerate cases not worth
	 * recognising.
	 */
	if (!saop->useOr || list_length(saop->args) != 2)
		return NIL;
	if (!vfdw_is_key_var(baserel, map, (Node *) linitial(saop->args)))
		return NIL;
	if (!IsA(lsecond(saop->args), Const))
		return NIL;

	cnst = (Const *) lsecond(saop->args);
	if (cnst->constisnull)
		return NIL;

	elemtype = get_element_type(cnst->consttype);
	if (!OidIsValid(elemtype) ||
		elemtype != ((Var *) linitial(saop->args))->vartype)
		return NIL;
	if (!vfdw_is_equality(saop->opno, elemtype))
		return NIL;
	if (!vfdw_collation_is_deterministic(saop->inputcollid))
		return NIL;

	arr = DatumGetArrayTypeP(cnst->constvalue);
	if (ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr)) > VFDW_MAX_KEY_LIST)
		return NIL;

	get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, elemtype, elmlen, elmbyval, elmalign,
					  &values, &nulls, &nelems);

	for (i = 0; i < nelems; i++)
	{
		/* "key = NULL" is never true, so a NULL element names nothing. */
		if (nulls[i])
			continue;
		keys = lappend(keys, vfdw_key_from_datum(elemtype, values[i]));
	}

	return keys;
}

/*
 * Drop repeats, and drop keys the table does not contain.
 *
 * Both are correctness, not speed. "IN ('a','a')" names one row, so fetching
 * twice would duplicate it. A key outside the table's keyprefix is not in the
 * table at all, and fetching it would produce a row whose key column then
 * satisfies the very clause that named it.
 *
 * strncmp and strcmp on the RENDERED spelling are correct here only because
 * vfdw_key_column_pushable has already refused every key column whose
 * rendering is not the key's own bytes. Its write-side counterpart,
 * vfdw_val_check_prefix, compares raw bytes with memcmp under a length guard,
 * and must: it sees keys from a bytea column, which never reach this
 * function. If the pushdown ever carries (bytes, len), this has to become the
 * same memcmp - as it stands it would silently drop every hex-rendered key
 * and leave the scan with an empty list, which is zero rows and no error.
 */
static List *
vfdw_keys_refine(VfdwTableMap *map, List *keys)
{
	List	   *result = NIL;
	size_t		prefixlen = map->keyprefix != NULL ? strlen(map->keyprefix) : 0;
	ListCell   *lc;

	foreach(lc, keys)
	{
		String	   *key = (String *) lfirst(lc);
		const char *str = strVal(key);
		bool		seen = false;
		ListCell   *lc2;

		if (prefixlen > 0 && strncmp(str, map->keyprefix, prefixlen) != 0)
			continue;

		foreach(lc2, result)
		{
			if (strcmp(strVal((String *) lfirst(lc2)), str) == 0)
			{
				seen = true;
				break;
			}
		}
		if (!seen)
			result = lappend(result, key);
	}

	return result;
}

/*
 * The first key restriction among the clauses, if any.
 *
 * Only the first is used. A second restriction on the same column would have
 * to be intersected with this one to narrow it further, and it stays in the
 * plan as a filter regardless, so ignoring it costs at most some reads.
 */
static bool
vfdw_plan_find_keys(RelOptInfo *baserel, VfdwTableMap *map, List *clauses,
					List **keys)
{
	ListCell   *lc;

	if (!vfdw_key_column_pushable(map))
		return false;

	foreach(lc, clauses)
	{
		Node	   *clause = (Node *) lfirst(lc);
		List	   *found = NIL;

		if (IsA(clause, RestrictInfo))
			clause = (Node *) ((RestrictInfo *) clause)->clause;

		if (IsA(clause, OpExpr))
			found = vfdw_keys_from_opexpr(baserel, map, (OpExpr *) clause);
		else if (IsA(clause, ScalarArrayOpExpr))
			found = vfdw_keys_from_saop(baserel, map,
										(ScalarArrayOpExpr *) clause);

		if (found != NIL)
		{
			*keys = vfdw_keys_refine(map, found);
			return true;
		}
	}

	return false;
}

/*
 * The literal prefix a LIKE pattern requires, or NULL when it has none.
 *
 * Stops at the first wildcard. A backslash escapes the character after it,
 * which is also how a custom ESCAPE clause arrives: the parser rewrites
 * "LIKE p ESCAPE e" into like_escape(p, e), and folding that constant yields
 * a pattern in the backslash convention.
 */
static char *
vfdw_like_prefix(const char *patt, int len)
{
	StringInfoData buf;
	int			i;

	initStringInfo(&buf);
	for (i = 0; i < len; i++)
	{
		if (patt[i] == '%' || patt[i] == '_')
			break;
		if (patt[i] == '\\' && ++i >= len)
			break;				/* trailing backslash: nothing follows it */
		appendStringInfoChar(&buf, patt[i]);
	}

	if (buf.len == 0)
	{
		pfree(buf.data);
		return NULL;
	}
	return buf.data;
}

/*
 * "key LIKE 'literal%'", narrowed to the cases where a byte prefix means what
 * it appears to mean.
 *
 * ILIKE is excluded because its prefix is not a byte prefix, and so is a
 * non-deterministic collation, under which two different byte strings can
 * compare equal. Matching the operator by OID is what PostgreSQL's own index
 * machinery does for LIKE; there is no type-cache entry to ask instead.
 *
 * vfdw_key_column_pushable is consulted here as well as on the equality path,
 * so the rule about which key columns a restriction may narrow is stated
 * once. It cannot currently fire here - OID_TEXT_LIKE_OP over a bare Var
 * means the column is already text - but a narrowed MATCH built from a
 * canonicalised rendering would lose rows in exactly the same way, and one
 * rule in one place is what keeps the two paths from drifting.
 */
static char *
vfdw_plan_find_like_prefix(RelOptInfo *baserel, VfdwTableMap *map,
						   List *clauses)
{
	ListCell   *lc;

	if (!vfdw_key_column_pushable(map))
		return NULL;

	foreach(lc, clauses)
	{
		Node	   *clause = (Node *) lfirst(lc);
		OpExpr	   *op;
		Const	   *cnst;
		text	   *patt;
		char	   *prefix;

		if (IsA(clause, RestrictInfo))
			clause = (Node *) ((RestrictInfo *) clause)->clause;

		if (!IsA(clause, OpExpr))
			continue;
		op = (OpExpr *) clause;

		if (op->opno != OID_TEXT_LIKE_OP || list_length(op->args) != 2)
			continue;
		if (!vfdw_is_key_var(baserel, map, (Node *) linitial(op->args)))
			continue;
		if (!IsA(lsecond(op->args), Const))
			continue;

		cnst = (Const *) lsecond(op->args);
		if (cnst->constisnull)
			continue;
		if (!vfdw_collation_is_deterministic(op->inputcollid))
			continue;

		patt = DatumGetTextPP(cnst->constvalue);
		prefix = vfdw_like_prefix(VARDATA_ANY(patt), VARSIZE_ANY_EXHDR(patt));
		if (prefix != NULL)
			return prefix;
	}

	return NULL;
}

/*
 * Escape the Valkey pattern metacharacters in a literal.
 *
 * A keyprefix is a literal, not a pattern, so a prefix containing '*' or '['
 * must match only itself. Without this a table scoped to "a[b]:" would match
 * an entirely different set of keys than its definition says.
 */
static char *
vfdw_escape_glob(const char *str, size_t len, size_t *outlen)
{
	char	   *result = palloc(len * 2 + 1);
	char	   *dst = result;
	size_t		i;

	for (i = 0; i < len; i++)
	{
		char		c = str[i];

		if (c == '*' || c == '?' || c == '[' || c == ']' || c == '\\')
			*dst++ = '\\';
		*dst++ = c;
	}
	*dst = '\0';
	*outlen = (size_t) (dst - result);
	return result;
}

char *
vfdw_plan_scan_pattern(VfdwTableMap *map, const char *like_prefix)
{
	const char *prefix = map->keyprefix;
	StringInfoData buf;
	char	   *escaped;
	size_t		esclen;

	/* SSCAN walks a set, not the keyspace, and accepts no MATCH. */
	if (map->keyset != NULL)
		return NULL;

	/*
	 * Both restrictions have to hold, so the longer wins - but only when one
	 * genuinely extends the other. Prefixes that diverge can be satisfied by
	 * no key at all; keeping the table's own prefix leaves that impossibility
	 * to the recheck rather than encoding it in a pattern.
	 */
	if (like_prefix != NULL &&
		(prefix == NULL || strncmp(like_prefix, prefix, strlen(prefix)) == 0))
		prefix = like_prefix;

	if (prefix == NULL)
		return NULL;

	escaped = vfdw_escape_glob(prefix, strlen(prefix), &esclen);
	initStringInfo(&buf);
	appendBinaryStringInfo(&buf, escaped, (int) esclen);
	appendStringInfoChar(&buf, '*');
	return buf.data;
}

/*
 * Wrap a pattern for fdw_private, which holds Nodes and not strings.
 */
static Node *
vfdw_pattern_node(char *pattern)
{
	return pattern != NULL ? (Node *) makeString(pattern) : NULL;
}

/*
 * The three trailing slots every plan carries, filled only by a search.
 *
 * Written by one function rather than at each return, because the slots are
 * read by fixed position: a strategy that returned a three-element list would
 * make vfdw_plan_knn_field read past the end of it, and a strategy that put
 * them in a different order would read the k as a field name.
 */
static List *
vfdw_plan_private(VfdwScanStrategy strategy, List *keys, Node *pattern,
				  const VfdwKnnPlan *knn, List *filter)
{
	List	   *priv;

	/* list_make5 is as wide as the macros go; the rest are appended. */
	priv = list_make5(makeInteger((int) strategy),
					  keys,
					  pattern,
					  knn != NULL ? (Node *) makeString(pstrdup(knn->field)) : NULL,
					  knn != NULL ? (Node *) makeString(pstrdup(knn->metric)) : NULL);

	priv = lappend(priv, makeInteger(knn != NULL ? knn->k : 0));
	return lappend(priv, filter);
}

/*
 * A vector table's whole access-path choice: the search, or a refusal.
 *
 * There is no keyspace to walk, no key to look up, and no qual that merely
 * narrows - the WHERE is part of the search or the query is refused.
 *
 * Matched a second time here, having already been matched in GetForeignPaths
 * to decide whether the path could claim an ordering. The matcher reads root,
 * the relation and the map and nothing else, and none of the three change
 * between the two callbacks, so the two calls cannot disagree. The alternative
 * - carrying the result on the path - would put a non-Node struct in a field
 * the planner copies.
 */
static List *
vfdw_plan_search(PlannerInfo *root, RelOptInfo *baserel, VfdwTableMap *map,
				 List **fdw_exprs)
{
	VfdwKnnPlan knn;
	List	   *filter;

	if (!vfdw_knn_match(root, baserel, map, &knn))
		vfdw_knn_refuse(root, baserel, map);

	/*
	 * The query vector FIRST and the filter bounds after it, because the
	 * executor pairs them back by position: element zero is the vector, and
	 * what follows lines up with the encoded terms.
	 */
	*fdw_exprs = list_make1(knn.qvec);
	filter = vfdw_filter_encode(knn.filter, fdw_exprs);

	return vfdw_plan_private(VFDW_SCAN_KNN, NIL, NULL, &knn, filter);
}

List *
vfdw_scan_plan(PlannerInfo *root, RelOptInfo *baserel, VfdwTableMap *map,
			   List *clauses, List **fdw_exprs)
{
	List	   *keys = NIL;

	*fdw_exprs = NIL;

	if (map->tabletype == VFDW_TABLE_VECTOR)
		return vfdw_plan_search(root, baserel, map, fdw_exprs);

	/*
	 * A singleton table already names its key, and no qual may displace it.
	 * Letting "key = 'other'" through would read some other key and then fill
	 * the key column with the value the recheck tests against, so rows from
	 * the wrong key would pass a filter meant to exclude them.
	 */
	if (map->singleton_key != NULL)
		return vfdw_plan_private(VFDW_SCAN_SINGLETON,
								 list_make1(makeString(pstrdup(map->singleton_key))),
								 NULL, NULL, NIL);

	/*
	 * Whether a named key belongs to a keyset table is a question only the
	 * server can answer - which is true, and used to be the reason no key list
	 * was built for one. The conclusion was the wrong one: the server answers
	 * it with SISMEMBER, in constant time, and the scan asks that alongside the
	 * value fetch it was already going to send.
	 *
	 * So a keyset table takes the key list like any other, and pays one small
	 * extra reply per named key instead of walking a set whose size the table
	 * does not bound.
	 */
	if (vfdw_plan_find_keys(baserel, map, clauses, &keys))
		return vfdw_plan_private(VFDW_SCAN_KEYS, keys, NULL, NULL, NIL);

	return vfdw_plan_private(VFDW_SCAN_KEYSPACE, NIL,
							 vfdw_pattern_node(vfdw_plan_scan_pattern(map,
																	  vfdw_plan_find_like_prefix(baserel, map, clauses))),
							 NULL, NIL);
}

List *
vfdw_plan_keys(List *fdw_private)
{
	return (List *) list_nth(fdw_private, VFDW_PRIV_KEYS);
}

const char *
vfdw_plan_pattern(List *fdw_private)
{
	Node	   *node = (Node *) list_nth(fdw_private, VFDW_PRIV_PATTERN);

	return node != NULL ? strVal((String *) node) : NULL;
}

/*
 * A String slot, or NULL when this plan did not fill it.
 *
 * The NULL is what a non-search plan stores, and it is checked rather than
 * assumed: EXPLAIN of any plan reads these, so a keyspace scan must get NULL
 * back rather than strVal on a null pointer.
 */
static const char *
vfdw_plan_string_at(List *fdw_private, int slot)
{
	Node	   *node = (Node *) list_nth(fdw_private, slot);

	return node != NULL ? strVal((String *) node) : NULL;
}

const char *
vfdw_plan_knn_field(List *fdw_private)
{
	return vfdw_plan_string_at(fdw_private, VFDW_PRIV_KNN_FIELD);
}

const char *
vfdw_plan_knn_metric(List *fdw_private)
{
	return vfdw_plan_string_at(fdw_private, VFDW_PRIV_KNN_METRIC);
}

int
vfdw_plan_knn_k(List *fdw_private)
{
	return intVal(list_nth(fdw_private, VFDW_PRIV_KNN_K));
}

List *
vfdw_plan_knn_filter(List *fdw_private)
{
	return (List *) list_nth(fdw_private, VFDW_PRIV_KNN_FILTER);
}

const char *
vfdw_scan_strategy_name(List *fdw_private)
{
	if (fdw_private == NIL)
		return "unknown";

	switch ((VfdwScanStrategy) intVal(list_nth(fdw_private, VFDW_PRIV_STRATEGY)))
	{
		case VFDW_SCAN_KEYS:
			return list_length(vfdw_plan_keys(fdw_private)) == 1
				? "Key Lookup" : "Key List";
		case VFDW_SCAN_SINGLETON:
			return "Singleton Key";
		case VFDW_SCAN_KNN:
			return "Vector KNN";
		case VFDW_SCAN_KEYSPACE:
			break;
	}
	return "Keyspace Scan";
}
