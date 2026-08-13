/*
 * Shared between the two test entry points that read Valkey replies.
 *
 * They were one file until it outgrew the size limit, and the split is along
 * the seam that matters rather than an arbitrary line: valkey_fdw_test_probe
 * RETURNS an error reply as data, and valkey_fdw_test_keys RAISES on one.
 * Two sibling entry points with opposite error contracts is how a suite
 * author writes a wrong assertion and believes it, so each file now states
 * its own contract at the top and this header carries only what both need.
 */
#ifndef VFDW_TESTPROBE_INTERNAL_H
#define VFDW_TESTPROBE_INTERNAL_H

#include "postgres.h"

#include "vfdw_cmd.h"
#include "vfdw_conn.h"

#include <valkey/valkey.h>

/* Columns of valkey_fdw_test_keys's result row. */
#define VFDW_KEYS_NCOLS		2

/* COUNT hint for the key dump's SCAN. */
#define VFDW_PROBE_SCAN_COUNT	1000

/*
 * Keys collected by a dump, before their types are looked up.
 */
typedef struct VfdwProbeKeys
{
	bytea	  **keys;
	int			nkeys;
	int			capacity;
} VfdwProbeKeys;

extern bytea *vfdw_probe_bytea(const char *data, size_t len);

/*
 * The children of an aggregate, guarded.
 *
 * A key dump walks SCAN's cursor-and-list reply with the same index
 * arithmetic the scan uses on a hash, and it is no more entitled to trust the
 * element count for having been asked for by a test. So it goes through the
 * decoder's own guard rather than a second one written here: two arity checks
 * are two things that can be wrong, and a heap overflow is what a wrong one
 * costs.
 */
static inline const valkeyReply *
vfdw_probe_child(const valkeyReply *reply, size_t i)
{
	return vfdw_reply_child(reply, i);
}

/*
 * Both entry points take their server from here and nowhere else, which is why
 * neither of them checks a privilege of its own: this is where the name is
 * resolved, so this is where vfdw_test_server demands USAGE on what it
 * resolved to.
 */
extern VfdwConn *vfdw_probe_connect(const char *servername, int database);
extern bool vfdw_probe_is_aggregate(int type);

#endif							/* VFDW_TESTPROBE_INTERNAL_H */
