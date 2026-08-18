/*-------------------------------------------------------------------------
 *
 * vfdw_scan.h
 *		Reading rows out of Valkey.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_scan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_SCAN_H
#define VFDW_SCAN_H

#include "vfdw.h"

#include "foreign/fdwapi.h"
#include "nodes/execnodes.h"

#include "vfdw_conn.h"

#include "vfdw_map.h"

extern void vfdw_scan_begin(ForeignScanState *node, int eflags);
extern TupleTableSlot *vfdw_scan_next(ForeignScanState *node);
extern void vfdw_scan_rescan(ForeignScanState *node);
extern void vfdw_scan_end(ForeignScanState *node);

/*
 * How the scan finds the keys it reads.
 *
 * The choice is made once, by the planner, and travels to the executor in the
 * plan's fdw_private. Deciding it in BeginForeignScan instead would leave
 * EXPLAIN with nothing to report, since a plan that is never executed never
 * reaches that callback.
 */
typedef enum VfdwScanStrategy
{
	VFDW_SCAN_KEYSPACE = 0,		/* SCAN, or SSCAN over a keyset */
	VFDW_SCAN_KEYS,				/* a known list of keys: fetch just those */
	VFDW_SCAN_SINGLETON,		/* singleton_key: one fixed key */

	/*
	 * FT.SEARCH: the rows come from an index, in the order it ranked them.
	 *
	 * The odd one out, and deliberately so. Every other strategy discovers
	 * keys and then reads them, so its rows are a SET that quals may narrow.
	 * This one produces a ranked LIST of exactly k rows, which is only the
	 * right answer for the query that asked for that k - see vfdw_knn.c.
	 */
	VFDW_SCAN_KNN
} VfdwScanStrategy;

/*
 * fdw_private slots. Fixed positions with fixed types, so that reading one
 * never depends on which strategy wrote it.
 */
#define VFDW_PRIV_STRATEGY	0	/* Integer, a VfdwScanStrategy */
#define VFDW_PRIV_KEYS		1	/* List of String; NIL for a keyspace scan */
#define VFDW_PRIV_PATTERN	2	/* String, the SCAN MATCH glob, or NULL */

/*
 * VFDW_SCAN_KNN only; NULL or 0 for every other strategy.
 *
 * The query vector is NOT here. It may be a Param, so it is an expression
 * rather than a value until the scan runs, and it travels in the plan's
 * fdw_exprs where setrefs.c and the expression walkers can reach it - which
 * is what makes a re-planned generic plan evaluate the new parameter instead
 * of the one the first execution saw.
 */
#define VFDW_PRIV_KNN_FIELD	3	/* String, the indexed vector attribute */
#define VFDW_PRIV_KNN_METRIC 4	/* String, what the OPERATOR measures */
#define VFDW_PRIV_KNN_K		5	/* Integer, rows to ask the server for */
#define VFDW_PRIV_KNN_FILTER 6	/* List, the compiled WHERE terms */

/*
 * Choose the access path and encode it for the executor and for EXPLAIN.
 * Defined in vfdw_plan.c.
 */
extern List *vfdw_scan_plan(PlannerInfo *root, RelOptInfo *baserel,
							VfdwTableMap *map, List *clauses,
							List **fdw_exprs);

extern List *vfdw_plan_keys(List *fdw_private);
extern const char *vfdw_plan_pattern(List *fdw_private);

/* The three KNN slots, or NULL/0 when the plan is not a search. */
extern const char *vfdw_plan_knn_field(List *fdw_private);
extern const char *vfdw_plan_knn_metric(List *fdw_private);
extern int	vfdw_plan_knn_k(List *fdw_private);
extern List *vfdw_plan_knn_filter(List *fdw_private);

/*
 * The MATCH glob a table is confined to, before any qual narrows it further.
 * ANALYZE needs this without a plan to read it from.
 */
extern char *vfdw_plan_scan_pattern(VfdwTableMap *map, const char *like_prefix);

/* Name the strategy encoded in a plan's fdw_private. */
extern const char *vfdw_scan_strategy_name(List *fdw_private);

/* Keys skipped because they expired or held another type. */
extern int64 vfdw_scan_skipped(ForeignScanState *node);

/*
 * SCAN/SSCAN round trips the scan made.
 *
 * The one thing scan_count changes that a caller can see. Without it, a suite
 * setting scan_count to 100 and one leaving it at 1000 read the same rows in
 * the same order and asserted the same numbers, so the option's only test was
 * that its value validated.
 */
extern int64 vfdw_scan_pages(ForeignScanState *node);

/*
 * The scan's producer, reachable without an executor node.
 *
 * ANALYZE drives these directly so that its sample comes from exactly the
 * code a query reads through. VfdwScanState stays opaque: everything a caller
 * needs is here.
 */
typedef struct VfdwScanState VfdwScanState;

extern VfdwScanState *vfdw_scan_state_create(Relation rel, MemoryContext parent);
extern TupleTableSlot *vfdw_scan_fetch(VfdwScanState *state, TupleTableSlot *slot);
extern void vfdw_scan_state_close(VfdwScanState *state);

/* ANALYZE's sample callback. */
extern int	vfdw_scan_acquire_sample_rows(Relation rel, int elevel,
										  HeapTuple *rows, int targrows,
										  double *totalrows,
										  double *totaldeadrows);

/*
 * How many batch contexts a scan has created, and how many it has reset.
 *
 * A rescan RESETS its batch context rather than creating another: a foreign
 * scan on the inner side of a nested loop is rescanned once per outer row, and
 * a context abandoned per pass leaves a struct and a memory-context reset
 * callback behind each time - so both memory and the callback chain grow with
 * the outer row count. Nothing fails; it just gets heavier, which is why the
 * fix had no test until these counters gave it one.
 *
 * Counted rather than measured, for the reason the overlay counters give: a
 * megabyte is a fact about the machine and a context is a fact about the code.
 */
extern void vfdw_scan_batch_stats(uint64 *contexts, uint64 *resets);

#endif							/* VFDW_SCAN_H */
