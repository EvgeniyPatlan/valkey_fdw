/*-------------------------------------------------------------------------
 *
 * vfdw_ledger.c
 *		Folding the write buffer into one plan per Valkey key.
 *
 * See src/vfdw_ledger.h for the precondition rule, which is the whole point
 * of this file, and for why the ledger has its own memory context.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_ledger.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_ledger.h"

#include "vfdw_ledger_fold.h"
#include "vfdw_ledger_internal.h"
#include "vfdw_refuse.h"

#include "access/htup_details.h"
#include "common/hashfn.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * Keys and member names are arbitrary bytes of arbitrary length, so neither
 * HASH_BLOBS nor HASH_STRINGS can key these tables: the first needs a fixed
 * width, the second stops at the first NUL. The entry key is a (pointer,
 * length) pair into the write buffer, with the hash and match functions below.
 */
MemoryContext vfdw_ledger_cxt = NULL;
static HTAB *vfdw_ledger_keys = NULL;
static HTAB *vfdw_ledger_members = NULL;

static VfdwKeyPlan *vfdw_ledger_head = NULL;
static VfdwKeyPlan *vfdw_ledger_tail = NULL;
static int	vfdw_ledger_count = 0;

/* Guards vfdw_ledger_rebuild_from_buffer against re-entering itself. */
static bool vfdw_ledger_rebuilding = false;

static uint32
vfdw_ledger_hash(const void *key, Size keysize)
{
	const VfdwLedgerKey *k = (const VfdwLedgerKey *) key;
	uint32		h = hash_bytes((const unsigned char *) k->key, (int) k->keylen);

	if (k->sub != NULL)
		h ^= hash_bytes((const unsigned char *) k->sub, (int) k->sublen);

	return h;
}

static int
vfdw_ledger_match(const void *a, const void *b, Size keysize)
{
	const VfdwLedgerKey *x = (const VfdwLedgerKey *) a;
	const VfdwLedgerKey *y = (const VfdwLedgerKey *) b;

	if (x->keylen != y->keylen || x->sublen != y->sublen)
		return 1;
	if ((x->sub == NULL) != (y->sub == NULL))
		return 1;
	if (x->keylen > 0 && memcmp(x->key, y->key, x->keylen) != 0)
		return 1;
	if (x->sub != NULL && x->sublen > 0 && memcmp(x->sub, y->sub, x->sublen) != 0)
		return 1;

	return 0;
}

static void
vfdw_ledger_init(void)
{
	HASHCTL		ctl;

	if (vfdw_ledger_cxt != NULL)
		return;

	vfdw_ledger_cxt = AllocSetContextCreate(TopMemoryContext,
											"valkey_fdw write ledger",
											ALLOCSET_START_SMALL_SIZES);

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(VfdwLedgerKey);
	ctl.entrysize = sizeof(VfdwKeyEntry);
	ctl.hash = vfdw_ledger_hash;
	ctl.match = vfdw_ledger_match;
	ctl.hcxt = vfdw_ledger_cxt;
	vfdw_ledger_keys = hash_create("valkey_fdw ledger keys", 64, &ctl,
								   HASH_ELEM | HASH_FUNCTION | HASH_COMPARE |
								   HASH_CONTEXT);

	ctl.entrysize = sizeof(VfdwMemberEntry);
	vfdw_ledger_members = hash_create("valkey_fdw ledger members", 64, &ctl,
									  HASH_ELEM | HASH_FUNCTION |
									  HASH_COMPARE | HASH_CONTEXT);
}

void
vfdw_ledger_reset(void)
{
	if (vfdw_ledger_cxt == NULL)
		return;

	/*
	 * Both tables live in this context, so deleting it takes them with it.
	 * Nulled rather than left dangling: hash_destroy on a context-owned table
	 * would be a second free of the same memory.
	 */
	MemoryContextDelete(vfdw_ledger_cxt);
	vfdw_ledger_cxt = NULL;
	vfdw_ledger_keys = NULL;
	vfdw_ledger_members = NULL;
	vfdw_ledger_head = NULL;
	vfdw_ledger_tail = NULL;
	vfdw_ledger_count = 0;
}

/* ---------------------------------------------------------------------
 * Building steps
 * --------------------------------------------------------------------- */

VfdwLedgerStep *
vfdw_ledger_step(int op, int nargs)
{
	VfdwLedgerStep *s = (VfdwLedgerStep *)
		MemoryContextAllocZero(vfdw_ledger_cxt, sizeof(VfdwLedgerStep));

	s->op = op;
	s->nargs = nargs;
	if (nargs > 0)
		s->args = (VfdwLedgerArg *)
			MemoryContextAllocZero(vfdw_ledger_cxt,
								   sizeof(VfdwLedgerArg) * nargs);
	return s;
}

void
vfdw_ledger_add_check(VfdwKeyPlan *plan, VfdwLedgerStep *s)
{
	if (plan->checks_tail == NULL)
		plan->checks = s;
	else
		plan->checks_tail->next = s;
	plan->checks_tail = s;
	plan->nchecks++;
}

void
vfdw_ledger_add_action(VfdwKeyPlan *plan, VfdwLedgerStep *s)
{
	if (plan->actions_tail == NULL)
		plan->actions = s;
	else
		plan->actions_tail->next = s;
	plan->actions_tail = s;
	plan->nactions++;
}

void
vfdw_ledger_set_arg(VfdwLedgerStep *s, int i, const char *data, size_t len)
{
	s->args[i].data = data;
	s->args[i].len = len;
}

/* An action with no arguments beyond the key the plan already names. */
void
vfdw_ledger_act0(VfdwKeyPlan *plan, int op)
{
	vfdw_ledger_add_action(plan, vfdw_ledger_step(op, 0));
}

void
vfdw_ledger_act1(VfdwKeyPlan *plan, int op, const char *a, size_t alen)
{
	VfdwLedgerStep *s = vfdw_ledger_step(op, 1);

	vfdw_ledger_set_arg(s, 0, a, alen);
	vfdw_ledger_add_action(plan, s);
}

void
vfdw_ledger_act2(VfdwKeyPlan *plan, int op, const char *a, size_t alen,
				 const char *b, size_t blen)
{
	VfdwLedgerStep *s = vfdw_ledger_step(op, 2);

	vfdw_ledger_set_arg(s, 0, a, alen);
	vfdw_ledger_set_arg(s, 1, b, blen);
	vfdw_ledger_add_action(plan, s);
}

void
vfdw_ledger_chk1(VfdwKeyPlan *plan, int op, const char *a, size_t alen)
{
	VfdwLedgerStep *s = vfdw_ledger_step(op, 1);

	vfdw_ledger_set_arg(s, 0, a, alen);
	vfdw_ledger_add_check(plan, s);
}

/* ---------------------------------------------------------------------
 * Key plans
 * --------------------------------------------------------------------- */

static void
vfdw_ledger_add_relid(VfdwKeyPlan *plan, Oid relid)
{
	int			i;
	Oid		   *grown;

	for (i = 0; i < plan->nrelids; i++)
	{
		if (plan->relids[i] == relid)
			return;
	}

	grown = (Oid *) MemoryContextAlloc(vfdw_ledger_cxt,
									   sizeof(Oid) * (plan->nrelids + 1));
	if (plan->nrelids > 0)
		memcpy(grown, plan->relids, sizeof(Oid) * plan->nrelids);
	grown[plan->nrelids] = relid;
	plan->relids = grown;
	plan->nrelids++;
}

/*
 * The plan for a key, created on first touch.
 *
 * *created tells the caller whether this call made it, which is exactly the
 * "first operation on a key" the precondition rule turns on. Deciding it from
 * an empty action list instead would be wrong for the second operation of a
 * DELETE-then-INSERT pair, whose first operation legitimately emits actions.
 */
static VfdwKeyPlan *
vfdw_ledger_plan_for(const char *key, size_t keylen, VfdwTableType ttype,
					 bool *created)
{
	VfdwLedgerKey lk;
	VfdwKeyEntry *entry;
	bool		found;

	memset(&lk, 0, sizeof(lk));
	lk.key = key;
	lk.keylen = keylen;

	entry = (VfdwKeyEntry *) hash_search(vfdw_ledger_keys, &lk, HASH_ENTER,
										 &found);
	if (found)
	{
		*created = false;
		return entry->plan;
	}

	entry->plan = (VfdwKeyPlan *)
		MemoryContextAllocZero(vfdw_ledger_cxt, sizeof(VfdwKeyPlan));
	entry->plan->key = key;
	entry->plan->keylen = keylen;
	entry->plan->ttype = ttype;
	entry->plan->require = VFDW_REQ_ANY;

	if (vfdw_ledger_tail == NULL)
		vfdw_ledger_head = entry->plan;
	else
		vfdw_ledger_tail->next = entry->plan;
	vfdw_ledger_tail = entry->plan;
	vfdw_ledger_count++;

	/*
	 * Every plan carries TYPE_OK, and it is the first check. It is what makes
	 * phase 2 incapable of raising WRONGTYPE - the realistic runtime error
	 * against a keyspace this wrapper does not own - by refusing in phase 1,
	 * where refusing is still free.
	 */
	vfdw_ledger_add_check(entry->plan,
						  vfdw_ledger_step(VFDW_CHK_TYPE_OK, 0));

	*created = true;
	return entry->plan;
}

VfdwMemberEntry *
vfdw_ledger_member(const char *key, size_t keylen,
				   const char *sub, size_t sublen, bool *created)
{
	VfdwLedgerKey lk;
	VfdwMemberEntry *entry;
	bool		found;

	memset(&lk, 0, sizeof(lk));
	lk.key = key;
	lk.keylen = keylen;
	lk.sub = sub;
	lk.sublen = sublen;

	entry = (VfdwMemberEntry *) hash_search(vfdw_ledger_members, &lk,
											HASH_ENTER, &found);
	/*
	 * Field by field, not a memset: dynahash has already written the key into
	 * the first bytes. Every field must be listed - present_removals was
	 * missed here once, and the uninitialised read surfaced as a recorded
	 * LVALUE_COUNT_GE of 1 on PostgreSQL 16 and 17 and 0 on 18.
	 */
	if (!found)
	{
		entry->present = false;
		entry->touched = false;
		entry->count = 0;
		entry->present_removals = 0;
	}
	*created = !found;
	return entry;
}

/* ---------------------------------------------------------------------
 * Conflicts
 * --------------------------------------------------------------------- */

/* ---------------------------------------------------------------------
 * The fold
 * --------------------------------------------------------------------- */

/*
 * Maintain the keyset index, when the table has one.
 *
 * The keyset is a Valkey set naming the table's keys. Left to the user to
 * keep current, it drifts away from the keys that actually exist. Here it is
 * part of the same atomic unit as the write it indexes, so it cannot be left
 * behind by a failure.
 */
static void
vfdw_ledger_keyset(VfdwKeyPlan *plan, const VfdwWriteOp *op,
				   const char *key, size_t keylen, bool add)
{
	if (op->keyset == NULL)
		return;

	vfdw_ledger_act2(plan, add ? VFDW_ACT_KSET_ADD : VFDW_ACT_KSET_REM,
					 op->keyset, op->keysetlen, key, keylen);
}

/*
 * Establish the precondition, if this is the first operation on the key.
 *
 * The DELETED case is the interesting one: a transaction that deletes a key
 * and then inserts it is legal, the actions are the DEL already recorded
 * followed by the create, and require stays KEY_PRESENT from the delete. What
 * it must NOT become is KEY_ABSENT - the key is present on the server, and
 * demanding its absence would fail the flush for doing exactly what SQL asked.
 */
static void
vfdw_ledger_precondition(VfdwKeyPlan *plan, bool first, VfdwOpKind kind,
						 bool rename_target)
{
	if (!first)
		return;

	/*
	 * A rename's destination is a key this transaction is CREATING, whatever
	 * the statement that produced it was. Treating it as an UPDATE would
	 * demand the new name already exist - which is precisely what must not be
	 * true - and would also let a rename onto a live key destroy it silently,
	 * since Valkey's RENAME overwrites. The old name's own requirement is
	 * declared separately, in vfdw_ledger_fold_rename.
	 */
	if (kind == VFDW_OP_INSERT || rename_target)
	{
		plan->require = VFDW_REQ_KEY_ABSENT;
		vfdw_ledger_add_check(plan,
							  vfdw_ledger_step(VFDW_CHK_KEY_ABSENT, 0));
		plan->state = VFDW_KEY_CREATED;
	}
	else
	{
		plan->require = VFDW_REQ_KEY_PRESENT;
		vfdw_ledger_add_check(plan,
							  vfdw_ledger_step(VFDW_CHK_KEY_PRESENT, 0));
		plan->state = VFDW_KEY_LIVE;
	}
}

/*
 * The duplicate rule, and the reason preconditions come from the first
 * operation rather than from every one.
 *
 * CREATED means this transaction made the key; LIVE means it existed and this
 * transaction touched it. Both refuse a second INSERT. DELETED does not -
 * deleting and re-inserting a key in one transaction is legal SQL and lands
 * as DEL followed by the create.
 *
 * Set and zset rows are exempt: their identity is (key, member), several rows
 * share one key by design, and the collision that matters for them is caught
 * one level down in vfdw_ledger_fold_member. A list is exempt for the
 * stronger reason that duplicate values are the point of a list.
 */
static void
vfdw_ledger_check_duplicate(VfdwKeyPlan *plan, const VfdwWriteOp *op,
							Relation rel, bool first)
{
	if (op->kind != VFDW_OP_INSERT || first)
		return;
	if (plan->state == VFDW_KEY_DELETED)
		return;
	if (plan->ttype != VFDW_TABLE_STRING && plan->ttype != VFDW_TABLE_JSON)
		return;

	vfdw_refuse_duplicate_key(rel, op->key, op->keylen);
}

/* Whether this operation moves a row from one Valkey key to another. */
static bool
vfdw_ledger_is_rename(const VfdwWriteOp *op)
{
	return op->kind == VFDW_OP_UPDATE && op->oldkey != NULL &&
		(op->oldkeylen != op->keylen ||
		 memcmp(op->oldkey, op->key, op->keylen) != 0);
}

/*
 * A key rename preserves whatever the table does not map, which a delete and
 * recreate would destroy. Both keys are in the plan's key set, so the
 * single-unit rule refuses a cross-slot rename in both directions rather than
 * deleting without creating.
 */
static void
vfdw_ledger_fold_rename(VfdwKeyPlan *plan, const VfdwWriteOp *op)
{
	if (!vfdw_ledger_is_rename(op))
		return;

	vfdw_ledger_act1(plan, VFDW_ACT_RENAME, op->oldkey, op->oldkeylen);
	vfdw_ledger_keyset(plan, op, op->oldkey, op->oldkeylen, false);
	vfdw_ledger_keyset(plan, op, op->key, op->keylen, true);

	/*
	 * The source has to be there, and saying so needs a plan of its own: a
	 * plan's checks are all about its own key, and there is no opcode for
	 * "some other key is present". A plan with checks and no actions is a
	 * pure precondition, which is exactly what this is.
	 */
	{
		bool		firstold;
		VfdwKeyPlan *old = vfdw_ledger_plan_for(op->oldkey, op->oldkeylen,
											   op->tabletype, &firstold);

		vfdw_ledger_add_relid(old, op->relid);
		if (firstold)
			vfdw_ledger_precondition(old, true, VFDW_OP_UPDATE, false);
	}
}

/* A whole-key delete: the shapes whose row IS the key. */
static bool
vfdw_ledger_fold_key_delete(VfdwKeyPlan *plan, const VfdwWriteOp *op)
{
	if (op->kind != VFDW_OP_DELETE)
		return false;
	if (op->tabletype != VFDW_TABLE_STRING &&
		op->tabletype != VFDW_TABLE_JSON &&
		op->tabletype != VFDW_TABLE_HASH)
		return false;

	vfdw_ledger_act0(plan, VFDW_ACT_DEL);
	vfdw_ledger_keyset(plan, op, op->key, op->keylen, false);
	plan->state = VFDW_KEY_DELETED;
	return true;
}

void
vfdw_ledger_note(const VfdwWriteOp *op, Relation rel)
{
	VfdwKeyPlan *plan;
	bool		first;

	vfdw_ledger_init();

	plan = vfdw_ledger_plan_for(op->key, op->keylen, op->tabletype, &first);
	vfdw_ledger_add_relid(plan, op->relid);

	vfdw_ledger_check_duplicate(plan, op, rel, first);
	vfdw_ledger_precondition(plan, first, op->kind,
							 vfdw_ledger_is_rename(op));
	vfdw_ledger_fold_rename(plan, op);

	if (vfdw_ledger_fold_key_delete(plan, op))
		return;

	switch (op->tabletype)
	{
		case VFDW_TABLE_STRING:
		case VFDW_TABLE_JSON:
			if (op->has_value)
				vfdw_ledger_act1(plan, VFDW_ACT_SET, op->value, op->valuelen);
			break;

		case VFDW_TABLE_HASH:
			vfdw_ledger_fold_hash(plan, op, rel);
			break;

		case VFDW_TABLE_SET:
		case VFDW_TABLE_ZSET:
			vfdw_ledger_fold_member(plan, op, rel);
			break;

		case VFDW_TABLE_LIST:
			vfdw_ledger_fold_list(plan, op);
			break;
	}

	if (op->kind == VFDW_OP_INSERT)
		vfdw_ledger_keyset(plan, op, op->key, op->keylen, true);

	if (plan->state == VFDW_KEY_DELETED)
		plan->state = VFDW_KEY_LIVE;
}

void
vfdw_ledger_rebuild_from_buffer(void)
{
	const VfdwWriteOp *op;

	/*
	 * The fold can raise, and re-folding a log that is already in the buffer
	 * must not: every operation in it was accepted once already, so a
	 * conflict here would mean the two paths disagree. Guard against
	 * re-entry rather than assume it, since the raise would arrive during an
	 * abort, where a second error replaces the first and the user is told
	 * about the wrong thing.
	 */
	if (vfdw_ledger_rebuilding)
		return;

	vfdw_ledger_reset();

	if (vfdw_wbuf_first() == NULL)
		return;

	vfdw_ledger_rebuilding = true;
	PG_TRY();
	{
		/*
		 * NULL relation, and no catalog access of any kind. This runs from
		 * SUBXACT_EVENT_ABORT_SUB, where opening a relation is not allowed -
		 * it takes a lock on a resource owner that is being torn down, and
		 * the backend dies. Everything the fold needs travels on the
		 * operation itself for exactly this reason. rel is used only to name
		 * a table in an error, and the guard above means no error is raised
		 * here.
		 */
		for (op = vfdw_wbuf_first(); op != NULL; op = op->next)
			vfdw_ledger_note(op, NULL);
	}
	PG_FINALLY();
	{
		vfdw_ledger_rebuilding = false;
	}
	PG_END_TRY();
}

/* ---------------------------------------------------------------------
 * Diagnostics
 * --------------------------------------------------------------------- */

const VfdwKeyPlan *
vfdw_ledger_first(void)
{
	return vfdw_ledger_head;
}

int
vfdw_ledger_nplans(void)
{
	return vfdw_ledger_count;
}

const char *
vfdw_ledger_state_name(VfdwKeyState state)
{
	switch (state)
	{
		case VFDW_KEY_CREATED:
			return "CREATED";
		case VFDW_KEY_LIVE:
			return "LIVE";
		case VFDW_KEY_DELETED:
			return "DELETED";
	}
	return "?";
}
