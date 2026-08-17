/*-------------------------------------------------------------------------
 *
 * vfdw_scan_cmd.c
 *		What a scan asks about one key.
 *
 * SEPARATE FROM vfdw_scan.c because these two functions answer a question the
 * rest of the scan does not ask: given a key, which commands go on the wire
 * and in what order. The scan's own subject is the state machine that turns
 * replies into tuples - when a page is exhausted, what "no tuple this call"
 * means against "the scan is over" (I5), how a redirect re-enters the loop.
 *
 * The separation earns its keep at the moment a key stops costing exactly one
 * reply. A ttl column adds a second command per key, and the ORDER of the two
 * is load-bearing rather than incidental: a batch reply is valid only until
 * the next is taken, so the reply that must outlive the other has to be taken
 * second. That rule is stated once, where the commands are queued, and the
 * consumer matches it - rather than being split across a file that is mostly
 * about something else.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_scan_cmd.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_scan.h"

#include "vfdw_cmd.h"
#include "vfdw_map.h"
#include "vfdw_scan_internal.h"
#include "vfdw_ttl.h"

/*
 * The command that reads one key's value, by table type.
 */
/*
 * The command that reads one hash key: HMGET of the named fields, or HGETALL.
 *
 * See VfdwTableMap.hmget for why the choice is made once at map build rather
 * than worked out here - the row decoder has to reach the same answer, and the
 * way two derivations stop agreeing is silently.
 */
/*
 * Does this scan have to ask whether a key belongs to the table's keyset?
 *
 * Only when the plan NAMED the keys. A keyspace scan over a keyset produced
 * them from the set itself, so the answer is already yes; a named key came
 * from a qual and nothing has checked it.
 */
static bool
vfdw_scan_asks_membership(const VfdwScanState *state)
{
	return state->map->keyset != NULL && state->strategy == VFDW_SCAN_KEYS;
}

static void
vfdw_scan_hash_command(VfdwScanState *state, VfdwCmd *cmd,
					   const char *key, size_t keylen)
{
	int			i;

	if (!state->map->hmget)
	{
		vfdw_cmd_add_cstr(cmd, "HGETALL");
		vfdw_cmd_add_bytes(cmd, key, keylen);
		return;
	}

	vfdw_cmd_add_cstr(cmd, "HMGET");
	vfdw_cmd_add_bytes(cmd, key, keylen);

	for (i = 0; i < state->map->natts; i++)
	{
		const VfdwColumn *c = &state->map->cols[i];

		if (c->kind != VFDW_COL_FIELD && c->kind != VFDW_COL_TTL)
			continue;

		/*
		 * Map order, which is the order vfdw_map_check_fetch assigned the
		 * slots in. The reply is positional, so a different order here would
		 * give every column after the difference another column's value.
		 */
		vfdw_cmd_add_bytes(cmd, c->field, strlen(c->field));
	}
}

static void
vfdw_scan_value_command(VfdwScanState *state, VfdwCmd *cmd,
						const char *key, size_t keylen)
{
	vfdw_cmd_reset(cmd);

	switch (state->map->tabletype)
	{
		case VFDW_TABLE_HASH:
			vfdw_scan_hash_command(state, cmd, key, keylen);
			break;

		case VFDW_TABLE_LIST:
			vfdw_cmd_add_cstr(cmd, "LRANGE");
			vfdw_cmd_add_bytes(cmd, key, keylen);
			vfdw_cmd_add_cstr(cmd, "0");
			vfdw_cmd_add_cstr(cmd, "-1");
			break;
		case VFDW_TABLE_SET:
			vfdw_cmd_add_cstr(cmd, "SMEMBERS");
			vfdw_cmd_add_bytes(cmd, key, keylen);
			break;
		case VFDW_TABLE_ZSET:
			vfdw_cmd_add_cstr(cmd, "ZRANGE");
			vfdw_cmd_add_bytes(cmd, key, keylen);
			vfdw_cmd_add_cstr(cmd, "0");
			vfdw_cmd_add_cstr(cmd, "-1");

			/*
			 * A packed table takes the members and not the scores, so it does
			 * not ask for them. WITHSCORES is answered by RESP2 with member
			 * and score alternating and by RESP3 with a nested pair per member
			 * (see vfdw_scan_member_at), so packing that reply would give the
			 * same table a differently-shaped array depending on which
			 * protocol the connection negotiated. A score is read by mapping a
			 * score column instead.
			 */
			if (!state->map->legacy_value)
				vfdw_cmd_add_cstr(cmd, "WITHSCORES");
			break;
		case VFDW_TABLE_STRING:
		default:
			vfdw_cmd_add_cstr(cmd, "GET");
			vfdw_cmd_add_bytes(cmd, key, keylen);
			break;
	}
}

/*
 * Queue value fetches for every key of the current page.
 */
void
vfdw_scan_queue_page(VfdwScanState *state)
{
	VfdwCmd		cmd;
	int			i;

	vfdw_cmd_init(&cmd, state->page_cxt, 4);
	for (i = 0; i < state->nkeys; i++)
	{
		/*
		 * EXPIRY FIRST, VALUE SECOND, and the order is not a preference.
		 *
		 * A batch reply is valid only until the next one is taken, so whichever
		 * of the two is taken first must be finished with before the other
		 * arrives. The expiry reply is a handful of integers that are copied
		 * out in one call; the value reply is the collection the rest of the
		 * tuple is built from, field by field, while the tuple is assembled.
		 * Only one of those can be the one that has to survive.
		 */
		/*
		 * FIRST, because it decides whether the rest of this key matters. A
		 * named key that is not in the set is not a row of this table, however
		 * well the value fetch behind it answers.
		 */
		if (vfdw_scan_asks_membership(state))
		{
			vfdw_cmd_reset(&cmd);
			vfdw_cmd_add_cstr(&cmd, "SISMEMBER");
			vfdw_cmd_add_bytes(&cmd, state->map->keyset,
							   strlen(state->map->keyset));
			vfdw_cmd_add_bytes(&cmd, state->keys[i], state->keylens[i]);
			vfdw_batch_add(state->batch, &cmd);
		}

		if (state->map->hmget)
		{
			vfdw_cmd_reset(&cmd);
			vfdw_cmd_add_cstr(&cmd, "HLEN");
			vfdw_cmd_add_bytes(&cmd, state->keys[i], state->keylens[i]);
			vfdw_batch_add(state->batch, &cmd);
		}

		if (state->map->nttl > 0)
		{
			vfdw_ttl_command(&cmd, state->map, state->keys[i], state->keylens[i]);
			vfdw_batch_add(state->batch, &cmd);
		}

		vfdw_scan_value_command(state, &cmd, state->keys[i], state->keylens[i]);
		vfdw_batch_add(state->batch, &cmd);
	}
}

valkeyReply *
vfdw_scan_take_replies(VfdwScanState *state)
{
	/*
	 * EXPIRIES FIRST, matching vfdw_scan_queue_page, and taken here rather
	 * than at the point of use so that no branch of the scan loop can skip a
	 * key having consumed only one of its two replies. That misalignment does
	 * not fail: it reads each key's expiry as the next key's value.
	 *
	 * A batch reply is valid only until the next is taken, which is why these
	 * integers are copied out now and the value reply is the one returned.
	 */
	state->cur_member = -1;
	if (vfdw_scan_asks_membership(state))
	{
		const valkeyReply *r = vfdw_batch_next(state->batch);

		/*
		 * Only an integer answers. An error - a keyset holding something other
		 * than a set, say - leaves the verdict unasked, and the value fetch
		 * queued behind reports it with the key in hand.
		 */
		if (r != NULL && r->type == VALKEY_REPLY_INTEGER)
			state->cur_member = (r->integer != 0) ? 1 : 0;
	}

	state->cur_hlen = -1;
	if (state->map->hmget)
	{
		const valkeyReply *r = vfdw_batch_next(state->batch);

		/*
		 * Only an integer is an answer. A WRONGTYPE against a key holding
		 * something other than a hash, or a server that refused the read,
		 * arrives as an error here and is left to the value reply queued
		 * behind it - which reports it with the key in hand, the way it did
		 * before this command existed.
		 */
		if (r != NULL && r->type == VALKEY_REPLY_INTEGER)
			state->cur_hlen = r->integer;
	}

	if (state->map->nttl > 0)
		vfdw_ttl_take(state->map, vfdw_batch_next(state->batch),
					  state->rowctx.ttl_ms);

	return vfdw_batch_next(state->batch);
}
