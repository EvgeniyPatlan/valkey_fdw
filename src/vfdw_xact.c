/*-------------------------------------------------------------------------
 *
 * vfdw_xact.c
 *		The single transaction-callback registration site, and the sequencer.
 *
 * See src/vfdw_xact.h for why there is exactly one of each callback and why
 * PRE_COMMIT is the only point at which anything may reach the wire.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_xact.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_xact.h"

#include "access/xact.h"

#include "vfdw_conn.h"
#include "vfdw_flush.h"
#include "vfdw_wbuf.h"

static bool vfdw_xact_registered = false;

/*
 * No default: the XactEvent enum is closed, and an exhaustive switch is what
 * makes a new event in a future major a compiler warning instead of a silent
 * fall-through past the flush.
 *
 * The buffer is always dealt with before the connections, so that a
 * connection sweep can never run before the work it would have carried.
 */
static void
vfdw_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			vfdw_flush_pre_commit();
			vfdw_conn_xact(event);
			break;

		case XACT_EVENT_PARALLEL_PRE_COMMIT:
			if (!vfdw_wbuf_is_empty())
				elog(ERROR, "valkey_fdw write buffer is not empty in a parallel worker");
			vfdw_conn_xact(event);
			break;

		case XACT_EVENT_PRE_PREPARE:
			if (!vfdw_wbuf_is_empty())
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot PREPARE a transaction that has written "
								"to valkey_fdw foreign tables"),
						 errdetail("%d write operations are buffered for one "
								   "atomic Valkey unit.", vfdw_wbuf_live_ops()),
						 errhint("Valkey has no two-phase commit, so the "
								 "wrapper cannot hold a prepared write. COMMIT "
								 "or ROLLBACK instead.")));
			vfdw_conn_xact(event);
			break;

		case XACT_EVENT_PREPARE:
			/* Unreachable: PRE_PREPARE refused above. Loud if it is not. */
			if (!vfdw_wbuf_is_empty())
				elog(ERROR, "valkey_fdw write buffer survived PREPARE");
			vfdw_conn_xact(event);
			break;

		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
			if (!vfdw_flush_done() && !vfdw_wbuf_is_empty())
				elog(ERROR, "valkey_fdw write buffer was not flushed at pre-commit");
			vfdw_wbuf_reset();
			vfdw_flush_reset();
			vfdw_conn_xact(event);
			break;

		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			vfdw_wbuf_reset();
			vfdw_flush_reset();
			vfdw_conn_xact(event);
			break;
	}
}

/*
 * Every subtransaction event reaches both halves unconditionally, and each
 * filters for itself. Buffer first, for the same reason as above: the
 * buffer's ABORT_SUB truncation must not run after a connection sweep that
 * could have raised.
 */
static void
vfdw_xact_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						   SubTransactionId parentSubid, void *arg)
{
	vfdw_wbuf_subxact(event, mySubid, parentSubid);
	vfdw_conn_subxact(event, mySubid, parentSubid);
}

void
vfdw_xact_ensure_registered(void)
{
	if (vfdw_xact_registered)
		return;

	RegisterXactCallback(vfdw_xact_callback, NULL);
	RegisterSubXactCallback(vfdw_xact_subxact_callback, NULL);
	vfdw_xact_registered = true;
}
