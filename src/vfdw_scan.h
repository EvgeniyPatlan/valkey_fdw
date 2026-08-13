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
	VFDW_SCAN_SINGLETON			/* singleton_key: one fixed key */
} VfdwScanStrategy;

/*
 * fdw_private slots. Fixed positions with fixed types, so that reading one
 * never depends on which strategy wrote it.
 */
#define VFDW_PRIV_STRATEGY	0	/* Integer, a VfdwScanStrategy */
#define VFDW_PRIV_KEYS		1	/* List of String; NIL for a keyspace scan */
#define VFDW_PRIV_PATTERN	2	/* String, the SCAN MATCH glob, or NULL */

/*
 * Choose the access path and encode it for the executor and for EXPLAIN.
 * Defined in vfdw_plan.c.
 */
extern List *vfdw_scan_plan(PlannerInfo *root, RelOptInfo *baserel,
							VfdwTableMap *map, List *clauses);

extern List *vfdw_plan_keys(List *fdw_private);
extern const char *vfdw_plan_pattern(List *fdw_private);

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

#endif							/* VFDW_SCAN_H */
