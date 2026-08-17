/*-------------------------------------------------------------------------
 *
 * vfdw_ledger_fold.h
 *		Folding one operation into a key's plan, per table shape.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_ledger_fold.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_LEDGER_FOLD_H
#define VFDW_LEDGER_FOLD_H

#include "vfdw_ledger_internal.h"

extern void vfdw_ledger_fold_hash(VfdwKeyPlan *plan, const VfdwWriteOp *op,
								  Relation rel);
extern void vfdw_ledger_fold_member(VfdwKeyPlan *plan, const VfdwWriteOp *op,
									Relation rel);
extern void vfdw_ledger_fold_list(VfdwKeyPlan *plan, const VfdwWriteOp *op);

/*
 * A packed row: empty the key, then rebuild it from the array. See the
 * definition for why the DEL is unconditional.
 */
extern void vfdw_ledger_fold_packed(VfdwKeyPlan *plan, const VfdwWriteOp *op);

#endif							/* VFDW_LEDGER_FOLD_H */
