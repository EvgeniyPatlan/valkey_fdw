/*-------------------------------------------------------------------------
 *
 * vfdw_flush.c
 *		Applying a transaction's writes at pre-commit, as one unit.
 *
 * PRE_COMMIT is the only legal point for this. The transaction is still
 * TRANS_INPROGRESS, the ResourceOwner and memory contexts are intact, an
 * ereport still aborts, and interrupts are NOT held - HOLD_INTERRUPTS is
 * about sixty lines later. Every other transaction event runs inside it,
 * where CHECK_FOR_INTERRUPTS is a no-op and invariant I6 cannot be met, so no
 * byte may reach the wire from any of them.
 *
 * INVARIANT I1: NOTHING IS CLEANED UP ON THE WAY OUT.
 *
 * I1 was originally written as "no PG_TRY appears in vfdw_flush.c", which was
 * a proxy for the real rule and turned out to be too strong. The rule is that
 * no resource is unwatched, freed, reset or closed before an ereport - because
 * a cleanup round trip on the way out can itself fail and destroy the real
 * diagnosis. The PG_TRY below frees nothing and closes nothing; it only
 * decides which SQLSTATE the error carries, which is a different act.
 *
 * The EVALSHA reply is batch-owned and released by the batch's
 * MemoryContextResetCallback on any unwind. The connection is never closed
 * here, on success or failure: a failure leaves in_conversation set, and
 * vfdw_conn_xact's abort branch discards the connection rather than reusing
 * one whose reply-stream offset is unknown. flush_cxt is deleted only on
 * success; on an error path it is a child of the buffer context and goes with
 * the reset in the ABORT branch.
 *
 * THE FOUR OUTCOMES
 *
 * 1. APPLIED - VFDW1 OK <n> with n equal to the plan count.
 * 2. NOT APPLIED, on positive proof only: a failure strictly before the
 *    EVALSHA reached the socket, a reply proving pre-execution rejection, or
 *    one of our phase-1 sentinels. Says so in the message.
 * 3. INDETERMINATE (08007) - any I/O failure, deadline, cancel or unexpected
 *    shape once the EVALSHA has been handed to the socket.
 * 4. PARTIALLY APPLIED (XX000) via VFDW1 INTERNAL - by construction a bug in
 *    the check set. Loud rather than silent.
 *
 * The classifier degrades toward "unknown", never toward a false "applied".
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_flush.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_cluster.h"
#include "vfdw_flush.h"
#include "vfdw_flush_cluster.h"

#include "foreign/foreign.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#include "vfdw_cmd.h"
#include "vfdw_conn.h"
#include "vfdw_error.h"
#include "vfdw_ledger.h"
#include "vfdw_script.h"
#include "vfdw_wbuf.h"

static bool vfdw_flush_flushed = false;
static uint64 vfdw_flush_ncalls = 0;
static uint64 vfdw_flush_nempty = 0;
static uint64 vfdw_flush_nflushes = 0;
static uint64 vfdw_flush_nretries = 0;

/*
 * Batches opened and closed by the flush, counted so the pair can be asserted.
 *
 * The retry path used to skip vfdw_batch_end, so a flush that went round again
 * left a batch behind - and a leak on the path taken only when a server is
 * reloading or has forgotten the program is a leak nobody meets until the day
 * that happens. Nothing went red, because the retry succeeds and the
 * transaction commits; the only visible difference is a count nobody was
 * keeping. These are that count.
 */
static uint64 vfdw_flush_batches_begun = 0;
static uint64 vfdw_flush_batches_ended = 0;

/*
 * How far the flush has got, and therefore what can honestly be said about
 * the state of Valkey if an error is raised from here.
 */
typedef enum VfdwFlushPhase
{
	VFDW_FLUSH_BEFORE_SEND = 0,
	VFDW_FLUSH_SENT,

	/*
	 * Sent, and then answered by something that PROVES it did not run - a
	 * phase-1 sentinel, or a server prefix that means pre-execution
	 * rejection. Without this the context line would say "may or may not have
	 * been applied" directly underneath a hint saying "no changes were
	 * applied", and a reader would rightly believe the more alarming of the
	 * two.
	 */
	VFDW_FLUSH_PROVEN_CLEAN
} VfdwFlushPhase;

static VfdwFlushPhase vfdw_flush_phase = VFDW_FLUSH_BEFORE_SEND;

/*
 * The context callback.
 *
 * This fires for EVERY error raised inside the region - ours, libvalkey's, a
 * query cancel, a transaction_timeout, an OOM - without a PG_TRY and without
 * intercepting anything. It is what lets a 57014 at COMMIT still tell the
 * truth about whether the write landed, which no amount of error handling on
 * our own paths could do.
 */
static void
vfdw_flush_errcontext(void *arg)
{
	if (vfdw_flush_phase == VFDW_FLUSH_SENT)
		errcontext("valkey_fdw pre-commit flush; the Valkey write may or may not have been applied.");
	else
		errcontext("valkey_fdw pre-commit flush; no changes were applied in Valkey.");
}

/* ---------------------------------------------------------------------
 * Outcomes
 * --------------------------------------------------------------------- */

/*
 * The write program's own message, as text an error may carry.
 *
 * This is user data and not diagnostics we wrote: the script's fail() puts
 * KEYS[kx] into every message it returns, and a bytea key column carries
 * arbitrary bytes by design. So invariant I2 applies to it exactly as it
 * applies to a reply body Valkey composed itself, and it goes through
 * vfdw_safe_text before it can reach the log or the wire.
 *
 * strlen on it is no I3 violation - I3 forbids strlen on Valkey-owned bytes,
 * and this is the classifier's own pnstrdup'd copy. An interior NUL ends the
 * string here exactly as it would inside errdetail's "%s", so nothing is lost
 * that was ever going to be printed.
 */
static const char *
vfdw_flush_detail(const char *detail)
{
	if (detail == NULL)
		return "(no detail)";

	return vfdw_safe_text(detail, strlen(detail));
}

static void
vfdw_flush_error_indeterminate(const char *cause)
{
	ereport(ERROR,
			(errcode(ERRCODE_TRANSACTION_RESOLUTION_UNKNOWN),
			 errmsg("the outcome of the Valkey write is unknown"),
			 errdetail("%s", cause),

	/*
	 * Three states, not two. "In full or not at all" would be a positive
	 * claim that outcome 4 contradicts: a precondition set that is
	 * incomplete can leave a unit half applied, and this message must not
	 * promise otherwise.
	 */
			 errhint("The transaction has been rolled back in PostgreSQL. The "
					 "write applied in full, applied not at all, or - if a "
					 "precondition was incomplete - applied in part. Inspect "
					 "the affected keys.")));
}

static void
vfdw_flush_error_sentinel(VfdwScriptVerdict verdict, const char *detail)
{
	int			code;
	const char *msg;

	/* Phase 1 refused, and phase 1 writes nothing. */
	vfdw_flush_phase = VFDW_FLUSH_PROVEN_CLEAN;

	switch (verdict)
	{
		case VFDW_SCRIPT_CONFLICT:
			code = ERRCODE_UNIQUE_VIOLATION;
			msg = "a key this transaction creates already exists in Valkey";
			break;
		case VFDW_SCRIPT_MISSING:
			code = ERRCODE_T_R_SERIALIZATION_FAILURE;
			msg = "a key this transaction updates was removed concurrently";
			break;
		case VFDW_SCRIPT_WRONGTYPE:
			code = ERRCODE_DATATYPE_MISMATCH;
			msg = "a key holds a different Valkey type than its table declares";
			break;
		default:
			code = ERRCODE_INTERNAL_ERROR;
			msg = "the Valkey write program rejected its own arguments";
			break;
	}

	/*
	 * Every one of these is a phase-1 refusal, and phase 1 writes nothing.
	 * That is the whole reason the script has two phases, so the message may
	 * say it without qualification.
	 */
	ereport(ERROR,
			(errcode(code),
			 errmsg("%s", msg),
			 errdetail("Valkey refused the write: %s",
					   vfdw_flush_detail(detail)),
			 errhint("No changes were applied in Valkey; the transaction has "
					 "been rolled back.")));
}

/*
 * VFDW1 INTERNAL: the unit is partially applied.
 *
 * By construction this is a bug in the check set, or an ACL change landing
 * between dispatch and a later server.call. It is reported loudly, with the
 * plan and action the server named, so the residual risk is never silent.
 */
static void
vfdw_flush_error_internal(const char *detail)
{
	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("the Valkey write was only partially applied"),
			 errdetail("The write program failed after it had begun writing: %s",
					   vfdw_flush_detail(detail)),
			 errhint("This is a valkey_fdw defect. The affected keys are named "
					 "above and are in an intermediate state; inspect them "
					 "before retrying.")));
}

/* ---------------------------------------------------------------------
 * The reply
 * --------------------------------------------------------------------- */

/*
 * Prefixes that prove the server rejected the program BEFORE executing any of
 * it. Each licenses a "nothing was applied" message; anything else, once the
 * command has been sent, is indeterminate.
 */
static bool
vfdw_flush_proves_not_applied(const valkeyReply *reply)
{
	static const char *const proofs[] = {
		"NOSCRIPT", "MOVED", "ASK", "CROSSSLOT", "NOPERM", "OOM", "TRYAGAIN",
		"LOADING", "BUSYGROUP", "READONLY", "MASTERDOWN", "NOREPLICAS"
	};
	int			i;

	for (i = 0; i < (int) lengthof(proofs); i++)
	{
		if (vfdw_reply_has_prefix(reply, proofs[i]))
			return true;
	}
	return false;
}

/*
 * TRYAGAIN and LOADING are the server saying "not now", not "no". Both prove
 * the program did not run, so retrying is safe rather than merely hopeful.
 */
static bool
vfdw_flush_is_retryable(const valkeyReply *reply)
{
	return vfdw_reply_has_prefix(reply, "TRYAGAIN") ||
		vfdw_reply_has_prefix(reply, "LOADING");
}

typedef enum VfdwFlushOutcome
{
	VFDW_FLUSH_APPLIED = 0,
	VFDW_FLUSH_RETRY_NOSCRIPT,	/* the server forgot the program */
	VFDW_FLUSH_RETRY_BUSY		/* TRYAGAIN or LOADING */
} VfdwFlushOutcome;

/*
 * An error reply, classified. Never returns anything but a retryable outcome:
 * every other case is terminal and raises here, because the caller has no
 * decision left to make.
 */
static VfdwFlushOutcome
vfdw_flush_classify_error(valkeyReply *reply)
{
	VfdwScriptVerdict verdict;
	char	   *detail = NULL;

	/* The two the caller can act on. Both prove nothing ran. */
	if (vfdw_reply_has_prefix(reply, "NOSCRIPT"))
		return VFDW_FLUSH_RETRY_NOSCRIPT;
	if (vfdw_flush_is_retryable(reply))
		return VFDW_FLUSH_RETRY_BUSY;

	verdict = vfdw_script_classify(reply->str, reply->len, &detail);
	if (verdict == VFDW_SCRIPT_INTERNAL)
		vfdw_flush_error_internal(detail);
	if (verdict != VFDW_SCRIPT_UNKNOWN)
		vfdw_flush_error_sentinel(verdict, detail);

	/*
	 * Not ours. If the server's own prefix proves it never ran the
	 * program, say so; otherwise the outcome is genuinely unknown and
	 * must be reported as such rather than guessed at.
	 */
	if (vfdw_flush_proves_not_applied(reply))
	{
		vfdw_flush_phase = VFDW_FLUSH_PROVEN_CLEAN;
		vfdw_error_from_reply(reply, "the Valkey write was refused");
	}

	{
		int			ignore;

		vfdw_flush_error_indeterminate(vfdw_error_reply_text(reply, &ignore));
	}

	pg_unreachable();
}

/*
 * Read one EVALSHA reply and decide the transaction's fate.
 *
 * Returns APPLIED, or one of the two outcomes the caller can act on: the
 * NOSCRIPT that asks for a reload, and the TRYAGAIN or LOADING that mean "not
 * now". Every other classification is terminal and raises before it can reach
 * a return.
 */
static VfdwFlushOutcome
vfdw_flush_classify(valkeyReply *reply, int nplans)
{
	VfdwScriptVerdict verdict;
	char	   *detail = NULL;
	char		expected[16];
	int			explen;

	if (reply == NULL)
		vfdw_flush_error_indeterminate("no reply was received for the write program");

	if (reply->type == VALKEY_REPLY_ERROR)
		return vfdw_flush_classify_error(reply);

	/*
	 * A well-formed reply of the wrong shape is caught rather than
	 * dereferenced: STATUS is what the program returns, and anything else
	 * means we are not talking to the program we think we are.
	 */
	/*
	 * VFDW_RTYPE builds the mask; the raw type constants are small integers
	 * and OR-ing them makes a number that matches nothing. Getting that wrong
	 * rejected the program's own success reply AFTER it had written - data in
	 * Valkey, transaction rolled back, which is the one outcome this file
	 * exists to prevent. Errors are handled above, so only STATUS is allowed
	 * here.
	 */
	vfdw_reply_expect(reply, VFDW_RTYPE(VALKEY_REPLY_STATUS),
					  "Valkey write failed");

	verdict = vfdw_script_classify(reply->str, reply->len, &detail);
	if (verdict != VFDW_SCRIPT_OK)
		vfdw_flush_error_indeterminate("the write program returned a status "
									   "this build does not recognise");

	/*
	 * The plan count travels back so a truncated or reordered ARGV cannot
	 * report success for fewer plans than were sent. Detail is "<n>".
	 *
	 * We know the number we sent, so this is a comparison and not a parse.
	 * The case against atoi is argued at vfdw_reject in src/vfdw_option.c:
	 * undefined on overflow, answerable to the locale, and returning 0 for
	 * text it never understood, so "VFDW1 OK" plus garbage read as an honest
	 * zero-plan reply. An int is at most 11 characters, so expected never
	 * truncates and explen is a length we wrote.
	 *
	 * strlen on detail is no I3 violation: I3 forbids strlen on Valkey-owned
	 * bytes, and detail is our own pnstrdup'd copy.
	 *
	 * An interior NUL therefore hides whatever follows it, and gains a hostile
	 * server nothing. Acceptance still requires the bytes strlen can see to be
	 * exactly the digits we sent, which is the same string an honest reply
	 * carries - so "1\0anything" is accepted only where "1" already would be.
	 */
	explen = snprintf(expected, sizeof(expected), "%d", nplans);
	if (detail == NULL || strlen(detail) != (size_t) explen ||
		memcmp(detail, expected, (size_t) explen) != 0)
		vfdw_flush_error_indeterminate("the write program applied a different "
									   "number of plans than were sent");

	return VFDW_FLUSH_APPLIED;
}

/* ---------------------------------------------------------------------
 * The flush
 * --------------------------------------------------------------------- */

/*
 * One attempt, and one vfdw_batch_end for the vfdw_batch_begin it opens.
 *
 * Returns a retryable outcome - NOSCRIPT, TRYAGAIN or LOADING - when the
 * caller should go round again having queued a SCRIPT LOAD. Every other
 * classification raises from vfdw_flush_classify and never reaches the foot of
 * this function, which is invariant I1 working as intended: the batch is left
 * open, the connection stays flagged mid-conversation, and vfdw_conn_xact's
 * abort branch discards a reply stream whose offset nobody knows.
 */
static VfdwFlushOutcome
vfdw_flush_attempt(VfdwConn *vconn, MemoryContext flush_cxt, int nplans,
				   bool force_load)
{
	VfdwBatch  *batch;
	VfdwCmd		cmd;
	VfdwFlushOutcome outcome;
	bool		load = force_load || !vfdw_conn_script_loaded(vconn);

	batch = vfdw_batch_begin(vconn, flush_cxt);
	vfdw_flush_batches_begun++;

	if (load)
	{
		vfdw_cmd_init(&cmd, flush_cxt, 3);
		vfdw_cmd_add_cstr(&cmd, "SCRIPT");
		vfdw_cmd_add_cstr(&cmd, "LOAD");
		vfdw_cmd_add_cstr(&cmd, vfdw_script_text());
		vfdw_batch_add(batch, &cmd);
	}

	vfdw_cmd_init(&cmd, flush_cxt, 32);
	vfdw_script_encode(&cmd, flush_cxt);
	vfdw_batch_add(batch, &cmd);

	/*
	 * From here the program may have executed. Everything above this line is
	 * pure local work, so a failure there is provably not applied.
	 */
	vfdw_flush_phase = VFDW_FLUSH_SENT;

	if (load)
	{
		valkeyReply *loaded = vfdw_batch_next(batch);

		/*
		 * The two commands are pipelined and execute in order, so an error
		 * here means the EVALSHA behind it necessarily saw NOSCRIPT and
		 * nothing ran. Reported as itself - a NOPERM where scripting is
		 * denied by ACL is far more useful than the NOSCRIPT it causes.
		 */
		if (loaded != NULL && loaded->type == VALKEY_REPLY_ERROR)
		{
			vfdw_flush_phase = VFDW_FLUSH_PROVEN_CLEAN;
			vfdw_error_from_reply(loaded, "could not load the Valkey write program");
		}
	}

	outcome = vfdw_flush_classify(vfdw_batch_next(batch), nplans);

	if (outcome == VFDW_FLUSH_APPLIED && load)
		vfdw_conn_set_script_loaded(vconn, true);

	/*
	 * Both retryable outcomes are the EVALSHA's own reply, so every queued
	 * command has been answered and the stream is at a known offset: the drain
	 * here finds nothing to read, and handing the connection back is a fact
	 * rather than a hope. The next attempt begins its own batch.
	 */
	vfdw_batch_end(batch);
	vfdw_flush_batches_ended++;
	return outcome;
}

/*
 * Re-raise a transport failure as 08007, and re-throw everything else.
 *
 * Section 8 says any I/O failure once the program has reached the socket is
 * transaction_resolution_unknown. Without this it was not: a closed connection
 * or a malformed frame is raised by vfdw_io/vfdw_cmd BENEATH the flush, so it
 * never reached the classifier and arrived carrying the transport's own code.
 * The errcontext line was attached either way, so the outcome was always
 * communicated - but an application switching on 08007 missed it.
 *
 * WHAT THIS MUST NOT SWALLOW, and why each is re-thrown untouched:
 *
 *   A query cancel. The user asked for this; turning their cancel into
 *   "the outcome is unknown" would be answering a different question. The
 *   errcontext line already tells them what it means for the write.
 *
 *   Anything at FATAL or above, and a pending die. The backend is going away
 *   and an ereport(ERROR) here would not stop it - it would only replace a
 *   true report with a lesser one.
 *
 *   Errors raised BEFORE the program reached the socket, and those we have
 *   already proven clean. Those are outcome 2, positively not applied, and
 *   08007 would be strictly less true than what they already say.
 *
 *   Our own classified outcomes. A sentinel already carries 23505, 40001,
 *   42804 or XX000, and each is more specific than 08007.
 */
static void
vfdw_flush_rethrow_indeterminate(void)
{
	ErrorData  *edata;
	MemoryContext ecxt;

	/* The error is live; copying it needs a context that survives the flush. */
	ecxt = MemoryContextSwitchTo(TopTransactionContext);
	edata = CopyErrorData();
	MemoryContextSwitchTo(ecxt);

	if (vfdw_flush_phase != VFDW_FLUSH_SENT ||
		edata->elevel >= FATAL ||
		edata->sqlerrcode == ERRCODE_QUERY_CANCELED ||
		edata->sqlerrcode == ERRCODE_ADMIN_SHUTDOWN ||
		edata->sqlerrcode == ERRCODE_TRANSACTION_RESOLUTION_UNKNOWN ||
		edata->sqlerrcode == ERRCODE_UNIQUE_VIOLATION ||
		edata->sqlerrcode == ERRCODE_T_R_SERIALIZATION_FAILURE ||
		edata->sqlerrcode == ERRCODE_DATATYPE_MISMATCH ||
		edata->sqlerrcode == ERRCODE_INTERNAL_ERROR ||
		QueryCancelPending || ProcDiePending)
	{
		FreeErrorData(edata);
		PG_RE_THROW();
	}

	/*
	 * The original message becomes the DETAIL rather than being discarded:
	 * "could not read reply from Valkey" is the cause, and a user who only
	 * saw "the outcome is unknown" would have nothing to act on.
	 */
	FlushErrorState();

	ereport(ERROR,
			(errcode(ERRCODE_TRANSACTION_RESOLUTION_UNKNOWN),
			 errmsg("the outcome of the Valkey write is unknown"),
			 errdetail("%s", edata->message),
			 errhint("The transaction has been rolled back in PostgreSQL. The "
					 "write applied in full, applied not at all, or - if a "
					 "precondition was incomplete - applied in part. Inspect "
					 "the affected keys.")));
}

/*
 * The connection this unit's program runs on.
 *
 * Off a cluster, the server's own. On one, the primary that owns the single
 * slot the unit writes - checked first, so a cross-slot unit is refused
 * before any connection is taken and cannot leave one mid-conversation.
 */
static VfdwConn *
vfdw_flush_connect(ForeignServer *server, UserMapping *user,
				   const VfdwWriteUnit *unit)
{
	VfdwServerOptions opts;

	/*
	 * volatile because this runs inside the flush's PG_TRY and the map load
	 * below can raise: without it the compiler is entitled to keep the
	 * pointer in a register that longjmp does not restore, and -Werror
	 * rightly refuses to build it.
	 */
	VfdwConn   *volatile vconn;
	const VfdwSlotMap *map;
	const VfdwClusterNode *node;
	int			slot;

	/*
	 * Read from the catalog rather than from a connection, so the cross-slot
	 * refusal below happens BEFORE anything is taken from the pool. A refusal
	 * then costs no round trip and cannot leave a connection
	 * mid-conversation - and no pointer is live across the ereport, which is
	 * what -Wclobbered is right to complain about inside a PG_TRY.
	 */
	vfdw_read_server_options(server->options, &opts);
	if (!opts.cluster)
		return vfdw_get_connection(server, user);

	slot = vfdw_flush_cluster_slot(unit);

	vconn = vfdw_get_connection_cluster(server, user);
	if (slot < 0)
		return vconn;

	map = vfdw_cluster_map(vconn);
	node = map->owner[slot] < 0 ? NULL : &map->nodes[map->owner[slot]];

	/*
	 * An unclaimed slot goes to whoever answered discovery and earns a MOVED
	 * we report. Invariant I8: the map is a cache, so "nobody claims it" is
	 * never grounds to refuse a write the server might well accept.
	 */
	if (node == NULL)
		return vconn;

	vfdw_release_connection(vconn);
	return vfdw_get_connection_node(server, user, node->host, node->port);
}

static VfdwConn *
vfdw_flush_acquire(const VfdwWriteUnit *unit)
{
	ForeignServer *server;
	UserMapping *user;
	VfdwConn   *vconn;

	/*
	 * A live scan batch on this connection would mean two readers on one
	 * reply stream. PreCommit_Portals is expected to have dropped every
	 * portal by now; this turns that expectation into a checked precondition,
	 * and it must run BEFORE vfdw_get_connection, which closes an invalidated
	 * connection inline and would free the very context the scan is caching.
	 */
	if (vfdw_conn_peek(unit->serverid, unit->userid) != NULL)
		elog(ERROR, "valkey_fdw: connection left in conversation at pre-commit");

	server = GetForeignServer(unit->serverid);
	user = GetUserMapping(unit->userid, unit->serverid);
	vconn = vfdw_flush_connect(server, user, unit);
	vfdw_conn_select_db(vconn, unit->database);

	return vconn;
}

/*
 * Attempt the flush until it applies or a retry budget runs out.
 *
 * Both retryable outcomes prove the program did not run, so going round again
 * cannot double-apply. NOSCRIPT gets exactly one forced reload however large
 * write_retry_count is; TRYAGAIN and LOADING get the budget.
 */
static void
vfdw_flush_run(VfdwConn *vconn, MemoryContext flush_cxt, int nplans,
			   int max_retries)
{
	int			busy_retries = 0;
	bool		force_load = false;

	for (;;)
	{
		VfdwFlushOutcome outcome = vfdw_flush_attempt(vconn, flush_cxt, nplans,
													  force_load);

		if (outcome == VFDW_FLUSH_APPLIED)
			break;

		/*
		 * Both retryable outcomes prove the program did not run, so going
		 * round again cannot double-apply. Each attempt is a fresh batch and
		 * therefore a fresh deadline; the phase goes back to BEFORE_SEND so
		 * that a failure during the retry still reports honestly.
		 */
		vfdw_flush_nretries++;
		vfdw_flush_phase = VFDW_FLUSH_BEFORE_SEND;

		if (outcome == VFDW_FLUSH_RETRY_NOSCRIPT)
		{
			/*
			 * Exactly one forced reload, however large write_retry_count is:
			 * a second NOSCRIPT for a program sent in the same round trip is
			 * not a transient condition, it is another client running SCRIPT
			 * FLUSH in a loop, and retrying that is just a slower failure.
			 */
			if (force_load)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Valkey does not have the write program after loading it"),
						 errdetail("The server answered NOSCRIPT to a program it "
								   "had just been sent in the same round trip."),
						 errhint("No changes were applied in Valkey. This usually "
								 "means another client is issuing SCRIPT FLUSH.")));

			vfdw_conn_set_script_loaded(vconn, false);
			force_load = true;
			continue;
		}

		/* TRYAGAIN or LOADING: bounded by the server's own option. */
		if (busy_retries++ >= max_retries)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("Valkey was too busy to apply the write"),
					 errdetail("The server asked for the write to be retried "
							   "more than write_retry_count (%d) times.",
							   max_retries),
					 errhint("No changes were applied in Valkey. Retry the "
							 "transaction, or raise write_retry_count on the "
							 "foreign server.")));
	}
}

void
vfdw_flush_pre_commit(void)
{
	const VfdwWriteUnit *unit = vfdw_wbuf_get_unit();
	ErrorContextCallback ctx;
	MemoryContext flush_cxt;
	VfdwConn   *vconn;
	int			nplans;
	int			max_retries;

	vfdw_flush_ncalls++;

	/* An empty transaction costs nothing and touches no connection. */
	if (vfdw_wbuf_is_empty())
	{
		vfdw_flush_nempty++;
		vfdw_flush_flushed = true;
		return;
	}

	vfdw_flush_phase = VFDW_FLUSH_BEFORE_SEND;
	ctx.callback = vfdw_flush_errcontext;
	ctx.arg = NULL;
	ctx.previous = error_context_stack;
	error_context_stack = &ctx;

	vconn = vfdw_flush_acquire(unit);

	/*
	 * A child of the write buffer's context, so an error path releases it
	 * with the buffer reset in the ABORT branch rather than needing a
	 * PG_FINALLY here.
	 */
	flush_cxt = AllocSetContextCreate(vfdw_wbuf_context(),
									  "valkey_fdw flush",
									  ALLOCSET_SMALL_SIZES);

	nplans = vfdw_ledger_nplans();
	max_retries = vfdw_conn_options(vconn)->write_retry_count;

	/*
	 * The PG_TRY frees nothing and closes nothing - see the I1 note at the top
	 * of this file. Its only job is to decide the SQLSTATE, because the layers
	 * beneath raise transport errors that cannot know they happened inside a
	 * commit.
	 */
	PG_TRY();
	{
		vfdw_flush_run(vconn, flush_cxt, nplans, max_retries);
	}
	PG_CATCH();
	{
		vfdw_flush_rethrow_indeterminate();
	}
	PG_END_TRY();

	MemoryContextDelete(flush_cxt);
	vfdw_flush_flushed = true;
	vfdw_flush_nflushes++;

	error_context_stack = ctx.previous;
}

bool
vfdw_flush_done(void)
{
	return vfdw_flush_flushed;
}

void
vfdw_flush_reset(void)
{
	vfdw_flush_flushed = false;
	vfdw_flush_phase = VFDW_FLUSH_BEFORE_SEND;
}

uint64
vfdw_flush_calls(void)
{
	return vfdw_flush_ncalls;
}

uint64
vfdw_flush_empty_returns(void)
{
	return vfdw_flush_nempty;
}

uint64
vfdw_flush_flushes(void)
{
	return vfdw_flush_nflushes;
}

uint64
vfdw_flush_retries(void)
{
	return vfdw_flush_nretries;
}

uint64
vfdw_flush_batches_opened(void)
{
	return vfdw_flush_batches_begun;
}

uint64
vfdw_flush_batches_closed(void)
{
	return vfdw_flush_batches_ended;
}
