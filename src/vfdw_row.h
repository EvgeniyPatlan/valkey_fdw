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

	/*
	 * The CURRENT key's per-field expiry, map->nttl entries, or NULL for a
	 * table with no ttl column.
	 *
	 * Here rather than read from a reply where it lies, because by the time a
	 * tuple is built the reply it came from is gone: a batch reply is valid
	 * only until the next one is taken, and the value reply the rest of the
	 * tuple is built from is taken after this one. See vfdw_ttl.h.
	 */
	int64	   *ttl_ms;
} VfdwRowCtx;

/*
 * Resolve everything tuple construction caches for the life of a scan.
 *
 * Here rather than in the scan because what a column's Datum is built THROUGH
 * is this file's business. A packed collection column is what makes the
 * difference visible: the function it needs cached is its ELEMENT's input
 * function, since the array is assembled from Datums and never passes through
 * array_in.
 */
extern void vfdw_row_ctx_init(VfdwRowCtx *ctx, VfdwTableMap *map,
							  MemoryContext cxt);

/*
 * Does one key of this table produce one row per member?
 *
 * Takes the MAP and not the table type, because the type alone cannot answer:
 * a packed table over a list is the same table type as a column-mapped one and
 * produces one row per KEY. Deciding from the type gives a packed table one
 * row per member with the whole collection repeated in every one of them.
 */
extern bool vfdw_scan_is_multirow(const VfdwTableMap *map);
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

/*
 * Find a field's value in a reply of alternating names and values.
 *
 * Returns the bytes as the server sent them; *data may be NULL, which is how
 * libvalkey reports a nil and is not the same as an empty value.
 */
extern bool vfdw_scan_hash_lookup(const valkeyReply *reply, const char *field,
								  const char **data, size_t *len);
extern void vfdw_scan_fill(VfdwRowCtx *ctx, TupleTableSlot *slot,
						   const char *key, size_t keylen, valkeyReply *reply);

#endif							/* VFDW_ROW_H */
