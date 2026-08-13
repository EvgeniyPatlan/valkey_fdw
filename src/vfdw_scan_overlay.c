/*-------------------------------------------------------------------------
 *
 * vfdw_scan_overlay.c
 *		The scan's half of the read-through overlay.
 *
 * src/vfdw_overlay.c answers "what has this transaction done to this key".
 * This file is what a scan does with that answer: the seen-set that stops a
 * key being emitted twice, and the tail iteration that produces rows the
 * server does not have yet.
 *
 * Split out of vfdw_scan.c because that file crossed the 800-line gate when
 * the overlay landed, and because the seam is real: everything here is about
 * reconciling two sources of rows, and nothing here talks to Valkey.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_scan_overlay.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw.h"

#include "common/hashfn.h"
#include "executor/tuptable.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "vfdw_overlay.h"
#include "vfdw_scan_internal.h"
#include "vfdw_scan_overlay.h"

/* ---------------------------------------------------------------------
 * The overlay's seen-set
 *
 * Keyed by key bytes; see the note on the struct field for why not by
 * ordinal. Created lazily, because a scan of a table this transaction has
 * not written pays for nothing.
 * --------------------------------------------------------------------- */

typedef struct VfdwSeenKey
{
	const char *key;
	size_t		keylen;
} VfdwSeenKey;

static uint32
vfdw_seen_hash(const void *key, Size keysize)
{
	const VfdwSeenKey *k = (const VfdwSeenKey *) key;

	return hash_bytes((const unsigned char *) k->key, (int) k->keylen);
}

static int
vfdw_seen_match(const void *a, const void *b, Size keysize)
{
	const VfdwSeenKey *x = (const VfdwSeenKey *) a;
	const VfdwSeenKey *y = (const VfdwSeenKey *) b;

	if (x->keylen != y->keylen)
		return 1;
	if (x->keylen == 0)
		return 0;
	return memcmp(x->key, y->key, x->keylen) != 0;
}

/*
 * Record a key the server's own pages produced.
 *
 * The bytes are COPIED into scan_cxt. They come from page_cxt, which is reset
 * per page, so a set holding borrowed pointers would compare against freed
 * memory the moment the second page arrived - and would do it silently,
 * because a freed chunk usually still reads.
 */
void
vfdw_scan_mark_seen(VfdwScanState *state, const char *key, size_t keylen)
{
	VfdwSeenKey probe;
	bool		found;
	MemoryContext old;

	if (!vfdw_overlay_active(state->map->relid))
		return;

	/*
	 * A context of its own, because a rescan drops the whole set: the table
	 * and every key byte in it belong to one pass, and dropping the pointer
	 * while they sat in the scan context leaked all of it per rescan - the
	 * same growth as an abandoned batch, in the same function, and unbounded
	 * in the outer row count of a nested loop.
	 *
	 * hash_destroy is not the alternative: it deletes the table's own hcxt,
	 * which would have taken the scan context with it.
	 */
	if (state->seen_cxt == NULL)
		state->seen_cxt = AllocSetContextCreate(state->scan_cxt,
												"valkey_fdw overlay seen",
												ALLOCSET_SMALL_SIZES);

	old = MemoryContextSwitchTo(state->seen_cxt);

	if (state->seen == NULL)
	{
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(VfdwSeenKey);
		ctl.entrysize = sizeof(VfdwSeenKey);
		ctl.hash = vfdw_seen_hash;
		ctl.match = vfdw_seen_match;
		ctl.hcxt = state->seen_cxt;
		state->seen = hash_create("valkey_fdw overlay seen", 64, &ctl,
								  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE |
								  HASH_CONTEXT);
	}

	memset(&probe, 0, sizeof(probe));
	probe.key = memcpy(palloc(Max(keylen, 1)), key, keylen);
	probe.keylen = keylen;
	(void) hash_search(state->seen, &probe, HASH_ENTER, &found);

	MemoryContextSwitchTo(old);
}

bool
vfdw_scan_already_seen(VfdwScanState *state, const char *key, size_t keylen)
{
	VfdwSeenKey probe;
	bool		found;

	if (state->seen == NULL)
		return false;

	memset(&probe, 0, sizeof(probe));
	probe.key = key;
	probe.keylen = keylen;
	(void) hash_search(state->seen, &probe, HASH_FIND, &found);
	return found;
}

/*
 * Emit one key this transaction created that the server does not have.
 *
 * Runs after the cursor is exhausted and BEFORE the scan is declared
 * finished. Returns false when there are none left, which is the only thing
 * that ends the scan from here - "no tuple this call" and "scan over" stay
 * distinct, which is invariant I5.
 */
bool
vfdw_scan_inject(VfdwScanState *state, TupleTableSlot *slot)
{
	for (;;)
	{
		const char *key = NULL;
		size_t		keylen = 0;
		HeapTuple	tup = vfdw_overlay_iter_next(&state->overlay_iter,
												 &key, &keylen);

		if (tup == NULL)
			return false;
		if (vfdw_scan_already_seen(state, key, keylen))
			continue;

		vfdw_scan_mark_seen(state, key, keylen);

		/*
		 * shouldFree = false: the tuple belongs to the write buffer and
		 * outlives this slot. Freeing it here would leave the ledger and the
		 * flush pointing at released memory.
		 */
		ExecForceStoreHeapTuple(tup, slot, false);
		return true;
	}
}

/*
 * What this transaction has done to the key just taken, applied to the slot.
 *
 * Called only AFTER the reply has been taken - the reply is positionally
 * paired with the key, so consulting the overlay instead of taking it would
 * leave the pipeline one reply out of step for the rest of the scan.
 */
VfdwOverlayVerdict
vfdw_scan_apply_overlay(VfdwScanState *state, const char *key, size_t keylen,
						TupleTableSlot *slot)
{
	HeapTuple	tup = NULL;
	VfdwOverlayVerdict v;

	if (!vfdw_overlay_active(state->map->relid))
		return VFDW_OVL_PASS;

	v = vfdw_overlay_check(state->map->relid, key, keylen,
						   state->overlay_cid, &tup);

	vfdw_scan_mark_seen(state, key, keylen);

	if (v == VFDW_OVL_REPLACE)
		ExecForceStoreHeapTuple(tup, slot, false);

	return v;
}

/*
 * The server has no more keys: produce what this transaction created itself.
 *
 * Runs before the scan may be declared finished, because otherwise a
 * transaction could not see its own INSERT - which is the whole point of the
 * overlay. Returning the empty slot is the ONLY place a scan ends, and
 * reaching it means the cursor is exhausted, nothing is in flight, and the
 * overlay's tail is spent; not merely that one reply produced no row.
 */
TupleTableSlot *
vfdw_scan_drain_tail(VfdwScanState *state, TupleTableSlot *slot)
{
	if (!vfdw_overlay_active(state->map->relid))
		return slot;

	if (!state->overlay_tail)
	{
		state->overlay_tail = true;
		vfdw_overlay_iter_init(&state->overlay_iter, state->map->relid,
							   state->overlay_cid);
	}

	(void) vfdw_scan_inject(state, slot);
	return slot;
}

/*
 * Drop the seen-set whole.
 *
 * Here rather than in the rescan that calls it, because the set, its table and
 * every key byte in it live in one context and this is the file that knows
 * that. Resetting the context is what returns the keys; nulling the pointer
 * alone left them in the scan context for the rest of the query, once per
 * rescan.
 */
void
vfdw_scan_seen_reset(VfdwScanState *state)
{
	state->seen = NULL;
	if (state->seen_cxt != NULL)
		MemoryContextReset(state->seen_cxt);
}
