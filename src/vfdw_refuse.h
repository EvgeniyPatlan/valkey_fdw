/*-------------------------------------------------------------------------
 *
 * vfdw_refuse.h
 *		The write path's refusal gates, in one place.
 *
 * Split out of src/vfdw_modify.c when that file crossed the 800-line gate.
 * The cut is here rather than anywhere else because a refusal is the one part
 * of the write path with no state: each of these functions reads its
 * arguments, decides, and either returns or raises. Nothing in this file may
 * move into src/vfdw_wbuf.c, which section 4 pins as pure memory that touches
 * no connection.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_refuse.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_REFUSE_H
#define VFDW_REFUSE_H

#include "vfdw.h"

#include "foreign/foreign.h"
#include "nodes/plannodes.h"

#include "vfdw_map.h"

/*
 * readonly, and the per-shape writability mask.
 *
 * Applied at plan time AND again at BeginForeignModify, because a plan cached
 * before an ALTER FOREIGN TABLE must not bypass either of them.
 */
extern void vfdw_refuse_unwritable(Oid relid, const VfdwTableMap *map,
								   CmdType operation);

extern void vfdw_refuse_on_conflict(const ModifyTable *plan);
extern void vfdw_refuse_prefer_replica(const ForeignServer *server);

/*
 * A server whose refusal of the write program was recorded when the connection
 * was opened. reason is the server's own bytes and is not NUL-terminated, so
 * its length comes with it (I3); the definition says why the whole write path
 * turns on one SCRIPT LOAD and why a read is left alone.
 */
extern void vfdw_refuse_no_write_program(const ForeignServer *server,
										 const char *reason, size_t reasonlen);

/*
 * A modify callback reached with no state. Unreachable: BeginForeignModify
 * sets one for plain DML, and BeginForeignInsert for COPY FROM and tuple
 * routing. It stays because the alternative to raising here is dereferencing
 * a NULL ri_FdwState, which is what a fourth entry point added later would
 * otherwise do.
 */
extern void vfdw_refuse_no_modify_state(void);

/*
 * A key or member this transaction already created. Raised by the ledger's
 * fold at the statement, not at commit: the alternative every plain per-key
 * fold produces is a last-writer-wins overwrite that reports two rows and
 * leaves one.
 *
 * rel may be NULL. The fold also runs from a subtransaction abort, where
 * there is no relation to name and, by construction, nothing left to refuse.
 */
extern void vfdw_refuse_duplicate_key(Relation rel, const char *key,
									  size_t keylen);
extern void vfdw_refuse_duplicate_member(Relation rel, const char *key,
										 size_t keylen, const char *sub,
										 size_t sublen, const char *what);

#endif							/* VFDW_REFUSE_H */
