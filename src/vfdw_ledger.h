/*-------------------------------------------------------------------------
 *
 * vfdw_ledger.h
 *		Folding the write buffer into one plan per Valkey key.
 *
 * The buffer is an ordered log of what SQL did. The ledger is what the server
 * has to be told: for each key, what must already be true, and the ordered
 * list of writes that follow. One fold, two consumers - the flush encoder
 * (S5/S6) and the overlay (S7) - so that what a SELECT sees inside the
 * transaction and what COMMIT applies cannot disagree by construction.
 *
 * PRECONDITIONS COME FROM THE FIRST OPERATION ON A KEY, NOT FROM EVERY ONE.
 * A first INSERT means the key must be absent; a first UPDATE or DELETE means
 * it must be present. That single rule is what makes intra-transaction
 * uniqueness and cross-transaction uniqueness the same question: the check
 * the server runs at flush is the same one this file enforces in memory, so a
 * second INSERT of a key raises 23505 here at the statement rather than
 * silently becoming a last-writer-wins overwrite that reports two rows.
 *
 * The same rule applies one level down, at (key, field) for hashes and
 * (key, member) for sets and zsets. Lists carry an occurrence count instead,
 * because a list may legitimately hold the same value more than once.
 *
 * MEMORY. The ledger has its own context, not the buffer's. It is rebuilt
 * from scratch whenever a subtransaction truncates the log, and rebuilding
 * into the buffer's context would pile a dead copy onto write_max_bytes every
 * time a savepoint rolled back. It points INTO the buffer for all key and
 * value bytes and copies none of them, so it must never outlive the buffer -
 * vfdw_wbuf_reset drops both together.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_ledger.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_LEDGER_H
#define VFDW_LEDGER_H

#include "vfdw.h"

#include "utils/relcache.h"

#include "vfdw_map.h"
#include "vfdw_opcode.h"
#include "vfdw_wbuf.h"

/* One argument of a check or an action. Points into the write buffer. */
typedef struct VfdwLedgerArg
{
	const char *data;
	size_t		len;
} VfdwLedgerArg;

typedef struct VfdwLedgerStep
{
	struct VfdwLedgerStep *next;
	int			op;				/* VfdwCheckOp or VfdwActionOp */
	int			nargs;
	VfdwLedgerArg *args;
} VfdwLedgerStep;

/*
 * The logical state of a key within this transaction.
 *
 * CREATED and LIVE are kept apart even though both mean "exists now": only
 * CREATED says this transaction is the one that made it, which is what the
 * overlay needs in S7 to decide whether a row is visible to a scan that ran
 * before the INSERT.
 */
typedef enum VfdwKeyState
{
	VFDW_KEY_CREATED = 0,		/* this transaction created it */
	VFDW_KEY_LIVE,				/* it existed and this transaction changed it */
	VFDW_KEY_DELETED			/* this transaction removed it */
} VfdwKeyState;

typedef struct VfdwKeyPlan
{
	struct VfdwKeyPlan *next;	/* first-touch order */

	const char *key;
	size_t		keylen;

	VfdwTableType ttype;
	VfdwRequire require;
	VfdwKeyState state;

	VfdwLedgerStep *checks;
	VfdwLedgerStep *checks_tail;
	VfdwLedgerStep *actions;
	VfdwLedgerStep *actions_tail;

	int			nchecks;
	int			nactions;

	/*
	 * Every relation that contributed an operation to this key. Two foreign
	 * tables may legitimately map one key - a hash mapping disjoint field
	 * sets is the normal case - and an error naming only the last one seen
	 * sends the reader to the wrong table definition.
	 */
	Oid		   *relids;
	int			nrelids;
} VfdwKeyPlan;

/*
 * Fold one operation into the ledger, raising if it conflicts with what this
 * transaction has already done. Called by each modify callback once
 * vfdw_wbuf_append has linked the operation, so that the incremental fold and
 * vfdw_ledger_rebuild_from_buffer see the same sequence and must agree - an
 * equivalence the suite asserts rather than assumes.
 */
extern void vfdw_ledger_note(const VfdwWriteOp *op, Relation rel);

/* Discard and re-fold the surviving log. Called after any truncation. */
extern void vfdw_ledger_rebuild_from_buffer(void);

/* Drop everything. Called from vfdw_wbuf_reset. */
extern void vfdw_ledger_reset(void);

/* Diagnostics and, from S5, the encoder's input. */
extern const VfdwKeyPlan *vfdw_ledger_first(void);
extern int	vfdw_ledger_nplans(void);

extern const char *vfdw_ledger_state_name(VfdwKeyState state);

#endif							/* VFDW_LEDGER_H */
