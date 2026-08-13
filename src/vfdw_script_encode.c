/*-------------------------------------------------------------------------
 *
 * vfdw_script_encode.c
 *		Turning the folded ledger into one EVALSHA.
 *
 * The layout is specified in src/vfdw_script.c, beside the program that
 * decodes it. This file is the only writer of that format and the script is
 * the only reader; test/regress/sql/script.sql holds a golden vector so any
 * change to either shows up as a diff rather than as a runtime BADPROTO.
 *
 * INVARIANT W2: EVERY KEY INDEX EMITTED IS IN RANGE.
 *
 * Keys travel as 1-based indices into KEYS, never as names, because the
 * shebang makes the server enforce the declared-key rule. An index past the
 * end of KEYS is a nil in Lua, which SISMEMBER would receive as a missing
 * argument and RENAME as a nil key - failures a long way from the cause. The
 * range is therefore asserted here, where the cause is visible, rather than
 * left to be discovered as a server-side error.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_script_encode.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_script.h"

#include "common/hashfn.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "vfdw_ledger.h"
#include "vfdw_opcode.h"

/*
 * The key table: bytes to a 1-based index into KEYS.
 *
 * Plan keys go in first and in plan order, so a plan's own key index is its
 * ordinal and needs no lookup. Only the extras - a RENAME source, a keyset
 * name - are searched, and a rename whose source is itself another plan's key
 * correctly resolves to that plan rather than being declared twice.
 */
typedef struct VfdwKeyIdxEntry
{
	const char *key;
	size_t		keylen;
	int			idx;
} VfdwKeyIdxEntry;

typedef struct VfdwKeyTable
{
	HTAB	   *bybytes;
	VfdwLedgerArg *keys;
	int			nkeys;
	int			capacity;
} VfdwKeyTable;

static uint32
vfdw_keyidx_hash(const void *key, Size keysize)
{
	const VfdwKeyIdxEntry *e = (const VfdwKeyIdxEntry *) key;

	return hash_bytes((const unsigned char *) e->key, (int) e->keylen);
}

static int
vfdw_keyidx_match(const void *a, const void *b, Size keysize)
{
	const VfdwKeyIdxEntry *x = (const VfdwKeyIdxEntry *) a;
	const VfdwKeyIdxEntry *y = (const VfdwKeyIdxEntry *) b;

	if (x->keylen != y->keylen)
		return 1;
	if (x->keylen == 0)
		return 0;
	return memcmp(x->key, y->key, x->keylen) != 0;
}

static void
vfdw_keytable_init(VfdwKeyTable *t, MemoryContext mcxt, int hint)
{
	HASHCTL		ctl;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = offsetof(VfdwKeyIdxEntry, idx);
	ctl.entrysize = sizeof(VfdwKeyIdxEntry);
	ctl.hash = vfdw_keyidx_hash;
	ctl.match = vfdw_keyidx_match;
	ctl.hcxt = mcxt;

	t->bybytes = hash_create("valkey_fdw script keys", Max(hint, 16), &ctl,
							 HASH_ELEM | HASH_FUNCTION | HASH_COMPARE |
							 HASH_CONTEXT);
	t->capacity = Max(hint, 16);
	t->keys = (VfdwLedgerArg *)
		MemoryContextAlloc(mcxt, sizeof(VfdwLedgerArg) * t->capacity);
	t->nkeys = 0;
}

/* The 1-based index of a key, declaring it if this is its first mention. */
static int
vfdw_keytable_intern(VfdwKeyTable *t, MemoryContext mcxt,
					 const char *key, size_t keylen)
{
	VfdwKeyIdxEntry probe;
	VfdwKeyIdxEntry *entry;
	bool		found;

	memset(&probe, 0, sizeof(probe));
	probe.key = key;
	probe.keylen = keylen;

	entry = (VfdwKeyIdxEntry *) hash_search(t->bybytes, &probe, HASH_ENTER,
											&found);
	if (found)
		return entry->idx;

	if (t->nkeys == t->capacity)
	{
		VfdwLedgerArg *grown;

		t->capacity *= 2;
		grown = (VfdwLedgerArg *)
			MemoryContextAlloc(mcxt, sizeof(VfdwLedgerArg) * t->capacity);
		memcpy(grown, t->keys, sizeof(VfdwLedgerArg) * t->nkeys);
		t->keys = grown;
	}

	t->keys[t->nkeys].data = key;
	t->keys[t->nkeys].len = keylen;
	t->nkeys++;
	entry->idx = t->nkeys;
	return entry->idx;
}

/*
 * Which argument of a step is a key index rather than data.
 *
 * A table rather than a condition at each emit site: the encoder and the two
 * Lua dispatch tables have to agree opcode by opcode, and a rule spelled out
 * once can be read against them. Returns -1 for "no key argument".
 */
static int
vfdw_check_key_arg(int op)
{
	switch (op)
	{
		case VFDW_CHK_KSET_ABSENT:
		case VFDW_CHK_KSET_PRESENT:
			return 0;
		default:
			return -1;
	}
}

static int
vfdw_action_key_arg(int op)
{
	switch (op)
	{
		case VFDW_ACT_RENAME:
		case VFDW_ACT_KSET_ADD:
		case VFDW_ACT_KSET_REM:
			return 0;
		default:
			return -1;
	}
}

/* Declare every key the plan's steps can touch, before anything is emitted. */
static void
vfdw_script_declare_keys(VfdwKeyTable *t, MemoryContext mcxt,
						 const VfdwKeyPlan *plan)
{
	const VfdwLedgerStep *s;

	for (s = plan->checks; s != NULL; s = s->next)
	{
		int			ka = vfdw_check_key_arg(s->op);

		if (ka >= 0 && ka < s->nargs)
			(void) vfdw_keytable_intern(t, mcxt, s->args[ka].data,
										s->args[ka].len);
	}
	for (s = plan->actions; s != NULL; s = s->next)
	{
		int			ka = vfdw_action_key_arg(s->op);

		if (ka >= 0 && ka < s->nargs)
			(void) vfdw_keytable_intern(t, mcxt, s->args[ka].data,
										s->args[ka].len);
	}
}

static void
vfdw_script_emit_steps(VfdwCmd *cmd, VfdwKeyTable *t,
					   const VfdwLedgerStep *head, bool is_action)
{
	const VfdwLedgerStep *s;

	for (s = head; s != NULL; s = s->next)
	{
		int			ka = is_action ? vfdw_action_key_arg(s->op)
			: vfdw_check_key_arg(s->op);
		int			i;

		vfdw_cmd_add_int(cmd, s->op);
		vfdw_cmd_add_int(cmd, s->nargs);

		for (i = 0; i < s->nargs; i++)
		{
			if (i == ka)
			{
				VfdwKeyIdxEntry probe;
				VfdwKeyIdxEntry *entry;
				bool		found;

				memset(&probe, 0, sizeof(probe));
				probe.key = s->args[i].data;
				probe.keylen = s->args[i].len;
				entry = (VfdwKeyIdxEntry *) hash_search(t->bybytes, &probe,
														HASH_FIND, &found);

				/*
				 * W2. Declaration ran over these same steps a moment ago, so
				 * a miss here means the two walks disagree - which would ship
				 * an index the script resolves to nil.
				 */
				if (!found || entry->idx < 1 || entry->idx > t->nkeys)
					elog(ERROR,
						 "valkey_fdw: key argument %d of opcode %d is not a "
						 "declared key", i, s->op);

				vfdw_cmd_add_int(cmd, entry->idx);
			}
			else
				vfdw_cmd_add_bytes(cmd, s->args[i].data, s->args[i].len);
		}
	}
}

void
vfdw_script_encode(VfdwCmd *cmd, MemoryContext mcxt)
{
	VfdwKeyTable t;
	const VfdwKeyPlan *plan;
	int			nplans = vfdw_ledger_nplans();
	int			i;

	vfdw_keytable_init(&t, mcxt, nplans + 8);

	/*
	 * Two passes, and the order is the point. Every key must be declared
	 * before KEYS is written, and KEYS is written before any ARGV - so the
	 * whole key set has to be known before a single byte goes into the
	 * command. Interning during emission would need numkeys before it was
	 * knowable.
	 */
	for (plan = vfdw_ledger_first(); plan != NULL; plan = plan->next)
		(void) vfdw_keytable_intern(&t, mcxt, plan->key, plan->keylen);
	for (plan = vfdw_ledger_first(); plan != NULL; plan = plan->next)
		vfdw_script_declare_keys(&t, mcxt, plan);

	vfdw_cmd_add_cstr(cmd, "EVALSHA");
	vfdw_cmd_add_cstr(cmd, vfdw_script_sha1());
	vfdw_cmd_add_int(cmd, t.nkeys);
	for (i = 0; i < t.nkeys; i++)
		vfdw_cmd_add_bytes(cmd, t.keys[i].data, t.keys[i].len);

	vfdw_cmd_add_cstr(cmd, "V1");
	vfdw_cmd_add_int(cmd, nplans);

	for (plan = vfdw_ledger_first(), i = 1; plan != NULL;
		 plan = plan->next, i++)
	{
		/*
		 * The plan's own key index is its ordinal by construction: the first
		 * pass interned exactly the plan keys, in order, before any extra.
		 * Asserted rather than assumed, because the two loops above are what
		 * makes it true and they are far enough apart to be edited alone.
		 */
		if (i < 1 || i > t.nkeys)
			elog(ERROR, "valkey_fdw: plan %d has no declared key", i);

		vfdw_cmd_add_int(cmd, i);
		vfdw_cmd_add_int(cmd, (int) plan->ttype);
		vfdw_cmd_add_int(cmd, (int) plan->require);
		vfdw_cmd_add_int(cmd, plan->nchecks);
		vfdw_cmd_add_int(cmd, plan->nactions);

		vfdw_script_emit_steps(cmd, &t, plan->checks, false);
		vfdw_script_emit_steps(cmd, &t, plan->actions, true);
	}

	hash_destroy(t.bybytes);
}
