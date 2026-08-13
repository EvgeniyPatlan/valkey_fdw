/*-------------------------------------------------------------------------
 *
 * vfdw_flush_cluster.c
 *		Where a transaction's writes may go on a cluster.
 *
 * THE PHASE 3 GUARANTEE DOES NOT SURVIVE A CLUSTER UNCHANGED, and this file
 * is where that is decided rather than discovered.
 *
 * A transaction's writes are applied by ONE Lua program on ONE connection at
 * pre-commit, so Valkey applies all of them or none. A cluster breaks the
 * premise: keys in different hash slots live on different primaries and a
 * single EVAL may only touch one slot - CROSSSLOT is the server refusing
 * exactly this. So a cross-slot transaction cannot be one program, and the
 * atomicity the README promises cannot be preserved as stated.
 *
 * THE CHOICE MADE HERE IS TO REFUSE. The alternative - one program per node,
 * applied in sequence - makes partial application a normal outcome, which is
 * the single thing Phase 3 works hardest to prevent, and it would have to be
 * documented as such rather than shipped quietly. A user who has not thought
 * about slots should not discover partial application in production.
 *
 * The refusal is actionable, which is what makes it a design rather than a
 * limitation: hash tags put related keys in one slot by construction, so
 * {tenant42}:orders and {tenant42}:items are writable in one transaction.
 * The message says so, because a refusal a user cannot act on is just a wall.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_flush_cluster.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "vfdw.h"

#include "vfdw_cluster.h"
#include "vfdw_error.h"
#include "vfdw_flush_cluster.h"
#include "vfdw_val.h"
#include "vfdw_wbuf.h"

/*
 * Every key one operation touches is in the same slot, or we refuse.
 *
 * `key` alone is not enough. A rename moves a row from oldkey to key and both
 * are written; a keyset table also writes the index. Checking only the first
 * would let a cross-slot unit through as single-slot, and the server would
 * then answer CROSSSLOT from inside the program - after the earlier writes in
 * it had already applied. That is outcome 4, partial application, arrived at
 * by the exact route this check exists to close.
 */
static void
vfdw_flush_slot_of(const char *key, size_t keylen, const char *what,
				   int *slot, const char **firstkey, size_t *firstlen)
{
	int			this_slot;

	if (key == NULL || keylen == 0)
		return;

	this_slot = (int) vfdw_val_slot(key, keylen);

	if (*slot < 0)
	{
		*slot = this_slot;
		*firstkey = key;
		*firstlen = keylen;
		return;
	}

	if (this_slot == *slot)
		return;

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("a transaction writing this Valkey Cluster spans more "
					"than one hash slot"),
	/*
	 * Both keys come out of the write buffer, so both are bytes a bytea key
	 * column put there and neither is ours. Invariant I2 covers them exactly
	 * as it covers a reply body: a length-counted "%.*s" straight from the
	 * source bytes is the spelling that bypasses the check.
	 */
			 errdetail("Key (%s) is in slot %d, but this transaction "
					   "already writes (%s) in slot %d%s.",
					   vfdw_safe_text(key, keylen), this_slot,
					   vfdw_safe_text(*firstkey, *firstlen), *slot, what),
			 errhint("A write is applied by one script on one node, so every "
					 "key it touches must share a slot. Put related keys in "
					 "the same slot with a hash tag - \"{tenant}:orders\" and "
					 "\"{tenant}:items\" hash only the braced part - or write "
					 "them in separate transactions.")));
}

/*
 * The one slot this unit writes, or -1 when it writes nothing.
 *
 * Called before the connection is taken, so a refusal costs no round trip and
 * cannot leave a connection mid-conversation.
 */
int
vfdw_flush_cluster_slot(const VfdwWriteUnit *unit)
{
	const VfdwWriteOp *op;
	const char *firstkey = NULL;
	size_t		firstlen = 0;
	int			slot = -1;

	for (op = vfdw_wbuf_first(); op != NULL; op = op->next)
	{
		vfdw_flush_slot_of(op->key, op->keylen, "", &slot, &firstkey,
						   &firstlen);
		vfdw_flush_slot_of(op->oldkey, op->oldkeylen,
						   " (the row is being renamed)", &slot, &firstkey,
						   &firstlen);
		vfdw_flush_slot_of(op->keyset, op->keysetlen,
						   " (the table's keyset index)", &slot, &firstkey,
						   &firstlen);
	}

	(void) unit;
	return slot;
}
