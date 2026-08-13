/*-------------------------------------------------------------------------
 *
 * vfdw_wbuf.c
 *		The transaction-scoped deferred write buffer.
 *
 * See src/vfdw_wbuf.h for why the log is a list, why nothing is ever freed,
 * and why the two counters are different. This file deliberately includes no
 * connection header.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_wbuf.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_wbuf.h"

#include "catalog/pg_foreign_server.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "vfdw_ledger.h"
#include "vfdw_overlay.h"
#include "vfdw_xact.h"

/*
 * A single backend-lifetime context under TopMemoryContext, reset - never
 * deleted - from the xact hook. Not TopTransactionContext: that gives no
 * control over savepoint-scoped release, AtCommit_Memory resets it out from
 * under us, and a named context makes an oversized COPY buffer legible in
 * MemoryContextStats instead of looking like a leak.
 *
 * ALLOCSET_START_SMALL_SIZES so a three-row transaction pays a 1 kB block and
 * a COPY grows to 8 MB blocks. ALLOCSET_DEFAULT_SIZES would charge every
 * small transaction 8 kB against a write_max_bytes whose minimum is 65536.
 */
static MemoryContext vfdw_wbuf_cxt = NULL;
static Size vfdw_wbuf_baseline = 0;

static VfdwWriteOp *vfdw_wbuf_head = NULL;
static VfdwWriteOp *vfdw_wbuf_tail = NULL;
static int	vfdw_wbuf_nlive = 0;
static uint64 vfdw_wbuf_gen = 0;
static VfdwWriteUnit vfdw_wbuf_unit;

/*
 * Rebuilt after any truncation, because both are derived from the operation
 * log and a suffix of it just vanished. The overlay is still empty and lands
 * with S7. The ordering - truncate, rebind, then rebuild the derived state -
 * is in the code because the next step has to preserve it.
 */
static void
vfdw_ledger_rebuild(void)
{
	vfdw_ledger_rebuild_from_buffer();
}

static void
vfdw_overlay_rebuild(void)
{
	/*
	 * Dropped rather than rebuilt: it is a view keyed off the buffer's
	 * generation and rebuilds itself on the next question. Rebuilding here
	 * would do the work inside an abort callback, where the answer may never
	 * be asked for.
	 */
	vfdw_overlay_reset();
}

/* ---------------------------------------------------------------------
 * Memory context and the byte counter
 * --------------------------------------------------------------------- */

/*
 * Bytes taken from the allocator since the last reset.
 *
 * MemoryContextMemAllocated(ctx, false) reads the context's own mem_allocated
 * field, which the allocator maintains: it counts chunk headers and
 * size-class rounding, which is what actually bounds memory. A hand-rolled
 * counter could only ever under-count, and an under-count is indistinguishable
 * from a smaller workload.
 *
 * The clamp cannot fire while the invariants above hold - nothing is ever
 * freed, and the baseline is recaptured on reset - but an underflow here
 * would silently disable write_max_bytes rather than fail, so it is not left
 * to argument.
 */
static uint64
vfdw_wbuf_alloc_bytes_internal(void)
{
	Size		now;

	if (vfdw_wbuf_cxt == NULL)
		return 0;

	now = MemoryContextMemAllocated(vfdw_wbuf_cxt, false);
	if (now <= vfdw_wbuf_baseline)
		return 0;

	return (uint64) (now - vfdw_wbuf_baseline);
}

MemoryContext
vfdw_wbuf_context(void)
{
	if (vfdw_wbuf_cxt == NULL)
	{
		vfdw_wbuf_cxt = AllocSetContextCreate(TopMemoryContext,
											  "valkey_fdw write buffer",
											  ALLOCSET_START_SMALL_SIZES);
		vfdw_wbuf_baseline = MemoryContextMemAllocated(vfdw_wbuf_cxt, false);

		/*
		 * A transaction that buffers without ever acquiring a connection must
		 * still get its callbacks. In a backend whose first statement is a
		 * write, this is the registering call: BeginForeignModify and COPY's
		 * begin both build their value context against this buffer before
		 * vfdw_modify_connect acquires anything, so vfdw_conn_init_pool
		 * registers second or not at all. Both sites register the same pair,
		 * so which of them arrives first is not observable.
		 */
		vfdw_xact_ensure_registered();
	}

	return vfdw_wbuf_cxt;
}

/* ---------------------------------------------------------------------
 * Unit binding
 * --------------------------------------------------------------------- */

/*
 * Three distinct message literals, not one parameterised message, because all
 * three carry SQLSTATE 0A000 and a test comparing only the SQLSTATE could not
 * tell any of them apart.
 */
static void
vfdw_wbuf_error_server(Relation rel, Oid serverid)
{
	const char *bound_name = GetForeignServer(vfdw_wbuf_unit.serverid)->servername;
	const char *this_name = GetForeignServer(serverid)->servername;

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot write to more than one Valkey server in one transaction"),
			 errdetail("This transaction is already writing to server \"%s\"; "
					   "table \"%s\" is on server \"%s\".",
					   bound_name, RelationGetRelationName(rel), this_name),
			 errhint("valkey_fdw applies a transaction's writes as one atomic "
					 "unit, which cannot span two servers. Split the work into "
					 "two transactions."),
			 errtable(rel)));
}

/*
 * Names the two ROLES rather than the two user-mapping OIDs. An OID in a
 * message is unstable across runs, so a recorded expected file could not
 * assert this text at all - and a refusal nobody can assert is a refusal
 * nobody notices losing.
 */
static void
vfdw_wbuf_error_umid(Relation rel, Oid userid)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot write through more than one Valkey user mapping in one transaction"),
			 errdetail("This transaction is already writing as role \"%s\"; "
					   "table \"%s\" would write as role \"%s\", which "
					   "resolves to a different user mapping.",
					   GetUserNameFromId(vfdw_wbuf_unit.userid, false),
					   RelationGetRelationName(rel),
					   GetUserNameFromId(userid, false)),
			 errhint("valkey_fdw applies a transaction's writes as one atomic "
					 "unit through one set of Valkey credentials. Split the "
					 "work into two transactions."),
			 errtable(rel)));
}

static void
vfdw_wbuf_error_database(Relation rel, int database)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot write to more than one Valkey database in one transaction"),
			 errdetail("This transaction is already writing to database %d; "
					   "table \"%s\" is on database %d.",
					   vfdw_wbuf_unit.database, RelationGetRelationName(rel),
					   database),
			 errhint("valkey_fdw applies a transaction's writes as one atomic "
					 "unit, and a Valkey transaction cannot span two logical "
					 "databases. Split the work into two transactions."),
			 errtable(rel)));
}

void
vfdw_wbuf_bind(Relation rel, Oid serverid, Oid umid, Oid userid,
			   int database, int max_ops, int64 max_bytes)
{
	/* Also registers the transaction callbacks on the first write. */
	(void) vfdw_wbuf_context();

	if (!vfdw_wbuf_unit.bound)
	{
		vfdw_wbuf_unit.bound = true;
		vfdw_wbuf_unit.serverid = serverid;
		vfdw_wbuf_unit.umid = umid;
		vfdw_wbuf_unit.userid = userid;
		vfdw_wbuf_unit.database = database;
		vfdw_wbuf_unit.max_ops = max_ops;
		vfdw_wbuf_unit.max_bytes = max_bytes;
		vfdw_wbuf_unit.level = GetCurrentTransactionNestLevel();
		return;
	}

	if (vfdw_wbuf_unit.serverid != serverid)
		vfdw_wbuf_error_server(rel, serverid);
	if (vfdw_wbuf_unit.umid != umid)
		vfdw_wbuf_error_umid(rel, userid);
	if (vfdw_wbuf_unit.database != database)
		vfdw_wbuf_error_database(rel, database);
}

/* ---------------------------------------------------------------------
 * Append, and the two caps
 * --------------------------------------------------------------------- */

/*
 * Two different errmsg literals for one SQLSTATE, which is the whole point: a
 * test asserting only 54000 would pass with the two checks swapped.
 */
static void
vfdw_wbuf_error_op_cap(Relation rel)
{
	ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("deferred write buffer would exceed write_max_ops"),
			 errdetail("The transaction holds %d live write operations and "
					   UINT64_FORMAT " bytes; write_max_ops is %d.",
					   vfdw_wbuf_nlive, vfdw_wbuf_alloc_bytes_internal(),
					   vfdw_wbuf_unit.max_ops),
			 errhint("Raise write_max_ops on the foreign server, or write in "
					 "smaller transactions."),
			 errtable(rel)));
}

static void
vfdw_wbuf_error_byte_cap(Relation rel)
{
	ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("deferred write buffer would exceed write_max_bytes"),
			 errdetail("The transaction holds %d live write operations and "
					   UINT64_FORMAT " bytes; write_max_bytes is " INT64_FORMAT ".",
					   vfdw_wbuf_nlive, vfdw_wbuf_alloc_bytes_internal(),
					   vfdw_wbuf_unit.max_bytes),
			 errhint("ROLLBACK TO SAVEPOINT does not return byte budget: the "
					 "bytes of a rolled-back operation are still held. Raise "
					 "write_max_bytes, or write in smaller transactions."),
			 errtable(rel)));
}

VfdwWriteOp *
vfdw_wbuf_op_new(Relation rel)
{
	if (!vfdw_wbuf_unit.bound)
		elog(ERROR, "valkey_fdw: write buffer unit is not bound");

	/*
	 * The byte cap is consulted here as well as in vfdw_wbuf_append, and this
	 * is the check that actually bounds the context. A row is rendered into
	 * the buffer between the two calls, and a row refused partway through
	 * rendering - a NULL key, a NaN score, a value that will not cast - keeps
	 * the bytes it had already retained and never reaches append. A caller
	 * looping over refused rows under an exception handler therefore grows the
	 * context forever if only append looks. Charging at allocation bounds the
	 * total at the cap plus the one row in flight, whatever happens to it.
	 */
	if (vfdw_wbuf_alloc_bytes_internal() > (uint64) vfdw_wbuf_unit.max_bytes)
		vfdw_wbuf_error_byte_cap(rel);

	return (VfdwWriteOp *) MemoryContextAllocZero(vfdw_wbuf_cxt,
												  sizeof(VfdwWriteOp));
}

/*
 * Both caps are enforced here, not at flush, because this is the only point
 * at which the error can name what the user did.
 *
 * The byte test reads "have we already allocated more than the cap", not
 * "would we". By the time this runs the row's bytes are in the context, so it
 * fires on the row that crossed the cap and cannot be defeated by a row that
 * was refused after allocating.
 *
 * The `true` in GetCurrentCommandId marks the command id used, so that
 * CommandCounterIncrement advances it at end of statement and S7's visibility
 * rule can tell two statements apart.
 *
 * It is currently redundant rather than load-bearing, and that was measured
 * rather than assumed: standard_ExecutorStart already calls
 * GetCurrentCommandId(true) for every CMD_INSERT/UPDATE/DELETE, and CopyFrom
 * does the same, so this is never the only thing marking it. Changing it to
 * false leaves the whole suite green. It stays `true` because the requirement
 * belongs where the stamp is taken rather than in an assumption about a
 * caller - but nothing here may be counted as testing it, and
 * the write-path design's claim that the `true` is
 * load-bearing is the part that is wrong.
 */
void
vfdw_wbuf_append(VfdwWriteOp *op, Relation rel)
{
	Assert(vfdw_wbuf_unit.bound);

	op->level = GetCurrentTransactionNestLevel();
	op->cid = GetCurrentCommandId(true);

	if (vfdw_wbuf_nlive + 1 > vfdw_wbuf_unit.max_ops)
		vfdw_wbuf_error_op_cap(rel);
	if (vfdw_wbuf_alloc_bytes_internal() > (uint64) vfdw_wbuf_unit.max_bytes)
		vfdw_wbuf_error_byte_cap(rel);

	op->prev = vfdw_wbuf_tail;
	op->next = NULL;
	if (vfdw_wbuf_tail != NULL)
		vfdw_wbuf_tail->next = op;
	else
		vfdw_wbuf_head = op;
	vfdw_wbuf_tail = op;

	vfdw_wbuf_nlive++;

	/*
	 * The generation means "the operation list changed", and an append
	 * changes it. Bumping only on truncation was a real defect: the overlay
	 * caches its index against this number, so an index built during a
	 * DELETE's own scan stayed "fresh" after the DELETE was appended, and the
	 * next statement read a keyspace view that predated it. A key created and
	 * then deleted in one transaction stayed visible.
	 */
	vfdw_wbuf_gen++;
}

/* ---------------------------------------------------------------------
 * Subtransactions
 * --------------------------------------------------------------------- */

/*
 * Unlink the last operation. Its memory stays in the context: see the
 * use-after-free note in the header.
 */
static void
vfdw_wbuf_unlink_tail(void)
{
	VfdwWriteOp *op = vfdw_wbuf_tail;

	vfdw_wbuf_tail = op->prev;
	if (vfdw_wbuf_tail != NULL)
		vfdw_wbuf_tail->next = NULL;
	else
		vfdw_wbuf_head = NULL;

	op->prev = NULL;
	op->next = NULL;
	vfdw_wbuf_nlive--;
}

/*
 * Every surviving operation matched the binding when it was appended, and
 * truncation only ever removes a suffix, so the survivors still agree with it
 * and the only thing that can change is whether there are any. Leaving it
 * bound to a database or slot whose only operation was just rolled back makes
 * every later statement in the transaction fail with an unrecoverable 0A000.
 *
 * Emptiness alone is NOT grounds to unbind, and reducing it to that was a live
 * defect: a binding is taken by BeginForeignModify before the first row is
 * buffered, so any subtransaction aborting in between - most commonly a
 * plpgsql BEGIN ... EXCEPTION inside an expression the statement is still
 * evaluating - found the buffer empty and dropped a binding its own statement
 * was about to use. The next row then died on vfdw_wbuf_op_new's elog. Hence
 * the level test: the binding is discarded when the subtransaction that took
 * it goes away, exactly as an operation is, and an abort below it leaves it
 * alone. The emptiness test stays because a binding promoted out of a
 * committed savepoint can outlive its level while operations still reference
 * it.
 *
 * If Phase 4 ever admits more than one unit per transaction this reduction
 * stops holding and the binding must be recomputed from the survivors.
 */
static void
vfdw_wbuf_rebind_after_truncate(int curlevel)
{
	if (vfdw_wbuf_nlive == 0 && vfdw_wbuf_unit.level >= curlevel)
		vfdw_wbuf_unit.bound = false;
}

/*
 * ABORT_SUB runs inside HOLD_INTERRUPTS. Everything below is pure memory: no
 * allocation, no syscall, and no CHECK_FOR_INTERRUPTS - one there would be a
 * no-op. The work is bounded by write_max_ops.
 *
 * At both ABORT_SUB and PRE_COMMIT_SUB the nest level is still the child's;
 * CleanupSubTransaction and CommitSubTransaction pop afterwards.
 *
 * The last two arms are explicit cases with comments rather than a default:
 * the switch is exhaustive by intent, not by omission.
 */
void
vfdw_wbuf_subxact(SubXactEvent event, SubTransactionId mySubid,
				  SubTransactionId parentSubid)
{
	int			curlevel;
	VfdwWriteOp *p;
	bool		dropped = false;

	switch (event)
	{
		case SUBXACT_EVENT_ABORT_SUB:
			curlevel = GetCurrentTransactionNestLevel();
			while (vfdw_wbuf_tail != NULL && vfdw_wbuf_tail->level >= curlevel)
			{
				vfdw_wbuf_unlink_tail();
				dropped = true;
			}
			if (dropped)
				vfdw_wbuf_gen++;
			vfdw_wbuf_rebind_after_truncate(curlevel);
			vfdw_ledger_rebuild();
			vfdw_overlay_rebuild();
			break;

		case SUBXACT_EVENT_PRE_COMMIT_SUB:
			curlevel = GetCurrentTransactionNestLevel();
			for (p = vfdw_wbuf_tail; p != NULL && p->level >= curlevel;
				 p = p->prev)
				p->level = curlevel - 1;

			/*
			 * The binding is promoted on the same rule as the operations: a
			 * committed savepoint's work belongs to its parent. Without this
			 * the unit would keep a level that no longer exists and the next
			 * abort at that depth would drop a binding it did not create.
			 */
			if (vfdw_wbuf_unit.bound && vfdw_wbuf_unit.level >= curlevel)
				vfdw_wbuf_unit.level = curlevel - 1;
			break;

		case SUBXACT_EVENT_COMMIT_SUB:
			/* Nothing: PRE_COMMIT_SUB already promoted the suffix. */
			break;

		case SUBXACT_EVENT_START_SUB:
			/* Nothing: operations carry their own level. */
			break;
	}
}

/* ---------------------------------------------------------------------
 * Reset and diagnostics
 * --------------------------------------------------------------------- */

/*
 * Assumes nothing about how far a flush got, so it is safe to call from
 * COMMIT and from ABORT alike. No Assert on prior state and no early return
 * on "already empty": that would make a double reset behave differently from
 * a single one. Never MemoryContextDelete - the context is backend-lifetime.
 */
void
vfdw_wbuf_reset(void)
{
	vfdw_wbuf_head = NULL;
	vfdw_wbuf_tail = NULL;
	vfdw_wbuf_nlive = 0;
	vfdw_wbuf_unit.bound = false;
	vfdw_wbuf_unit.level = 0;
	vfdw_wbuf_gen++;

	/*
	 * The ledger points into the buffer and must go first: resetting the
	 * buffer leaves every key and value pointer in the ledger dangling, and a
	 * ledger that outlived one reset would be read against the next
	 * transaction's bytes.
	 */
	vfdw_ledger_reset();
	vfdw_overlay_reset();

	if (vfdw_wbuf_cxt != NULL)
	{
		MemoryContextReset(vfdw_wbuf_cxt);
		vfdw_wbuf_baseline = MemoryContextMemAllocated(vfdw_wbuf_cxt, false);
	}
}

bool
vfdw_wbuf_is_empty(void)
{
	return vfdw_wbuf_nlive == 0;
}

int
vfdw_wbuf_live_ops(void)
{
	return vfdw_wbuf_nlive;
}

uint64
vfdw_wbuf_alloc_bytes(void)
{
	return vfdw_wbuf_alloc_bytes_internal();
}

uint64
vfdw_wbuf_generation(void)
{
	return vfdw_wbuf_gen;
}

const VfdwWriteUnit *
vfdw_wbuf_get_unit(void)
{
	return &vfdw_wbuf_unit;
}

const VfdwWriteOp *
vfdw_wbuf_first(void)
{
	return vfdw_wbuf_head;
}
