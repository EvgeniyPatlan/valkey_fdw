/*-------------------------------------------------------------------------
 *
 * vfdw_overlay.c
 *		What a scan sees of its own transaction's uncommitted writes.
 *
 * See src/vfdw_overlay.h for the visibility rule, why HIDE and inject are
 * both "no tuple this call", and why the seen-set is keyed by key bytes.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_overlay.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_overlay.h"

#include "common/hashfn.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

/*
 * The index, keyed by (relid, key bytes).
 *
 * Two foreign tables may map one Valkey key with different column sets, so
 * the relid is part of the identity: a scan of one must not see the other's
 * buffered row, whose tupledesc would not even match.
 */
typedef struct VfdwOverlayKey
{
	Oid			relid;
	const char *key;
	size_t		keylen;
} VfdwOverlayKey;

/*
 * Every operation on that key, in buffer order.
 *
 * The whole list is kept rather than only the latest, because "latest" is a
 * function of the asking command: a scan with an older curcid must see the
 * state as of ITS command, not the newest one. Collapsing at build time would
 * answer the newest question to every asker.
 */
typedef struct VfdwOverlayEntry
{
	VfdwOverlayKey k;			/* must be first */
	const VfdwWriteOp **ops;
	int			nops;
	int			capacity;
} VfdwOverlayEntry;

static MemoryContext vfdw_overlay_cxt = NULL;
static HTAB *vfdw_overlay_tab = NULL;
static uint64 vfdw_overlay_built_gen = 0;

/*
 * What the index was built against, beyond the generation: the shrink counter
 * that says whether anything indexed can have gone away, and the last
 * operation recorded, which is where an extension resumes.
 */
static uint64 vfdw_overlay_built_shrink_gen = 0;
static const VfdwWriteOp *vfdw_overlay_built_tail = NULL;

/*
 * How the index has been brought up to date, counted for the suites.
 *
 * The property worth asserting is not how long a bulk DELETE takes - that is a
 * measurement of the machine - but how many times its scan threw the index
 * away. One rebuild and n extensions is linear; n rebuilds is the quadratic
 * this counter exists to make visible.
 */
static uint64 vfdw_overlay_rebuilds = 0;
static uint64 vfdw_overlay_extends = 0;
static bool vfdw_overlay_have = false;

static uint32
vfdw_overlay_hash(const void *key, Size keysize)
{
	const VfdwOverlayKey *k = (const VfdwOverlayKey *) key;

	return hash_combine(hash_uint32((uint32) k->relid),
						hash_bytes((const unsigned char *) k->key,
								   (int) k->keylen));
}

static int
vfdw_overlay_match(const void *a, const void *b, Size keysize)
{
	const VfdwOverlayKey *x = (const VfdwOverlayKey *) a;
	const VfdwOverlayKey *y = (const VfdwOverlayKey *) b;

	if (x->relid != y->relid || x->keylen != y->keylen)
		return 1;
	if (x->keylen == 0)
		return 0;
	return memcmp(x->key, y->key, x->keylen) != 0;
}

void
vfdw_overlay_reset(void)
{
	if (vfdw_overlay_cxt == NULL)
		return;

	MemoryContextDelete(vfdw_overlay_cxt);
	vfdw_overlay_cxt = NULL;
	vfdw_overlay_tab = NULL;
	vfdw_overlay_have = false;
	vfdw_overlay_built_gen = 0;
	vfdw_overlay_built_shrink_gen = 0;
	vfdw_overlay_built_tail = NULL;
}

/*
 * Append one operation to its key's list.
 *
 * The whole list is kept rather than only the newest, because "newest" is a
 * function of the asking command: a scan with an older curcid must see the
 * state as of ITS command. Collapsing at build time would answer the newest
 * question to every asker.
 */
static void
vfdw_overlay_record(const VfdwWriteOp *op)
{
	VfdwOverlayKey probe;
	VfdwOverlayEntry *entry;
	bool		found;

	memset(&probe, 0, sizeof(probe));
	probe.relid = op->relid;
	probe.key = op->key;
	probe.keylen = op->keylen;

	entry = (VfdwOverlayEntry *) hash_search(vfdw_overlay_tab, &probe,
											 HASH_ENTER, &found);
	if (!found)
	{
		entry->capacity = 4;
		entry->nops = 0;
		entry->ops = (const VfdwWriteOp **)
			MemoryContextAlloc(vfdw_overlay_cxt,
							   sizeof(VfdwWriteOp *) * entry->capacity);
	}
	else if (entry->nops == entry->capacity)
	{
		const VfdwWriteOp **grown;

		entry->capacity *= 2;
		grown = (const VfdwWriteOp **)
			MemoryContextAlloc(vfdw_overlay_cxt,
							   sizeof(VfdwWriteOp *) * entry->capacity);
		memcpy(grown, entry->ops, sizeof(VfdwWriteOp *) * entry->nops);
		entry->ops = grown;
	}

	entry->ops[entry->nops++] = op;
}

/*
 * Bring the index up to date with the buffer.
 *
 * Keyed off vfdw_wbuf_generation, which advances whenever the operation list
 * changes - including on every append, because an index that stayed "fresh"
 * across a DELETE's own appends let the next statement read a keyspace view
 * that predated them.
 *
 * THAT CORRECTNESS FIX MADE THIS QUADRATIC, which is the reason for the second
 * generation. A DELETE appends an operation per row and its scan consults the
 * overlay per row, so "the buffer moved, throw the index away" rebuilt an
 * index over every operation so far, once per row: 4000 rows took 18 seconds
 * and 10000 exceeded the command timeout, while the same INSERT stayed at
 * milliseconds because nothing reads through the overlay on the way in.
 *
 * So a buffer that only GREW is extended from where the last build stopped,
 * which is one pass over the operations in total. Only a shrink - a
 * subtransaction rollback, or a reset - can invalidate what is already
 * indexed, and vfdw_wbuf_shrink_generation is what says one happened.
 */
static void
vfdw_overlay_build(void)
{
	const VfdwWriteOp *op;
	HASHCTL		ctl;

	if (vfdw_overlay_have &&
		vfdw_overlay_built_gen == vfdw_wbuf_generation())
		return;

	/*
	 * Only appends since the last build: extend rather than rebuild. The tail
	 * recorded then is still linked, because nothing has been unlinked, so its
	 * successor is the first operation this index has not seen.
	 */
	if (vfdw_overlay_have &&
		vfdw_overlay_built_shrink_gen == vfdw_wbuf_shrink_generation())
	{
		for (op = (vfdw_overlay_built_tail != NULL
				   ? vfdw_overlay_built_tail->next
				   : vfdw_wbuf_first());
			 op != NULL; op = op->next)
			vfdw_overlay_record(op);

		vfdw_overlay_built_tail = vfdw_wbuf_last();
		vfdw_overlay_built_gen = vfdw_wbuf_generation();
		vfdw_overlay_extends++;
		return;
	}

	vfdw_overlay_reset();

	vfdw_overlay_cxt = AllocSetContextCreate(TopMemoryContext,
											 "valkey_fdw write overlay",
											 ALLOCSET_START_SMALL_SIZES);

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = offsetof(VfdwOverlayEntry, ops);
	ctl.entrysize = sizeof(VfdwOverlayEntry);
	ctl.hash = vfdw_overlay_hash;
	ctl.match = vfdw_overlay_match;
	ctl.hcxt = vfdw_overlay_cxt;
	vfdw_overlay_tab = hash_create("valkey_fdw overlay", 64, &ctl,
								   HASH_ELEM | HASH_FUNCTION | HASH_COMPARE |
								   HASH_CONTEXT);

	for (op = vfdw_wbuf_first(); op != NULL; op = op->next)
		vfdw_overlay_record(op);

	vfdw_overlay_built_gen = vfdw_wbuf_generation();
	vfdw_overlay_built_shrink_gen = vfdw_wbuf_shrink_generation();
	vfdw_overlay_built_tail = vfdw_wbuf_last();
	vfdw_overlay_have = true;
	vfdw_overlay_rebuilds++;
}

void
vfdw_overlay_index_stats(uint64 *rebuilds, uint64 *extends)
{
	*rebuilds = vfdw_overlay_rebuilds;
	*extends = vfdw_overlay_extends;
}

bool
vfdw_overlay_active(Oid relid)
{
	/*
	 * Deliberately not per-relation: answering "has this transaction written
	 * to THIS table" would need the index, and building it is the cost this
	 * call exists to avoid. An empty buffer is the case that matters, and it
	 * is the common one.
	 */
	return !vfdw_wbuf_is_empty();
}

VfdwOverlayVerdict
vfdw_overlay_check(Oid relid, const char *key, size_t keylen,
				   CommandId curcid, HeapTuple *tup)
{
	VfdwOverlayKey probe;
	VfdwOverlayEntry *entry;
	bool		found;
	int			i;

	if (vfdw_wbuf_is_empty())
		return VFDW_OVL_PASS;

	vfdw_overlay_build();

	memset(&probe, 0, sizeof(probe));
	probe.relid = relid;
	probe.key = key;
	probe.keylen = keylen;

	entry = (VfdwOverlayEntry *) hash_search(vfdw_overlay_tab, &probe,
											 HASH_FIND, &found);
	if (!found)
		return VFDW_OVL_PASS;

	/*
	 * Backwards to the newest operation this command may see. Forwards would
	 * find the oldest, which is the state before the transaction did
	 * anything - the very answer the overlay exists to correct.
	 */
	for (i = entry->nops - 1; i >= 0; i--)
	{
		const VfdwWriteOp *op = entry->ops[i];

		if (op->cid >= curcid)
			continue;

		if (op->kind == VFDW_OP_DELETE)
			return VFDW_OVL_HIDE;

		/*
		 * No stored tuple means nothing to put in the slot, so the server's
		 * row is the best available answer. Reachable for a shape whose
		 * write does not carry a whole row.
		 */
		if (op->newtup == NULL)
			return VFDW_OVL_PASS;

		if (tup != NULL)
			*tup = op->newtup;
		return VFDW_OVL_REPLACE;
	}

	return VFDW_OVL_PASS;
}

void
vfdw_overlay_iter_init(VfdwOverlayIter *it, Oid relid, CommandId curcid)
{
	it->relid = relid;
	it->curcid = curcid;
	it->generation = vfdw_wbuf_generation();
	it->next = vfdw_wbuf_first();
}

HeapTuple
vfdw_overlay_iter_next(VfdwOverlayIter *it, const char **key, size_t *keylen)
{
	/*
	 * A rebuild renumbers everything, so an iterator that spanned one would
	 * be walking a list that no longer exists. Restarting is the only safe
	 * answer; the caller's seen-set is keyed by key bytes precisely so that
	 * restarting does not re-emit what it already produced.
	 */
	if (it->generation != vfdw_wbuf_generation())
		vfdw_overlay_iter_init(it, it->relid, it->curcid);

	while (it->next != NULL)
	{
		const VfdwWriteOp *op = it->next;
		HeapTuple	tup = NULL;

		it->next = op->next;

		if (op->relid != it->relid || op->kind != VFDW_OP_INSERT)
			continue;

		/*
		 * The visibility rule is asked of check() and is NOT repeated here.
		 * Writing it twice was the first version and it cost the suite its
		 * teeth: a mutation that disabled the CommandId test in check() left
		 * every assertion green, because the copy in this loop was still
		 * enforcing it. One rule, one place.
		 *
		 * check() also answers the two questions this loop would otherwise
		 * have to track itself - a key created then deleted (HIDE), and a key
		 * created then updated (whose newest tuple is what must be emitted,
		 * not the original INSERT's).
		 */
		if (vfdw_overlay_check(op->relid, op->key, op->keylen, it->curcid,
							   &tup) != VFDW_OVL_REPLACE)
			continue;

		if (key != NULL)
			*key = op->key;
		if (keylen != NULL)
			*keylen = op->keylen;
		return tup;
	}

	return NULL;
}
