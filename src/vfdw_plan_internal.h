/*-------------------------------------------------------------------------
 *
 * vfdw_plan_internal.h
 *		Shared between src/vfdw_plan.c and its parameterised-path half.
 *
 * Private to the planner. vfdw_scan.h keeps the callbacks' own interface;
 * this exists because the two files are one component split for length, in
 * the same way vfdw_scan_internal.h serves the scan.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_plan_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_PLAN_INTERNAL_H
#define VFDW_PLAN_INTERNAL_H

#include "vfdw.h"

#include "nodes/pathnodes.h"

#include "vfdw_map.h"

/* Is this node the key column of the relation being planned? */
extern bool vfdw_is_key_var(RelOptInfo *baserel, VfdwTableMap *map, Node *node);

/* Is this operator equality for that type, and safe to narrow by? */
extern bool vfdw_is_equality(Oid opno, Oid vartype);

/* May this table's key column be pushed down at all? */
extern bool vfdw_key_column_pushable(VfdwTableMap *map);

/* The join clause this table can be driven by; see the definition. */
extern Node *vfdw_plan_param_key(PlannerInfo *root, RelOptInfo *baserel,
								 VfdwTableMap *map, Relids *outer);

#endif							/* VFDW_PLAN_INTERNAL_H */
