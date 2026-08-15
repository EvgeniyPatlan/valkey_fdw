/*-------------------------------------------------------------------------
 *
 * vfdw_conn_internal.h
 *		The pool's own types, shared with its transaction half.
 *
 * Private to the connection pool. src/vfdw_conn.h is the interface everyone
 * else uses; this exists only because src/vfdw_conn_xact.c is the same
 * component split across two files rather than a separate consumer.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_conn_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_CONN_INTERNAL_H
#define VFDW_CONN_INTERNAL_H

#include "vfdw_conn.h"

#include "utils/hsearch.h"

/* Longest node endpoint the map may hand back; DNS caps a name at 253. */
#define VFDW_MAX_NODE_HOST 256

/*
 * How much of a refused SCRIPT LOAD is kept. A NOPERM naming a user and a
 * command fits several times over, and the far end chose these bytes: what
 * does not fit is dropped rather than believed.
 */
#define VFDW_MAX_WRITE_REFUSAL 256

/*
 * What a server has been found to support.
 *
 * Three states and not a bool, because "not asked yet" and "asked, and it
 * cannot" are different: collapsed into one false, a lazily probed capability
 * is re-probed on every use, and collapsed into one true it is never probed at
 * all. The distinction is the whole reason a lazy probe can be cached.
 */
typedef enum VfdwCap
{
	VFDW_CAP_UNKNOWN = 0,		/* not asked; must be the zero value */
	VFDW_CAP_PRESENT,
	VFDW_CAP_ABSENT
} VfdwCap;

typedef struct VfdwConnKey
{
	Oid			serverid;
	Oid			userid;

	/*
	 * Which of this identity's connections. Almost always 0: a second one
	 * exists only while two scans are reading at once, which a nested loop
	 * over two foreign tables on the same server does.
	 */
	int			slot;
} VfdwConnKey;

struct VfdwConn
{
	VfdwConnKey key;			/* hash key; must be first */

	valkeyContext *conn;		/* NULL when not currently open */

	/*
	 * Which CLUSTER node this connection is to, empty for a plain server.
	 *
	 * The pool keys on (serverid, userid, slot), and `slot` there is a
	 * connection index rather than a hash slot - an unfortunate collision of
	 * words that predates the cluster work. Node identity could not join the
	 * KEY without hashing a string, so it lives on the entry and the free-slot
	 * search matches on it instead. That also makes repointing a slot at a
	 * different node explicit: it has to close the socket first, which a hash
	 * key would have hidden.
	 *
	 * Fixed-size rather than a pointer because a hash entry outlives every
	 * context a host name could plausibly have been allocated in.
	 */
	char		node_host[VFDW_MAX_NODE_HOST];
	int			node_port;
	VfdwServerOptions opts;
	bool		resp3;
	int			database;		/* currently selected logical database */

	/*
	 * Set while commands may be outstanding. A connection abandoned in this
	 * state has an unknown number of unread replies, so it cannot be reused:
	 * the next statement's first reply would be this statement's last.
	 */
	bool		in_conversation;

	/* Dropped at end of transaction when a DDL change invalidated it. */
	bool		invalidated;

	/*
	 * Held by one reader for the whole of its scan.
	 *
	 * A valkeyContext is one reply stream, and replies come back in the order
	 * their commands were sent. Two readers pipelining onto one context
	 * therefore interleave, and each takes whichever reply arrives next -
	 * including the other's. Where the shapes differ that surfaces as a
	 * protocol error; where they agree, as in two string tables, it silently
	 * returns the wrong value. Rather than demultiplex a shared stream, a
	 * reader takes a connection to itself and gives it back when it is done.
	 */
	bool		leased;

	/* Whether this connection has been sent our Lua program (S6). */
	bool		script_loaded;

	/*
	 * Whether this server would TAKE the program, and its own words when it
	 * would not.
	 *
	 * Both are written by every vfdw_conn_open and read only through a
	 * connection that is open, so unlike script_loaded there is nothing here a
	 * brand-new entry can read uninitialised: the verdict is a property of a
	 * socket that exists, and vfdw_conn_close drops it with the socket.
	 *
	 * The reason is the server's own bytes, kept raw and length-counted (I3)
	 * in an array rather than behind a pointer for node_host's reason: a hash
	 * entry outlives every context they could have been palloc'd in. The
	 * encoding check they need before they may be reported belongs at the
	 * report (I2), where there is a context to palloc the checked copy in.
	 */
	bool		write_refused;
	int			write_refusal_len;
	char		write_refusal[VFDW_MAX_WRITE_REFUSAL];

	/*
	 * Whether this server has per-field expiry, asked the first time a table
	 * that needs it is read. See vfdw_ttl.h for why it is asked rather than
	 * read off a version.
	 *
	 * UNKNOWN until asked, and asked LAZILY rather than in vfdw_conn_open,
	 * because most tables have no ttl column and the question costs a round
	 * trip. That makes this the one capability here a new entry can read
	 * uninitialised, so unlike write_refused it is set where the entry is
	 * created as well as where the socket is - which vfdw_conn_close returns
	 * it to, since a reconnect may land on a different server.
	 */
	VfdwCap		field_ttl;

	/*
	 * Which subtransaction took the lease, so an aborting one can release what
	 * it took and nothing else. See vfdw_conn_subxact.
	 */
	SubTransactionId lease_subid;

	/*
	 * Syscache hash values for the server and user mapping behind this
	 * connection, so an invalidation can be matched to the entries it
	 * actually concerns. Without them, creating an unrelated foreign server
	 * would drop every pooled connection in the backend.
	 */
	uint32		server_hashvalue;
	uint32		mapping_hashvalue;
};

/*
 * Concurrent readers per (server, user) before the wrapper refuses. A plan
 * needing more than this is pathological, and an unbounded pool would trade a
 * wrong answer for exhausted file descriptors.
 */
#define VFDW_MAX_CONN_SLOTS 32


extern HTAB *vfdw_conn_pool;

/*
 * Drop the socket and everything that described it. Never called on the way
 * out of an error inside a conversation - see invariant I1.
 */
extern void vfdw_conn_close(VfdwConn *vconn);

/*
 * The first free entry for this identity already pointing at this node, or a
 * free one repointed at it. A NULL host means the server's own endpoint.
 */
extern VfdwConn *vfdw_conn_take_free_slot(Oid serverid, Oid userid,
										  const char *node_host,
										  int node_port, bool *found);

extern void vfdw_conn_close(VfdwConn *vconn);
extern HTAB *vfdw_conn_pool;

#endif							/* VFDW_CONN_INTERNAL_H */
