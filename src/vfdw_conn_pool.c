/*-------------------------------------------------------------------------
 *
 * vfdw_conn_pool.c
 *		Choosing which pooled entry serves a request.
 *
 * Split from src/vfdw_conn.c when allocation became node-aware. The pool keys
 * on (serverid, userid, slot), where `slot` is a connection index rather than
 * a cluster hash slot - a collision of words that predates the cluster work.
 * Node identity could not join that key without hashing a string, so it lives
 * on the entry and the search here matches on it instead.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_conn_pool.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "vfdw.h"

#include "access/xact.h"
#include "utils/hsearch.h"

#include "vfdw_conn_internal.h"

/*
 * Fill a brand-new pool entry.
 *
 * hash_search(HASH_ENTER) writes only the key, and vfdw_conn_close never runs
 * for an entry that has never been opened - so a field cleared only in close
 * is read uninitialised the first time this identity is used.
 */
static void
vfdw_conn_init_entry(VfdwConn *vconn)
{
	/* hash_search only fills the key; the rest is ours to initialise. */
	vconn->conn = NULL;
	vconn->resp3 = false;
	vconn->database = -1;
	vconn->in_conversation = false;
	vconn->invalidated = false;
	vconn->leased = false;
	vconn->lease_subid = InvalidSubTransactionId;

	/*
	 * Initialised HERE as well as cleared in vfdw_conn_close, and
	 * both are needed. hash_search(HASH_ENTER) fills only the key,
	 * and close never runs for a brand-new entry - so a field cleared
	 * only in close is read uninitialised by the first flush against
	 * a new (serverid, userid), which would skip a SCRIPT LOAD the
	 * server has never seen.
	 */
	vconn->script_loaded = false;
	vconn->node_host[0] = '\0';
	vconn->node_port = 0;
}

/*
 * Does this entry already point at the node being asked for?
 *
 * NULL host means "the server's own configured endpoint", which is every
 * non-cluster caller and the cluster seed. That case must match an entry whose
 * node_host is empty, or a plain server would repoint - and close - its one
 * connection on every call.
 */
static bool
vfdw_conn_node_matches(const VfdwConn *vconn, const char *host, int port)
{
	if (host == NULL)
		return vconn->node_host[0] == '\0';

	return vconn->node_port == port &&
		strncmp(vconn->node_host, host, VFDW_MAX_NODE_HOST) == 0;
}

/*
 * The first connection for this identity that nobody is reading from.
 *
 * Slot 0 answers every sequential statement, so the common case is still one
 * connection reused for the life of the backend. A further slot is opened
 * only while an earlier reader still holds one, which happens when the
 * planner puts two foreign scans of the same server in one plan.
 */
/*
 * Point a free entry at this node and lease it.
 *
 * Repointing at a DIFFERENT node closes the socket first. Reusing it would
 * leave a connection whose peer is not the one the caller asked for, which is
 * a wrong answer rather than an error - exactly the failure mode routing
 * exists to prevent. An entry with nothing open makes the close a no-op.
 */
static VfdwConn *
vfdw_conn_claim(VfdwConn *vconn, const char *node_host, int node_port)
{
	vfdw_conn_close(vconn);

	if (node_host == NULL)
		vconn->node_host[0] = '\0';
	else
		strlcpy(vconn->node_host, node_host, VFDW_MAX_NODE_HOST);
	vconn->node_port = node_port;

	vconn->leased = true;
	vconn->lease_subid = GetCurrentSubTransactionId();
	return vconn;
}

/*
 * Out of connection slots for one identity.
 *
 * Its own function so the search above stays readable, and because the wording
 * has to name what a user can actually do about it. A cluster reaches this
 * sooner than a plain server does: a fan-out scan holds one connection per
 * primary, so the ceiling is slots divided by primaries.
 */
static void
vfdw_conn_no_slots(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
			 errmsg("too many concurrent Valkey scans on one server"),
			 errdetail("A reader holds a connection for the whole of its scan, "
					   "and %d are already in use for this server and user.",
					   VFDW_MAX_CONN_SLOTS),
			 errhint("Reduce the number of foreign tables from this server in "
					 "one query, or allow the planner to materialise them.")));
	pg_unreachable();
}

VfdwConn *
vfdw_conn_take_free_slot(Oid serverid, Oid userid, const char *node_host,
						 int node_port, bool *found)
{
	VfdwConnKey key;
	VfdwConn   *vconn;
	VfdwConn   *virgin = NULL;
	VfdwConn   *victim = NULL;

	memset(&key, 0, sizeof(key));
	key.serverid = serverid;
	key.userid = userid;

	for (key.slot = 0; key.slot < VFDW_MAX_CONN_SLOTS; key.slot++)
	{
		vconn = (VfdwConn *) hash_search(vfdw_conn_pool, &key, HASH_ENTER, found);

		if (!*found)
			vfdw_conn_init_entry(vconn);

		if (vconn->leased)
			continue;

		/*
		 * An entry already pointing at this node is reused as it stands,
		 * which is the whole point of pooling: a fan-out scan across three
		 * primaries must find its three connections again on the next
		 * statement rather than reconnecting.
		 */
		if (vfdw_conn_node_matches(vconn, node_host, node_port))
		{
			vconn->leased = true;
			vconn->lease_subid = GetCurrentSubTransactionId();
			return vconn;
		}

		/*
		 * Otherwise remember it, preferring one with NOTHING OPEN.
		 *
		 * The distinction is what makes pooling work across nodes at all. A
		 * fresh entry matches no particular node, so without it every node
		 * falls back to the same first free slot and evicts the previous
		 * node's connection - three primaries thrash one slot and reconnect
		 * on every pass. Measured: a second visit to three nodes opened three
		 * more sockets, which is what caught this.
		 */
		if (vconn->conn == NULL)
		{
			if (virgin == NULL)
				virgin = vconn;
		}
		else if (victim == NULL)
			victim = vconn;
	}

	vconn = virgin != NULL ? virgin : victim;
	if (vconn != NULL)
		return vfdw_conn_claim(vconn, node_host, node_port);

	vfdw_conn_no_slots();
	pg_unreachable();
}
