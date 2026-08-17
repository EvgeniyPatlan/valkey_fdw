/*-------------------------------------------------------------------------
 *
 * vfdw_wbuf.h
 *		The transaction-scoped deferred write buffer.
 *
 * Every write a transaction performs is appended here and nothing is sent
 * until pre-commit, so that a transaction's writes reach Valkey as one atomic
 * unit or not at all. This file is pure memory: every function either
 * succeeds or ereports, and none of them touches a connection. Nothing here
 * may include vfdw_conn.h - that exclusion is the whole reason the buffer is
 * its own file, and it is checkable by eye.
 *
 * WHY THE OPERATION LOG IS A LINKED LIST, AND WHY NOTHING IS EVER FREED
 *
 * Operations are held on a doubly-linked list with a tail pointer. Append is
 * O(1) and ROLLBACK TO SAVEPOINT walks backwards from the tail, which is
 * correct only because levels along the live list are non-decreasing:
 * SUBXACT_EVENT_ABORT_SUB removes a suffix, and SUBXACT_EVENT_PRE_COMMIT_SUB
 * lowers a suffix to curlevel - 1, which is less than or equal to every
 * earlier operation's own level. A handler that appended out of level order
 * would break the backwards walk silently, so it is stated here rather than
 * left to be inferred.
 *
 * Truncation UNLINKS and never frees. An open cursor's slot may hold a
 * pointer into a dropped operation's HeapTuple, so returning that memory
 * would be a use-after-free that only shows up under a cursor. The dropped
 * chunks stay in the context until vfdw_wbuf_reset(). Nothing in the buffer
 * context is ever pfree'd or repalloc'ed, which is also what makes the byte
 * counter below monotonic by construction rather than by an argument about
 * AllocSet free lists: repalloc of a chunk over allocChunkLimit frees the old
 * block, and mem_allocated would drop.
 *
 * THE TWO COUNTERS, DELIBERATELY DIFFERENT
 *
 * live_ops counts operations currently on the list. It is what write_max_ops
 * bounds and what the flush must carry.
 *
 * alloc_bytes is monotonic within a transaction and is what write_max_bytes
 * bounds. It is read from the context's own mem_allocated rather than
 * maintained by hand: a hand-maintained counter can only ever be wrong by
 * under-counting, an under-count is silent, and no test can tell an
 * under-count from a smaller workload. It is never decremented, anywhere - a
 * design that gave byte budget back on ROLLBACK TO SAVEPOINT would let a
 * LOOP ... EXCEPTION ... END over failing rows allocate without bound while
 * the cap read as untouched. Because the counter has AllocSet block
 * granularity, no test may assert an absolute value; tests assert relations.
 *
 * HEAPTUPLE LIFETIME
 *
 * VfdwWriteOp.newtup is copied with ExecCopySlotHeapTuple into the buffer
 * context, because the slot's own storage dies with the per-tuple context
 * long before the pre-commit flush runs. Every other byte an operation points
 * at is in that same context for the same reason - including hash field
 * names, which otherwise point into a VfdwTableMap that dies with the
 * statement.
 *
 * INVARIANT I3 AND strlen
 *
 * The bytes an operation points at arrived either from VARDATA_ANY /
 * VARSIZE_ANY_EXHDR of a bytea, with embedded NULs intact and a length that
 * travelled with them, or from the column's cached output function into a
 * palloc'd buffer, where src/vfdw_val.c applies strlen. That strlen is
 * correct precisely because the string is OURS - an output function returns a
 * cstring by the type system's own contract - and not because anything
 * checked it. I3 forbids strlen on Valkey-owned bytes; none of these are.
 * Lengths travel with data everywhere below.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_wbuf.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_WBUF_H
#define VFDW_WBUF_H

#include "vfdw.h"

#include "access/htup.h"
#include "access/xact.h"
#include "nodes/bitmapset.h"
#include "utils/relcache.h"

#include "vfdw_map.h"

typedef enum VfdwOpKind
{
	VFDW_OP_INSERT = 0,
	VFDW_OP_UPDATE,
	VFDW_OP_DELETE
} VfdwOpKind;

/*
 * One named argument of an operation: a hash field, today.
 *
 * isnull is meaningful and is not "absent": a mapped hash field explicitly set
 * to NULL is an HDEL at flush time, and a field the statement did not assign
 * is not in this array at all. Collapsing the two would make an UPDATE that
 * nulls a field indistinguishable from one that leaves it alone.
 */
typedef struct VfdwWriteArg
{
	const char *name;
	size_t		namelen;
	bool		isnull;
	const char *data;
	size_t		len;
} VfdwWriteArg;

typedef struct VfdwWriteOp
{
	struct VfdwWriteOp *prev;	/* live list only; truncation unlinks */
	struct VfdwWriteOp *next;

	VfdwOpKind	kind;
	Oid			relid;
	VfdwTableType tabletype;

	int			level;			/* GetCurrentTransactionNestLevel() at append */
	CommandId	cid;			/* GetCurrentCommandId(true) at append */
	uint16		hashslot;		/* vfdw_val_slot(key), per operation */

	/* Identity. oldkey is NULL for INSERT; it differs from key on a rename. */
	const char *key;
	size_t		keylen;
	const char *oldkey;
	size_t		oldkeylen;
	const char *member;
	size_t		memberlen;
	bool		has_member;
	const char *oldmember;
	size_t		oldmemberlen;
	bool		has_oldmember;

	/* Payload. */
	const char *value;
	size_t		valuelen;
	bool		has_value;
	const char *score;
	size_t		scorelen;
	bool		has_score;
	VfdwWriteArg *fields;
	int			nfields;

	/*
	 * The expiries this operation sets, one per ttl column, with the FIELD's
	 * name and the milliseconds as decimal bytes. isnull means persist - the
	 * expiry is removed and the field is not.
	 *
	 * Kept apart from fields[] rather than folded into it, because the two
	 * describe different things about the same field and both may be set by
	 * one row: an INSERT that gives a field a value and a lifetime emits an
	 * HSET and an HPEXPIRE, in that order, and a single array would have to
	 * carry two payloads per entry to say so.
	 */
	VfdwWriteArg *ttls;
	int			nttls;

	/*
	 * A packed row's whole contents: every element of the array column, in the
	 * array's own order, with only `data` and `len` meaningful.
	 *
	 * Separate from fields[] and from member/value because it is a different
	 * KIND of write. Those name one thing inside a key; this names everything
	 * the key should hold afterwards, which is why it folds into a DEL and a
	 * rebuild rather than into an edit.
	 */
	VfdwWriteArg *packed;
	int			npacked;

	/*
	 * Whether this operation is a packed one at all, which packed/npacked
	 * cannot say: a packed row whose array is NULL or empty has no elements
	 * and still means "the key should hold nothing", where a non-packed op
	 * with no elements means nothing of the sort.
	 */
	bool		packed_shape;

	/* The overlay's payload (S7); see the HeapTuple note in the file prose. */
	HeapTuple	newtup;

	/* Which attributes the statement assigned; one per statement (S4). */
	const Bitmapset *target_attrs;

	/*
	 * The table's keyset index name, retained here rather than looked up from
	 * the catalog when the ledger folds this operation. The fold runs from the
	 * subtransaction abort callback, where opening a relation is not allowed -
	 * see the HOLD_INTERRUPTS note on vfdw_wbuf_subxact - and this is the only
	 * thing the fold ever wanted the table map for. NULL when the table has no
	 * keyset.
	 */
	const char *keyset;
	size_t		keysetlen;
} VfdwWriteOp;

/*
 * The one Valkey identity a transaction's writes may address.
 *
 * Compared on umid rather than on the SQL user: umid determines the
 * credentials that reach Valkey, so two SQL users sharing a PUBLIC mapping
 * are one Valkey identity and must not be refused. userid is kept separately
 * because the flush re-acquires with GetUserMapping(userid, serverid), which
 * takes the SQL user rather than the mapping.
 */
typedef struct VfdwWriteUnit
{
	bool		bound;
	Oid			serverid;
	Oid			umid;
	Oid			userid;
	int			database;
	int			max_ops;		/* the bound server's write_max_ops */
	int64		max_bytes;		/* the bound server's write_max_bytes */

	/*
	 * The nest level the binding was taken at, carried for exactly the reason
	 * an operation carries one: so an aborting subtransaction can discard what
	 * it created and nothing else. A binding outlives the statement that took
	 * it, so "the buffer is empty" is not on its own grounds to drop it - the
	 * statement that bound may still be running.
	 */
	int			level;
} VfdwWriteUnit;

/*
 * Bind the transaction to one Valkey identity, or refuse a second one.
 *
 * The caps are taken from the FIRST bind and are not re-read if an ALTER
 * SERVER lands mid-transaction: re-reading per statement would let a cap
 * tighten under a transaction that has already exceeded it.
 */
extern void vfdw_wbuf_bind(Relation rel, Oid serverid, Oid umid, Oid userid,
						   int database, int max_ops, int64 max_bytes);

/* The buffer context, created lazily. Also registers the xact callbacks. */
extern MemoryContext vfdw_wbuf_context(void);

/*
 * Allocate a zeroed operation in the buffer context.
 *
 * Takes the relation because it consults the byte cap: a row is rendered into
 * the context between here and vfdw_wbuf_append, and a row refused partway
 * through rendering never reaches append at all. Checking only at append lets
 * a loop over refused rows grow the context without any cap ever firing.
 */
extern VfdwWriteOp *vfdw_wbuf_op_new(Relation rel);
extern void vfdw_wbuf_append(VfdwWriteOp *op, Relation rel);
extern void vfdw_wbuf_reset(void);
extern void vfdw_wbuf_subxact(SubXactEvent event, SubTransactionId mySubid,
							  SubTransactionId parentSubid);

extern bool vfdw_wbuf_is_empty(void);
extern int	vfdw_wbuf_live_ops(void);
extern uint64 vfdw_wbuf_alloc_bytes(void);
extern uint64 vfdw_wbuf_generation(void);
extern const VfdwWriteUnit *vfdw_wbuf_get_unit(void);

/* Diagnostics only: walk with op->next. */
extern const VfdwWriteOp *vfdw_wbuf_first(void);

#endif							/* VFDW_WBUF_H */
