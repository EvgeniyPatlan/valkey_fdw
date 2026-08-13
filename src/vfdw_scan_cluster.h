/*-------------------------------------------------------------------------
 *
 * vfdw_scan_cluster.h
 *		Fan-out across primaries for a scan. See the .c for invariant I5.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_scan_cluster.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_SCAN_CLUSTER_H
#define VFDW_SCAN_CLUSTER_H

#include "vfdw.h"

#include "foreign/foreign.h"

#include "vfdw_cluster.h"

typedef struct VfdwScanState VfdwScanState;

extern bool vfdw_scan_cluster_setup(VfdwScanState *state,
									ForeignServer *server, UserMapping *user);
extern void vfdw_scan_cluster_attach(VfdwScanState *state,
									 ForeignServer *server, UserMapping *user,
									 int n);
extern bool vfdw_scan_cluster_advance(VfdwScanState *state,
									  ForeignServer *server,
									  UserMapping *user);
extern void vfdw_scan_cluster_keys_for_node(VfdwScanState *state);
extern bool vfdw_scan_keys_page(VfdwScanState *state);
extern void vfdw_scan_redirected(VfdwScanState *state,
								 const valkeyReply *reply,
								 const char *key, size_t keylen);
extern bool vfdw_scan_retry_pass(VfdwScanState *state);
extern void vfdw_scan_cluster_copy_nodes(VfdwScanState *state,
										 const VfdwSlotMap *map);

/* The scan's identity, refetched from syscache rather than held. */
extern ForeignServer *vfdw_scan_server(VfdwScanState *state);
extern UserMapping *vfdw_scan_user(VfdwScanState *state);

#endif							/* VFDW_SCAN_CLUSTER_H */
