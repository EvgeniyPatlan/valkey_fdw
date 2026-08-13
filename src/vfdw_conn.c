/*-------------------------------------------------------------------------
 *
 * vfdw_conn.c
 *		Connection pooling and transaction integration.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_conn.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_conn_internal.h"

#include "access/xact.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_user_mapping.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#include "vfdw_error.h"
#include "vfdw_io.h"
#include "vfdw_script.h"
#include "vfdw_tls.h"
#include "vfdw_xact.h"

HTAB	   *vfdw_conn_pool = NULL;
static bool vfdw_conn_callbacks_registered = false;
static int64 vfdw_conn_opens = 0;

static void vfdw_conn_inval_callback(Datum arg, int cacheid, uint32 hashvalue);

/*
 * Close a pooled connection, leaving the entry in place so its options and
 * key survive. Reopening is cheap relative to getting reuse wrong.
 */
void
vfdw_conn_close(VfdwConn *vconn)
{
	if (vconn->conn != NULL)
	{
		valkeyFree(vconn->conn);
		vconn->conn = NULL;
	}
	vconn->in_conversation = false;
	vconn->resp3 = false;
	vconn->database = -1;

	/* A new server has never seen our script, nor said if it will take it. */
	vconn->script_loaded = false;
	vconn->write_refused = false;
	vconn->write_refusal_len = 0;
}

static void
vfdw_conn_init_pool(void)
{
	HASHCTL		ctl;

	if (vfdw_conn_pool != NULL)
		return;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(VfdwConnKey);
	ctl.entrysize = sizeof(VfdwConn);
	vfdw_conn_pool = hash_create("valkey_fdw connections", 8, &ctl,
								 HASH_ELEM | HASH_BLOBS);

	if (!vfdw_conn_callbacks_registered)
	{
		/*
		 * The transaction hooks are registered by src/vfdw_xact.c, which owns
		 * the single registration site and calls vfdw_conn_xact() and
		 * vfdw_conn_subxact() in an order it writes down. Registering them
		 * here as well would put the flush and the connection sweep in
		 * whichever order the two lazy registrations happened to run in.
		 *
		 * The syscache callbacks stay: invalidation is sequenced against
		 * nothing.
		 */
		vfdw_xact_ensure_registered();

		/*
		 * ALTER SERVER or ALTER USER MAPPING must take effect without asking
		 * the user to reconnect, so a catalog change drops the affected
		 * connections.
		 */
		CacheRegisterSyscacheCallback(FOREIGNSERVEROID,
									  vfdw_conn_inval_callback, (Datum) 0);
		CacheRegisterSyscacheCallback(USERMAPPINGOID,
									  vfdw_conn_inval_callback, (Datum) 0);
		vfdw_conn_callbacks_registered = true;
	}
}

/*
 * Send one command and insist on a non-error reply.
 *
 * Used only for connection setup - AUTH, SELECT, HELLO - where a failure
 * means the connection is unusable.
 */
static valkeyReply *
vfdw_conn_command(VfdwConn *vconn, const char *what, int argc,
				  const char **argv, const size_t *arglens)
{
	TimestampTz deadline = vfdw_io_deadline(vconn->opts.command_timeout_ms);
	bool		saved = vconn->in_conversation;
	valkeyReply *reply;

	/*
	 * A reply is outstanding from here until it has been read, so the pool
	 * must not believe this connection is quiescent in between. Marking it
	 * here rather than at each call site is deliberate: SELECT used to be
	 * sent after vfdw_get_connection had already cleared the flag, so a
	 * cancel or a timeout mid-command left the connection in the pool with an
	 * unread reply, and the next statement received it.
	 *
	 * The restore is not in a PG_FINALLY, and must not be. An error path
	 * longjmps past it, leaving the flag set so the transaction callback
	 * discards the connection - which is exactly right, and is invariant I1:
	 * nothing is cleaned up on the way out of a failure. Saving and restoring
	 * rather than clearing lets this nest inside vfdw_conn_open, which holds
	 * the flag across the whole AUTH/HELLO/SELECT setup.
	 */
	vconn->in_conversation = true;

	if (valkeyAppendCommandArgv(vconn->conn, argc, argv, arglens) != VALKEY_OK)
		vfdw_error_from_context(vconn->conn, what);

	vfdw_io_flush(vconn->conn, deadline);
	reply = vfdw_io_get_reply(vconn->conn, deadline);

	if (vfdw_reply_is_error(reply))
		vfdw_error_from_reply_free(reply, what);

	vconn->in_conversation = saved;
	return reply;
}

/*
 * Force a lazily-negotiated TLS handshake to completion.
 *
 * On a non-blocking context libvalkey returns success from the TLS setup even
 * when SSL_connect reported WANT_READ or WANT_WRITE; the handshake finishes
 * during the first real read or write. Without this round trip a rejected
 * certificate would surface as whatever command happened to run next -
 * "authentication failed", say - which sends the user looking in the wrong
 * place entirely.
 */
static void
vfdw_conn_force_handshake(VfdwConn *vconn)
{
	const char *argv[] = {"PING"};
	const size_t arglens[] = {4};

	PG_TRY();
	{
		freeReplyObject(vfdw_conn_command(vconn, "TLS handshake with Valkey failed",
										  1, argv, arglens));
	}
	PG_CATCH();
	{
		char	   *reason = vfdw_tls_take_error();
		const char *hint;
		const char *verify = vfdw_tls_verify_reason(&hint);

		/*
		 * The connection belongs to the pool entry already, so it is not
		 * released here; the transaction callback finds it mid-conversation
		 * and closes it (invariant I1).
		 *
		 * If OpenSSL recorded a reason, replace the I/O-shaped error with it.
		 * "could not send command to Valkey / Success" tells a user nothing;
		 * "certificate verify failed" tells them their certificate does not
		 * name the host they asked for.
		 *
		 * And name the check that refused it. "certificate verify failed" is
		 * one string for a wrong hostname, an untrusted CA and an expired
		 * certificate; without the specific result the three are
		 * indistinguishable to a user and to a test, so a regression that
		 * disabled expiry checking while the CA happened to be untrusted would
		 * be invisible.
		 */
		if (reason != NULL)
		{
			FlushErrorState();
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("TLS handshake with Valkey failed"),
					 verify != NULL
					 ? errdetail("%s: %s", reason, verify)
					 : errdetail("%s", reason),
					 errhint("%s", hint)));
		}

		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Authenticate.
 *
 * Two-argument AUTH when a username is configured, which is what Valkey 6+
 * ACL users need; one-argument otherwise.
 */
static void
vfdw_conn_authenticate(VfdwConn *vconn, const VfdwUserOptions *user)
{
	const char *argv[3];
	size_t		arglens[3];
	int			argc = 0;

	if (user->password == NULL)
		return;

	argv[argc] = "AUTH";
	arglens[argc++] = 4;

	if (user->username != NULL)
	{
		argv[argc] = user->username;
		arglens[argc++] = strlen(user->username);
	}

	argv[argc] = user->password;
	arglens[argc++] = strlen(user->password);

	/*
	 * Deliberately not naming the credential in the failure message: it would
	 * end up in the server log and in the client's error output.
	 */
	freeReplyObject(vfdw_conn_command(vconn, "authentication to Valkey failed",
									  argc, argv, arglens));
}

/*
 * Is this error reply a server declining RESP3, as opposed to HELLO failing?
 *
 * Exactly two answers mean "cannot", and both leave a usable connection:
 * NOPROTO from a server that knows HELLO and will not raise the protocol, and
 * an unknown-command error from one older than 6.0 that has no HELLO at all.
 * The distinction has to be drawn on the reply because everything else a
 * server can say here - NOPERM, LOADING, MISCONF - means the setup did not
 * happen, which is not the same event and must not share its outcome.
 */
static bool
vfdw_conn_resp3_declined(const valkeyReply *reply)
{
	static const char unknown[] = "unknown command";
	const size_t unknownlen = sizeof(unknown) - 1;
	size_t		i;

	if (vfdw_reply_has_prefix(reply, "NOPROTO"))
		return true;

	if (!vfdw_reply_has_prefix(reply, "ERR"))
		return false;

	/*
	 * Searched rather than compared, because the server names the arguments
	 * it did not understand after the phrase. Bounded by reply->len and not
	 * by a terminator (I3): the body is a byte range, and the sender chose
	 * its contents.
	 */
	for (i = 0; i + unknownlen <= reply->len; i++)
	{
		if (pg_strncasecmp(reply->str + i, unknown, unknownlen) == 0)
			return true;
	}

	return false;
}

/*
 * What a refused HELLO is actually about.
 *
 * A server whose default user is disabled answers HELLO with NOAUTH, and a
 * mapping carrying the wrong credentials draws WRONGPASS. The operator's
 * problem there is the credentials, and a message naming the protocol sends
 * them to read about RESP3 instead. The prefixes that mean the identity was
 * refused get a message about the identity; everything else is the
 * negotiation itself failing. The server's own text follows as the detail
 * either way, and the SQLSTATE comes from the same prefix.
 */
static const char *
vfdw_conn_hello_failure(const valkeyReply *reply)
{
	if (vfdw_reply_has_prefix(reply, "NOAUTH") ||
		vfdw_reply_has_prefix(reply, "WRONGPASS") ||
		vfdw_reply_has_prefix(reply, "NOPERM"))
		return "authentication to Valkey failed";

	return "could not negotiate RESP3";
}

/*
 * Negotiate RESP3, tolerating a server that does not speak it.
 *
 * RESP3 is worth asking for - HGETALL comes back as a typed map and zset
 * scores as real doubles, instead of a flat array we have to re-pair and
 * re-parse - but it must not be a hard requirement.
 */
static void
vfdw_conn_negotiate_resp3(VfdwConn *vconn)
{
	TimestampTz deadline = vfdw_io_deadline(vconn->opts.command_timeout_ms);
	const char *argv[] = {"HELLO", "3"};
	const size_t arglens[] = {5, 1};
	valkeyReply *reply;

	if (valkeyAppendCommandArgv(vconn->conn, 2, argv, arglens) != VALKEY_OK)
		vfdw_error_from_context(vconn->conn, "could not negotiate RESP3");

	vfdw_io_flush(vconn->conn, deadline);
	reply = vfdw_io_get_reply(vconn->conn, deadline);

	/*
	 * A decline leaves us on RESP2, which is not a lesser version of the same
	 * shapes but different ones - a flat alternating array where RESP3 sends
	 * a map. Every other error reply is the HELLO itself failing and is
	 * raised, because a connection whose setup was refused must not enter the
	 * pool: recorded as a downgrade, an ACL user who may not run HELLO, or a
	 * server still LOADING, would be believed to be an old server and would
	 * fail at whichever command ran next, reporting that command rather than
	 * the setup - and reporting RESP2 for a protocol nothing negotiated.
	 *
	 * A peer that goes away mid-HELLO does not arrive here at all: the send
	 * and the read both raise from the context inside vfdw_io.c, so what this
	 * sees is always a reply the server chose to send. The two are different
	 * events and the connection failure keeps its own SQLSTATE.
	 */
	if (vfdw_reply_is_error(reply) && !vfdw_conn_resp3_declined(reply))
		vfdw_error_from_reply_free(reply, vfdw_conn_hello_failure(reply));

	vconn->resp3 = (reply->type == VALKEY_REPLY_MAP);
	freeReplyObject(reply);
}

/*
 * Ask, once per connection, whether this server will take the write program.
 *
 * Why it is a SCRIPT LOAD of the real program, and why a refusal is recorded
 * rather than raised, is given over vfdw_conn_write_refusal. Only which
 * refusals count as a verdict is decided here: LOADING and BUSY mean "not
 * yet", so they leave the connection eligible to write and the flush's own
 * load meets them again. The strlen is of our own literal, not Valkey's (I3).
 */
static void
vfdw_conn_probe_script(VfdwConn *vconn)
{
	TimestampTz deadline = vfdw_io_deadline(vconn->opts.command_timeout_ms);
	const char *argv[] = {"SCRIPT", "LOAD", vfdw_script_text()};
	const size_t arglens[] = {6, 4, strlen(argv[2])};
	valkeyReply *reply;

	if (valkeyAppendCommandArgv(vconn->conn, 3, argv, arglens) != VALKEY_OK)
		vfdw_error_from_context(vconn->conn,
								"could not load the Valkey write program");

	vfdw_io_flush(vconn->conn, deadline);
	reply = vfdw_io_get_reply(vconn->conn, deadline);

	vconn->script_loaded = !vfdw_reply_is_error(reply);
	vconn->write_refused = !vconn->script_loaded &&
		!vfdw_reply_has_prefix(reply, "LOADING") &&
		!vfdw_reply_has_prefix(reply, "BUSY");
	vconn->write_refusal_len = 0;
	if (vconn->write_refused && reply->str != NULL)
	{
		vconn->write_refusal_len = (int) Min((size_t) reply->len,
											 sizeof(vconn->write_refusal));
		memcpy(vconn->write_refusal, reply->str,
			   (size_t) vconn->write_refusal_len);
	}

	freeReplyObject(reply);
}

void
vfdw_conn_select_db(VfdwConn *vconn, int database)
{
	char		dbbuf[16];
	const char *argv[2];
	size_t		arglens[2];

	if (vconn->database == database)
		return;

	if (vconn->opts.cluster)
	{
		if (database != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("Valkey Cluster supports only database 0"),
					 errdetail("The table requests database %d.", database)));
		vconn->database = 0;
		return;
	}

	snprintf(dbbuf, sizeof(dbbuf), "%d", database);
	argv[0] = "SELECT";
	arglens[0] = 6;
	argv[1] = dbbuf;
	arglens[1] = strlen(dbbuf);

	/*
	 * Forget which database this connection is on before asking it to move,
	 * not after. If the round trip is cancelled or times out, the connection
	 * is somewhere unknown; recording the new database only on success would
	 * leave the entry claiming the old one, and recording it beforehand would
	 * claim the new one. Neither is true, and a connection that lies about
	 * its database reads the wrong keyspace rather than failing.
	 *
	 * In practice the connection is discarded anyway, because vfdw_conn_command
	 * marks it mid-conversation for the duration. This is the belt to that
	 * bracing: the two together mean no surviving connection can be wrong.
	 */
	vconn->database = -1;

	freeReplyObject(vfdw_conn_command(vconn, "could not select Valkey database",
									  2, argv, arglens));
	vconn->database = database;
}

/*
 * Open the underlying connection for an entry.
 */
static void
vfdw_conn_open(VfdwConn *vconn, const VfdwUserOptions *user)
{
	valkeyContext *conn;

	Assert(vconn->conn == NULL);

	/*
	 * A node endpoint from the slot map overrides the server's own host and
	 * port, and suppresses the Unix socket: that path names one local server
	 * and cannot reach a cluster's other members.
	 */
	if (vconn->node_host[0] != '\0')
		conn = vfdw_io_connect(vconn->node_host, vconn->node_port, NULL,
							   vconn->opts.connect_timeout_ms);
	else
		conn = vfdw_io_connect(vconn->opts.host, vconn->opts.port,
							   vconn->opts.unix_socket_path,
							   vconn->opts.connect_timeout_ms);

	/*
	 * Ownership transfers to the entry immediately, before any command can
	 * fail. If setup below raises, the transaction callback finds the entry
	 * mid-conversation and closes it - nothing leaks and nothing is freed on
	 * the error path itself (invariant I1).
	 */
	vconn->conn = conn;
	vconn->database = -1;
	vconn->in_conversation = true;
	vfdw_conn_opens++;

	/*
	 * Bound the reader before anything can be read. A multi-bulk header
	 * declares how many elements follow before any of them arrive, so an
	 * unbounded reader commits to whatever the far end claims - and the far
	 * end is not always the server the operator thinks it is.
	 */
	vfdw_io_set_limits(conn, (size_t) vconn->opts.reader_buffer_bytes,
					   vconn->opts.max_reply_elements);

	if (vconn->opts.tls)
	{
		vfdw_tls_attach(conn, &vconn->opts);
		vfdw_conn_force_handshake(vconn);
	}

	vfdw_conn_authenticate(vconn, user);
	vfdw_conn_negotiate_resp3(vconn);
	vfdw_conn_probe_script(vconn);

	vconn->in_conversation = false;
}

/*
 * Refuse to borrow the server's network identity on behalf of a
 * non-superuser without a password.
 *
 * This is postgres_fdw's rule. Without it, a user granted USAGE on a foreign
 * server can reach anything the PostgreSQL process itself can reach - a
 * Valkey instance trusting the database host by address, say.
 */
static void
vfdw_conn_check_password_required(UserMapping *user,
								  const VfdwUserOptions *uopts)
{
	if (!uopts->password_required)
		return;
	if (uopts->password != NULL)
		return;
	if (superuser_arg(user->userid))
		return;

	ereport(ERROR,
			(errcode(ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED),
			 errmsg("password is required"),
			 errdetail("Non-superusers must provide a password in the user mapping."),
			 errhint("Set password_required 'false' on the user mapping to allow this; "
					 "only a superuser may do so.")));
}

/*
 * Give a connection back to the pool.
 *
 * Called when a reader finishes. Not called on an error path: a scan that
 * raised leaves its lease held, and the transaction callback releases it
 * along with everything else (invariant I1).
 */
void
vfdw_release_connection(VfdwConn *vconn)
{
	if (vconn != NULL)
	{
		vconn->leased = false;
		vconn->lease_subid = InvalidSubTransactionId;
	}
}

/*
 * The refusal that used to sit on the socket open.
 *
 * It moved here so that a CLUSTER-AWARE caller can reach a cluster while
 * every caller that is not stays refused. That direction matters: an
 * unrouted scan against a cluster does not fail, it silently returns the
 * keys of whichever node it happened to reach, and a wrong answer is worse
 * than a refusal. So the permission is opt-in per call site and each slice
 * of Phase 4 opts its own path in once that path can route.
 */
static VfdwConn *
vfdw_get_connection_internal(ForeignServer *server, UserMapping *user,
							 bool cluster_aware, const char *node_host,
							 int node_port)
{
	VfdwConn   *vconn;
	bool		found;
	VfdwUserOptions uopts;

	vfdw_conn_init_pool();

	vconn = vfdw_conn_take_free_slot(server->serverid, user->userid,
									 node_host, node_port, &found);
	if (vconn->invalidated)
	{
		vfdw_conn_close(vconn);
		vconn->invalidated = false;
	}

	vconn->server_hashvalue =
		GetSysCacheHashValue1(FOREIGNSERVEROID,
							  ObjectIdGetDatum(server->serverid));
	vconn->mapping_hashvalue =
		GetSysCacheHashValue1(USERMAPPINGOID, ObjectIdGetDatum(user->umid));

	vfdw_read_server_options(server->options, &vconn->opts);
	vfdw_read_user_options(user->options, &uopts);
	vfdw_conn_check_password_required(user, &uopts);

	if (vconn->opts.cluster && !cluster_aware)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("this operation does not support Valkey Cluster"),
				 errdetail("Scans and writes route across the cluster; this "
						   "entry point does not."),
				 errhint("Reads and single-slot writes work against a server "
						 "with cluster 'true'.")));

	if (vconn->conn == NULL)
		vfdw_conn_open(vconn, &uopts);

	return vconn;
}

VfdwConn *
vfdw_get_connection(ForeignServer *server, UserMapping *user)
{
	return vfdw_get_connection_internal(server, user, false, NULL, 0);
}

VfdwConn *
vfdw_get_connection_cluster(ForeignServer *server, UserMapping *user)
{
	return vfdw_get_connection_internal(server, user, true, NULL, 0);
}

VfdwConn *
vfdw_get_connection_node(ForeignServer *server, UserMapping *user,
						 const char *node_host, int node_port)
{
	return vfdw_get_connection_internal(server, user, true, node_host,
										node_port);
}

valkeyContext *
vfdw_conn_context(VfdwConn *vconn)
{
	return vconn->conn;
}

const VfdwServerOptions *
vfdw_conn_options(VfdwConn *vconn)
{
	return &vconn->opts;
}

bool
vfdw_conn_is_cluster(const VfdwConn *vconn)
{
	return vconn->opts.cluster;
}

Oid
vfdw_conn_serverid(const VfdwConn *vconn)
{
	return vconn->key.serverid;
}

Oid
vfdw_conn_userid(const VfdwConn *vconn)
{
	return vconn->key.userid;
}

bool
vfdw_conn_is_resp3(VfdwConn *vconn)
{
	return vconn->resp3;
}

void
vfdw_conn_begin(VfdwConn *vconn)
{
	vconn->in_conversation = true;
}

void
vfdw_conn_end(VfdwConn *vconn)
{
	vconn->in_conversation = false;
}

/*
 * A catalog change may have altered the address, credentials or timeouts
 * behind an open connection. Mark rather than close: we may be inside a
 * transaction that is still using it.
 */
static void
vfdw_conn_inval_callback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS scan;
	VfdwConn   *vconn;

	if (vfdw_conn_pool == NULL)
		return;

	Assert(cacheid == FOREIGNSERVEROID || cacheid == USERMAPPINGOID);

	hash_seq_init(&scan, vfdw_conn_pool);
	while ((vconn = (VfdwConn *) hash_seq_search(&scan)) != NULL)
	{
		if (vconn->conn == NULL)
			continue;

		/*
		 * hashvalue 0 is the "cache reset, assume everything changed" signal.
		 * Otherwise only the entries whose own server or mapping changed are
		 * affected - creating an unrelated foreign server must not cost every
		 * open connection in the backend.
		 */
		if (hashvalue == 0 ||
			(cacheid == FOREIGNSERVEROID &&
			 vconn->server_hashvalue == hashvalue) ||
			(cacheid == USERMAPPINGOID &&
			 vconn->mapping_hashvalue == hashvalue))
			vconn->invalidated = true;
	}
}

int
vfdw_conn_pool_size(void)
{
	HASH_SEQ_STATUS scan;
	VfdwConn   *vconn;
	int			open = 0;

	if (vfdw_conn_pool == NULL)
		return 0;

	hash_seq_init(&scan, vfdw_conn_pool);
	while ((vconn = (VfdwConn *) hash_seq_search(&scan)) != NULL)
	{
		if (vconn->conn != NULL)
			open++;
	}
	return open;
}

int64
vfdw_conn_open_count(void)
{
	return vfdw_conn_opens;
}

/*
 * The pooled connection for (serverid, userid), or NULL - WITHOUT creating,
 * opening, or reading options.
 *
 * The pre-commit flush needs to know whether a scan is still mid-conversation
 * before it acquires anything, and vfdw_get_connection cannot answer that: it
 * closes an invalidated connection inline, freeing a valkeyContext a live
 * scan batch still caches. A guard written in terms of its return value can
 * only run after the destructive part and turns a wrong answer into a
 * use-after-free. This is the only probe that is safe there, which is why it
 * does nothing but look.
 */
VfdwConn *
vfdw_conn_peek(Oid serverid, Oid userid)
{
	VfdwConnKey key;
	VfdwConn   *vconn;

	if (vfdw_conn_pool == NULL)
		return NULL;

	memset(&key, 0, sizeof(key));
	key.serverid = serverid;
	key.userid = userid;

	for (key.slot = 0; key.slot < VFDW_MAX_CONN_SLOTS; key.slot++)
	{
		bool		found;

		vconn = (VfdwConn *) hash_search(vfdw_conn_pool, &key, HASH_FIND,
										 &found);
		if (found && vconn->conn != NULL && vconn->in_conversation)
			return vconn;
	}
	return NULL;
}

bool
vfdw_conn_script_loaded(const VfdwConn *vconn)
{
	return vconn->script_loaded;
}

void
vfdw_conn_set_script_loaded(VfdwConn *vconn, bool loaded)
{
	vconn->script_loaded = loaded;
}

const char *
vfdw_conn_write_refusal(const VfdwConn *vconn, size_t *len)
{
	if (!vconn->write_refused)
		return NULL;
	*len = (size_t) vconn->write_refusal_len;
	return vconn->write_refusal;
}

/*
 * Slots currently leased to a reader.
 *
 * Exposed for one assertion that has no other observable: that an aborting
 * subtransaction releases the leases it took and leaves everyone else's
 * alone. Releasing a lease early is not visible in the data unless the two
 * readers happen to have commands in flight at the same instant, so a test
 * written against the rows passes while the pool is wrong.
 */
int
vfdw_conn_leased_count(void)
{
	HASH_SEQ_STATUS scan;
	VfdwConn   *vconn;
	int			leased = 0;

	if (vfdw_conn_pool == NULL)
		return 0;

	hash_seq_init(&scan, vfdw_conn_pool);
	while ((vconn = (VfdwConn *) hash_seq_search(&scan)) != NULL)
	{
		if (vconn->leased)
			leased++;
	}
	return leased;
}
