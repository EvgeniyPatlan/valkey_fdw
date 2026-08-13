/*-------------------------------------------------------------------------
 *
 * vfdw_opcode.h
 *		The check and action vocabulary shared by the ledger and the script.
 *
 * A file of its own because it has two independent readers that must agree
 * byte for byte: src/vfdw_ledger.c emits these opcodes, and the Lua program
 * in src/vfdw_script.c (S5) decodes them through two dispatch tables. Keeping
 * the enum and its names in one small place is what lets S5 assert parity
 * between them mechanically rather than by review.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_opcode.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_OPCODE_H
#define VFDW_OPCODE_H

#include "vfdw.h"

/*
 * What must be true of the key before any action on it runs.
 *
 * ANY is not "no check": it is the state of a key whose plan carries only a
 * TYPE_OK, which every plan has. It exists so a later step cannot mistake
 * "nothing recorded" for "absence required".
 */
typedef enum VfdwRequire
{
	VFDW_REQ_ANY = 0,
	VFDW_REQ_KEY_ABSENT,
	VFDW_REQ_KEY_PRESENT
} VfdwRequire;

/*
 * Check and action opcodes.
 *
 * The numbering is wire-visible from S5 on: it is encoded into ARGV and
 * decoded by two Lua dispatch tables. Append only, and never renumber - a
 * reordered enum and an unchanged script disagree silently, each doing
 * something valid for the wrong opcode. S5 adds the parity assertion that
 * makes that a test failure rather than a corruption.
 */
typedef enum VfdwCheckOp
{
	VFDW_CHK_KEY_ABSENT = 0,
	VFDW_CHK_KEY_PRESENT,
	VFDW_CHK_TYPE_OK,
	VFDW_CHK_HFIELD_ABSENT,
	VFDW_CHK_HFIELD_PRESENT,
	VFDW_CHK_SMEMBER_ABSENT,
	VFDW_CHK_SMEMBER_PRESENT,
	VFDW_CHK_ZMEMBER_ABSENT,
	VFDW_CHK_ZMEMBER_PRESENT,
	VFDW_CHK_LVALUE_COUNT_GE,
	VFDW_CHK_KSET_ABSENT,
	VFDW_CHK_KSET_PRESENT
} VfdwCheckOp;

typedef enum VfdwActionOp
{
	VFDW_ACT_SET = 0,
	VFDW_ACT_DEL,
	VFDW_ACT_HSET,
	VFDW_ACT_HDEL,
	VFDW_ACT_SADD,
	VFDW_ACT_SREM,
	VFDW_ACT_ZADD,
	VFDW_ACT_ZREM,
	VFDW_ACT_RPUSH,
	VFDW_ACT_LREM1,
	VFDW_ACT_RENAME,
	VFDW_ACT_KSET_ADD,
	VFDW_ACT_KSET_REM
} VfdwActionOp;

/*
 * Names rather than numbers in every diagnostic and every recorded file.
 *
 * A recorded expected file full of small integers is one renumbering away
 * from being wrong and still passing, because every number would move
 * together. These names are also what S5 compares against the Lua tables.
 */
extern const char *vfdw_ledger_require_name(VfdwRequire require);
extern const char *vfdw_ledger_check_name(int op);
extern const char *vfdw_ledger_action_name(int op);

/* How many of each the enums define; S5's parity assertion reads these. */
extern int	vfdw_check_op_count(void);
extern int	vfdw_action_op_count(void);

#endif							/* VFDW_OPCODE_H */
