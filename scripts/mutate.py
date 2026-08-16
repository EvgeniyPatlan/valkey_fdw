#!/usr/bin/env python3
"""
Re-run the mutation behind each closed defect.

WHY THIS EXISTS. A defect is closed here when a test names it, not when
someone believes it is fixed. That rule holds at the moment the defect is
closed and then decays: the code moves, the suite moves, and the assertion
that once caught the defect quietly stops catching it. Nothing noticed, because
a passing suite looks the same either way.

It is not hypothetical. Three closed defects were found stale in one session:

  The ledger's key states were written down as distinguished by nothing, so
  collapsing CREATED into LIVE was supposed to be invisible. It already failed
  three suites - the note predated the work that made it false and was never
  re-read. That mutation is G9 below.

  The overlay's per-key walk was written down as "unbounded in principle".
  write_max_ops bounds it, which one measurement showed.

  A reply-arity guard was written down as unreachable, for a reason that
  described a different function entirely.

And four assertions in this tree could not fail at all when checked. Two of
those were written in the same session that found the other two, by someone
who knew about the trap. Reading a test is not how you find this; running the
mutation is the only thing that ever has.

EACH ENTRY NAMES the defect it protects, the edit that reintroduces it, and
the target that should go red. A mutation that leaves everything green is
reported as a FAILURE of this script, because that is the whole signal: the
assertion behind that defect no longer asserts it.

Usage:  ./scripts/harness.sh mutate [--pg N] [--only ID]
"""

import argparse
import os
import shutil
import subprocess
import sys

# (id, file, find, replace, topology, suite)
#
# `find` must appear EXACTLY ONCE and is checked: a pattern that stopped
# matching after a refactor would otherwise report the row as passing while
# never applying the mutation, which is the same silent failure this script
# exists to catch, one level up.
MUTATIONS = [
    # A server that will not load the write program can be read from and not
    # written to, and the refusal belongs at the statement rather than at the
    # commit. Ungated, the write is accepted, buffered, and dies in the flush.
    ("A8-noscript", "src/vfdw_modify.c",
     "\tif (refusal != NULL)",
     "\tif (false && refusal != NULL)",
     "acl", "acl"),

    # The privilege model. Resolving a server by name is a catalogue lookup and
    # not an authorisation, so without the check any role holding a mapping
    # reaches a keyspace it was never granted a table on.
    ("A1-acl", "src/vfdw_test_common.c",
     "\tif (aclresult != ACLCHECK_OK)",
     "\tif (false && aclresult != ACLCHECK_OK)",
     "standalone", "priv"),

    # Only a superuser may switch the password rule off. Ungated, the role the
    # rule constrains can waive it on its own mapping.
    ("A3-pwreq", "src/vfdw_option.c",
     '\t\t"true", false, VFDW_OPTPRIV_SUPERUSER_TO_DISABLE,',
     '\t\t"true", false, VFDW_OPTPRIV_NONE,',
     "standalone", "priv"),

    # An error reply is not a missing key. Admitting the whole error class
    # answers a denied fetch with a short result set and no error at all.
    ("A4-skip", "src/vfdw_scan.c",
     '\t\tvfdw_reply_has_prefix(reply, "WRONGTYPE");',
     "\t\treply->type == VALKEY_REPLY_ERROR;",
     "fault", "fault"),

    # Key bytes reach an error message only after being checked against the
    # database encoding; a bytea key carries arbitrary bytes by design.
    ("A5-safetext", "src/vfdw_refuse.c",
     '\t\t\t errdetail("Key (%s) was already created or modified by this "\n'
     '\t\t\t\t\t   "transaction, so inserting it again would overwrite a "\n'
     '\t\t\t\t\t   "row this transaction has not committed yet.",\n'
     "\t\t\t\t\t   vfdw_safe_text(key, keylen)),",
     '\t\t\t errdetail("Key (%.*s) was already created or modified by this "\n'
     '\t\t\t\t\t   "transaction, so inserting it again would overwrite a "\n'
     '\t\t\t\t\t   "row this transaction has not committed yet.",\n'
     "\t\t\t\t\t   (int) keylen, key),",
     "standalone", "val"),

    # A reply's declared length is chosen by the far end, and the allocator is
    # the only place libvalkey offers to refuse one.
    #
    # The predicate and not one of the wrappers. A large reply is built in
    # stages and two of them refuse it independently, so removing the ceiling
    # from malloc alone - or from realloc alone - leaves io green and proves
    # nothing. Both were run. What every wrapper consults is what has to move.
    ("M1-ceiling", "src/vfdw_io.c",
     "\treturn size > vfdw_alloc_ceiling;",
     "\treturn false;",
     "standalone", "io"),

    # Owed a retry pass and out of budget is not the same answer as having
    # nothing left to chase; collapsing them drops the keys in silence.
    #
    # THE EDIT RESTORES THE COLLAPSE, and deliberately not by disabling the
    # guard. `if (false && ...)` removes the only exit from the chase, and the
    # fixture this runs against holds an ASK window open and never resolves it,
    # so the mutant does not fail the suite - it spins in the retry pass until
    # something outside kills it. A mutation that hangs proves nothing about
    # the assertion and stalls every mutation after it, which is what a run of
    # the whole set did here three times before this was understood.
    #
    # Returning false is the defect as it actually shipped: the scan reports
    # itself finished while keys it was owed are still unfetched, the keyed
    # lookup answers 0 rows, and cluster's recorded sqlstate changes.
    ("A7-redirect", "src/vfdw_scan_cluster.c",
     "\tif (state->redirect_passes <= 0)",
     "\tif (state->redirect_passes <= 0)\n\t\treturn false;\n\tif (false)",
     "cluster", "cluster"),

    # The RESP2 halves of the row decoder. HELLO 3 succeeds against every
    # image here, so the alternating member/score layout runs only behind the
    # proxy rule that answers HELLO as a pre-6.0 server would - which is to
    # say a user on an old server would have found this, not us. Taking the
    # score from the member's own index is exactly what a branch nobody has
    # ever executed gets wrong.
    ("G1", "src/vfdw_row.c",
     "\t\t*member = vfdw_reply_child(reply, (size_t) idx);\n"
     "\t\t*score = vfdw_reply_child(reply, (size_t) idx + 1);",
     "\t\t*member = vfdw_reply_child(reply, (size_t) idx);\n"
     "\t\t*score = vfdw_reply_child(reply, (size_t) idx);",
     "fault", "resp"),

    # A RESP3 boolean. A stock server converts Lua true to the integer 1 and
    # false to a nil under both protocols, so only the fabricated frame can
    # reach this arm at all; blanking it makes the probe report no number for
    # a reply that carries one.
    ("U1", "src/vfdw_testprobe.c",
     "\t\tcase VALKEY_REPLY_BOOL:\n\t\t\tout->has_num = true;",
     "\t\tcase VALKEY_REPLY_BOOL:\n\t\t\tout->has_num = false;",
     "fault", "resp"),

    # An error frame carrying no text. libvalkey ALLOCATES an empty string for
    # "-\r\n" rather than leaving str NULL, so testing only for NULL copies the
    # empty message faithfully and the user gets a bare DETAIL: with nothing
    # after it - the outcome this branch exists to prevent.
    ("U2", "src/vfdw_error.c",
     "reply->str == NULL || reply->len == 0",
     "reply->str == NULL",
     "fault", "resp"),

    # The target-column bitmap is built on FIRST USE, not at BeginForeignModify.
    # Its lifetime still has to be the write buffer's - the fold reads it at
    # pre-commit, long after the planning statement is gone - so building it
    # eagerly charges every statement against write_max_bytes even when it
    # buffers no rows. The mutation restores the eager build; the suite runs
    # 500 UPDATEs matching nothing and requires zero growth.
    ("G5", "src/vfdw_modify.c",
     "\tst->target_attrs = NULL;\n\tst->target_attnos = (List *) list_nth(fdw_private,\n"
     "\t\t\t\t\t\t\t\t\t\t  VFDW_MOD_PRIV_TARGET_ATTRS);",
     "\tst->target_attnos = (List *) list_nth(fdw_private,\n"
     "\t\t\t\t\t\t\t\t\t\t  VFDW_MOD_PRIV_TARGET_ATTRS);\n"
     "\tst->target_attrs = vfdw_modify_attr_bitmap(st->target_attnos);",
     "standalone", "wbuf"),

    # An INSERT targets every non-dropped column; an UPDATE targets only what
    # the statement assigned. Losing the updatedCols branch collapses the two,
    # and an UPDATE then rewrites fields it was never asked to touch. Plan-time
    # logic with nothing observable is why this needs a mutation rather than a
    # reading: returning NIL from it once left every suite green.
    ("G7", "src/vfdw_modify.c",
     "\tif (operation == CMD_UPDATE && rte->perminfoindex != 0)",
     "\tif (false && operation == CMD_UPDATE && rte->perminfoindex != 0)",
     "standalone", "wbuf"),

    # Phase 4 slice 1. Both are the mistakes a map-building parse actually
    # makes, and both produce a PLAUSIBLE map rather than an error - which is
    # why the suite has to reach the server for its verdict.
    ("P4-range", "src/vfdw_cluster.c",
     "\t\tfor (s = (int) lo->integer; s <= (int) hi->integer; s++)",
     "\t\tfor (s = (int) lo->integer; s < (int) hi->integer; s++)",
     "cluster", "cluster"),

    ("P4-role", "src/vfdw_cluster.c",
     "\t\t\trole->len != 6 || strncmp(role->str, \"master\", 6) != 0)",
     "\t\t\trole->len != 6 || strncmp(role->str, \"master\", 6) == 0)",
     "cluster", "cluster"),

    # Slice 2. The first is a pool that ignores which node it was asked for;
    # the second is the eviction bug this slice actually shipped and fixed,
    # where every node fell back to one slot and thrashed it.
    ("P4-node", "src/vfdw_conn_pool.c",
     "\tif (host == NULL)\n\t\treturn vconn->node_host[0] == '\\0';",
     "\tif (host == NULL || true)\n\t\treturn vconn->node_host[0] == '\\0';",
     "cluster", "cluster"),

    ("P4-pool", "src/vfdw_conn_pool.c",
     "\t\tif (vconn->conn == NULL)\n\t\t{\n\t\t\tif (virgin == NULL)\n"
     "\t\t\t\tvirgin = vconn;\n\t\t}\n\t\telse if (victim == NULL)\n"
     "\t\t\tvictim = vconn;",
     "\t\tif (victim == NULL)\n\t\t\tvictim = vconn;",
     "cluster", "cluster"),

    # Slice 3, invariant I5 in its cluster form. The first stops the scan at
    # the first exhausted cursor - roughly a third of the keyspace, returned
    # with no error. The second breaks the keyed lookup's node partitioning.
    ("P4-i5", "src/vfdw_scan_cluster.c",
     "\tif (!state->fanout || state->cur_node + 1 >= state->nnodes)\n\t\treturn false;",
     "\tif (true || !state->fanout || state->cur_node + 1 >= state->nnodes)\n\t\treturn false;",
     "cluster", "cluster"),

    ("P4-keys", "src/vfdw_scan_cluster.c",
     "\t\tif (owner != NULL &&\n\t\t\t(owner->port != node->port ||\n"
     "\t\t\t strcmp(owner->host, node->host) != 0))\n\t\t\tcontinue;",
     "\t\tif (owner != NULL &&\n\t\t\t(owner->port == node->port &&\n"
     "\t\t\t strcmp(owner->host, node->host) == 0))\n\t\t\tcontinue;",
     "cluster", "cluster"),

    # Slice 4. Folds a redirect back into "an error means the key vanished",
    # which loses the row silently.
    ("P4-moved", "src/vfdw_scan.c",
     "\tif (vfdw_cluster_is_redirect(reply))\n\t\tvfdw_scan_redirected(state, reply, key, keylen);",
     "\tif (false && vfdw_cluster_is_redirect(reply))\n\t\tvfdw_scan_redirected(state, reply, key, keylen);",
     "cluster", "cluster"),

    # Slice 5. Removes the cross-slot refusal, so a transaction spanning two
    # slots is sent as one program and the server answers CROSSSLOT - after
    # the writes before it in the program have already applied.
    ("P4-xslot", "src/vfdw_flush_cluster.c",
     "\tif (this_slot == *slot)\n\t\treturn;",
     "\tif (this_slot == *slot || true)\n\t\treturn;",
     "cluster", "cluster"),

    # CREATED against LIVE. The state decides the precondition the fold emits -
    # KEY_ABSENT for a key this transaction made, KEY_PRESENT for one it only
    # touched - and that precondition is what makes a concurrent writer lose at
    # its own COMMIT rather than silently win. Collapsing the two fails wbuf,
    # script and dml.
    ("G9", "src/vfdw_ledger.c",
     "\t\tplan->state = VFDW_KEY_CREATED;",
     "\t\tplan->state = VFDW_KEY_LIVE;",
     "standalone", "wbuf"),

    # One row per key against one row per member. The table TYPE cannot decide
    # this: a packed table over a list is the same type as a column-mapped one.
    # Taking the answer from the type gives a packed list one row per MEMBER,
    # each carrying a copy of the whole array, so the row count silently
    # becomes the member count - a result that looks like data rather than
    # like a failure. legacy asserts the row counts and the arrays beside them.
    ("PK1", "src/vfdw_row.c",
     "\tif (map->legacy_value)\n\t\treturn false;",
     "\tif (false && map->legacy_value)\n\t\treturn false;",
     "standalone", "legacy"),

    # An expiry the server does not have is not a duration. -1 is a field with
    # no expiry and -2 is a field that is not there; admitting either builds an
    # interval out of a sentinel, and ttl asserts that a key whose fields never
    # expired reports NULL for both of them.
    ("T1-noexpiry", "src/vfdw_ttl.c",
     "\tif (ms == VFDW_TTL_NO_EXPIRY || ms == VFDW_TTL_ABSENT || ms < 0)",
     "\tif (false)",
     "standalone", "ttl"),

    # Each ttl column reads ITS OWN field's answer. The reply is positional, so
    # taking every column's expiry from the first slot is a mutation that keeps
    # the row shape, keeps the nullness of the common case, and silently gives
    # two fields one duration - which is why ttl expires two fields to plainly
    # different durations and compares them rather than reading one.
    ("T2-slot", "src/vfdw_row.c",
     "vfdw_ttl_datum(ctx->ttl_ms[col->ttl_slot], &value)",
     "vfdw_ttl_datum(ctx->ttl_ms[0], &value)",
     "standalone", "ttl"),

    # The capability verdict itself. Reporting every server as lacking the
    # command refuses a server that has it, which is the direction a Valkey 9
    # run can prove; the other direction is what ttl_absent asserts on 8.
    ("T3-cap", "src/vfdw_ttl.c",
     "\tvconn->field_ttl = present ? VFDW_CAP_PRESENT : VFDW_CAP_ABSENT;",
     "\tvconn->field_ttl = (present && false) ? VFDW_CAP_PRESENT : VFDW_CAP_ABSENT;",
     "standalone", "ttl"),

    # One key, two replies, and the consumer has to take both. Taking only the
    # value leaves each key's expiry to be read as the NEXT key's value, which
    # is the misalignment the queue and take comments both exist to prevent.
    ("T4-pair", "src/vfdw_scan_cmd.c",
     "\tif (state->map->nttl > 0)\n"
     "\t\tvfdw_ttl_take(state->map, vfdw_batch_next(state->batch),",
     "\tif (false && state->map->nttl > 0)\n"
     "\t\tvfdw_ttl_take(state->map, vfdw_batch_next(state->batch),",
     "standalone", "ttl"),

    # NULL on a ttl column is PERSIST, not "expire now". Sending HPEXPIRE with
    # the field's own name and no duration would be a syntax error and fail
    # loudly; the mutation that does NOT fail loudly is treating the absence as
    # a zero-length expiry, which deletes the field. ttl asserts the field is
    # still there after the expiry is cleared, which is what tells the two
    # apart.
    ("T5-persist", "src/vfdw_ledger_fold.c",
     "\t\tif (t->isnull)",
     "\t\tif (false && t->isnull)",
     "standalone", "ttl"),

    # An expiry is applied AFTER the field it belongs to exists. HPEXPIRE on a
    # missing field answers -2 and sets nothing, so folding the expiries before
    # the field loop loses exactly the INSERT that creates a field and gives it
    # a lifetime in one row - and loses it silently, since -2 is not an error.
    ("T6-order", "src/vfdw_ledger_fold.c",
     "\tvfdw_ledger_fold_fields(plan, op, rel);\n"
     "\tvfdw_ledger_fold_ttls(plan, op);",
     "\tvfdw_ledger_fold_ttls(plan, op);\n"
     "\tvfdw_ledger_fold_fields(plan, op, rel);",
     "standalone", "ttl"),

    # A duration already past deletes the field rather than setting a property
    # of it, so it is refused. Admitting it turns an UPDATE into a delete.
    ("T7-nonpositive", "src/vfdw_val.c",
     "\tif (ms <= 0)",
     "\tif (false)",
     "standalone", "ttl"),

    # A list member's position is its index, and each key starts again at
    # zero. Numbering from a counter that survives the key it belongs to gives
    # the second list positions 3 and 4 - which still passes an ORDER BY pos
    # assertion made about one key, so position asserts the minimum per key.
    ("L1-pos", "src/vfdw_row.c",
     "\t\t? Int64GetDatum((int64) ctx->cur_elem)\n"
     "\t\t: Int32GetDatum(ctx->cur_elem);",
     "\t\t? Int64GetDatum((int64) ctx->cur_elem + 1)\n"
     "\t\t: Int32GetDatum(ctx->cur_elem + 1);",
     "standalone", "position"),

    # A position column contributes nothing to the wire, so nothing on the
    # write path would read it unless something says to. Without that read the
    # refusal is unreachable: a row naming a position is accepted, the position
    # discarded, and the member appended somewhere else with nothing said.
    ("L2-poswrite", "src/vfdw_render.c",
     "\tvfdw_modify_check_position(st, slot);",
     "\tif (false)\n\t\tvfdw_modify_check_position(st, slot);",
     "standalone", "position"),

    # The row-identity Var must name the range table entry it is filed under.
    #
    # UNDER CASSERT, because what catches this is core's own assertion in
    # add_row_identity_var, which an ordinary build compiles out. CI runs the
    # whole suite that way for this class of defect.
    #
    # The mutation is hardcoding 1, which is what a plain UPDATE or DELETE
    # would never notice: the result relation IS the first entry there, so the
    # wrong answer and the right one are the same number. dml reaches it
    # through an inherited child, where the parent takes entry one.
    ("R1-rtindex", "src/vfdw_rowid.c",
     "\tvar = makeVar(rtindex, attno, attr->atttypid, attr->atttypmod,",
     "\tvar = makeVar(1, attno, attr->atttypid, attr->atttypmod,",
     "standalone+cassert", "dml"),

    # A packed zset takes members, not scores. Restoring WITHSCORES makes the
    # array's shape follow the negotiated protocol rather than the data: RESP2
    # returns member and score alternating, RESP3 a nested pair per member
    # whose children carry no bytes of their own. The zset round trip in
    # legacy asserts the members alone, so both spellings go red.
    ("PK2", "src/vfdw_scan_cmd.c",
     "\t\t\tif (!state->map->legacy_value)\n"
     "\t\t\t\tvfdw_cmd_add_cstr(cmd, \"WITHSCORES\");",
     "\t\t\tvfdw_cmd_add_cstr(cmd, \"WITHSCORES\");",
     "standalone", "legacy"),
]

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# Long enough for a cold topology - a cluster formation plus a build is minutes
# - and short enough that a mutant which does not terminate is reported in one
# sitting rather than stalling the run behind it.
SUITE_TIMEOUT = 900


def run_suite(pg, topology, suite):
    # A topology may carry "+cassert", which runs the suite against a
    # PostgreSQL built with --enable-cassert. Some defects are caught by core's
    # own assertions rather than by anything this tree asserts - a row-identity
    # Var whose varno disagrees with the range table index it is filed under is
    # one - and those assertions are compiled out of an ordinary build. CI runs
    # a cassert job for the same reason.
    cassert = topology.endswith("+cassert")
    if cassert:
        topology = topology[: -len("+cassert")]

    # A mutation can remove a loop's only exit, and then the suite neither
    # passes nor fails: it runs until something kills it. Unbounded, that is
    # indistinguishable from slow, and every mutation queued after it is never
    # reached - so the set reports nothing at all rather than reporting a
    # problem. Bounded, it becomes a verdict of its own.
    try:
        cmd = ["./scripts/harness.sh", "test", "--pg", str(pg),
               "--topology", topology, "--suite", suite]
        if cassert:
            cmd.append("--cassert")
        r = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True,
                           timeout=SUITE_TIMEOUT)
    except subprocess.TimeoutExpired:
        return "TIMED-OUT"
    out = r.stdout + r.stderr
    # A build failure is not a caught mutation. An earlier runner in this
    # project read "did not exit 0" as "the test caught it", which counted
    # broken builds as proof and made every mutation look successful.
    if "Error 1" in out and "regression" not in out:
        return "BUILD-FAILED"
    return "RED" if r.returncode != 0 else "GREEN"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pg", default="17")
    ap.add_argument("--only")
    args = ap.parse_args()

    todo = [m for m in MUTATIONS if not args.only or m[0] == args.only]
    if not todo:
        print(f"no mutation with id {args.only}")
        return 1

    ok = True
    for rid, path, find, repl, topology, suite in todo:
        full = os.path.join(HERE, path)
        src = open(full).read()

        if src.count(find) != 1:
            print(f"{rid:5s} {'PATTERN-NOT-FOUND':18s} "
                  f"<-- {path} no longer contains it exactly once")
            ok = False
            continue

        shutil.copy(full, full + ".mutbak")
        try:
            open(full, "w").write(src.replace(find, repl, 1))
            verdict = run_suite(args.pg, topology, suite)
        finally:
            shutil.move(full + ".mutbak", full)
            # The restored file must be NEWER than the object built from the
            # mutated copy, or make skips the rebuild and the mutated library
            # stays installed - which then reads as a real defect on clean
            # source. This has bitten here.
            os.utime(full, None)

        good = verdict == "RED"
        ok = ok and good
        # A timeout is its own complaint. Reporting it as "asserts nothing"
        # would send a reader to look at the suite, when what needs changing is
        # the mutation: it did not terminate, so the suite never got to answer.
        if good:
            note = "caught"
        elif verdict == "TIMED-OUT":
            note = f"<-- DID NOT TERMINATE in {SUITE_TIMEOUT}s; mutation, not suite"
        else:
            note = "<-- ASSERTS NOTHING"
        print(f"{rid:5s} {verdict:18s} {topology}/{suite:8s} {note}")

    # Leave the tree built from unmutated source rather than from whichever
    # mutation ran last.
    subprocess.run(["./scripts/harness.sh", "build", "--pg", str(args.pg)],
                   cwd=HERE, capture_output=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
