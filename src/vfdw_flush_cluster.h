/*-------------------------------------------------------------------------
 *
 * vfdw_flush_cluster.h
 *		Where a transaction's writes may go on a cluster. See the .c.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_flush_cluster.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_FLUSH_CLUSTER_H
#define VFDW_FLUSH_CLUSTER_H

#include "vfdw.h"

#include "vfdw_wbuf.h"

/*
 * The single hash slot this unit writes, or -1 if it writes nothing.
 *
 * Raises if the unit spans slots: one script on one node cannot apply it, and
 * splitting it would make partial application a normal outcome.
 */
extern int	vfdw_flush_cluster_slot(const VfdwWriteUnit *unit);

#endif							/* VFDW_FLUSH_CLUSTER_H */
