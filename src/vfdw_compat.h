/*-------------------------------------------------------------------------
 *
 * vfdw_compat.h
 *		What each supported PostgreSQL major calls the same thing.
 *
 * EVERY NAMED SHIM IS HERE, and deliberately not "wherever the compiler
 * complained". A version test scattered through the tree is a version test
 * nobody can audit: dropping a major later means finding every one of them,
 * and adding a major means discovering them one build error at a time. Here
 * the whole surface is a page, and a reader can see exactly how far the tree
 * reaches and why.
 *
 * TWO SHAPES CANNOT BE NAMED, so they stay where they are and are listed here
 * instead - which is the same audit, done by reading rather than by grep:
 *
 *   a conditional #include, because the header is what the version changed:
 *   storage/waiteventset.h in vfdw_io.c and commands/explain_format.h and
 *   explain_state.h in valkey_fdw.c and vfdw_modify.h, all PostgreSQL 18
 *   splits of headers that used to hold both halves;
 *
 *   an argument in the middle of a call, where a major added a parameter
 *   rather than changing a name: create_foreignscan_path gained
 *   fdw_restrictinfo in 17 and disabled_nodes in 18, both at both of its call
 *   sites in valkey_fdw.c.
 *
 * NOTHING HERE IS A GUESS. Every entry is a rename or a signature change made
 * by a specific PostgreSQL release, and each says which. Where a version
 * genuinely lacks a capability rather than spelling it differently, that is
 * not a shim and does not belong here - it belongs in a refusal.
 *
 * The range itself - floor, ceiling, and why each is where it is - is stated
 * once, at the version gate in vfdw.h. It is not repeated here, because two
 * accounts of one fact are two things to keep true.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_compat.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_COMPAT_H
#define VFDW_COMPAT_H

/*
 * varatt.h, PostgreSQL 16.
 *
 * The VARDATA_ANY family moved out of postgres.h into a header of its own.
 * Included by name rather than through utils/builtins.h for the reason
 * src/vfdw_val.c gives, so the include has to be conditional rather than the
 * usage.
 */
#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

/*
 * The headers the shims below declare against. Included here rather than left
 * to whoever includes this file: a shim that compiles only when its caller
 * happens to have included the right header first is a shim that breaks on
 * the next file to use it.
 */
#include "access/stratnum.h"
#include "miscadmin.h"
#include "parser/parse_relation.h"
#include "utils/sampling.h"

/*
 * object_aclcheck, PostgreSQL 16.
 *
 * The per-catalog aclcheck functions - pg_foreign_server_aclcheck and its
 * dozens of siblings - were replaced by one that takes the catalog OID.
 * Only the foreign-server one is used here, so only that mapping is written:
 * a general shim would be a general claim, and this file makes the narrowest
 * one that is true.
 *
 * Which makes the discarded classid a trap - a second caller passing some
 * other catalog would silently be answered about foreign servers, and be
 * right often enough not to notice. So it is asserted rather than documented:
 * on 16 and later the real function reads the argument, and below 16 a
 * cassert build fails on the spot. CI runs one.
 */
#if PG_VERSION_NUM < 160000
#include "catalog/pg_foreign_server.h"
#include "utils/acl.h"
#define object_aclcheck(classid, objid, roleid, mode) \
	(AssertMacro((classid) == ForeignServerRelationId), \
	 pg_foreign_server_aclcheck((objid), (roleid), (mode)))
#endif

/*
 * RTEPermissionInfo, PostgreSQL 16.
 *
 * Permission bookkeeping moved off RangeTblEntry into a list hanging from the
 * Query, so `rte->updatedCols` became a lookup. The shim is a function rather
 * than a macro because the two shapes need different arguments and a macro
 * that swallowed `root` would hide which version reads it.
 *
 * WHAT THIS IS FOR: the set of columns an UPDATE actually assigns. Getting it
 * wrong does not fail - it writes columns the statement never mentioned, or
 * misses ones it did.
 */
#if PG_VERSION_NUM < 160000
static inline Bitmapset *
vfdw_rte_updated_cols(PlannerInfo *root, RangeTblEntry *rte)
{
	(void) root;
	return rte->updatedCols;
}
#else
static inline Bitmapset *
vfdw_rte_updated_cols(PlannerInfo *root, RangeTblEntry *rte)
{
	RTEPermissionInfo *perminfo;

	if (rte->perminfoindex == 0)
		return NULL;

	perminfo = getRTEPermissionInfo(root->parse->rteperminfos, rte);
	return perminfo->updatedCols;
}
#endif

/*
 * Var.varno, PostgreSQL 16.
 *
 * It was an Index (unsigned) and became an int, so the one comparison this
 * tree makes - is this Var a plain reference to the relation being planned -
 * is a signedness mismatch on one side of 16 or the other. Both operands are
 * widened to int here: every value either side can hold is a small positive
 * range table index or one of the special varnos (INNER_VAR and friends, all
 * under 65536), so nothing is lost on any supported major.
 *
 * varlevelsup travels with it because the four call sites all asked both
 * questions, and a Var from an outer query that happens to share an index is
 * not a reference to this relation.
 */
static inline bool
vfdw_var_of_rel(const Var *var, Index relid)
{
	return (int) var->varno == (int) relid && var->varlevelsup == 0;
}

/*
 * sampler_random_fract, PostgreSQL 15.
 *
 * ReservoirStateData's randstate was an erand48 seed - unsigned short[3] -
 * and became a pg_prng_state. The argument is therefore an array that decays
 * on 14 and a struct that must be addressed on 15 and later; the difference
 * is invisible at the call site and caught only by a warning, which is why it
 * is spelled out here rather than left as a cast.
 */
static inline double
vfdw_sampler_fract(ReservoirState rstate)
{
#if PG_VERSION_NUM < 150000
	return sampler_random_fract(rstate->randstate);
#else
	return sampler_random_fract(&rstate->randstate);
#endif
}

/*
 * pg_attribute_noreturn, PostgreSQL 18.
 *
 * The trailing pg_attribute_noreturn() was replaced by a leading pg_noreturn,
 * so the attribute moved to the other end of the declaration. Declaring
 * through both spellings keeps one source working across the whole range.
 */
#if PG_VERSION_NUM >= 180000
#define VFDW_NORETURN		pg_noreturn
#define VFDW_NORETURN_TAIL
#else
#define VFDW_NORETURN
#define VFDW_NORETURN_TAIL	pg_attribute_noreturn()
#endif

/*
 * PathKey.pk_strategy, PostgreSQL 18.
 *
 * It became pk_cmptype, and the constant it is compared against changed with
 * it. Both spell the same two directions.
 */
#if PG_VERSION_NUM >= 180000
#define VFDW_PK_IS_ASC(pk)	((pk)->pk_cmptype == COMPARE_LT)
#else
#define VFDW_PK_IS_ASC(pk)	((pk)->pk_strategy == BTLessStrategyNumber)
#endif

/*
 * String, PostgreSQL 15.
 *
 * The Value node was split into String, Integer, Float and Boolean. T_String
 * is the tag in both, and makeString and strVal already work on either - what
 * changed is the struct's name, which this tree writes when it casts a list
 * element.
 */
#if PG_VERSION_NUM < 150000
typedef Value String;
#endif

/*
 * InitMaterializedSRF, PostgreSQL 15.
 *
 * A set-returning function in materialize mode used to spell out the same
 * dozen lines every time: check the call context, build a tuple descriptor,
 * switch memory contexts, create the tuplestore, hand it back through
 * ReturnSetInfo. The helper does exactly that, so the shim is the boilerplate
 * it replaced rather than an approximation of it.
 *
 * MAT_SRF_USE_EXPECTED_DESC is the only flag this tree passes, and the
 * expansion assumes it: a caller wanting the other behaviour would be writing
 * a different function, and would notice.
 */
#if PG_VERSION_NUM < 150000
#define MAT_SRF_USE_EXPECTED_DESC 0x01

static inline void
InitMaterializedSRF(FunctionCallInfo fcinfo, bits32 flags)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext old;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;

	(void) flags;

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
		(rsinfo->allowedModes & SFRM_Materialize) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in a context that cannot "
						"accept a set")));

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	old = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = CreateTupleDescCopy(tupdesc);
	MemoryContextSwitchTo(old);
}
#endif

#endif							/* VFDW_COMPAT_H */
