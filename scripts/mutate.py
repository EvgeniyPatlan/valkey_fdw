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
    ("A7-redirect", "src/vfdw_scan_cluster.c",
     "\tif (state->redirect_passes <= 0)",
     "\tif (false && state->redirect_passes <= 0)",
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
]

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run_suite(pg, topology, suite):
    r = subprocess.run(
        ["./scripts/harness.sh", "test", "--pg", str(pg),
         "--topology", topology, "--suite", suite],
        cwd=HERE, capture_output=True, text=True)
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
        note = "caught" if good else "<-- ASSERTS NOTHING"
        print(f"{rid:5s} {verdict:18s} {topology}/{suite:8s} {note}")

    # Leave the tree built from unmutated source rather than from whichever
    # mutation ran last.
    subprocess.run(["./scripts/harness.sh", "build", "--pg", str(args.pg)],
                   cwd=HERE, capture_output=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
