/*-------------------------------------------------------------------------
 *
 * vfdw_scan_internal.h
 *		The scan's state, shared between vfdw_scan.c and its overlay half.
 *
 * Private to the scan. vfdw_scan.h keeps VfdwScanState opaque for everyone
 * else, and this exists only because src/vfdw_scan_overlay.c is the same
 * component split across two files rather than a separate consumer.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_scan_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_SCAN_INTERNAL_H
#define VFDW_SCAN_INTERNAL_H

#include "vfdw.h"

#include "utils/hsearch.h"

#include "vfdw_cmd.h"
#include "vfdw_conn.h"
#include "vfdw_map.h"
#include "vfdw_overlay.h"
#include "vfdw_cluster.h"
#include "vfdw_row.h"
#include "vfdw_scan.h"

typedef struct VfdwScanState
{
	VfdwTableMap *map;
	VfdwConn   *vconn;
	VfdwScanStrategy strategy;

	MemoryContext scan_cxt;		/* whole scan; outlives ReScan */
	MemoryContext page_cxt;		/* one page of keys; reset per page */

	/*
	 * The batch gets a context of its own so that a rescan can reset it.
	 *
	 * vfdw_batch_begin allocates the batch in the context it is handed and
	 * registers a reset callback on that context, and vfdw_batch_end releases
	 * neither. Beginning each pass in scan_cxt, which lasts as long as the
	 * query, therefore leaves a struct and a callback behind per pass: a
	 * foreign scan on the inner side of a nested loop grows both memory and
	 * the reset-callback chain with the outer row count.
	 * Resetting runs the old callback - which is what hands a reply still
	 * held back to libvalkey - and drops the struct with it.
	 */
	MemoryContext batch_cxt;

	/* The overlay seen-set's own context; reset whole on a rescan. */
	MemoryContext seen_cxt;
	VfdwBatch  *batch;

	/* Type resolution and the input functions live here; see vfdw_row_ctx_init. */
	VfdwRowCtx	rowctx;

	/*
	 * Keys named by the plan, for a strategy that does not discover any.
	 * These live in scan_cxt, not page_cxt, so they survive the per-page
	 * reset and a rescan can fetch them again.
	 */
	char	  **plan_keys;
	size_t	   *plan_keylens;
	int			n_plan_keys;

	const char *pattern;		/* SCAN MATCH glob, or NULL */

	char	   *cursor;			/* SCAN cursor; NULL once this NODE is done */
	bool		started;

	/*
	 * Keys the server said live somewhere else, and how many more passes may
	 * be spent chasing them.
	 *
	 * A redirect is not a row that is absent, it is a row that moved, so it
	 * may never be skipped. Collected here and re-fetched after the fan-out
	 * finishes, with the map refreshed - which reuses the keyed page loop
	 * rather than unwinding a batch that still has replies in flight. The
	 * budget is what stops two nodes redirecting at each other forever.
	 */
	char	  **redirected;
	size_t	   *redirected_lens;
	int			n_redirected;
	int			redirect_passes;
	bool		retry_pass;

	bool		fanout;

	/*
	 * The map this pass routes by, captured when the pass began.
	 *
	 * NOT read live. A redirect refreshes the map mid-pass, and routing off
	 * the live one then offers the same key to two different nodes - the one
	 * the stale map named and the one the fresh map names - so it is fetched
	 * twice and the scan returns a duplicate row. Measured: a migrated key
	 * came back twice. Same class as copying the node list, one level down.
	 */
	const VfdwSlotMap *pass_map;

	/*
	 * Cluster fan-out. SCAN is per node, so a keyspace scan visits every
	 * primary in turn - see src/vfdw_scan_cluster.c and invariant I5, which
	 * is why `cursor == NULL` above says "this node" rather than "the scan".
	 *
	 * The node list is a COPY: the slot map is a cache that another statement
	 * may reload, and a scan halfway through must not have it move.
	 *
	 * The identity is kept so a node change can take the next connection. An
	 * Oid rather than the struct, which belongs to a context a scan outlives.
	 */
	VfdwClusterNode *nodes;
	int			nnodes;
	int			cur_node;
	Oid			serverid;

	/* Keys of the current page whose values are in flight. */
	char	  **keys;
	size_t	   *keylens;
	int			nkeys;
	int			next_key;

	/*
	 * A list, set or zset key holds many members, and each becomes a row. The
	 * reply is held across Iterate calls while its members are emitted; the
	 * batch owns it until the next take, which is exactly the lifetime needed.
	 */
	valkeyReply *cur_reply;
	int			cur_elem;
	const char *cur_key;
	size_t		cur_keylen;

	int64		skipped;		/* keys that vanished or held another type */

	/* The overlay's tail and seen-set; see src/vfdw_scan_overlay.c. */
	CommandId	overlay_cid;
	bool		overlay_tail;	/* the server's keys are exhausted */
	VfdwOverlayIter overlay_iter;
	HTAB	   *seen;

	/*
	 * SCAN/SSCAN round trips issued. Reported by EXPLAIN (ANALYZE) because it
	 * is the only externally visible consequence of the scan_count option: a
	 * suite that asserts only the row count reads the same number at every
	 * page size, so deleting the line that reads the option would change
	 * nothing it can see.
	 */
	int64		pages;
} VfdwScanState;

/*
 * Send the value fetches for the keys now in state->keys.
 *
 * Shared with the fan-out, which fills that array per node.
 */
extern void vfdw_scan_queue_page(VfdwScanState *state);

/*
 * Take one key's replies off the batch and return the one the tuple is built
 * from. Declared beside the queuer and defined beside it, because the order it
 * takes them in is only correct while it matches the order that queued them.
 */
extern valkeyReply *vfdw_scan_take_replies(VfdwScanState *state);

extern void vfdw_scan_seen_reset(VfdwScanState *state);

#endif							/* VFDW_SCAN_INTERNAL_H */
