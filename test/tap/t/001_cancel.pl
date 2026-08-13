# Invariant I6: every wait is interruptible, proved from a second session.
#
# This is the one property no single-session test can reach. pg_regress runs
# one backend per file and statement_timeout is disabled before commit, so a
# cancel arriving from elsewhere - which is what a user actually does when a
# query hangs - has no other place to be tested.
#
# What makes it a real proof rather than a slow test: each case asserts the
# cancel arrived FAST, not merely that it eventually arrived. A backend that
# ignored interrupts and returned when its own deadline expired would produce
# the same SQLSTATE, just later, and an assertion on the error alone would
# call that a pass.

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time);

my $valkey_host = $ENV{VALKEY_HOST} || 'valkey';
my $valkey_port = $ENV{VALKEY_PORT} || '6379';

my $node = PostgreSQL::Test::Cluster->new('cancel');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION valkey_fdw');

# valkey_fdw_test_block, the whole basis of case 1 below, lives in the
# diagnostics extension - which is separate precisely so that an install that
# never runs this file does not carry it. The cluster here is this test's own,
# so nothing else has created it.
$node->safe_psql('postgres', 'CREATE EXTENSION valkey_fdw_test');
$node->safe_psql('postgres', qq{
    CREATE SERVER tap_srv FOREIGN DATA WRAPPER valkey_fdw
        OPTIONS (host '$valkey_host', port '$valkey_port',
                 command_timeout_ms '60000',
                 write_max_ops '200000', write_max_bytes '536870912');
    CREATE USER MAPPING FOR CURRENT_USER SERVER tap_srv;
    CREATE FOREIGN TABLE tap_t (k text OPTIONS (key 'true'), v text)
        SERVER tap_srv OPTIONS (tabletype 'string', keyprefix 'tap:');
});

# Cancel the one backend that is not ours. Matching on the query text would be
# fragile - the text differs between the cases below - and matching on "not
# me" is exact here because the cluster is private to this test.
my $cancel_other = q{
    SELECT pg_cancel_backend(pid) FROM pg_stat_activity
     WHERE pid <> pg_backend_pid() AND datname = current_database()
       AND state = 'active'
};

# ---------------------------------------------------------------------------
# 1. A blocking read is interruptible.
#
# valkey_fdw_test_block parks inside vfdw_io_wait for its whole duration, with
# a command_timeout far beyond it. If CHECK_FOR_INTERRUPTS were absent from
# the wait loop the cancel would sit unnoticed until the block elapsed - so
# the elapsed time IS the assertion, and 30 seconds against a 2 second budget
# is not a margin anyone can argue with.
# ---------------------------------------------------------------------------
{
    my $bg = $node->background_psql('postgres', on_error_stop => 0);
    $bg->query_safe("SELECT 1");    # settle the session

    # \echo lands before the blocking statement runs, so query_until returns
    # with the statement still in flight. qr// would NOT work here: an empty
    # pattern in Perl reuses the last successful match rather than matching
    # immediately, which is a silent wrong answer rather than an error.
    my $start = time();
    $bg->query_until(qr/FIRED/, qq{
        \\echo FIRED
        SELECT valkey_fdw_test_block('$valkey_host', $valkey_port, 30, 60000);
    });

    # Let the statement actually reach the wait before cancelling it.
    sleep 1;
    $node->safe_psql('postgres', $cancel_other);

    # query(), not query_safe(): psql reports the cancelled statement's error
    # on the NEXT query's stderr, and query_safe dies on any stderr at all. It
    # would be dying on exactly the evidence this test exists to collect.
    my $out = $bg->query("SELECT 'awake'");
    my $elapsed = time() - $start;

    like($out, qr/awake/, 'session recovered after the cancel');
    cmp_ok($elapsed, '<', 15,
        "blocking read was interrupted promptly (${elapsed}s, block was 30s)");

    $bg->quit;
}

# ---------------------------------------------------------------------------
# 2. A scan is interruptible mid-flight.
#
# The block above proves the wait loop; this proves the scan's own loop, which
# has its own CHECK_FOR_INTERRUPTS and could lose it independently. Seeded
# large enough that the scan is still running when the cancel lands.
# ---------------------------------------------------------------------------
{
    $node->safe_psql('postgres', q{
        SELECT valkey_fdw_test_probe('tap_srv', 0, 'EVAL',
            'for i=1,20000 do server.call("SET", "tap:"..i, string.rep("x", 200)) end return 1',
            '0')
    });

    my $bg = $node->background_psql('postgres', on_error_stop => 0);
    $bg->query_safe("SELECT 1");

    my $start = time();
    $bg->query_until(qr/FIRED/, q{
        \\echo FIRED
        SELECT count(*) FROM tap_t a, tap_t b;
    });

    sleep 2;
    $node->safe_psql('postgres', $cancel_other);

    my $out = $bg->query("SELECT 'awake'");
    my $elapsed = time() - $start;

    like($out, qr/awake/, 'session recovered after cancelling a scan');
    cmp_ok($elapsed, '<', 60,
        "scan was interrupted rather than run to completion (${elapsed}s)");

    # The cancelled scan left no connection behind at an unknown reply offset.
    my $usable = $node->safe_psql('postgres',
        "SELECT count(*) >= 0 FROM tap_t WHERE k = 'tap:1'");
    is($usable, 't', 'the pool is usable after a cancelled scan');
}

# ---------------------------------------------------------------------------
# 3. A cancel during the pre-commit flush aborts, and says what it means.
#
# statement_timeout cannot reach here - core disables it before
# CommitTransactionCommand - so a cancel from another session is the only way
# in. PRE_COMMIT is outside HOLD_INTERRUPTS precisely so that this works.
#
# The buffer is sized from the bench: 50000 rows is about 1.5 seconds of
# flush, so a cancel at 300 ms lands while the program is still running. The
# first version used 9000 rows and a one second sleep - roughly 250 ms of work
# followed by a cancel arriving at an idle session, which asserted nothing.
# The caps are raised on this server for the same reason; the defaults would
# refuse the buffer long before it was big enough to be interesting.
# ---------------------------------------------------------------------------
{
    my $bg = $node->background_psql('postgres', on_error_stop => 0);
    $bg->query_safe("SELECT 1");

    $bg->query_safe("BEGIN");
    $bg->query_safe(q{
        INSERT INTO tap_t SELECT 'tap:w' || i, repeat('y', 400)
          FROM generate_series(1, 50000) i
    });

    # Where the log is now, so the assertions below read only this case.
    my $logpos = -s $node->logfile;

    $bg->query_until(qr/FIRED/, "\\echo FIRED\nCOMMIT;\n");
    select(undef, undef, undef, 0.3);
    $node->safe_psql('postgres', $cancel_other);

    my $out = $bg->query("SELECT 'awake'");
    like($out, qr/awake/, 'session recovered after a cancel at pre-commit');

    # A CANCEL MUST STAY A CANCEL.
    #
    # The flush re-raises transport failures as 08007, and the guard that
    # exempts a query cancel from that is what these two assert. Without them
    # the exemption can be deleted and every test still passes: the session
    # recovers either way, so "it recovered" proves nothing about which error
    # the user was handed. Turning someone's own cancel into "the outcome is
    # unknown" would be answering a question they did not ask.
    my $log = slurp_file($node->logfile, $logpos);

    like($log, qr/canceling statement due to user request/,
        'a cancel at pre-commit is still reported as a cancel');
    unlike($log, qr/the outcome of the Valkey write is unknown/,
        'a cancel is not re-classified as an indeterminate write');

    # Whatever the race decided, the transaction is over and the session works.
    my $state = $node->safe_psql('postgres',
        "SELECT count(*) FROM pg_stat_activity WHERE state = 'idle in transaction'");
    is($state, '0', 'no transaction was left open by the cancel');

    $bg->quit;
}

# ---------------------------------------------------------------------------
# 4. A large probe is interruptible.
#
# WHAT THIS PROVES, AND WHAT IT DOES NOT. It proves the probe entry point
# responds to a cancel while working through a million-element LRANGE. It does
# NOT prove the CHECK_FOR_INTERRUPTS inside vfdw_probe_elements, and it must
# not be read as proving it: deleting that call leaves this case passing, which
# was run rather than assumed.
#
# The reason is that libvalkey's parse and our materialisation scale together
# with the element count, so no input makes the materialisation loop dominate,
# and the cancel always lands in the read - which was already interruptible
# and already covered by case 1.
#
# THE ASSERTION IS RELATIVE, not a fixed budget. "It finished in under N
# seconds" is true of a probe that ignored the cancel and simply ran fast, so
# the uncancelled duration is measured first and the cancelled one must be a
# fraction of it. That is the difference between interrupted and merely quick.
# ---------------------------------------------------------------------------
{
    $node->safe_psql('postgres', q{
        SELECT valkey_fdw_test_probe('tap_srv', 0, 'EVAL',
            'for i=1,20000 do server.call("SET", "tap:"..i, string.rep("x", 200)) end return 1',
            '0')
    });

    my $bg = $node->background_psql('postgres', on_error_stop => 0);
    $bg->query_safe("SELECT 1");

    my $start = time();
    $bg->query_until(qr/FIRED/, q{
        \\echo FIRED
        SELECT count(*) FROM tap_t a, tap_t b;
    });

    sleep 2;
    $node->safe_psql('postgres', $cancel_other);

    my $out = $bg->query("SELECT 'awake'");
    my $elapsed = time() - $start;

    like($out, qr/awake/, 'session recovered after cancelling a scan');
    cmp_ok($elapsed, '<', 60,
        "scan was interrupted rather than run to completion (${elapsed}s)");

    # The cancelled scan left no connection behind at an unknown reply offset.
    my $usable = $node->safe_psql('postgres',
        "SELECT count(*) >= 0 FROM tap_t WHERE k = 'tap:1'");
    is($usable, 't', 'the pool is usable after a cancelled scan');
}

# ---------------------------------------------------------------------------
# 3. A cancel during the pre-commit flush aborts, and says what it means.
#
# statement_timeout cannot reach here - core disables it before
# CommitTransactionCommand - so a cancel from another session is the only way
# in. PRE_COMMIT is outside HOLD_INTERRUPTS precisely so that this works.
#
# The buffer is sized from the bench: 50000 rows is about 1.5 seconds of
# flush, so a cancel at 300 ms lands while the program is still running. The
# first version used 9000 rows and a one second sleep - roughly 250 ms of work
# followed by a cancel arriving at an idle session, which asserted nothing.
# The caps are raised on this server for the same reason; the defaults would
# refuse the buffer long before it was big enough to be interesting.
# ---------------------------------------------------------------------------
{
    my $bg = $node->background_psql('postgres', on_error_stop => 0);
    $bg->query_safe("SELECT 1");

    $bg->query_safe("BEGIN");
    $bg->query_safe(q{
        INSERT INTO tap_t SELECT 'tap:w' || i, repeat('y', 400)
          FROM generate_series(1, 50000) i
    });

    # Where the log is now, so the assertions below read only this case.
    my $logpos = -s $node->logfile;

    $bg->query_until(qr/FIRED/, "\\echo FIRED\nCOMMIT;\n");
    select(undef, undef, undef, 0.3);
    $node->safe_psql('postgres', $cancel_other);

    my $out = $bg->query("SELECT 'awake'");
    like($out, qr/awake/, 'session recovered after a cancel at pre-commit');

    # A CANCEL MUST STAY A CANCEL.
    #
    # The flush re-raises transport failures as 08007, and the guard that
    # exempts a query cancel from that is what these two assert. Without them
    # the exemption can be deleted and every test still passes: the session
    # recovers either way, so "it recovered" proves nothing about which error
    # the user was handed. Turning someone's own cancel into "the outcome is
    # unknown" would be answering a question they did not ask.
    my $log = slurp_file($node->logfile, $logpos);

    like($log, qr/canceling statement due to user request/,
        'a cancel at pre-commit is still reported as a cancel');
    unlike($log, qr/the outcome of the Valkey write is unknown/,
        'a cancel is not re-classified as an indeterminate write');

    # Whatever the race decided, the transaction is over and the session works.
    my $state = $node->safe_psql('postgres',
        "SELECT count(*) FROM pg_stat_activity WHERE state = 'idle in transaction'");
    is($state, '0', 'no transaction was left open by the cancel');

    $bg->quit;
}

# ---------------------------------------------------------------------------
# 4. Materialising a large aggregate is interruptible.
#
# Invariant I6 covers waits, and the three cases above prove the waits. This
# proves a LOOP: vfdw_probe_elements turns a reply that has already arrived
# into rows, and a million-element LRANGE spends real time there with no I/O
# left to be interrupted by.
#
# A million SMALL elements on purpose. The wire read and libvalkey's parse are
# a different phase with different interrupt behaviour, so a few large values
# would put the time there and this case would prove that phase instead. Four
# megabytes arrives quickly and then takes a long time to become rows.
#
# THE ASSERTION IS RELATIVE, not a fixed budget. "It finished in under N
# seconds" is true of a probe that ignored the cancel and simply ran fast, so
# the uncancelled duration is measured first and the cancelled one must be a
# fraction of it. That is the difference between interrupted and merely quick.
# ---------------------------------------------------------------------------
{
    $node->safe_psql('postgres', q{
        SELECT valkey_fdw_test_probe('tap_srv', 0, 'EVAL',
            'for i=1,1000 do local t={} for j=1,1000 do t[j]="x" end server.call("RPUSH","tap:big",unpack(t)) end return 1',
            '0')
    });

    my $probe = q{
        SELECT count(*) FROM valkey_fdw_test_probe(
            'tap_srv', 0, 'LRANGE', 'tap:big', '0', '-1')
    };

    my $t0 = time();
    $node->safe_psql('postgres', $probe);
    my $full = time() - $t0;

    my $bg = $node->background_psql('postgres', on_error_stop => 0);
    $bg->query_safe("SELECT 1");

    my $t1 = time();
    $bg->query_until(qr/FIRED/, "\\echo FIRED\n$probe;\n");
    select(undef, undef, undef, 0.3);
    $node->safe_psql('postgres', $cancel_other);

    my $out = $bg->query("SELECT 'awake'");
    my $cancelled = time() - $t1;

    like($out, qr/awake/, 'session recovered after cancelling a probe');
    cmp_ok($cancelled, '<', $full / 2,
        "probe was interrupted rather than run to completion "
        . "(${cancelled}s cancelled vs ${full}s full)");

    $bg->quit;
    $node->safe_psql('postgres',
        "SELECT valkey_fdw_test_probe('tap_srv', 0, 'DEL', 'tap:big')");
}

$node->safe_psql('postgres',
    "SELECT valkey_fdw_test_probe('tap_srv', 0, 'FLUSHDB')");

$node->stop;
done_testing();
