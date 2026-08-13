/*-------------------------------------------------------------------------
 *
 * vfdw_conn.h
 *		Connection pooling and transaction integration.
 *
 * The obvious arrangement opens a connection per statement phase - one during
 * planning, one for the scan, one for the modify - and closes none of them on
 * an error path. Because a raw socket belongs to no ResourceOwner, an aborted
 * statement leaks its file descriptor outright, and a loop of failing
 * statements exhausts the backend.
 *
 * Here a connection is opened once per (server, user) and lives for the
 * backend. That bounds the descriptor count by the number of distinct
 * mappings rather than by the number of statements, and removes the
 * per-statement connect, AUTH and SELECT round trips entirely.
 *
 * Transaction integration is by xact callback rather than by ResourceOwner:
 * connections deliberately outlive transactions. What a transaction end has
 * to guarantee is that no connection is left mid-conversation - an aborted
 * statement can leave unread replies queued, and handing that connection to
 * the next statement would pair its replies with the wrong requests.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_conn.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_CONN_H
#define VFDW_CONN_H

#include "vfdw.h"

#include <valkey/valkey.h>

#include "access/xact.h"
#include "foreign/foreign.h"

#include "vfdw_option.h"

typedef struct VfdwConn VfdwConn;

/*
 * Fetch the pooled connection for this server and user, opening it if needed.
 *
 * The returned handle is owned by the pool. Callers must not close it.
 */
extern VfdwConn *vfdw_get_connection(ForeignServer *server, UserMapping *user);

/*
 * As above, from a caller that can cope with a cluster.
 *
 * vfdw_get_connection REFUSES a server with cluster 'true', and that refusal
 * is the safe default rather than a leftover: an unrouted scan against a
 * cluster does not fail, it silently returns whatever keys the node it
 * reached happens to hold. Only code that routes may use this, and Phase 4
 * moves the read and write paths onto it one slice at a time.
 */
extern VfdwConn *vfdw_get_connection_cluster(ForeignServer *server,
											 UserMapping *user);

/*
 * A pooled connection to ONE named cluster node.
 *
 * The endpoint comes from the slot map and overrides the server's configured
 * host and port. Pooling is per node, so a fan-out scan across three primaries
 * finds its three connections again on the next statement instead of
 * reconnecting - and a slot repointed at a different node closes its socket
 * first, because reusing it would silently talk to the wrong peer.
 */
extern VfdwConn *vfdw_get_connection_node(ForeignServer *server,
										  UserMapping *user,
										  const char *node_host,
										  int node_port);

/*
 * Give a leased connection back.
 *
 * A reader holds one for the whole of its scan, because a valkeyContext is a
 * single reply stream and two pipelining readers on one context take each
 * other's replies. Not needed on an error path: the transaction callback
 * releases whatever is still held.
 */
extern void vfdw_release_connection(VfdwConn *vconn);

extern valkeyContext *vfdw_conn_context(VfdwConn *vconn);
extern const VfdwServerOptions *vfdw_conn_options(VfdwConn *vconn);
extern bool vfdw_conn_is_resp3(VfdwConn *vconn);

/* Whether this server was declared cluster 'true'. */
extern bool vfdw_conn_is_cluster(const VfdwConn *vconn);

/*
 * Bracket a conversation.
 *
 * Between begin and end the connection is "in conversation": commands may
 * have been sent whose replies are not yet consumed. If the transaction
 * aborts in that window the connection is discarded rather than reused,
 * because its reply stream is at an unknown offset.
 */
extern void vfdw_conn_begin(VfdwConn *vconn);
extern void vfdw_conn_end(VfdwConn *vconn);

/* Select the logical database, if it differs from the one already selected. */
extern void vfdw_conn_select_db(VfdwConn *vconn, int database);

/*
 * The transaction handlers, exported rather than registered.
 *
 * src/vfdw_xact.c owns the single RegisterXactCallback/RegisterSubXactCallback
 * site and calls these in an order it writes down, because RegisterXactCallback
 * prepends and CallXactCallbacks walks head-first: two independently
 * registered callbacks would run in whichever order their lazy registrations
 * happened to occur, which here depends on whether the backend's first
 * valkey_fdw statement was a SELECT or an INSERT.
 *
 * vfdw_conn_xact discards any connection left mid-conversation. On commit
 * that should never happen and means a caller forgot its vfdw_conn_end; on
 * abort it is the normal case, because the error unwound past the point where
 * replies would have been drained. Either way the reply stream is at an
 * unknown offset and the connection cannot be handed on.
 *
 * vfdw_conn_subxact filters events for itself.
 */
extern void vfdw_conn_xact(XactEvent event);
extern void vfdw_conn_subxact(SubXactEvent event, SubTransactionId mySubid,
							  SubTransactionId parentSubid);

/* Diagnostics, used by the test suite to assert pooling behaviour. */
extern int	vfdw_conn_pool_size(void);
extern int64 vfdw_conn_open_count(void);
extern int	vfdw_conn_leased_count(void);

/*
 * A pooled connection that is mid-conversation, or NULL. Looks only: never
 * creates, opens, closes or reads options, so it is safe to call from
 * pre-commit before anything has been acquired. See the comment on the
 * definition for why vfdw_get_connection cannot be used for this.
 */
extern VfdwConn *vfdw_conn_peek(Oid serverid, Oid userid);

/*
 * The identity behind a connection.
 *
 * The pool keys on these, and so does anything else cached per identity - the
 * cluster slot map is the first. Exposed as accessors rather than by making
 * the struct public: the pool's internals have stayed private through three
 * phases and one cache is not a reason to open them.
 */
extern Oid	vfdw_conn_serverid(const VfdwConn *vconn);
extern Oid	vfdw_conn_userid(const VfdwConn *vconn);

/* Whether this connection has been told our script, and setting that. */
extern bool vfdw_conn_script_loaded(const VfdwConn *vconn);
extern void vfdw_conn_set_script_loaded(VfdwConn *vconn, bool loaded);

#endif							/* VFDW_CONN_H */
