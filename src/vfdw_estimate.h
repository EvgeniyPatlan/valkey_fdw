/*-------------------------------------------------------------------------
 *
 * vfdw_estimate.h
 *		Plan-time row counts for the shapes the server can count cheaply.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_estimate.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_ESTIMATE_H
#define VFDW_ESTIMATE_H

#include "vfdw_map.h"

/*
 * Rows this table is estimated to hold, or -1 when nothing cheap can say.
 *
 * -1 means "use the placeholder", not "zero rows". The two shapes that can be
 * asked are singleton_key and keyset; everything else is a keyspace, and
 * counting one means scanning it. See the definition for why a collection's
 * size is not always its row count.
 */
extern double vfdw_estimate_rows(Oid relid, const VfdwTableMap *map);

#endif							/* VFDW_ESTIMATE_H */
