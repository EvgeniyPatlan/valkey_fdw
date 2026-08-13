/*-------------------------------------------------------------------------
 *
 * vfdw_ledger_internal.h
 *		The ledger's own building blocks, shared with its fold half.
 *
 * Private to the ledger. src/vfdw_ledger.h is the interface everyone else
 * uses; this exists only because src/vfdw_ledger_fold.c is the same component
 * split across two files rather than a separate consumer.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_ledger_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_LEDGER_INTERNAL_H
#define VFDW_LEDGER_INTERNAL_H

#include "vfdw_ledger.h"

typedef struct VfdwLedgerKey
{
	const char *key;
	size_t		keylen;
	const char *sub;			/* field/member/value, or NULL for a key entry */
	size_t		sublen;
} VfdwLedgerKey;

typedef struct VfdwKeyEntry
{
	VfdwLedgerKey k;			/* must be first */
	VfdwKeyPlan *plan;
} VfdwKeyEntry;

/*
 * Per-member state.
 *
 * count is meaningful only for lists, where the same value may appear more
 * than once and "is it there" is not a yes/no question. For hashes, sets and
 * zsets it stays 0 and present carries the answer.
 */
typedef struct VfdwMemberEntry
{
	VfdwLedgerKey k;			/* must be first */
	bool		present;
	bool		touched;
	int			count;			/* lists: occurrences this transaction pushed */
	int			present_removals;	/* lists: occurrences it expects to find */
} VfdwMemberEntry;


/* Steps, and the two ordered lists they go on. */
extern VfdwLedgerStep *vfdw_ledger_step(int op, int nargs);
extern void vfdw_ledger_set_arg(VfdwLedgerStep *s, int i, const char *data,
								size_t len);
extern void vfdw_ledger_add_check(VfdwKeyPlan *plan, VfdwLedgerStep *s);
extern void vfdw_ledger_add_action(VfdwKeyPlan *plan, VfdwLedgerStep *s);
extern void vfdw_ledger_act0(VfdwKeyPlan *plan, int op);
extern void vfdw_ledger_act1(VfdwKeyPlan *plan, int op, const char *a,
							 size_t alen);
extern void vfdw_ledger_act2(VfdwKeyPlan *plan, int op, const char *a,
							 size_t alen, const char *b, size_t blen);
extern void vfdw_ledger_chk1(VfdwKeyPlan *plan, int op, const char *a,
							 size_t alen);

/* Per-member state, created on first touch of (key, sub). */
extern VfdwMemberEntry *vfdw_ledger_member(const char *key, size_t keylen,
										   const char *sub, size_t sublen,
										   bool *created);

#endif							/* VFDW_LEDGER_INTERNAL_H */
