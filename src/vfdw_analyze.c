/*-------------------------------------------------------------------------
 *
 * vfdw_analyze.c
 *		Drawing a sample for ANALYZE.
 *
 * Split out from vfdw_scan.c, which has no room left and no reason to hold
 * this: sampling runs once per ANALYZE, outside the executor, and shares with
 * the scan only the producer it deliberately reuses.
 *
 * That reuse is the point. A sampler of its own would be free to disagree
 * with the scan about what the table contains, and statistics describing a
 * different table than the one being read are worse than no statistics.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_analyze.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_scan.h"

#include "access/htup_details.h"
#include "executor/executor.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/sampling.h"

/*
 * The reservoir is full: replace an existing row, at a decreasing rate, so
 * that every row read still has an equal chance of ending up in the sample.
 */
static void
vfdw_scan_sample_replace(HeapTuple *rows, int targrows, TupleTableSlot *slot,
						 ReservoirState rstate, double samplerows,
						 double *rowstoskip)
{
	if (*rowstoskip < 0)
		*rowstoskip = reservoir_get_next_S(rstate, samplerows, targrows);

	if (*rowstoskip <= 0)
	{
		int			k = (int) (targrows * vfdw_sampler_fract(rstate));

		Assert(k >= 0 && k < targrows);
		heap_freetuple(rows[k]);
		rows[k] = ExecCopySlotHeapTuple(slot);
	}

	*rowstoskip -= 1;
}

/*
 * Draw a sample for ANALYZE.
 *
 * Without this, every foreign table plans against the same invented row
 * count, so a join with a Valkey table on one side is chosen from a number
 * nobody measured.
 *
 * Valkey offers no way to sample a keyspace, so the whole table is read once
 * and reservoir-sampled down to targrows. That makes totalrows exact rather
 * than extrapolated, which is the compensation for the cost.
 */
int
vfdw_scan_acquire_sample_rows(Relation rel, int elevel, HeapTuple *rows,
							  int targrows, double *totalrows,
							  double *totaldeadrows)
{
	VfdwScanState *state;
	TupleTableSlot *slot;
	ReservoirStateData rstate;
	MemoryContext sample_cxt;
	double		samplerows = 0;
	double		rowstoskip = -1;
	int			numrows = 0;

	sample_cxt = AllocSetContextCreate(CurrentMemoryContext,
									   "valkey_fdw analyze",
									   ALLOCSET_DEFAULT_SIZES);

	state = vfdw_scan_state_create(rel, sample_cxt);
	slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &TTSOpsVirtual);

	reservoir_init_selection_state(&rstate, targrows);

	while (vfdw_scan_fetch(state, slot), !TupIsNull(slot))
	{
		if (numrows < targrows)
			rows[numrows++] = ExecCopySlotHeapTuple(slot);
		else
			vfdw_scan_sample_replace(rows, targrows, slot, &rstate,
									 samplerows, &rowstoskip);

		samplerows += 1;
	}

	vfdw_scan_state_close(state);
	ExecDropSingleTupleTableSlot(slot);
	MemoryContextDelete(sample_cxt);

	/*
	 * The scan visited every row, so this is a count and not an estimate.
	 * Valkey has no dead tuples to report.
	 */
	*totalrows = samplerows;
	*totaldeadrows = 0;

	ereport(elevel,
			(errmsg("\"%s\": scanned %.0f rows, %d sampled",
					RelationGetRelationName(rel), samplerows, numrows)));

	return numrows;
}

