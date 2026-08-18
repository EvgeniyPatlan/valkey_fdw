/*-------------------------------------------------------------------------
 *
 * vfdw_overlay.h
 *		What a scan sees of its own transaction's uncommitted writes.
 *
 * Writes are buffered until pre-commit, so without this a transaction could
 * not read what it had just written - and worse, would read the PRE-write
 * state and call it current. The overlay is a view over the buffer that the
 * scan consults per key: hide a row this transaction deleted, replace one it
 * updated, and inject at the tail the keys it created that the server does
 * not have yet.
 *
 * VISIBILITY IS POSTGRESQL'S OWN RULE, NOT A NEW ONE.
 *
 * An entry is visible only when its CommandId is older than the scan's
 * es_snapshot->curcid. That is exactly what stops INSERT INTO t SELECT * FROM
 * t from re-reading its own output forever. `INSERT; SELECT` is two commands
 * and sees the write; `INSERT ... SELECT` is one command and does not.
 *
 * HIDE AND INJECT ARE BOTH "NO TUPLE THIS CALL", NOT "SCAN OVER".
 *
 * That distinction is invariant I5, and it is why the verdict is consumed
 * AFTER vfdw_batch_next has taken the reply, never instead of it: the reply
 * is positionally paired with the key, and skipping a take desynchronises the
 * pipeline exactly as two readers on one connection would.
 *
 * NO SYNTHETIC valkeyReply IS EVER MANUFACTURED.
 *
 * An injected row is stored into the slot from the buffered HeapTuple with
 * ExecForceStoreHeapTuple. Building a fake reply instead would put it in
 * state->cur_reply, which is dereferenced across Iterate calls, and a
 * ROLLBACK TO SAVEPOINT that rebuilds the overlay would free it under an open
 * cursor.
 *
 * THE SEEN-SET IS KEYED BY KEY BYTES, NOT BY ENTRY ORDINAL.
 *
 * A rebuild renumbers every entry, so an ordinal-keyed bitmap held by a
 * cursor across a savepoint rollback silently skips or repeats rows - the
 * same silent-truncation shape as a rescan that replays nothing,
 * reintroduced through the
 * seen-set. The iterator also carries the buffer generation and restarts if
 * it moved.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_overlay.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_OVERLAY_H
#define VFDW_OVERLAY_H

#include "vfdw.h"

#include "access/htup.h"
#include "utils/snapshot.h"

#include "vfdw_wbuf.h"

typedef enum VfdwOverlayVerdict
{
	VFDW_OVL_PASS = 0,			/* the server's row stands */
	VFDW_OVL_HIDE,				/* this transaction deleted it */
	VFDW_OVL_REPLACE			/* this transaction rewrote it */
} VfdwOverlayVerdict;

/*
 * A cheap short-circuit. A read-only transaction must pay nothing for the
 * overlay existing, which means this has to be answerable without building
 * anything.
 */
extern bool vfdw_overlay_active(Oid relid);

/*
 * What this transaction has done to one key, as of curcid.
 *
 * *tup receives the buffered row for VFDW_OVL_REPLACE and is untouched
 * otherwise. It belongs to the write buffer and must not be freed.
 */
extern VfdwOverlayVerdict vfdw_overlay_check(Oid relid, const char *key,
											 size_t keylen, CommandId curcid,
											 HeapTuple *tup);

/*
 * Tail iteration: keys this transaction created that the server has not got.
 *
 * The caller drives it after its cursor is exhausted and BEFORE declaring the
 * scan finished. seen is the caller's set of key bytes already emitted from
 * the server's own pages, so a key that was both present and updated is not
 * emitted twice.
 */
typedef struct VfdwOverlayIter
{
	Oid			relid;
	CommandId	curcid;
	uint64		generation;		/* the buffer's, when this iterator started */
	const VfdwWriteOp *next;
} VfdwOverlayIter;

extern void vfdw_overlay_iter_init(VfdwOverlayIter *it, Oid relid,
								   CommandId curcid);

/*
 * The next created-in-buffer row, or NULL when there are none left.
 *
 * Returns the key bytes alongside the tuple so the caller can record it in
 * its seen-set without re-deriving it from the tuple, which would need the
 * mapping layer and could disagree.
 */
extern HeapTuple vfdw_overlay_iter_next(VfdwOverlayIter *it,
										const char **key, size_t *keylen);

/* Dropped with the buffer; see vfdw_wbuf_reset. */
extern void vfdw_overlay_reset(void);

/*
 * How many times the index was rebuilt from scratch, and how many times it was
 * extended in place. A bulk DELETE's scan must not rebuild per row; see the
 * definition of vfdw_overlay_build.
 */
extern void vfdw_overlay_index_stats(uint64 *rebuilds, uint64 *extends);

#endif							/* VFDW_OVERLAY_H */
