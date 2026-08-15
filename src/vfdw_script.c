/*-------------------------------------------------------------------------
 *
 * vfdw_script.c
 *		The server-side program, its identity, and how its replies are read.
 *
 * See src/vfdw_script.h for the two-phase rule, why no user data is ever
 * concatenated into the text, and why the write verbs are literal.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_script.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_script.h"

#include "common/cryptohash.h"
#include "common/sha1.h"
#include "utils/memutils.h"

#include "vfdw_opcode.h"

/*
 * ARGV layout, which this program and src/vfdw_script_encode.c must agree on:
 *
 *	ARGV[1]  "V1"
 *	ARGV[2]  number of plans
 *	per plan:  keyidx ttype require nchecks nactions
 *	           nchecks  x  checkop  nargs arg...
 *	           nactions x  actionop nargs arg...
 *
 * Opcodes are the integers of VfdwCheckOp and VfdwActionOp. The two dispatch
 * tables below are index-parallel with those enums - Lua indexes from 1, so
 * each table is written with an explicit [0]= first entry rather than as a
 * bare list, which would silently shift every opcode by one.
 *
 * Arguments that are KEYS arrive as 1-based indices into KEYS, not as names:
 * RENAME's source, and the keyset name for KSET_ABSENT/KSET_PRESENT/KSET_ADD/
 * KSET_REM. With the shebang the server enforces the declared-key rule, so a
 * name passed through ARGV would be an undeclared key - accepted on a single
 * node right up until a slot migrates, then refused. vfdw_script_encode.c owns
 * the mapping and asserts every index it emits is in range (invariant W2).
 */
static const char vfdw_script_src[] =
"#!lua\n"
"local V = 'VFDW1'\n"
"local i = 1\n"
"local function nextarg() local v = ARGV[i]; i = i + 1; return v end\n"
"local function fail(code, plan, key, detail)\n"
"  local m = V .. ' ' .. code .. ' ' .. plan .. ' ' .. (key or '-')\n"
"  if detail then m = m .. ' ' .. detail end\n"
"  return server.error_reply(m)\n"
"end\n"
"\n"
"if nextarg() ~= 'V1' then return fail('BADPROTO', 0, '-', 'version') end\n"
"local nplans = tonumber(nextarg())\n"
"if nplans == nil then return fail('BADPROTO', 0, '-', 'nplans') end\n"
"\n"
"-- Decode every plan up front. A decode failure after a write would be the\n"
"-- partially-applied outcome; here nothing has been written yet.\n"
"local plans = {}\n"
"for p = 1, nplans do\n"
"  local kx = tonumber(nextarg())\n"
"  local ttype = tonumber(nextarg())\n"
"  local require_ = tonumber(nextarg())\n"
"  local nchecks = tonumber(nextarg())\n"
"  local nactions = tonumber(nextarg())\n"
"  if kx == nil or ttype == nil or require_ == nil\n"
"     or nchecks == nil or nactions == nil then\n"
"    return fail('BADPROTO', p, '-', 'plan header')\n"
"  end\n"
"  if kx < 1 or kx > #KEYS then return fail('BADPROTO', p, '-', 'keyidx') end\n"
"  local checks, actions = {}, {}\n"
"  for c = 1, nchecks do\n"
"    local op = tonumber(nextarg())\n"
"    local n = tonumber(nextarg())\n"
"    if op == nil or n == nil then return fail('BADPROTO', p, KEYS[kx], 'check') end\n"
"    local a = {}\n"
"    for x = 1, n do a[x] = nextarg() end\n"
"    checks[c] = {op = op, a = a}\n"
"  end\n"
"  for c = 1, nactions do\n"
"    local op = tonumber(nextarg())\n"
"    local n = tonumber(nextarg())\n"
"    if op == nil or n == nil then return fail('BADPROTO', p, KEYS[kx], 'action') end\n"
"    local a = {}\n"
"    for x = 1, n do a[x] = nextarg() end\n"
"    actions[c] = {op = op, a = a}\n"
"  end\n"
"  plans[p] = {k = KEYS[kx], ttype = ttype, req = require_,\n"
"              checks = checks, actions = actions}\n"
"end\n"
"if i ~= #ARGV + 1 then return fail('BADPROTO', 0, '-', 'trailing') end\n"
"\n"
"local TYPENAME = {[0]='string',[1]='hash',[2]='list',[3]='set',[4]='zset',[5]='string'}\n"
"\n"
"-- ==== PHASE 1: CHECKS ONLY. READ VERBS ONLY. SINGLE EXIT. ====\n"
"local CHECK = {\n"
"  [0] = function(k, a) if server.call('EXISTS', k) == 1 then return 'CONFLICT' end end,\n"
"  [1] = function(k, a) if server.call('EXISTS', k) == 0 then return 'MISSING' end end,\n"
"  [2] = nil,\n"
"  [3] = function(k, a) if server.call('HEXISTS', k, a[1]) == 1 then return 'CONFLICT' end end,\n"
"  [4] = function(k, a) if server.call('HEXISTS', k, a[1]) == 0 then return 'MISSING' end end,\n"
"  [5] = function(k, a) if server.call('SISMEMBER', k, a[1]) == 1 then return 'CONFLICT' end end,\n"
"  [6] = function(k, a) if server.call('SISMEMBER', k, a[1]) == 0 then return 'MISSING' end end,\n"
"  [7] = function(k, a) if server.call('ZSCORE', k, a[1]) then return 'CONFLICT' end end,\n"
"  [8] = function(k, a) if not server.call('ZSCORE', k, a[1]) then return 'MISSING' end end,\n"
"  [9] = function(k, a)\n"
"          local n, want = 0, tonumber(a[2])\n"
"          for _, v in ipairs(server.call('LRANGE', k, 0, -1)) do\n"
"            if v == a[1] then n = n + 1 end\n"
"          end\n"
"          if n < want then return 'MISSING' end\n"
"        end,\n"
"  [10] = function(k, a) if server.call('SISMEMBER', KEYS[tonumber(a[1])], a[2]) == 1 then return 'CONFLICT' end end,\n"
"  [11] = function(k, a) if server.call('SISMEMBER', KEYS[tonumber(a[1])], a[2]) == 0 then return 'MISSING' end end,\n"
"}\n"
"\n"
"for p = 1, nplans do\n"
"  local pl = plans[p]\n"
"  -- TYPE_OK is implicit in every plan and is checked here rather than\n"
"  -- through the table: it is what makes phase 2 incapable of WRONGTYPE.\n"
"  local t = server.call('TYPE', pl.k)['ok']\n"
"  if t ~= 'none' and t ~= TYPENAME[pl.ttype] then\n"
"    return fail('WRONGTYPE', p, pl.k, t)\n"
"  end\n"
"  for c = 1, #pl.checks do\n"
"    local ch = pl.checks[c]\n"
"    local fn = CHECK[ch.op]\n"
"    if ch.op ~= 2 then\n"
"      if fn == nil then return fail('BADPROTO', p, pl.k, 'checkop ' .. ch.op) end\n"
"      local bad = fn(pl.k, ch.a)\n"
"      if bad then return fail(bad, p, pl.k, 'check ' .. c) end\n"
"    end\n"
"  end\n"
"end\n"
"\n"
"-- ==== PHASE 2: WRITES ====\n"
"-- Literal verbs, one per closure. See the W1 note in src/vfdw_script.h.\n"
"local ACTION = {\n"
"  [0] = function(k, a) return server.pcall('SET', k, a[1]) end,\n"
"  [1] = function(k, a) return server.pcall('DEL', k) end,\n"
"  [2] = function(k, a) return server.pcall('HSET', k, a[1], a[2]) end,\n"
"  [3] = function(k, a) return server.pcall('HDEL', k, a[1]) end,\n"
"  [4] = function(k, a) return server.pcall('SADD', k, a[1]) end,\n"
"  [5] = function(k, a) return server.pcall('SREM', k, a[1]) end,\n"
"  [6] = function(k, a) return server.pcall('ZADD', k, a[1], a[2]) end,\n"
"  [7] = function(k, a) return server.pcall('ZREM', k, a[1]) end,\n"
"  [8] = function(k, a) return server.pcall('RPUSH', k, a[1]) end,\n"
"  [9] = function(k, a) return server.pcall('LREM', k, 1, a[1]) end,\n"
"  [10] = function(k, a) return server.pcall('RENAME', KEYS[tonumber(a[1])], k) end,\n"
"  [11] = function(k, a) return server.pcall('SADD', KEYS[tonumber(a[1])], a[2]) end,\n"
"  [12] = function(k, a) return server.pcall('SREM', KEYS[tonumber(a[1])], a[2]) end,\n"
"  [13] = function(k, a) return server.pcall('HPEXPIRE', k, a[2], 'FIELDS', 1, a[1]) end,\n"
"  [14] = function(k, a) return server.pcall('HPERSIST', k, 'FIELDS', 1, a[1]) end,\n"
"}\n"
"\n"
"for p = 1, nplans do\n"
"  local pl = plans[p]\n"
"  for c = 1, #pl.actions do\n"
"    local ac = pl.actions[c]\n"
"    local fn = ACTION[ac.op]\n"
"    if fn == nil then\n"
"      return fail('INTERNAL', p, pl.k, 'actionop ' .. ac.op)\n"
"    end\n"
"    local r = fn(pl.k, ac.a)\n"
"    if type(r) == 'table' and r['err'] then\n"
"      return fail('INTERNAL', p, pl.k, c .. ' ' .. r['err'])\n"
"    end\n"
"  end\n"
"end\n"
"\n"
"return server.status_reply(V .. ' OK ' .. nplans)\n";

const char *
vfdw_script_text(void)
{
	return vfdw_script_src;
}

/*
 * The SHA1 the server will know the program by, computed once per backend.
 *
 * Computed rather than hard-coded: a literal would be one edit away from
 * naming a script that is not this one, and EVALSHA would then either miss
 * (a wasted round trip) or - far worse on a server where some other client
 * loaded a script with that digest - run something else entirely. The suite
 * still checks it against what SCRIPT LOAD returns, because agreeing with
 * ourselves proves nothing.
 */
const char *
vfdw_script_sha1(void)
{
	static char hex[SHA1_DIGEST_LENGTH * 2 + 1];
	static bool computed = false;
	uint8		digest[SHA1_DIGEST_LENGTH];
	pg_cryptohash_ctx *ctx;
	int			i;

	if (computed)
		return hex;

	ctx = pg_cryptohash_create(PG_SHA1);
	if (ctx == NULL)
		elog(ERROR, "valkey_fdw: could not create SHA1 context");

	/*
	 * sizeof - 1, not strlen: the trailing NUL of the literal is not part of
	 * the script and hashing it would produce a digest the server never
	 * agrees with. Off by exactly one byte is the kind of mistake that looks
	 * like a server problem.
	 */
	if (pg_cryptohash_init(ctx) < 0 ||
		pg_cryptohash_update(ctx, (const uint8 *) vfdw_script_src,
							 sizeof(vfdw_script_src) - 1) < 0 ||
		pg_cryptohash_final(ctx, digest, sizeof(digest)) < 0)
	{
		pg_cryptohash_free(ctx);
		elog(ERROR, "valkey_fdw: could not compute the script SHA1");
	}
	pg_cryptohash_free(ctx);

	for (i = 0; i < SHA1_DIGEST_LENGTH; i++)
		snprintf(hex + i * 2, 3, "%02x", digest[i]);

	computed = true;
	return hex;
}

/* ---------------------------------------------------------------------
 * Reply classification
 * --------------------------------------------------------------------- */

static const struct
{
	const char *code;
	VfdwScriptVerdict verdict;
}			vfdw_script_codes[] =
{
	{"OK", VFDW_SCRIPT_OK},
	{"CONFLICT", VFDW_SCRIPT_CONFLICT},
	{"MISSING", VFDW_SCRIPT_MISSING},
	{"WRONGTYPE", VFDW_SCRIPT_WRONGTYPE},
	{"BADPROTO", VFDW_SCRIPT_BADPROTO},
	{"INTERNAL", VFDW_SCRIPT_INTERNAL}
};

/*
 * True when word occupies [pos, pos+wordlen) of str and is bounded there.
 *
 * "Bounded" means the next byte is a space or the end of the reply. Without
 * it every code is a prefix of a longer one that means something else, and a
 * server message reading "VFDW1 CONFLICTX" would be reported to the user as a
 * unique violation. That is not hypothetical politeness: the sentinel space
 * is ours to extend, and the day CONFLICT gains a sibling the old build must
 * refuse to guess.
 */
static bool
vfdw_script_word_at(const char *str, size_t len, size_t pos,
					const char *word, size_t wordlen)
{
	if (pos + wordlen > len)
		return false;
	if (memcmp(str + pos, word, wordlen) != 0)
		return false;
	if (pos + wordlen == len)
		return true;
	return str[pos + wordlen] == ' ';
}

VfdwScriptVerdict
vfdw_script_classify(const char *str, size_t len, char **detail)
{
	static const char prefix[] = "VFDW1";
	const size_t plen = sizeof(prefix) - 1;
	size_t		pos;
	int			i;

	if (detail != NULL)
		*detail = NULL;

	if (str == NULL || !vfdw_script_word_at(str, len, 0, prefix, plen))
		return VFDW_SCRIPT_UNKNOWN;

	/* Exactly one space after the prefix; the script emits no other form. */
	pos = plen;
	if (pos >= len || str[pos] != ' ')
		return VFDW_SCRIPT_UNKNOWN;
	pos++;

	for (i = 0; i < (int) lengthof(vfdw_script_codes); i++)
	{
		size_t		clen = strlen(vfdw_script_codes[i].code);

		if (!vfdw_script_word_at(str, len, pos, vfdw_script_codes[i].code, clen))
			continue;

		/*
		 * The detail is copied as BYTES, with the length, and is not checked
		 * against the database encoding here. Classification is a byte
		 * operation - the caller may be a diagnostic that wants the reply
		 * exactly as it arrived, interior NUL and all - while I2 is about what
		 * reaches error-reporting code. The script's fail() concatenates
		 * KEYS[kx] into this text and a bytea key column carries arbitrary
		 * bytes, so it is user data whatever it looks like: whoever reports it
		 * puts it through vfdw_safe_text, which is what vfdw_flush_detail in
		 * src/vfdw_flush.c exists for.
		 */
		if (detail != NULL && pos + clen + 1 < len)
			*detail = pnstrdup(str + pos + clen + 1,
							   len - (pos + clen + 1));

		return vfdw_script_codes[i].verdict;
	}

	/*
	 * Our prefix with a code we do not know. Not UNKNOWN: an unrecognised
	 * VFDW1 code means this build and the loaded script disagree, which is
	 * exactly what BADPROTO names.
	 */
	if (detail != NULL && pos < len)
		*detail = pnstrdup(str + pos, len - pos);
	return VFDW_SCRIPT_BADPROTO;
}

const char *
vfdw_script_verdict_name(VfdwScriptVerdict v)
{
	switch (v)
	{
		case VFDW_SCRIPT_UNKNOWN:
			return "UNKNOWN";
		case VFDW_SCRIPT_OK:
			return "OK";
		case VFDW_SCRIPT_CONFLICT:
			return "CONFLICT";
		case VFDW_SCRIPT_MISSING:
			return "MISSING";
		case VFDW_SCRIPT_WRONGTYPE:
			return "WRONGTYPE";
		case VFDW_SCRIPT_BADPROTO:
			return "BADPROTO";
		case VFDW_SCRIPT_INTERNAL:
			return "INTERNAL";
	}
	return "?";
}
