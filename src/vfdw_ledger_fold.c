/*-------------------------------------------------------------------------
 *
 * vfdw_ledger_fold.c
 *		Folding one operation into a key's plan, per table shape.
 *
 * The rule these share is stated once, in src/vfdw_ledger.h: a precondition
 * comes from the FIRST touch of a thing, not from every touch. Each function
 * here applies it one level down - at (key, field) for hashes, (key, member)
 * for sets and zsets, and as an occurrence count for lists, where the same
 * value may legitimately appear more than once.
 *
 * Split out of vfdw_ledger.c at the 800-line gate.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_ledger_fold.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_ledger_fold.h"

#include "utils/memutils.h"

#include "vfdw_refuse.h"

/* The ledger owns the context everything below allocates in. */
extern MemoryContext vfdw_ledger_cxt;

void
vfdw_ledger_fold_hash(VfdwKeyPlan *plan, const VfdwWriteOp *op, Relation rel)
{
	int			i;

	for (i = 0; i < op->nfields; i++)
	{
		const VfdwWriteArg *f = &op->fields[i];
		bool		firstf;
		VfdwMemberEntry *m = vfdw_ledger_member(op->key, op->keylen,
												f->name, f->namelen, &firstf);

		/*
		 * The member precondition, like the key one, comes from the first
		 * touch. An INSERT of a field this transaction already set is the
		 * same duplicate as an INSERT of a key it already created.
		 */
		if (firstf)
		{
			m->touched = true;

			/*
			 * A key this unit CREATES - by INSERT, or as a rename's
			 * destination - cannot already hold the field, so neither a
			 * present nor an absent check means anything on it. Requiring
			 * presence there is how a rename came to demand its own
			 * destination already be populated.
			 */
			if (plan->state == VFDW_KEY_CREATED)
				 /* no field precondition is meaningful */ ;
			else if (op->kind == VFDW_OP_INSERT)
				vfdw_ledger_chk1(plan, VFDW_CHK_HFIELD_ABSENT,
								 f->name, f->namelen);
			else
				vfdw_ledger_chk1(plan, VFDW_CHK_HFIELD_PRESENT,
								 f->name, f->namelen);
		}
		else if (op->kind == VFDW_OP_INSERT && m->present)
			vfdw_refuse_duplicate_member(rel, op->key, op->keylen,
											   f->name, f->namelen, "field");

		if (f->isnull)
		{
			vfdw_ledger_act1(plan, VFDW_ACT_HDEL, f->name, f->namelen);
			m->present = false;
		}
		else
		{
			VfdwLedgerStep *s = vfdw_ledger_step(VFDW_ACT_HSET, 2);

			vfdw_ledger_set_arg(s, 0, f->name, f->namelen);
			vfdw_ledger_set_arg(s, 1, f->data, f->len);
			vfdw_ledger_add_action(plan, s);
			m->present = true;
		}
	}
}

/*
 * The member precondition, established by the first touch of (key, member).
 *
 * The member whose prior existence this is about is the one the ROW HAD,
 * which on a rename is oldmember rather than member. Asking for the new name
 * to be present would refuse every rename - it is exactly the name that must
 * not be there yet.
 */
static VfdwMemberEntry *
vfdw_ledger_member_precondition(VfdwKeyPlan *plan, const VfdwWriteOp *op,
								Relation rel, bool is_zset)
{
	const char *idm = op->has_oldmember ? op->oldmember : op->member;
	size_t		idmlen = op->has_oldmember ? op->oldmemberlen : op->memberlen;
	bool		firstm;
	VfdwMemberEntry *m = vfdw_ledger_member(op->key, op->keylen, idm, idmlen,
										   &firstm);

	if (firstm)
	{
		m->touched = true;

		/*
		 * A key this unit creates cannot already hold the member, in either
		 * direction: the server-side check would be dead weight on every row
		 * of a bulk load, and a presence check would be actively wrong. The
		 * in-memory ledger still catches a repeat, which is where a repeat
		 * actually comes from.
		 */
		if (plan->state == VFDW_KEY_CREATED)
			 /* no member precondition is meaningful */ ;
		else if (op->kind == VFDW_OP_INSERT)
			vfdw_ledger_chk1(plan,
							 is_zset ? VFDW_CHK_ZMEMBER_ABSENT
							 : VFDW_CHK_SMEMBER_ABSENT, idm, idmlen);
		else
			vfdw_ledger_chk1(plan,
							 is_zset ? VFDW_CHK_ZMEMBER_PRESENT
							 : VFDW_CHK_SMEMBER_PRESENT, idm, idmlen);
	}
	else if (op->kind == VFDW_OP_INSERT && m->present)
		vfdw_refuse_duplicate_member(rel, op->key, op->keylen,
										   idm, idmlen, "member");

	return m;
}

void
vfdw_ledger_fold_member(VfdwKeyPlan *plan, const VfdwWriteOp *op, Relation rel)
{
	bool		is_zset = (plan->ttype == VFDW_TABLE_ZSET);
	VfdwMemberEntry *m;

	if (!op->has_member)
		return;

	m = vfdw_ledger_member_precondition(plan, op, rel, is_zset);

	if (op->kind == VFDW_OP_DELETE)
	{
		vfdw_ledger_act1(plan, is_zset ? VFDW_ACT_ZREM : VFDW_ACT_SREM,
						 op->member, op->memberlen);
		m->present = false;
		return;
	}

	/*
	 * A member rename is a remove and an add, and the remove has to come
	 * first: adding the new name before removing the old is correct only
	 * because they differ, and would silently delete the row if a later
	 * change ever made them equal.
	 */
	if (op->kind == VFDW_OP_UPDATE && op->has_oldmember &&
		(op->oldmemberlen != op->memberlen ||
		 memcmp(op->oldmember, op->member, op->memberlen) != 0))
	{
		bool		firstn;

		vfdw_ledger_act1(plan, is_zset ? VFDW_ACT_ZREM : VFDW_ACT_SREM,
						 op->oldmember, op->oldmemberlen);
		m->present = false;

		/* From here on the row is known by its new name. */
		m = vfdw_ledger_member(op->key, op->keylen, op->member,
							   op->memberlen, &firstn);
		m->touched = true;
	}

	if (is_zset)
	{
		VfdwLedgerStep *s = vfdw_ledger_step(VFDW_ACT_ZADD, 2);

		vfdw_ledger_set_arg(s, 0, op->has_score ? op->score : "0",
							op->has_score ? op->scorelen : 1);
		vfdw_ledger_set_arg(s, 1, op->member, op->memberlen);
		vfdw_ledger_add_action(plan, s);
	}
	else
		vfdw_ledger_act1(plan, VFDW_ACT_SADD, op->member, op->memberlen);

	m->present = true;
}

/*
 * A list is the one shape where the same value may appear more than once, so
 * "is it present" is replaced by a count and the check is LVALUE_COUNT_GE.
 */
void
vfdw_ledger_fold_list(VfdwKeyPlan *plan, const VfdwWriteOp *op)
{
	bool		firstm;
	VfdwMemberEntry *m;

	if (!op->has_member)
		return;

	m = vfdw_ledger_member(op->key, op->keylen, op->member, op->memberlen,
						   &firstm);
	m->touched = true;

	if (op->kind == VFDW_OP_DELETE)
	{
		/*
		 * The count this transaction is about to remove has to exist BEFORE
		 * it starts, and only the removals it has not itself pushed need to
		 * be there. Counting them all would refuse a transaction that pushed
		 * a value and then removed it.
		 */
		if (m->count > 0)
			m->count--;
		else
		{
			VfdwLedgerStep *s = vfdw_ledger_step(VFDW_CHK_LVALUE_COUNT_GE, 2);
			char	   *n = MemoryContextAlloc(vfdw_ledger_cxt, 16);

			snprintf(n, 16, "%d", ++m->present_removals);
			vfdw_ledger_set_arg(s, 0, op->member, op->memberlen);
			vfdw_ledger_set_arg(s, 1, n, strlen(n));
			vfdw_ledger_add_check(plan, s);
		}
		vfdw_ledger_act1(plan, VFDW_ACT_LREM1, op->member, op->memberlen);
		return;
	}

	vfdw_ledger_act1(plan, VFDW_ACT_RPUSH, op->member, op->memberlen);
	m->count++;
}


