/*-------------------------------------------------------------------------
 *
 * vfdw_xact.h
 *		The single transaction-callback registration site, and the sequencer.
 *
 * WHY THERE IS EXACTLY ONE CALLBACK
 *
 * RegisterXactCallback prepends and CallXactCallbacks walks head-first, so a
 * callback registered BEFORE another runs AFTER it. Registering a write
 * callback beside the connection callback would therefore make the order
 * between "flush the buffer" and "sweep the connections" depend on which of
 * the two lazy registrations happened to run first in this backend - which in
 * turn depends on whether the backend's first valkey_fdw statement was a
 * SELECT or an INSERT. That is not a hazard to document; it is a hazard to
 * remove, by having one callback that calls both in a written order.
 *
 * src/vfdw_conn.c therefore registers nothing and exports vfdw_conn_xact()
 * and vfdw_conn_subxact() instead. Their bodies are untouched: in particular
 * vfdw_conn_subxact keeps its own event filter, because the knowledge of
 * which events matter to connections belongs in the file that is about
 * connections.
 *
 * WHY PRE_COMMIT IS THE ONLY LEGAL FLUSH POINT
 *
 * At XACT_EVENT_PRE_COMMIT the transaction is still TRANS_INPROGRESS,
 * interrupts are not held (HOLD_INTERRUPTS is some sixty lines later in
 * CommitTransaction), the ResourceOwner and memory contexts are intact, and
 * an ereport still aborts the transaction. Every other event is pure
 * in-memory work: COMMIT, ABORT, PREPARE and ABORT_SUB all run inside
 * HOLD_INTERRUPTS, where CHECK_FOR_INTERRUPTS is a no-op and invariant I6
 * cannot be satisfied - so zero network I/O happens there. There is no test
 * and no lint for that rule, deliberately: a structural grep over these
 * switch arms passes trivially the moment the code is factored into helpers,
 * so it is a rule that could only ever be green. This comment and review are
 * the enforcement.
 *
 * The callback is re-entrant across a failed flush: an ereport at PRE_COMMIT
 * re-enters as XACT_EVENT_ABORT, and vfdw_wbuf_reset() assumes nothing about
 * how far the flush got.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_xact.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_XACT_H
#define VFDW_XACT_H

#include "vfdw.h"

/*
 * Idempotent. Called from vfdw_conn_init_pool() and from
 * vfdw_wbuf_context(), so no state can exist without its callbacks. Not from
 * a _PG_init: the module has none, and adding one would change when the
 * constructor runs relative to shared_preload_libraries.
 */
extern void vfdw_xact_ensure_registered(void);

#endif							/* VFDW_XACT_H */
