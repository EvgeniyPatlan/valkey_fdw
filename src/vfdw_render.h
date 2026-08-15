/*-------------------------------------------------------------------------
 *
 * vfdw_render.h
 *		Turning one tuple slot into the bytes an operation carries.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_render.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_RENDER_H
#define VFDW_RENDER_H

#include "vfdw.h"

#include "executor/tuptable.h"

#include "vfdw_modify.h"
#include "vfdw_wbuf.h"

extern const char *vfdw_modify_retain_name(const char *field, size_t *namelen);
extern void vfdw_modify_render_fields(VfdwModifyState *st,
									  TupleTableSlot *slot, VfdwWriteOp *op);

/* The expiries a row sets; see the definition for why they are not fields. */
extern void vfdw_modify_render_ttls(VfdwModifyState *st, TupleTableSlot *slot,
									VfdwWriteOp *op);
extern void vfdw_modify_render_payload(VfdwModifyState *st,
									   TupleTableSlot *slot, VfdwWriteOp *op);

#endif							/* VFDW_RENDER_H */
