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
static void
vfdw_scan_value_command(VfdwScanState *state, VfdwCmd *cmd,
						const char *key, size_t keylen)
{
	vfdw_cmd_reset(cmd);

	switch (state->map->tabletype)
	{
		case VFDW_TABLE_HASH:
			vfdw_cmd_add_cstr(cmd, "HGETALL");
			vfdw_cmd_add_bytes(cmd, key, keylen);
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
	if (state->map->nttl > 0)
		vfdw_ttl_take(state->map, vfdw_batch_next(state->batch),
					  state->rowctx.ttl_ms);

	return vfdw_batch_next(state->batch);
}
