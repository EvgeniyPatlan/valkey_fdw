/*-------------------------------------------------------------------------
 *
 * vfdw_row.h
 *		Turning a Valkey reply into a PostgreSQL tuple.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_row.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_ROW_H
#define VFDW_ROW_H

#include "vfdw.h"

#include <valkey/valkey.h>

#include "nodes/execnodes.h"

#include "vfdw_map.h"

/*
 * Everything tuple construction needs from the scan. Passing this rather
 * than the whole scan state keeps the dependency one-way.
 */
typedef struct VfdwRowCtx
{
	VfdwTableMap *map;
	FmgrInfo   *infuncs;		/* one per attribute */

	/*
	 * Per-attribute DomainIOData cache for the binary branch, which bypasses
	 * the input function and so has to apply a domain's constraints itself.
	 * Both may be NULL, at the cost of rebuilding the check per value.
	 */
	void	  **domain_extra;	/* one per attribute, lazily filled */
	MemoryContext cache_cxt;	/* where those entries live */

	int			cur_elem;		/* element index within a collection reply */
} VfdwRowCtx;

extern bool vfdw_scan_is_multirow(VfdwTableType type);
extern int	vfdw_scan_member_stride(VfdwTableType type, const valkeyReply *reply);

/*
 * Bytes from Valkey to a Datum of the column's declared type.
 *
 * The inbound half of the round trip src/vfdw_val.c inverts; exposed so a
 * diagnostic can drive both halves through the code the scan actually uses,
 * rather than through a copy that could agree with itself and with nothing
 * else.
 */
extern Datum vfdw_row_datum_from_bytes(VfdwRowCtx *ctx, const VfdwColumn *col,
									   const char *data, size_t len);
extern void vfdw_scan_fill(VfdwRowCtx *ctx, TupleTableSlot *slot,
						   const char *key, size_t keylen, valkeyReply *reply);

#endif							/* VFDW_ROW_H */
