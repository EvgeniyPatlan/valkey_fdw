/*-------------------------------------------------------------------------
 *
 * vfdw_map_check.h
 *		The shape rules vfdw_map.c applies after resolving a table.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_map_check.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_MAP_CHECK_H
#define VFDW_MAP_CHECK_H

#include "vfdw_map.h"

#include "access/tupdesc.h"

/* Reject column kinds that cannot mean anything for this table type. */
extern void vfdw_map_check_types(VfdwTableMap *map, TupleDesc tupdesc);

/* Refuse what the option grammar accepts but the scan cannot yet read. */
extern void vfdw_map_check_implemented(VfdwTableMap *map, TupleDesc tupdesc);

#endif							/* VFDW_MAP_CHECK_H */
