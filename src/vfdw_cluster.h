/*-------------------------------------------------------------------------
 *
 * vfdw_cluster.h
 *		The slot map: which node owns which of the 16384 hash slots.
 *
 * INVARIANT I8: THE SLOT MAP IS A CACHE, NEVER AN AUTHORITY.
 *
 * Every routing decision this map informs must be able to be wrong, because
 * during resharding it will be. The server corrects us with MOVED, and code
 * that treats the map as truth breaks silently at exactly the moment cluster
 * support is being exercised for real. Nothing here may therefore raise on a
 * slot it cannot place: an unknown owner is a reason to ask a node and be
 * redirected, not a reason to refuse.
 *
 * Discovery is CLUSTER SHARDS, which is Valkey 7.0 and later. There is
 * deliberately NO CLUSTER SLOTS fallback: no image this suite runs against is
 * old enough to need one, so the fallback would be parsing code that nothing
 * ever executes, which is the thing this project refuses to ship. A server too
 * old to answer CLUSTER SHARDS sends an error where an array was expected, and
 * the reply-shape check raises on it, carrying the server's own wording as the
 * detail. No version is examined here and none is named.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_cluster.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_CLUSTER_H
#define VFDW_CLUSTER_H

#include "vfdw.h"

#include "vfdw_conn.h"

/*
 * The cluster's slot count is fixed by the protocol, not by configuration:
 * the key hash is CRC-16 mod this number, so it is as much a constant as the
 * polynomial is.
 */
#define VFDW_CLUSTER_NSLOTS 16384

typedef struct VfdwClusterNode
{
	char	   *host;			/* in the map's own context */
	int			port;
} VfdwClusterNode;

typedef struct VfdwSlotMap
{
	VfdwClusterNode *nodes;
	int			nnodes;

	/*
	 * Index into nodes, or -1 for a slot no primary claimed.
	 *
	 * Unclaimed is a real state, not a parse failure: a cluster mid-resharding
	 * or only partly formed genuinely has slots nobody serves yet, and
	 * refusing to build a map because of it would make the wrapper unusable
	 * precisely when a user most needs to look at what is going on.
	 */
	int16		owner[VFDW_CLUSTER_NSLOTS];
} VfdwSlotMap;

/*
 * The slot map for this connection's server, loading it if it is absent or
 * has been invalidated.
 *
 * The connection is used to ask, so the caller must hold it; the map itself
 * outlives the call and belongs to the backend.
 */
extern const VfdwSlotMap *vfdw_cluster_map(VfdwConn *vconn);

/*
 * Mark the map stale, so the next vfdw_cluster_map reloads it.
 *
 * Called when the server says MOVED, which is the only thing that actually
 * proves the map wrong.
 */
extern void vfdw_cluster_invalidate(Oid serverid, Oid userid);

/*
 * Which node owns this key's slot, or NULL if nothing claims it.
 *
 * NULL is not an error; see I8 above. It means "ask someone and expect to be
 * redirected", which is what a client without a map does anyway.
 */
extern const VfdwClusterNode *vfdw_cluster_route(const VfdwSlotMap *map,
												 const char *key,
												 size_t keylen);

/*
 * Whether a reply is MOVED or ASK, and which.
 *
 * These arrive as ordinary error replies and MUST NOT be folded into the
 * scan's "an error means the key vanished or held another type" path: the row
 * is not absent, it is elsewhere, and skipping it loses data with no error.
 */
extern bool vfdw_cluster_is_redirect(const valkeyReply *reply);
extern bool vfdw_cluster_redirect_is_ask(const valkeyReply *reply);

#endif							/* VFDW_CLUSTER_H */
