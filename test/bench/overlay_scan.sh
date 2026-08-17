#!/usr/bin/env bash
#
# What does a scan cost when one key has been written many times?
#
# The overlay keeps, per key, the list of operations this transaction has
# buffered against it, and a scan consults that list for every key it returns.
# The walk is linear in the operations on THAT key, which is small in practice
# and unbounded in principle - a transaction that wrote ten thousand times to
# one key and then scanned it pays for all of them, on every scanned row that
# shares the key.
#
# The walk was recorded as correct and unmeasured. This
# measures it. The question it answers is not "is linear acceptable" in the
# abstract, it is whether the constant is small enough that the shape only
# matters at depths no real transaction reaches.
#
# Reports scan latency against overlay depth; the caller reads it and decides.

set -euo pipefail

PSQL=${PSQL:-psql}
DB=${DB:-bench_overlay}
VALKEY_HOST=${VALKEY_HOST:-valkey}
VALKEY_PORT=${VALKEY_PORT:-6379}

# Depths bracketing anything sane, far enough out that a linear term would be
# unmistakable by the last row rather than lost in noise.
DEPTHS=${DEPTHS:-"1 10 100 1000 5000 10000"}

# Enough other keys that the scan does real work besides the hot key, so the
# figure is a scan cost and not a single lookup.
OTHER_KEYS=${OTHER_KEYS:-1000}

$PSQL -X -q -d postgres -c "DROP DATABASE IF EXISTS $DB" -c "CREATE DATABASE $DB" >/dev/null
$PSQL -X -q -d "$DB" -c "CREATE EXTENSION valkey_fdw" >/dev/null
# valkey_fdw_test_probe, used below to clear the database, is in the
# diagnostics extension; this script builds its own database, so it names both.
$PSQL -X -q -d "$DB" -c "CREATE EXTENSION valkey_fdw_test" >/dev/null

$PSQL -X -q -d "$DB" >/dev/null <<SQL
CREATE SERVER o FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host '$VALKEY_HOST', port '$VALKEY_PORT',
             write_max_ops '1000000', write_max_bytes '1073741824',
             command_timeout_ms '120000');
CREATE USER MAPPING FOR CURRENT_USER SERVER o;
CREATE FOREIGN TABLE os (k text OPTIONS (key 'true'), v text)
    SERVER o OPTIONS (tabletype 'string', keyprefix 'o:');
SQL

$PSQL -X -q -d "$DB" -c "SELECT valkey_fdw_test_flush('o')" >/dev/null

# Seeded OUTSIDE the measured transaction and committed, so the scan below
# reads them from Valkey rather than from the overlay. Only the hot key's
# depth is meant to vary.
$PSQL -X -q -d "$DB" >/dev/null <<SQL
BEGIN;
INSERT INTO os SELECT 'o:k' || i, 'seed' FROM generate_series(1, $OTHER_KEYS) i;
COMMIT;
SQL

printf '%8s  %12s  %14s  %s\n' "depth" "scan_ms" "us_per_write" "note"

for n in $DEPTHS; do
    # Each UPDATE is its own statement, so the buffer really does accumulate n
    # operations against the one key. A single set-returning UPDATE would
    # append one, which would measure nothing.
    out=$($PSQL -X -q -d "$DB" 2>&1 <<SQL
BEGIN;
DO \$\$
BEGIN
    FOR i IN 1..$n LOOP
        UPDATE os SET v = i::text WHERE k = 'o:k1';
    END LOOP;
END \$\$;
\\timing on
SELECT count(*) FROM os;
\\timing off
ROLLBACK;
SQL
)
    ms=$(echo "$out" | grep -oE 'Time: [0-9.]+ ms' | tail -1 | grep -oE '[0-9.]+')
    [ -z "$ms" ] && ms=0

    per=$(awk -v m="$ms" -v n="$n" 'BEGIN{ if (n>0) printf "%.2f", m*1000/n; else print "-" }')

    note=""
    awk -v m="$ms" 'BEGIN{ exit !(m >= 1000) }' && note="over a second"

    printf '%8s  %12s  %14s  %s\n' "$n" "$ms" "$per" "$note"
done

$PSQL -X -q -d "$DB" -c "SELECT valkey_fdw_test_flush('o')" >/dev/null
