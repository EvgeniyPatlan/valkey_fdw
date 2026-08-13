/*-------------------------------------------------------------------------
 *
 * vfdw_cluster.c
 *		Discovering which node owns which hash slot.
 *
 * See src/vfdw_cluster.h for invariant I8, which governs everything here: the
 * map is a cache and the server is the authority.
 *
 * The parse is written against BOTH protocol shapes on purpose. CLUSTER
 * SHARDS answers with a flat alternating array under RESP2 and with a map
 * under RESP3, and libvalkey reports the map as elements alternating key and
 * value - the same layout. So one pairwise walk serves both, exactly as
 * vfdw_row.c does for HGETALL. Reading one as the other is how a wrapper ends
 * up with an empty map against a perfectly healthy cluster.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_cluster.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "vfdw.h"

#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "vfdw_cluster.h"
#include "vfdw_cmd.h"
#include "vfdw_error.h"
#include "vfdw_val.h"

typedef struct VfdwMapKey
{
	Oid			serverid;
	Oid			userid;
} VfdwMapKey;

typedef struct VfdwMapEntry
{
	VfdwMapKey	key;			/* hash key; must be first */
	VfdwSlotMap *map;			/* NULL when invalidated */
} VfdwMapEntry;

static HTAB *vfdw_cluster_maps = NULL;
static MemoryContext vfdw_cluster_ctx = NULL;

/*
 * One aggregate reply's value for a named field.
 *
 * Works for a RESP2 flat array and a RESP3 map alike, because libvalkey lays
 * both out as alternating key and value. Returns NULL when the field is
 * absent, which is not an error: CLUSTER SHARDS gained fields between
 * versions and will gain more.
 *
 * The pair walk is indexed from the server's own element count, so children
 * are taken through vfdw_reply_child rather than read out of element[]
 * directly, where a miscounted reply would be a heap overflow. An absent field
 * is a NULL return; an element the reply declared and did not carry is not a
 * field question at all.
 */
static const valkeyReply *
vfdw_cluster_field(const valkeyReply *agg, const char *name)
{
	size_t		i;

	if (agg == NULL ||
		(agg->type != VALKEY_REPLY_ARRAY && agg->type != VALKEY_REPLY_MAP))
		return NULL;

	for (i = 0; i + 1 < agg->elements; i += 2)
	{
		const valkeyReply *k = vfdw_reply_child(agg, i);

		if (k->str == NULL)
			continue;
		/*
		 * strncmp against the reply's own length, never strlen on it:
		 * invariant I3. The field names are our literals, so their length is
		 * known at compile time and the comparison is bounded by both.
		 */
		if (k->len == strlen(name) && strncmp(k->str, name, k->len) == 0)
			return vfdw_reply_child(agg, i + 1);
	}
	return NULL;
}

/*
 * Add one primary to the map, returning its node index.
 *
 * A node already present is reused rather than duplicated, so the node array
 * is the set of primaries and an owner index means one thing.
 */
static int
vfdw_cluster_intern(VfdwSlotMap *map, const char *host, size_t hostlen,
					int port)
{
	int			i;

	for (i = 0; i < map->nnodes; i++)
		if (map->nodes[i].port == port &&
			strlen(map->nodes[i].host) == hostlen &&
			strncmp(map->nodes[i].host, host, hostlen) == 0)
			return i;

	map->nodes = repalloc(map->nodes,
						  sizeof(VfdwClusterNode) * (map->nnodes + 1));
	/* Copied: the reply is freed long before the map is. Invariant I2. */
	map->nodes[map->nnodes].host = pnstrdup(host, hostlen);
	map->nodes[map->nnodes].port = port;
	return map->nnodes++;
}

/*
 * The primary of one shard, or -1 if the shard reports none.
 *
 * A shard whose primary is down has replicas and no master, and that is a
 * cluster state rather than a parse failure - its slots stay unowned and the
 * server will say MOVED when we guess.
 */
static int
vfdw_cluster_shard_primary(VfdwSlotMap *map, const valkeyReply *shard)
{
	const valkeyReply *nodes = vfdw_cluster_field(shard, "nodes");
	size_t		i;

	if (nodes == NULL || nodes->type != VALKEY_REPLY_ARRAY)
		return -1;

	for (i = 0; i < nodes->elements; i++)
	{
		const valkeyReply *node = vfdw_reply_child(nodes, i);
		const valkeyReply *role = vfdw_cluster_field(node, "role");
		const valkeyReply *host;
		const valkeyReply *port;

		if (role == NULL || role->str == NULL ||
			role->len != 6 || strncmp(role->str, "master", 6) != 0)
			continue;

		/*
		 * endpoint is what a client should connect to and ip is where the
		 * node lives; they differ behind NAT and under announce-ip. Preferring
		 * endpoint and falling back to ip is what the server documents.
		 */
		host = vfdw_cluster_field(node, "endpoint");
		if (host == NULL || host->str == NULL || host->len == 0)
			host = vfdw_cluster_field(node, "ip");
		port = vfdw_cluster_field(node, "port");

		if (host == NULL || host->str == NULL || host->len == 0 ||
			port == NULL || port->type != VALKEY_REPLY_INTEGER)
			continue;

		return vfdw_cluster_intern(map, host->str, host->len,
								   (int) port->integer);
	}
	return -1;
}

/*
 * Assign one shard's slot ranges to its primary.
 *
 * The ranges arrive as a flat array of integers taken two at a time. A range
 * running outside 0..16383 is ignored rather than clamped: a server that says
 * something impossible should not have it quietly made plausible.
 */
static void
vfdw_cluster_apply_slots(VfdwSlotMap *map, const valkeyReply *shard, int node)
{
	const valkeyReply *slots = vfdw_cluster_field(shard, "slots");
	size_t		i;

	if (slots == NULL || slots->type != VALKEY_REPLY_ARRAY)
		return;

	for (i = 0; i + 1 < slots->elements; i += 2)
	{
		const valkeyReply *lo = vfdw_reply_child(slots, i);
		const valkeyReply *hi = vfdw_reply_child(slots, i + 1);
		int			s;

		if (lo->type != VALKEY_REPLY_INTEGER ||
			hi->type != VALKEY_REPLY_INTEGER)
			continue;
		if (lo->integer < 0 || hi->integer >= VFDW_CLUSTER_NSLOTS ||
			lo->integer > hi->integer)
			continue;

		for (s = (int) lo->integer; s <= (int) hi->integer; s++)
			map->owner[s] = (int16) node;
	}
}

/*
 * Copy a map that parsed whole into the context it will live in.
 *
 * The cluster context has backend lifetime and is never reset, so anything
 * allocated in it during a discovery that then raises stays there until the
 * backend ends - a VfdwSlotMap is 32 KB of owner[] alone, and a cluster that
 * refuses CLUSTER SHARDS is asked again on every statement. Only a finished
 * map is copied in; a partial one dies with the scratch context.
 */
static VfdwSlotMap *
vfdw_cluster_install(const VfdwSlotMap *built)
{
	MemoryContext old = MemoryContextSwitchTo(vfdw_cluster_ctx);
	VfdwSlotMap *map = (VfdwSlotMap *) palloc(sizeof(VfdwSlotMap));
	int			i;

	memcpy(map, built, sizeof(VfdwSlotMap));

	/*
	 * The memcpy above brought the node array's POINTER, which still refers to
	 * the scratch context the build happened in; leaving it would hand every
	 * later route a pointer into memory freed the moment discovery returned.
	 * The array is reallocated and the hosts copied even when there are none,
	 * so nodes always refers into the map's own context - a cluster reporting
	 * no primary at all is a state and not a failure (I8).
	 */
	map->nodes = (VfdwClusterNode *)
		palloc(sizeof(VfdwClusterNode) * Max(built->nnodes, 1));
	for (i = 0; i < built->nnodes; i++)
	{
		map->nodes[i].host = pstrdup(built->nodes[i].host);
		map->nodes[i].port = built->nodes[i].port;
	}

	MemoryContextSwitchTo(old);
	return map;
}

/*
 * Ask the server and build a map from the answer.
 *
 * Built in a scratch context and only then copied into the map's own, so a
 * failed or partial parse leaves the previous map in place rather than a
 * half-built one, and leaves nothing at all in the context that never gets
 * reset. A wrong map is worse than a stale map: stale gets corrected by
 * MOVED, wrong sends writes somewhere confidently.
 *
 * Nothing is freed on the way out (invariant I1): a raise anywhere below
 * unwinds past the scratch context, which is a child of the caller's and goes
 * with it.
 */
static VfdwSlotMap *
vfdw_cluster_load(VfdwConn *vconn)
{
	MemoryContext scratch;
	MemoryContext old;
	VfdwBatch  *batch;
	VfdwCmd		cmd;
	valkeyReply *reply;
	VfdwSlotMap *map;
	VfdwSlotMap *installed;
	size_t		i;

	scratch = AllocSetContextCreate(CurrentMemoryContext, "vfdw cluster scan",
									ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(scratch);

	map = (VfdwSlotMap *) palloc0(sizeof(VfdwSlotMap));
	map->nodes = (VfdwClusterNode *) palloc(sizeof(VfdwClusterNode));
	map->nnodes = 0;
	memset(map->owner, -1, sizeof(map->owner));

	batch = vfdw_batch_begin(vconn, scratch);
	vfdw_cmd_init(&cmd, scratch, 2);
	vfdw_cmd_add_cstr(&cmd, "CLUSTER");
	vfdw_cmd_add_cstr(&cmd, "SHARDS");
	vfdw_batch_add(batch, &cmd);

	reply = vfdw_batch_next(batch);
	vfdw_reply_expect(reply, VFDW_RTYPE(VALKEY_REPLY_ARRAY),
					  "could not read the cluster's slot map");

	for (i = 0; i < reply->elements; i++)
	{
		const valkeyReply *shard = vfdw_reply_child(reply, i);
		int			node = vfdw_cluster_shard_primary(map, shard);

		if (node >= 0)
			vfdw_cluster_apply_slots(map, shard, node);
	}

	MemoryContextSwitchTo(old);

	/*
	 * The batch is closed before the map is installed, so that draining it -
	 * the last thing here that can raise - happens while a failure still
	 * costs nothing. After the install a raise would abandon a whole map in
	 * the context that is never reset.
	 */
	vfdw_batch_end(batch);
	installed = vfdw_cluster_install(map);
	MemoryContextDelete(scratch);
	return installed;
}

static VfdwMapEntry *
vfdw_cluster_entry(Oid serverid, Oid userid, bool create, bool *found)
{
	VfdwMapKey	key;
	VfdwMapEntry *entry;

	if (vfdw_cluster_maps == NULL)
	{
		HASHCTL		ctl;

		vfdw_cluster_ctx = AllocSetContextCreate(TopMemoryContext,
												 "vfdw cluster maps",
												 ALLOCSET_START_SMALL_SIZES);
		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(VfdwMapKey);
		ctl.entrysize = sizeof(VfdwMapEntry);
		ctl.hcxt = vfdw_cluster_ctx;
		vfdw_cluster_maps = hash_create("valkey_fdw cluster maps", 8, &ctl,
										HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	MemSet(&key, 0, sizeof(key));
	key.serverid = serverid;
	key.userid = userid;

	entry = (VfdwMapEntry *) hash_search(vfdw_cluster_maps, &key,
										 create ? HASH_ENTER : HASH_FIND,
										 found);

	/*
	 * Cleared here, from the flag dynahash itself set, and never from the
	 * field being cleared. hash_search writes the key into a new entry and
	 * nothing else, so map holds whatever the freelist chunk last held;
	 * asking "is map non-NULL" to decide whether map needs initialising reads
	 * exactly those bytes, and a chunk whose residue happens to be non-zero
	 * then passes for a loaded map. The load is skipped and the residue is
	 * returned to vfdw_cluster_route, which dereferences it as
	 * map->owner[slot] and map->nodes[owner].
	 *
	 * NULL is also the invalidated state (invariant I8), so an entry that
	 * starts NULL is an entry that loads on first use, which is what every
	 * caller of vfdw_cluster_map already expects.
	 */
	if (create && !*found)
		entry->map = NULL;

	return entry;
}

const VfdwSlotMap *
vfdw_cluster_map(VfdwConn *vconn)
{
	Oid			serverid = vfdw_conn_serverid(vconn);
	Oid			userid = vfdw_conn_userid(vconn);
	VfdwMapEntry *entry;
	bool		found;

	entry = vfdw_cluster_entry(serverid, userid, true, &found);

	if (entry->map == NULL)
		entry->map = vfdw_cluster_load(vconn);

	return entry->map;
}

void
vfdw_cluster_invalidate(Oid serverid, Oid userid)
{
	VfdwMapEntry *entry;
	bool		found;

	if (vfdw_cluster_maps == NULL)
		return;

	entry = vfdw_cluster_entry(serverid, userid, false, &found);
	if (entry != NULL)
	{
		/*
		 * Dropped rather than reloaded here. The caller is usually handling a
		 * MOVED mid-command, which is the worst moment to issue another
		 * command on the same connection - the reply it is holding is not
		 * finished with.
		 *
		 * AND NOT FREED. A scan captures the map for the duration of a pass
		 * so its routing cannot change underneath it, and freeing here would
		 * leave that capture dangling - a use-after-free reachable by nothing
		 * more exotic than a slot moving while a query runs. The retired map
		 * stays in the cluster context until the backend ends; one is
		 * abandoned per reshard event observed, which is rare and small
		 * beside the cost of getting this wrong.
		 */
		entry->map = NULL;
	}
}

const VfdwClusterNode *
vfdw_cluster_route(const VfdwSlotMap *map, const char *key, size_t keylen)
{
	uint16		slot;
	int16		owner;

	if (map == NULL)
		return NULL;

	slot = vfdw_val_slot(key, keylen);
	owner = map->owner[slot];

	return owner < 0 ? NULL : &map->nodes[owner];
}

/*
 * Is this reply the server telling us the key is somewhere else?
 *
 * MOVED means the slot has a new owner and our map is stale. ASK means the
 * slot is mid-migration and this ONE key has already moved, without the
 * ownership having changed - so a MOVED must refresh the map and an ASK must
 * not, or a scan would rewrite its whole routing table from a transient
 * state.
 *
 * Both arrive as ordinary error replies, which is precisely the danger: the
 * scan treats an error on a value fetch as a key that vanished or held
 * another type, and skips it. That is right for WRONGTYPE and catastrophic
 * for a redirect - the row is not absent, it is elsewhere, and skipping it
 * loses data silently. Nothing here may be folded into that path.
 */
bool
vfdw_cluster_is_redirect(const valkeyReply *reply)
{
	if (reply == NULL || reply->type != VALKEY_REPLY_ERROR ||
		reply->str == NULL)
		return false;

	return (reply->len > 6 && strncmp(reply->str, "MOVED ", 6) == 0) ||
		(reply->len > 4 && strncmp(reply->str, "ASK ", 4) == 0);
}

bool
vfdw_cluster_redirect_is_ask(const valkeyReply *reply)
{
	return reply != NULL && reply->type == VALKEY_REPLY_ERROR &&
		reply->str != NULL && reply->len > 4 &&
		strncmp(reply->str, "ASK ", 4) == 0;
}
