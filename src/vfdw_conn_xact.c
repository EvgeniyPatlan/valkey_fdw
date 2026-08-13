/*-------------------------------------------------------------------------
 *
 * vfdw_conn_xact.c
 *		What the pool does at transaction and subtransaction boundaries.
 *
 * Registered by src/vfdw_xact.c, never here: there is exactly one
 * RegisterXactCallback in this extension, and it sequences the flush ahead of
 * the pool so a commit is applied before the connection it used is swept.
 *
 * Split out of vfdw_conn.c at the 800-line gate. The seam is that nothing
 * here opens, reads or writes - it only decides what to keep.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_conn_xact.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_conn_internal.h"

void
vfdw_conn_xact(XactEvent event)
{
	HASH_SEQ_STATUS scan;
	VfdwConn   *vconn;

	if (vfdw_conn_pool == NULL)
		return;

	switch (event)
	{
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			return;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			break;
		case XACT_EVENT_PREPARE:
			return;
		default:
			return;
	}

	hash_seq_init(&scan, vfdw_conn_pool);
	while ((vconn = (VfdwConn *) hash_seq_search(&scan)) != NULL)
	{
		if (vconn->conn == NULL)
			continue;

		if (vconn->in_conversation || vconn->invalidated)
			vfdw_conn_close(vconn);

		/*
		 * No reader survives the transaction, so any lease still held belongs
		 * to a scan that raised before it could give one back. Releasing here
		 * rather than in the scan's error path is what keeps invariant I1:
		 * nothing is cleaned up on the way out of a failure.
		 */
		vconn->leased = false;
		vconn->lease_subid = InvalidSubTransactionId;
	}
}

/*
 * A subtransaction went away. Anything it was holding goes with it.
 *
 * The connection half of this is unconditional: a connection caught
 * mid-conversation cannot be handed to anyone, because the reply to whatever
 * was in flight is still coming.
 *
 * The event filter stays here rather than moving into src/vfdw_xact.c: which
 * subtransaction events matter to a connection is knowledge about
 * connections.
 *
 * The lease half is not, and the difference matters. Releasing every lease
 * here would break a live outer scan: "SELECT f(x) FROM t" with an EXCEPTION
 * block inside f runs subtransactions to completion and failure while the
 * scan on t is open and rightly holding its connection, and giving that
 * connection to a second reader is the reply-stream aliasing the lease exists
 * to prevent. Only a lease this subtransaction took itself is released.
 *
 * Without this, a caught error leaked a slot: the scan raised, invariant I1
 * left the lease alone, the EXCEPTION block swallowed the error, and nothing
 * until end of transaction gave the slot back. Forty caught failures in one
 * statement used all 32, and every attempt after that reported "too many
 * concurrent Valkey scans" in place of whatever had actually gone wrong.
 */
void
vfdw_conn_subxact(SubXactEvent event, SubTransactionId mySubid,
				  SubTransactionId parentSubid)
{
	HASH_SEQ_STATUS scan;
	VfdwConn   *vconn;

	if (event != SUBXACT_EVENT_ABORT_SUB || vfdw_conn_pool == NULL)
		return;

	hash_seq_init(&scan, vfdw_conn_pool);
	while ((vconn = (VfdwConn *) hash_seq_search(&scan)) != NULL)
	{
		if (vconn->conn != NULL && vconn->in_conversation)
			vfdw_conn_close(vconn);

		if (vconn->leased && vconn->lease_subid == mySubid)
		{
			vconn->leased = false;
			vconn->lease_subid = InvalidSubTransactionId;

			/*
			 * Initialised HERE as well as cleared in vfdw_conn_close, and
			 * both are needed. hash_search(HASH_ENTER) fills only the key,
			 * and close never runs for a brand-new entry - so a field cleared
			 * only in close is read uninitialised by the first flush against
			 * a new (serverid, userid), which would skip a SCRIPT LOAD the
			 * server has never seen.
			 */
			vconn->script_loaded = false;
		}
	}
}

