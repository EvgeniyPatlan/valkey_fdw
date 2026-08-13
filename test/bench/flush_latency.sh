#!/usr/bin/env bash
#
# How long does a flush block the Valkey server?
#
# write_max_ops shipped as a guess, and the README tells users to measure
# before raising it. This is that measurement, and the reason it matters is
# not throughput:
#
#   A Lua script holds the whole server for its duration - Valkey is single
#   threaded and EVAL is not preemptible. Past busy-reply-threshold (5000 ms
#   by default) every other client starts getting BUSY. And a script that has
#   already written CANNOT BE KILLED: SCRIPT KILL refuses once a write has
#   happened, so the only way out is SHUTDOWN NOSAVE, which loses the dataset
#   unless AOF is on.
#
# So the cap is not a performance knob, it is a blast radius. The number this
# prints is how long one transaction can stop every other client in the
# system, and the default must leave a wide margin below the threshold.
#
# Reports the per-size COMMIT latency; the caller reads it and decides.

set -euo pipefail

PSQL=${PSQL:-psql}
DB=${DB:-bench}
VALKEY_HOST=${VALKEY_HOST:-valkey}
VALKEY_PORT=${VALKEY_PORT:-6379}

# Sizes bracketing the shipped default of 10000, far enough either side that
# the shape of the curve is visible rather than one point being extrapolated.
SIZES=${SIZES:-"100 500 1000 2500 5000 10000 25000 50000"}

$PSQL -X -q -d postgres -c "DROP DATABASE IF EXISTS $DB" -c "CREATE DATABASE $DB" >/dev/null
$PSQL -X -q -d "$DB" -c "CREATE EXTENSION valkey_fdw" >/dev/null
# valkey_fdw_test_probe, used below to clear the database between sizes, is in
# the diagnostics extension; this script builds its own database, so it names
# both.
$PSQL -X -q -d "$DB" -c "CREATE EXTENSION valkey_fdw_test" >/dev/null

$PSQL -X -q -d "$DB" >/dev/null <<SQL
CREATE SERVER b FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host '$VALKEY_HOST', port '$VALKEY_PORT',
             write_max_ops '1000000', write_max_bytes '1073741824',
             command_timeout_ms '120000');
CREATE USER MAPPING FOR CURRENT_USER SERVER b;
CREATE FOREIGN TABLE bs (k text OPTIONS (key 'true'), v text)
    SERVER b OPTIONS (tabletype 'string', keyprefix 'b:');
CREATE FOREIGN TABLE bh (k text OPTIONS (key 'true'),
                         a text OPTIONS (field 'alpha'),
                         c text OPTIONS (field 'beta'))
    SERVER b OPTIONS (tabletype 'hash', keyprefix 'bh:');
SQL

printf '%8s  %12s  %12s  %s\n' "rows" "commit_ms" "us_per_row" "note"

for n in $SIZES; do
    $PSQL -X -q -d "$DB" -c \
        "SELECT valkey_fdw_test_probe('b', 0, 'FLUSHDB')" >/dev/null

    # A realistic mix rather than the cheapest possible row: two thirds
    # strings, one third three-field hashes. A benchmark of the cheapest
    # operation sets a cap that the expensive one then blows through.
    out=$($PSQL -X -q -d "$DB" 2>&1 <<SQL
\\timing on
BEGIN;
INSERT INTO bs SELECT 'b:' || i, repeat('x', 100)
  FROM generate_series(1, $((n * 2 / 3))) i;
INSERT INTO bh SELECT 'bh:' || i, repeat('y', 60), repeat('z', 60)
  FROM generate_series(1, $((n / 3))) i;
\\timing off
\\timing on
COMMIT;
SQL
)
    # The LAST timing line is the COMMIT, which is the flush.
    ms=$(echo "$out" | grep -oE 'Time: [0-9.]+ ms' | tail -1 | grep -oE '[0-9.]+')
    [ -z "$ms" ] && ms=0

    per=$(awk -v m="$ms" -v n="$n" 'BEGIN{ if (n>0) printf "%.1f", m*1000/n; else print "-" }')

    note=""
    awk -v m="$ms" 'BEGIN{ exit !(m >= 5000) }' && note="OVER busy-reply-threshold"
    awk -v m="$ms" 'BEGIN{ exit !(m >= 1000 && m < 5000) }' && note="within 5x of threshold"

    printf '%8s  %12s  %12s  %s\n' "$n" "$ms" "$per" "$note"
done

$PSQL -X -q -d "$DB" -c "SELECT valkey_fdw_test_probe('b', 0, 'FLUSHDB')" >/dev/null
