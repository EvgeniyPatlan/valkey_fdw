/*-------------------------------------------------------------------------
 *
 * vfdw_flush.h
 *		The pre-commit flush.
 *
 * A transaction's writes are applied here as one unit: a quiescence precheck,
 * the connection the unit's keys belong on, SELECT, a pipelined SCRIPT LOAD +
 * EVALSHA, retry classification, and the four outcomes src/vfdw_flush.c sets
 * out at length. None of that reaches a caller, which sees only the two states
 * this header exposes - the unit applied, or an error whose SQLSTATE says what
 * is true of Valkey.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_flush.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_FLUSH_H
#define VFDW_FLUSH_H

#include "vfdw.h"

/*
 * Called from XACT_EVENT_PRE_COMMIT and from nowhere else. Returns once the
 * unit has been applied, and at once on an empty buffer, which touches no
 * connection. Every other outcome ereports, and the transaction aborts.
 */
extern void vfdw_flush_pre_commit(void);

/*
 * Is this transaction's buffer settled? True on both branches - a unit
 * applied, and an empty buffer that needed nothing sent - because COMMIT reads
 * it to catch a write that slipped past pre-commit unflushed.
 */
extern bool vfdw_flush_done(void);
extern void vfdw_flush_reset(void);

/*
 * Counters, for the one observation that distinguishes "the flush returned at
 * step 0" from "the flush was never called at all". Backend-lifetime and
 * never reset, so a suite reads them as deltas.
 */
extern uint64 vfdw_flush_calls(void);
extern uint64 vfdw_flush_retries(void);

/*
 * Batches the flush opened and closed. Equal unless an attempt returned
 * without closing its own - which the retry path once did.
 */
extern uint64 vfdw_flush_batches_opened(void);
extern uint64 vfdw_flush_batches_closed(void);
extern uint64 vfdw_flush_empty_returns(void);
extern uint64 vfdw_flush_flushes(void);

#endif							/* VFDW_FLUSH_H */
