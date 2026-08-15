/*-------------------------------------------------------------------------
 *
 * vfdw_opcode.c
 *		Names for the check and action vocabulary. See src/vfdw_opcode.h.
 *
 * The two arrays below are index-parallel with the enums, which is the whole
 * contract: an entry inserted in the middle of one and not the other renames
 * every opcode after it, silently and consistently, so both the diagnostic
 * and the recorded file would agree on the wrong answer. The counts are
 * exported so S5 can assert the arrays and the Lua tables are the same length.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_opcode.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_opcode.h"

static const char *const vfdw_check_names[] = {
	"KEY_ABSENT", "KEY_PRESENT", "TYPE_OK",
	"HFIELD_ABSENT", "HFIELD_PRESENT",
	"SMEMBER_ABSENT", "SMEMBER_PRESENT",
	"ZMEMBER_ABSENT", "ZMEMBER_PRESENT",
	"LVALUE_COUNT_GE", "KSET_ABSENT", "KSET_PRESENT"
};

static const char *const vfdw_action_names[] = {
	"SET", "DEL", "HSET", "HDEL", "SADD", "SREM", "ZADD", "ZREM",
	"RPUSH", "LREM1", "RENAME", "KSET_ADD", "KSET_REM",
	"HPEXPIRE", "HPERSIST"
};

const char *
vfdw_ledger_check_name(int op)
{
	if (op < 0 || op >= (int) lengthof(vfdw_check_names))
		elog(ERROR, "valkey_fdw: check opcode %d out of range", op);
	return vfdw_check_names[op];
}

const char *
vfdw_ledger_action_name(int op)
{
	if (op < 0 || op >= (int) lengthof(vfdw_action_names))
		elog(ERROR, "valkey_fdw: action opcode %d out of range", op);
	return vfdw_action_names[op];
}


int
vfdw_check_op_count(void)
{
	return (int) lengthof(vfdw_check_names);
}

int
vfdw_action_op_count(void)
{
	return (int) lengthof(vfdw_action_names);
}

const char *
vfdw_ledger_require_name(VfdwRequire require)
{
	switch (require)
	{
		case VFDW_REQ_ANY:
			return "ANY";
		case VFDW_REQ_KEY_ABSENT:
			return "KEY_ABSENT";
		case VFDW_REQ_KEY_PRESENT:
			return "KEY_PRESENT";
	}
	return "?";
}
