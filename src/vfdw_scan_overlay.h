/*-------------------------------------------------------------------------
 *
 * vfdw_scan_overlay.h
 *		The scan's half of the read-through overlay.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_scan_overlay.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_SCAN_OVERLAY_H
#define VFDW_SCAN_OVERLAY_H

#include "vfdw.h"

#include "executor/tuptable.h"

#include "vfdw_overlay.h"
#include "vfdw_scan.h"

/*
 * Record a key the server's own pages produced, so the tail does not emit it
 * again. The bytes are COPIED: they come from the per-page context, which is
 * reset between pages.
 */
extern void vfdw_scan_mark_seen(VfdwScanState *state, const char *key,
								size_t keylen);
extern bool vfdw_scan_already_seen(VfdwScanState *state, const char *key,
								   size_t keylen);

/*
 * Emit one key this transaction created that the server does not have.
 * Returns false when there are none left - which is the only thing that ends
 * a scan from here, keeping "no tuple this call" and "scan over" distinct.
 */
extern bool vfdw_scan_inject(VfdwScanState *state, TupleTableSlot *slot);

/* What this transaction has done to one scanned key, applied to the slot. */
extern VfdwOverlayVerdict vfdw_scan_apply_overlay(VfdwScanState *state,
												  const char *key,
												  size_t keylen,
												  TupleTableSlot *slot);

/* The server's keys are exhausted; produce what this transaction created. */
extern TupleTableSlot *vfdw_scan_drain_tail(VfdwScanState *state,
											TupleTableSlot *slot);

#endif							/* VFDW_SCAN_OVERLAY_H */
