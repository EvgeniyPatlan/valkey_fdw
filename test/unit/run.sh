#!/usr/bin/env bash
#
# Unit and consistency checks that do not belong in a regression suite.
#
# Runs inside the build container, against the PostgreSQL instance the
# entrypoint started. Phase 0 covers documentation drift and the structural
# invariants; the C unit binary lands with the connection layer in Phase 1.
set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."

DB=vfdw_unit
status=0

pass() { printf '\033[1;32mok\033[0m   %s\n' "$*"; }
fail() { printf '\033[1;31mnot ok\033[0m %s\n' "$*" >&2; status=1; }

# A check that did not run is not a check that passed. Printing "ok" for a
# skipped cross-check is how a comparison that executes on no topology this
# project runs came to look green on all of them.
skip() { printf '\033[1;33mskip\033[0m %s\n' "$*"; }

psql -X -q -c "DROP DATABASE IF EXISTS ${DB}" postgres
psql -X -q -c "CREATE DATABASE ${DB}" postgres
psql -X -q -c "CREATE EXTENSION valkey_fdw" "${DB}"
# The diagnostics the checks below drive are a separate, superuser-only
# extension. pg_regress loads both for the regression suites; this database is
# built here, so both are named here.
psql -X -q -c "CREATE EXTENSION valkey_fdw_test" "${DB}"

q() { psql -X -tA -d "${DB}" -c "$1"; }

# ---------------------------------------------------------------------------
# Every option is documented.
#
# The option table is the source of truth; README drifting away from it is how
# users end up trusting a document that no longer matches the code.
# ---------------------------------------------------------------------------
undocumented=()
while IFS= read -r opt; do
    [[ -z "$opt" ]] && continue
    if ! grep -qE "\`${opt}\`" README.md; then
        undocumented+=("$opt")
    fi
done < <(q "SELECT name FROM valkey_fdw_options() ORDER BY name")

if (( ${#undocumented[@]} == 0 )); then
    pass "every option is documented in README.md"
else
    fail "options missing from README.md: ${undocumented[*]}"
fi

# ---------------------------------------------------------------------------
# Nothing documented as an option is absent from the table (the other
# direction of the same drift).
# ---------------------------------------------------------------------------
known="$(q "SELECT string_agg(name, '|') FROM valkey_fdw_options()")"
stray=()
while IFS= read -r opt; do
    [[ -z "$opt" ]] && continue
    [[ "|${known}|" == *"|${opt}|"* ]] || stray+=("$opt")
done < <(sed -n '/<!-- options:begin -->/,/<!-- options:end -->/p' README.md \
         | grep -oE '^\| `[a-z_]+`' | tr -d '| `')

if (( ${#stray[@]} == 0 )); then
    pass "README documents no option the code does not accept"
else
    fail "README documents unknown options: ${stray[*]}"
fi

# ---------------------------------------------------------------------------
# Sensitive options must never appear in the SRF's default_value column, and
# must be flagged. A credential leaking through a diagnostic view is exactly
# the kind of thing that is easy to add and hard to notice.
# ---------------------------------------------------------------------------
leaky="$(q "SELECT count(*) FROM valkey_fdw_options()
            WHERE sensitive AND default_value IS NOT NULL")"
if [[ "$leaky" == "0" ]]; then
    pass "no sensitive option carries a default value"
else
    fail "${leaky} sensitive option(s) expose a default value"
fi

# ---------------------------------------------------------------------------
# The README's Superuser column agrees with the option table.
#
# A privilege rule a user can only find out about by being refused is not a
# documented rule, and that column is the only place the README states one.
# Checked in both directions for the same reason the option names are: a row
# that quietly loses its flag reads as an option anyone may set, and a flag
# the code has dropped reads as a restriction that is still enforced.
#
# The column is the LAST cell, so $(NF-1) under -F'|' - the trailing pipe
# makes the final field empty. Anything other than "no" counts as restricted,
# because password_required's cell says "to disable" rather than "yes" and
# collapsing the two spellings here would hide which one a row carries.
# ---------------------------------------------------------------------------
code_priv="$(q "SELECT coalesce(string_agg(name, ' ' ORDER BY name), '')
                FROM valkey_fdw_options() WHERE requires_superuser")"
readme_priv="$(sed -n '/<!-- options:begin -->/,/<!-- options:end -->/p' README.md \
               | awk -F'|' '/^\| `[a-z_]+`/ && $(NF-1) !~ /^ *no *$/ {
                     gsub(/[` ]/, "", $2); print $2 }' \
               | sort | tr '\n' ' ' | sed 's/ $//')"

if [[ "$code_priv" == "$readme_priv" ]]; then
    pass "the README's Superuser column matches the option table"
else
    fail "Superuser column drift: code says [${code_priv}], README says [${readme_priv}]"
fi

# ---------------------------------------------------------------------------
# The version function must agree with the control file's default_version.
# ---------------------------------------------------------------------------
control_version="$(grep -oP "default_version\s*=\s*'\K[^']+" valkey_fdw.control)"
code_major="$(q "SELECT valkey_fdw_version() / 100")"
control_major="${control_version%%.*}"
if [[ "$code_major" == "$control_major" ]]; then
    pass "code version agrees with control file (${control_version})"
else
    fail "code major ${code_major} disagrees with control default_version ${control_version}"
fi

# ---------------------------------------------------------------------------
# Value handling: src/vfdw_val.c, src/vfdw_score.c, src/vfdw_slot.c
#
# S2 builds the write direction before any write path exists, which is only
# possible because none of it touches Valkey. The assertions therefore live
# here rather than in a regression suite: they are unit tests driven through
# psql, and every one of them uses bytea rather than text, because the
# properties worth asserting - an embedded NUL, an invalidly encoded byte, and
# the difference between an empty value and a NULL one - all disappear the
# moment a probe hands its result through a text type.
# ---------------------------------------------------------------------------

# Report nothing on success; on failure, name every vector that disagreed.
#
# ON_ERROR_STOP and the exit status are load-bearing. Without them a query that
# fails to parse produces no rows, which is indistinguishable from a query
# whose vectors all passed - so a typo in an assertion reads as a green tick.
check() {
    local out

    if ! out="$(psql -X -tA -v ON_ERROR_STOP=1 -d "${DB}" -c "$2" 2>&1)"; then
        fail "$1: the assertion itself failed: ${out}"
        return
    fi

    if [[ -z "$out" ]]; then
        pass "$1"
    else
        fail "$1: $out"
    fi
}

psql -X -q -d "${DB}" <<'SQL'
-- A refusal is as much a result as a value, so both are reduced to a string a
-- vector table can compare. WHEN OTHERS rather than a named condition so a
-- wrong-but-plausible SQLSTATE fails the comparison instead of escaping it.
CREATE FUNCTION vfdw_try_key(key bytea, keyprefix text DEFAULT NULL,
                             keyset text DEFAULT NULL,
                             singleton_key text DEFAULT NULL,
                             has_key_column boolean DEFAULT true)
RETURNS text LANGUAGE plpgsql AS $$
BEGIN
    RETURN 'ok:' || encode(valkey_fdw_test_build_key(key, keyprefix, keyset,
                                                     singleton_key,
                                                     has_key_column), 'escape');
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE;
END $$;

CREATE FUNCTION vfdw_try_score(score bytea)
RETURNS text LANGUAGE plpgsql AS $$
BEGIN
    PERFORM valkey_fdw_test_score(score, true);
    RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE;
END $$;

-- A literal is never toasted, so the only way to present a compressed or an
-- out-of-line datum to the write path is to select one out of a heap column.
-- Row 1 is incompressible and lands out of line; row 2 compresses; row 3 is
-- short enough to carry a 1-byte varlena header, where VARSIZE - VARHDRSZ is
-- wrong by exactly 3.
CREATE TABLE vfdw_toast(id int, blob bytea);
INSERT INTO vfdw_toast
SELECT 1, decode(string_agg(md5(g::text), ''), 'hex') FROM generate_series(1, 65536) g;
INSERT INTO vfdw_toast VALUES (2, convert_to(repeat('a', 1000000), 'UTF8'));
INSERT INTO vfdw_toast VALUES (3, convert_to(repeat('b', 100), 'UTF8'));
SQL

# ---------------------------------------------------------------------------
# CRC-16/XMODEM. The published check value pins the polynomial, the zero init,
# the absence of input and output reflection and the zero final xor, all four
# in one number and independently of any key.
# ---------------------------------------------------------------------------
check "crc16('123456789') is the published XMODEM check value 0x31C3" "
    SELECT CASE WHEN valkey_fdw_test_crc16('123456789') = 12739 THEN ''
                ELSE 'got ' || valkey_fdw_test_crc16('123456789') END"

# ---------------------------------------------------------------------------
# Hash-tag extraction, exactly as keyHashSlot does it. Every vector below is a
# case where a plausible simplification - brace balancing, searching for '}'
# from index 0, retrying after a degenerate pair, treating the key as a C
# string - gives a different answer and, in cluster mode, a different node.
# ---------------------------------------------------------------------------
check "hash-tag extraction matches keyHashSlot on every brace shape" "
    WITH v(k, want, why) AS (VALUES
        ('{a}b'::bytea,        'a'::bytea,        'tag at the start, suffix after'),
        ('a{b}c',              'b',               'prefix and suffix both excluded'),
        ('}{a}',               'a',               'a closer left of the opener is invisible'),
        ('{a}{b}',             'a',               'the first well-formed pair wins'),
        ('{}',                 '{}',              'adjacent braces hash the WHOLE key'),
        ('{}b',                '{}b',             'the documented {} escape hatch'),
        ('a{}',                'a{}',             'degenerate pair at the end'),
        ('a{}b',               'a{}b',            'degenerate pair, bytes both sides'),
        ('{}{tag}',            '{}{tag}',         'a degenerate pair does not fall through'),
        ('foo{}{bar}',         'foo{}{bar}',      'the same trap, as the spec spells it'),
        ('a{b',                'a{b',             'unterminated opener'),
        ('{',                  '{',               'closer search handed a zero length'),
        ('}',                  '}',               'no opener at all'),
        ('a}b{c',              'a}b{c',           'closer before opener: no negative length'),
        ('{a{b}c}',            'a{b',             'braces are not balanced'),
        ('foo{{bar}}zap',      '{bar',            'the tag may itself contain a brace'),
        ('foo{bar}{zap}',      'bar',             'first pair wins, second unexamined'),
        ('{ }',                ' ',               'one byte suffices; no trimming'),
        ('a\\000b',             'a\\000b',          'embedded NUL, no braces'),
        ('{a\\000b}c',          'a\\000b',          'a NUL is legal inside a tag'),
        ('{a}\\000b',           'a',               'a NUL in the suffix is not a terminator'),
        ('{\\000}',             '\\000',            'a NUL is content: the pair is not degenerate'),
        ('',                   '',                'total over the empty key')
    )
    SELECT coalesce(string_agg(format('%s (%s)', encode(k, 'escape'), why), '; '), '')
    FROM v WHERE valkey_fdw_test_hashtag(k) IS DISTINCT FROM want"

# ---------------------------------------------------------------------------
# Slots, against the server's own CLUSTER KEYSLOT further below. These are the
# vectors that exercise the reduction (a raw CRC above 16384), the high-bit
# byte that a signed char index would corrupt, and the slot-0 aliasing that
# makes 0 unusable as an "unbound" sentinel.
# ---------------------------------------------------------------------------
check "slots agree with the recorded cluster-spec vectors" "
    WITH v(k, want, why) AS (VALUES
        ('123456789'::bytea,        12739, 'raw CRC below 16384: the mask is a no-op'),
        ('x',                       16287, 'raw CRC 65439: this one exercises the mask'),
        ('a',                       15495, 'raw CRC 31879'),
        ('foo',                     12182, 'raw CRC 44950'),
        ('hello',                     866, 'raw CRC 50018'),
        ('abc',                      7638, 'raw CRC 40406'),
        ('somekey',                 11058, 'worked example from CLUSTER KEYSLOT'),
        ('foo{hash_tag}',            2515, 'worked example; equals bar{hash_tag}'),
        ('bar{hash_tag}',            2515, 'two keys, one slot, by hash tag'),
        ('{user1000}.following',     3443, 'the canonical co-location example'),
        ('{user1000}.followers',     3443, 'and its sibling'),
        ('user1000',                 3443, 'both equal the bare tag''s slot'),
        ('{}',                      15257, 'NOT slot 0: the whole key is hashed'),
        ('',                            0, 'the empty key'),
        ('\\000',                        0, 'a NUL byte: CRC init 0 is a fixed point at 0'),
        ('\\000\\000',                    0, 'and stays one'),
        ('{\\000}',                      0, 'a valid 1-byte tag that still lands on 0'),
        ('a\\000b',                   8383, 'NOT slot(''a'') = 15495'),
        ('{a\\000b}c',                8383, 'the same tag reached through braces'),
        ('\\377\\376',                 3374, 'high-bit bytes: a signed char index breaks here'),
        ('caf\\303\\251',              5735, 'and here, on ordinary UTF-8')
    )
    SELECT coalesce(string_agg(format('%s: got %s want %s (%s)', encode(k, 'escape'),
                                      valkey_fdw_test_keyslot(k), want, why), '; '), '')
    FROM v WHERE valkey_fdw_test_keyslot(k) <> want"

# ---------------------------------------------------------------------------
# Ground truth from the running server rather than from a table anyone could
# transcribe wrongly.
#
# CLUSTER KEYSLOT does NOT answer on every server: checked in the container,
# the standalone topology replies "ERR This instance has cluster support
# disabled", and the tls and acl topologies need credentials a bare valkey-cli
# does not have. So the cross-check runs where it can and announces itself as
# SKIPPED elsewhere. The SQL vectors above pin the arithmetic on every
# topology; this pins them to the server.
#
# It used to print "ok" on the skip branch, which meant the one comparison
# against the server's own arithmetic executed on no topology this project
# runs while reporting success on all of them - and the entire 60-vector slot
# table in val.sql rests on it, plus the single published XMODEM check value.
# So the skip is now visibly a skip, and it is a hard FAILURE on the cluster
# topology, where the server can answer and a skip would mean the check was
# lost rather than unavailable.
# ---------------------------------------------------------------------------
VK_HOST="${VALKEY_HOST:-valkey}"
VK_PORT="${VALKEY_PORT:-6379}"

server_slot="$(valkey-cli -h "$VK_HOST" -p "$VK_PORT" CLUSTER KEYSLOT somekey 2>&1)"

if [[ "$server_slot" != "11058" && "${VFDW_TOPOLOGY:-}" == "cluster" ]]; then
    fail "CLUSTER KEYSLOT cross-check did not run on the cluster topology (server said: ${server_slot})"
fi

if [[ "$server_slot" == "11058" ]]; then
    mismatches=()
    for key in '{a}b' 'a{b}c' '{}' 'a{b' 'a}b{c' '{a{b}c}' '{}{tag}' '{' '}' \
               'foo{{bar}}zap' 'foo{bar}{zap}' '{user1000}.following' \
               '123456789' 'x' 'somekey' 'foo{hash_tag}' '{ }' '{}b' 'a{}b'; do
        theirs="$(printf '%s' "$key" | valkey-cli -h "$VK_HOST" -p "$VK_PORT" -x CLUSTER KEYSLOT)"
        ours="$(q "SELECT valkey_fdw_test_keyslot(convert_to(\$\$${key}\$\$, 'UTF8'))")"
        [[ "$theirs" == "$ours" ]] || mismatches+=("${key}: server ${theirs}, ours ${ours}")
    done
    if (( ${#mismatches[@]} == 0 )); then
        pass "every slot matches the server's own CLUSTER KEYSLOT"
    else
        fail "slot disagreements: ${mismatches[*]}"
    fi
else
    skip "CLUSTER KEYSLOT cross-check NOT RUN (server said: ${server_slot})"
fi

# ---------------------------------------------------------------------------
# Score syntax. The validator runs on the RENDERED bytes, so a text column
# holding 'nan' must fail exactly as a float8 column holding NaN does; a
# type-level check would pass one of them straight through to a ZADD in phase
# 2 of the flush, which is the partial-application outcome the design exists to
# make unreachable.
# ---------------------------------------------------------------------------
check "the score grammar accepts and refuses exactly what it claims" "
    WITH v(s, want, why) AS (VALUES
        ('0'::bytea,        'ok',           'the smallest whole number'),
        ('-0',              'ok',           'signed zero'),
        ('+1',              'ok',           'a leading plus is allowed'),
        ('1.5',             'ok',           'fixed point'),
        ('.5',              'ok',           'no integer part'),
        ('1.',              'ok',           'no fractional digits'),
        ('1e3',             'ok',           'scientific'),
        ('1E+3',            'ok',           'either case, signed exponent'),
        ('1e-320',          'ok',           'subnormal: ERANGE alone is not a refusal'),
        ('9007199254740993','ok',           'accepted, then stored as the nearest double'),
        ('1.10',            'ok',           'numeric_out keeps the trailing zero'),
        ('',                '22023',        'empty is not NULL and not a number'),
        ('nan',             '22023',        'the server refuses this one too'),
        ('NaN',             '22023',        'numeric_out spells it this way'),
        ('inf',             '22023',        'policy: Valkey would have accepted it'),
        ('+inf',            '22023',        'same'),
        ('-inf',            '22023',        'same'),
        ('Infinity',        '22023',        'float8_out spells it this way'),
        ('-Infinity',       '22023',        'and this way'),
        ('INF',             '22023',        'case makes no difference'),
        ('1e400',           '22023',        'overflow: ERANGE with HUGE_VAL'),
        ('-1e400',          '22023',        'and its negative'),
        ('1e-400',          '22023',        'underflow to exactly zero'),
        (' 1',              '22023',        'leading whitespace, as the server refuses it'),
        ('1 ',              '22023',        'trailing anything'),
        ('1\012',           '22023',        'including a newline'),
        ('0x1p3',           '22023',        'hex floats: refused on every server version'),
        ('1e',              '22023',        'an incomplete exponent is trailing garbage'),
        ('1\\0002',          '22023',        'an embedded NUL, walked as bytes not as a string'),
        ('--1',             '22023',        'one sign only'),
        ('.',               '22023',        'no digits at all')
    )
    SELECT coalesce(string_agg(format('%s: got %s want %s (%s)', encode(s, 'escape'),
                                      vfdw_try_score(s), want, why), '; '), '')
    FROM v WHERE vfdw_try_score(s) <> want"

# ---------------------------------------------------------------------------
# The key builder. keyprefix filters and never transforms, so the key column
# carries the complete key on write exactly as it does on read; a write that
# prepended would create a key the writing table can never scan back.
# ---------------------------------------------------------------------------
check "the key builder validates the prefix and passes the bytes through" "
    WITH v(k, prefix, kset, singleton, has_col, want, why) AS (VALUES
        ('str:a'::bytea, 'str:',  NULL,   NULL,   true,  'ok:str:a',
            'in-prefix: the full key reaches Valkey unchanged'),
        ('str:',         'str:',  NULL,   NULL,   true,  'ok:str:',
            'exactly the prefix: SCAN MATCH str:* returns it, so writes must too'),
        ('a',            'str:',  NULL,   NULL,   true,  '23514',
            'out of prefix: would be invisible to the table that wrote it'),
        ('str',          'str:',  NULL,   NULL,   true,  '23514',
            'shorter than the prefix: this is why memcmp needs a length guard'),
        ('a\\000b',       'a',     NULL,   NULL,   true,  'ok:a\\000b',
            'a NUL one byte past the prefix must not terminate anything'),
        ('a*bc',         'a*b',   NULL,   NULL,   true,  'ok:a*bc',
            'the prefix is a literal'),
        ('axbc',         'a*b',   NULL,   NULL,   true,  '23514',
            'and not a glob: the scan escapes the star, so a write must not match it'),
        ('',             NULL,    NULL,   NULL,   true,  'ok:',
            'the empty string is a valid Valkey key'),
        ('',             'str:',  NULL,   NULL,   true,  '23514',
            'refused here for the prefix, not for being empty'),
        (NULL,           NULL,    NULL,   NULL,   true,  '23502',
            'a null key names no row'),
        (NULL,           'str:',  NULL,   NULL,   true,  '23502',
            'and is reported as null before it is reported as out of prefix'),
        ('tags',         NULL,    NULL,   'tags', true,  'ok:tags',
            'a singleton key column may restate the option'),
        (NULL,           NULL,    NULL,   'tags', true,  'ok:tags',
            'or leave it unspecified, which is what COPY produces'),
        (NULL,           NULL,    NULL,   'tags', false, 'ok:tags',
            'or not exist at all: the path that would index cols[-1]'),
        ('other',        NULL,    NULL,   'tags', true,  '23514',
            'writing another name would silently discard the user''s value'),
        ('k1',           NULL,    'idx',  NULL,   true,  'ok:k1',
            'a keyset table has no prefix to satisfy'),
        ('idx',          NULL,    'idx',  NULL,   true,  '23514',
            'but a row may not collide with the table''s own index')
    )
    SELECT coalesce(string_agg(format('%s: got %s want %s (%s)',
                                      coalesce(encode(k, 'escape'), 'NULL'),
                                      vfdw_try_key(k, prefix, kset, singleton, has_col),
                                      want, why), '; '), '')
    FROM v
    WHERE vfdw_try_key(k, prefix, kset, singleton, has_col) <> want"

# ---------------------------------------------------------------------------
# Datum to bytes. bytea is byte-exact by construction; everything else is
# out(in(bytes)), which is the only round trip a value through Valkey can have.
# ---------------------------------------------------------------------------
check "bytea round-trips byte for byte, whatever the bytes are" "
    WITH v(payload, why) AS (VALUES
        (''::bytea,                          'empty, which is a value and not NULL'),
        ('a\\000b',                           'an embedded NUL survives'),
        ('\\000\\000\\000\\000',                    'and so does a value that is nothing else'),
        ('\\377\\376\\375',                       'invalid UTF-8 is never validated outbound'),
        ('caf\\303\\251',                       'ordinary multibyte text'),
        (convert_to(repeat('z', 1000), 'UTF8'), 'long enough to need a 4-byte varlena header')
    )
    SELECT coalesce(string_agg(why, '; '), '')
    FROM v WHERE valkey_fdw_test_val_roundtrip(payload, 'bytea'::regtype) IS DISTINCT FROM payload"

check "a NULL is distinguishable from an empty value in both directions" "
    SELECT CASE
        WHEN valkey_fdw_test_val_out(NULL::bytea) IS NOT NULL
            THEN 'NULL rendered to bytes'
        WHEN valkey_fdw_test_val_out(''::bytea) IS NULL
            THEN 'an empty value rendered to NULL'
        WHEN length(valkey_fdw_test_val_out(''::bytea)) <> 0
            THEN 'an empty value did not render to zero bytes'
        WHEN valkey_fdw_test_val_out(NULL::text) IS NOT NULL
            THEN 'a NULL text rendered to bytes'
        ELSE '' END"

check "other types render through their own output function" "
    WITH v(payload, typ, want, why) AS (VALUES
        ('hello'::bytea,      'text',      'hello'::bytea,    'text is verbatim both ways'),
        ('',                  'text',      '',                'and empty text stays empty'),
        ('42',                'int4',      '42',              'canonical already'),
        ('007',               'int4',      '7',               'out(in(x)) is not x for most types'),
        (' 007',              'int4',      '7',               'which is why text or bytea keys are advised'),
        ('0.1',               'float8',    '0.1',             'shortest round-trip form'),
        ('1.10',              'numeric',   '1.10',            'numeric keeps its trailing zero'),
        ('t',                 'bool',      't',               'bool'),
        ('2020-01-02 03:04:05','timestamp','2020-01-02 03:04:05', 'stable under the default DateStyle')
    )
    SELECT coalesce(string_agg(format('%s as %s: got %s want %s (%s)',
                                      encode(payload, 'escape'), typ,
                                      encode(valkey_fdw_test_val_roundtrip(payload, typ::regtype), 'escape'),
                                      encode(want, 'escape'), why), '; '), '')
    FROM v
    WHERE valkey_fdw_test_val_roundtrip(payload, typ::regtype) IS DISTINCT FROM want"

# ---------------------------------------------------------------------------
# The detoast. Without it VARDATA_ANY reads an 18-byte varatt_external pointer
# struct and writes garbage of the wrong length - and no test built from
# literals can catch it, because a literal is never toasted.
# ---------------------------------------------------------------------------
check "a toasted datum is detoasted before it is rendered" "
    SELECT coalesce(string_agg(format('row %s: %s bytes rendered from %s', id,
                                      length(valkey_fdw_test_val_out(blob)),
                                      octet_length(blob)), '; '), '')
    FROM vfdw_toast
    WHERE valkey_fdw_test_val_out(blob) IS DISTINCT FROM blob"

check "a short-header datum reports its own length, not three bytes less" "
    SELECT CASE WHEN length(valkey_fdw_test_val_out(blob)) = 100 THEN ''
                ELSE 'got ' || length(valkey_fdw_test_val_out(blob)) END
    FROM vfdw_toast WHERE id = 3"

psql -X -q -c "DROP DATABASE ${DB}" postgres

exit $status
