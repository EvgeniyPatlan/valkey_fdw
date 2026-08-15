-- Value handling: Datum to bytes, the key rules, score syntax, hash tags and
-- the cluster slot - the layer that decides one row's bytes before any command
-- is built.
--
-- Nothing here writes to Valkey. Every assertion but the three refusals in
-- B.2, which need a foreign table because a refusal raised from the ledger is
-- reachable no other way and which roll back, runs through the diagnostic
-- entry points in src/vfdw_testval.c, which drive src/vfdw_val.c,
-- src/vfdw_score.c and src/vfdw_slot.c directly - which is the whole point of
-- ordering the phase this way. The write path does not exist yet, so if these
-- properties were only reachable through INSERT they could not be pinned
-- until every command builder and the two-phase flush were also written, and
-- by then a wrong answer here would be indistinguishable from a wrong answer
-- there.
--
-- Every vector below was derived before the implementation existed: the slot
-- numbers from an independent bitwise CRC-16/XMODEM cross-checked against a
-- live 6-node Valkey 9.0.5 cluster, the score behaviours from a raw RESP
-- socket against a live standalone, the renderings from PostgreSQL 17 with
-- default GUCs. Where the code disagrees with one of them, the vector stays
-- and the disagreement is what this file records.
--
-- Every probe takes and returns bytea rather than text. An embedded NUL, a
-- byte that is not valid in the server encoding, and the difference between an
-- empty value and a NULL one all vanish the moment a result is handed through
-- a text type, and those three are most of what is worth asserting here.
--
-- Each vector carries its own verdict column. 'ok' means the code answered
-- what the vector said it would; DISAGREES means it did not, and because
-- expected output is recorded from a real run rather than written by hand, a
-- DISAGREES here is a recorded open question, not an accepted result. Anything
-- reading this file for reassurance should grep for it first.

-- ---------------------------------------------------------------------------
-- Helpers.
--
-- A refusal is as much a result as a value, so each helper reduces both to one
-- string a vector table can compare against. WHEN OTHERS rather than a named
-- condition, so a wrong-but-plausible SQLSTATE fails the comparison instead of
-- being caught by the handler that was looking for the right one.
-- ---------------------------------------------------------------------------
CREATE FUNCTION val_key(key bytea, keyprefix text DEFAULT NULL,
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

CREATE FUNCTION val_score(score bytea)
RETURNS text LANGUAGE plpgsql AS $$
BEGIN
    PERFORM valkey_fdw_test_score(score, true);
    RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE;
END $$;

-- The verdict alongside the SQLSTATE. Two refusals that share 22023 are not
-- the same refusal, and a vector that only checked the SQLSTATE would pass
-- while the validator rejected the value for entirely the wrong reason.
CREATE FUNCTION val_score_why(score bytea)
RETURNS text LANGUAGE plpgsql AS $$
BEGIN
    RETURN valkey_fdw_test_score(score, false);
EXCEPTION WHEN OTHERS THEN
    RETURN 'raised ' || SQLSTATE;
END $$;

-- Which key rule fired, alongside the SQLSTATE. Three of the five refusals
-- share 23514, so a vector comparing only the SQLSTATE passes unchanged if the
-- builder rejects a key through the wrong branch, or if the three branches
-- collapse into one. This is val_score_why's counterpart on the key side.
CREATE FUNCTION val_key_why(key bytea, keyprefix text DEFAULT NULL,
                            keyset text DEFAULT NULL,
                            singleton_key text DEFAULT NULL,
                            has_key_column boolean DEFAULT true)
RETURNS text LANGUAGE plpgsql AS $$
BEGIN
    RETURN valkey_fdw_test_key_why(key, keyprefix, keyset, singleton_key,
                                   has_key_column);
EXCEPTION WHEN OTHERS THEN
    RETURN 'raised ' || SQLSTATE;
END $$;

-- One slot value through vfdw_val_from_slot, which is the entry point a
-- statement calls. 'null' is a rendered NULL, which is a result and not a
-- refusal; a bare SQLSTATE is the refusal.
CREATE FUNCTION val_slot(payload bytea, typ text, kind text,
                         keyprefix text DEFAULT NULL,
                         keyset text DEFAULT NULL,
                         singleton_key text DEFAULT NULL)
RETURNS text LANGUAGE plpgsql AS $$
DECLARE
    r bytea;
BEGIN
    r := valkey_fdw_test_val_slot(payload, typ::regtype, kind, keyprefix,
                                  keyset, singleton_key);
    IF r IS NULL THEN
        RETURN 'null';
    END IF;
    RETURN 'ok:' || encode(r, 'escape');
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE;
END $$;

-- SQLSTATE, message and detail together. Nothing in this suite used to inspect
-- error TEXT, so an invariant I2 regression - a Valkey- or Datum-originated
-- string reaching ereport as a format string rather than as an argument to
-- "%s" - produced no diff at all.
CREATE FUNCTION val_msg(stmt text)
RETURNS text LANGUAGE plpgsql AS $$
DECLARE
    m text;
    d text;
BEGIN
    EXECUTE stmt;
    RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
    GET STACKED DIAGNOSTICS m = MESSAGE_TEXT, d = PG_EXCEPTION_DETAIL;
    RETURN SQLSTATE || ' | ' || m || ' | ' || d;
END $$;

CREATE FUNCTION val_roundtrip(payload bytea, typ text)
RETURNS text LANGUAGE plpgsql AS $$
BEGIN
    RETURN 'ok:' || encode(valkey_fdw_test_val_roundtrip(payload, typ::regtype),
                           'escape');
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE;
END $$;

CREATE FUNCTION val_raises(stmt text)
RETURNS text LANGUAGE plpgsql AS $$
BEGIN
    EXECUTE stmt;
    RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE;
END $$;

-- User-defined types, because the write direction deliberately has no
-- supported-type whitelist: it asks the type for its output function the same
-- way the read direction asks for its input function. A whitelist would have
-- to be maintained and would be a second place that could disagree with the
-- read path about a type's wire form.
CREATE TYPE val_color AS ENUM ('red', 'green');
CREATE DOMAIN val_dom AS text;
CREATE DOMAIN val_blob AS bytea;
CREATE DOMAIN val_blob_checked AS bytea CHECK (octet_length(VALUE) = 2);

-- ===========================================================================
-- A.1  Datum -> bytes, output-function branch
--
-- Every non-bytea column is written as the bytes its own output function
-- produces. What would be wrong if these did not hold: a per-type table in the
-- write direction and out/in in the read direction is exactly how a value
-- comes back different from how it went in, and a whitelist would refuse
-- user-defined types (C-37, C-38) for no reason at all.
--
-- The date, time and interval vectors were derived under DateStyle 'ISO, MDY'
-- and IntervalStyle 'postgres'. pg_regress overrides both for the whole run,
-- so the settings in force here are printed first: without them the three
-- rows that disagree below read as a defect in the renderer rather than as
-- what they are, which is C-30's point made by the harness itself.
-- ===========================================================================
SHOW DateStyle;
SHOW IntervalStyle;

WITH v(id, got, want, why) AS (VALUES
    ('C-10', valkey_fdw_test_val_out('hello'::text), 'hello'::bytea,
        'baseline'),
    ('C-11', valkey_fdw_test_val_out(''::text), ''::bytea,
        'empty is not NULL: a zero-length argument is emitted, never dropped'),
    ('C-12', valkey_fdw_test_val_out(convert_from('\x68c3a96c6c6f'::bytea, 'UTF8')),
        '\x68c3a96c6c6f'::bytea,
        'multibyte passes through with no pg_verifymbstr on the way out'),
    ('C-13', valkey_fdw_test_val_out('ab'::varchar(10)), 'ab'::bytea,
        'varchar does not pad'),
    ('C-14', valkey_fdw_test_val_out('ab'::char(5)), 'ab   '::bytea,
        'bpcharout pads to 5; a test written through ::text proves nothing'),
    ('C-15', valkey_fdw_test_val_out('abc'::name), 'abc'::bytea,
        'name, a fixed-width type whose output is not padded'),
    ('C-16a', valkey_fdw_test_val_out(true), 't'::bytea,
        'boolout spells it t, not true and not 1'),
    ('C-16b', valkey_fdw_test_val_out(false), 'f'::bytea,
        'and f'),
    ('C-17', valkey_fdw_test_val_out('-2147483648'::int4), '-2147483648'::bytea,
        'int4 at its negative bound'),
    ('C-18', valkey_fdw_test_val_out('9007199254740993'::int8),
        '9007199254740993'::bytea,
        'exact as a value; R-04 records what a zset score does to it'),
    ('C-19', valkey_fdw_test_val_out(1.0::float8 / 3.0::float8),
        '0.3333333333333333'::bytea,
        'shortest round-trip form at the default extra_float_digits'),
    ('C-21', valkey_fdw_test_val_out('-0.0'::float8), '-0'::bytea,
        'the quoted literal keeps the sign; (-0.0)::float8 renders 0'),
    ('C-22', valkey_fdw_test_val_out('1e20'::float8), '1e+20'::bytea,
        'float8out chooses the exponent form and signs the exponent'),
    ('C-23', valkey_fdw_test_val_out('0.1'::float4), '0.1'::bytea,
        'float4 shortest round-trip'),
    ('C-24', valkey_fdw_test_val_out('1.10'::numeric), '1.10'::bytea,
        'numeric_out keeps the trailing zero: scale is part of the value'),
    ('C-25', valkey_fdw_test_val_out('0.1234567890123456789'::numeric),
        '0.1234567890123456789'::bytea,
        'and keeps every digit, unlike the double it would become as a score'),
    ('C-28', valkey_fdw_test_val_out('2026-08-06'::date), '2026-08-06'::bytea,
        'date under the default DateStyle'),
    ('C-29', valkey_fdw_test_val_out('2026-08-06 12:34:56.789'::timestamp),
        '2026-08-06 12:34:56.789'::bytea,
        'timestamp keeps the fractional seconds it was given'),
    ('C-31', valkey_fdw_test_val_out('1 day 2 hours'::interval),
        '1 day 02:00:00'::bytea,
        'the postgres IntervalStyle'),
    ('C-32', valkey_fdw_test_val_out('0b7d0a55-1f9a-4c3e-9d21-9f8c1a2b3c4d'::uuid),
        '0b7d0a55-1f9a-4c3e-9d21-9f8c1a2b3c4d'::bytea,
        'uuid_out is lowercase and hyphenated'),
    ('C-33', valkey_fdw_test_val_out('{"b":1,"a":2}'::json),
        '{"b":1,"a":2}'::bytea,
        'json is stored and emitted verbatim, key order and all'),
    ('C-34', valkey_fdw_test_val_out('{"b":1,"a":2}'::jsonb),
        '{"a": 2, "b": 1}'::bytea,
        'jsonb normalises: the same input, different bytes on the wire'),
    ('C-35', valkey_fdw_test_val_out(ARRAY[1,2,3]), '{1,2,3}'::bytea,
        'an array is one value, not three arguments'),
    ('C-36', valkey_fdw_test_val_out(ARRAY['a','b,c']), '{a,"b,c"}'::bytea,
        'array quoting is the output function''s business, not ours'),
    ('C-37', valkey_fdw_test_val_out('red'::val_color), 'red'::bytea,
        'a user-defined enum works: there is no supported-type whitelist'),
    ('C-38', valkey_fdw_test_val_out('x'::val_dom), 'x'::bytea,
        'a domain resolves to its base type''s output function')
)
-- Bracketed, because C-14's answer is 'ab' followed by three spaces and an
-- unbracketed column renders it identically to C-13's 'ab'. A reviewer would
-- then be reading a row that looks the same whether bpcharout padded or not.
SELECT id,
       '[' || encode(got, 'escape') || ']' AS got,
       '[' || encode(want, 'escape') || ']' AS want,
       length(got) AS got_len,
       CASE WHEN got IS NOT DISTINCT FROM want THEN 'ok' ELSE 'DISAGREES' END
           AS verdict,
       why
FROM v ORDER BY id;

-- Renderings too long to print. C-26 and C-27 are the trap: numeric_out never
-- uses an exponent, so 1e400 is 401 bytes of digits and 1e-400 is 402. A test
-- that expected '1e+400' would be asserting a form PostgreSQL never produces -
-- and S-14/S-15 depend on these being the bytes the score validator sees.
WITH v(id, got, want, why) AS (VALUES
    ('C-26', valkey_fdw_test_val_out('1e400'::numeric),
        convert_to('1' || repeat('0', 400), 'UTF8'),
        'numeric_out writes 401 digits, not an exponent'),
    ('C-27', valkey_fdw_test_val_out('1e-400'::numeric),
        convert_to('0.' || repeat('0', 399) || '1', 'UTF8'),
        'and 402 bytes the other way'),
    ('C-39', valkey_fdw_test_val_out(repeat('ab', 200000)),
        convert_to(repeat('ab', 200000), 'UTF8'),
        'a long non-binary value is not truncated at any buffer boundary')
)
SELECT id, length(got) AS got_len, length(want) AS want_len,
       CASE WHEN got IS NOT DISTINCT FROM want THEN 'ok' ELSE 'DISAGREES' END
           AS verdict,
       why
FROM v ORDER BY id;

-- ---------------------------------------------------------------------------
-- C-20, C-30: the rendering is a function of session GUCs, which is a
-- documented property and not a defect - but it is the argument for text or
-- bytea key columns, so it is asserted rather than left as folklore. If these
-- did not hold, the wrapper would be pretending a float8 or timestamp key is
-- stable across sessions when two clients with different settings would write
-- two different Valkey keys for the same row.
-- ---------------------------------------------------------------------------
SET extra_float_digits = -3;
SELECT encode(valkey_fdw_test_val_out(1.0::float8 / 3.0::float8), 'escape')
           AS "C-20 got",
       '0.333333333333' AS "C-20 want (DBL_DIG - 3 = 12 significant digits)",
       CASE WHEN valkey_fdw_test_val_out(1.0::float8 / 3.0::float8)
                 = '0.333333333333'::bytea THEN 'ok' ELSE 'DISAGREES' END
           AS verdict;
RESET extra_float_digits;

SET DateStyle = 'German, DMY';
SELECT encode(valkey_fdw_test_val_out('2026-08-06 12:34:56.789'::timestamp),
              'escape') AS "C-30 got",
       '06.08.2026 12:34:56.789' AS "C-30 want",
       CASE WHEN valkey_fdw_test_val_out('2026-08-06 12:34:56.789'::timestamp)
                 = '06.08.2026 12:34:56.789'::bytea THEN 'ok' ELSE 'DISAGREES' END
           AS verdict;
RESET DateStyle;

-- ---------------------------------------------------------------------------
-- C-40: no non-bytea type can carry an interior NUL, which is why strlen on
-- an output function's result is not an invariant I3 violation. This is the
-- assertion that pins that reasoning to something checkable. If a text value
-- containing a NUL were constructible, the output-function branch's strlen
-- would truncate it silently and the row would be written short.
-- ---------------------------------------------------------------------------
SELECT val_raises($$ SELECT 'ab'::text $$) AS control_must_be_ok,
       val_raises($$ SELECT E'a\000b'::text $$) AS nul_in_a_literal,
       val_raises($$ SELECT chr(0) $$) AS nul_from_chr;

-- ===========================================================================
-- A.2  Datum -> bytes, binary branch
--
-- bytea takes the datum's bytes verbatim, with its length. What would be wrong
-- if these did not hold is NUL truncation in the write direction: a C-string
-- path reports success for '\x00' having written nothing, and a length taken
-- with strlen writes a different key or value than the statement named.
-- ===========================================================================
WITH v(id, got, want, why) AS (VALUES
    ('C-50', valkey_fdw_test_val_out('\x00'::bytea), '\x00'::bytea,
        'one NUL byte is a one-byte value, not an empty one'),
    ('C-51', valkey_fdw_test_val_out(''::bytea), ''::bytea,
        'empty bytea is a value; data may be any pointer, isnull decides'),
    ('C-52', valkey_fdw_test_val_out('\xfffefd'::bytea), '\xfffefd'::bytea,
        'no pg_verifymbstr anywhere on the write path, even in a UTF8 database'),
    ('C-53', valkey_fdw_test_val_out('\x610062'::bytea), '\x610062'::bytea,
        'an embedded NUL in the middle survives'),
    ('C-54', valkey_fdw_test_val_out('\x00000000'::bytea), '\x00000000'::bytea,
        'all-NUL: only the length distinguishes it from C-51')
)
SELECT id, '[' || encode(got, 'escape') || ']' AS got,
       '[' || encode(want, 'escape') || ']' AS want,
       length(got) AS got_len,
       CASE WHEN got IS NOT DISTINCT FROM want THEN 'ok' ELSE 'DISAGREES' END
           AS verdict,
       why
FROM v ORDER BY id;

-- ---------------------------------------------------------------------------
-- C-55 to C-57: the varlena cases a literal can never produce.
--
-- A literal is never toasted and never carries a short header, so all three of
-- these are invisible to a suite built from literals alone - they need a value
-- that has been through a heap tuple. Without PG_DETOAST_DATUM_PACKED the
-- write path sends the 18-byte varatt_external pointer struct instead of the
-- value; without VARSIZE_ANY_EXHDR it reports a length three bytes too large
-- for every value of 126 bytes or fewer.
-- ---------------------------------------------------------------------------
CREATE TABLE val_heap(id int, b bytea);
ALTER TABLE val_heap ALTER COLUMN b SET STORAGE EXTERNAL;
INSERT INTO val_heap
    VALUES (56, decode(repeat((SELECT string_agg(lpad(to_hex(g), 2, '0'), '')
                               FROM generate_series(0, 255) g), 4096), 'hex'));
ALTER TABLE val_heap ALTER COLUMN b SET STORAGE EXTENDED;
INSERT INTO val_heap VALUES (57, decode(repeat('00', 1048576), 'hex'));
INSERT INTO val_heap VALUES (55, decode(repeat('61', 100), 'hex'));

SELECT id,
       octet_length(b) AS value_bytes,
       pg_column_size(b) AS stored_bytes,
       CASE WHEN pg_column_size(b) < octet_length(b) THEN 'compressed'
            ELSE 'uncompressed' END AS storage,
       length(valkey_fdw_test_val_out(b)) AS rendered_bytes,
       CASE WHEN valkey_fdw_test_val_out(b) IS NOT DISTINCT FROM b THEN 'ok'
            ELSE 'DISAGREES' END AS verdict
FROM val_heap ORDER BY id;

-- C-55 spelled out on its own: the vector is stored_bytes = 101 for a 100-byte
-- value, which is the 1-byte varlena header. VARSIZE - VARHDRSZ is wrong here
-- and only here, and it is wrong by exactly three.
SELECT pg_column_size(b) AS "C-55 stored_bytes (want 101)",
       length(valkey_fdw_test_val_out(b)) AS "C-55 rendered (want 100)",
       CASE WHEN pg_column_size(b) = 101
                 AND length(valkey_fdw_test_val_out(b)) = 100 THEN 'ok'
            ELSE 'DISAGREES' END AS verdict
FROM val_heap WHERE id = 55;

-- ---------------------------------------------------------------------------
-- C-58: a domain over bytea is binary in both directions, because is_binary is
-- computed from the base type. If it were computed from atttypid the domain
-- would take the non-binary branch, '\x00ff' would go out as the six-byte text
-- \x00ff, and the read path would push raw bytes through pg_verifymbstr and
-- bytea_in - one expression, two answers, and a value that does not survive
-- its own round trip.
--
-- The binary branch bypasses the input function, and with it the domain's
-- CHECK constraint, so the read direction has to apply that check itself. The
-- last two rows are what proves it still does.
-- ---------------------------------------------------------------------------
SELECT val_roundtrip('\x00ff'::bytea, 'val_blob') AS "C-58 domain over bytea",
       length(valkey_fdw_test_val_out('\x00ff'::val_blob))
           AS "C-58 rendered length (want 2)",
       val_roundtrip('\x00ff'::bytea, 'val_blob_checked')
           AS "domain CHECK satisfied",
       val_roundtrip('\x00ffee'::bytea, 'val_blob_checked')
           AS "domain CHECK violated (want 23514)",
       CASE WHEN val_roundtrip('\x00ff'::bytea, 'val_blob') = 'ok:\000\377'
                 AND length(valkey_fdw_test_val_out('\x00ff'::val_blob)) = 2
                 AND val_roundtrip('\x00ff'::bytea, 'val_blob_checked') = 'ok:\000\377'
                 AND val_roundtrip('\x00ffee'::bytea, 'val_blob_checked') = '23514'
            THEN 'ok' ELSE 'DISAGREES' END AS verdict;

-- Bytes in through the read direction and back out through the write one. For
-- bytea the result must equal the input byte for byte; for anything else it is
-- out(in(bytes)), which is the only round trip a value through Valkey has.
WITH v(id, payload, typ, want, why) AS (VALUES
    ('R-b1', ''::bytea,               'bytea', 'ok:',
        'empty, which is a value and not NULL'),
    ('R-b2', '\x610062'::bytea,       'bytea', 'ok:a\000b',
        'an embedded NUL survives both directions'),
    ('R-b3', '\x00000000'::bytea,     'bytea', 'ok:\000\000\000\000',
        'and so does a value that is nothing else'),
    ('R-b4', '\xfffefd'::bytea,       'bytea', 'ok:\377\376\375',
        'invalid UTF-8 is never validated in either direction for bytea'),
    ('R-t1', 'hello'::bytea,          'text',  'ok:hello',
        'text is verbatim both ways'),
    ('R-t2', '\xfffefd'::bytea,       'text',  '22021',
        'the same bytes through a text column are refused inbound, not mangled'),
    ('R-t3', '\x610062'::bytea,       'text',  '22021',
        'a NUL cannot become text either'),
    ('R-i1', '007'::bytea,            'int4',  'ok:7',
        'out(in(x)) is not x for most types: K-18 is what that costs'),
    ('R-i2', ' 007'::bytea,           'int4',  'ok:7',
        'and leading space is absorbed, so the key written differs from the key read')
)
SELECT id, encode(payload, 'escape') AS payload, typ,
       val_roundtrip(payload, typ) AS got, want,
       CASE WHEN val_roundtrip(payload, typ) = want THEN 'ok' ELSE 'DISAGREES' END
           AS verdict,
       why
FROM v ORDER BY id;

-- ===========================================================================
-- B  NULL semantics
--
-- Only the key and score rules are reachable from the probes this file drives
-- directly. N-05 to N-09 (hash field NULL becomes an absent field on INSERT
-- and an HDEL on UPDATE, and all-fields-NULL is refused), N-10's INSERT shape,
-- N-12 and N-13 all need a statement to execute, so they belong to the dml
-- suite. They are named here so that their absence is a recorded gap rather
-- than an oversight.
--
-- What would be wrong if the two below did not hold: a null key names no row,
-- so accepting one would write to the key spelled by whatever the renderer
-- made of a zero Datum; and a missing score is a different mistake from a
-- malformed one, so the two must not share an error.
-- ===========================================================================
-- N-04 goes through val_slot rather than val_score, and that is the vector
-- being satisfied rather than weakened. valkey_fdw_test_score takes BYTES, and
-- no byte string means SQL NULL, so a NULL argument is the probe refusing its
-- own input (22004) and never reaches the per-role rule at all - which is why
-- this row read DISAGREES for as long as the rule had no caller. The rule
-- itself lives in vfdw_val_from_slot and answers 23502, as the vector said it
-- should. The probe's own refusal is printed beside it so the two stay
-- distinguishable.
SELECT val_key(NULL)                     AS "N-01 null key (want 23502)",
       val_key(NULL, 'p:')               AS "K-11 null before prefix (want 23502)",
       val_slot(NULL, 'text', 'score')   AS "N-04 null score (want 23502)",
       val_score(NULL)                   AS "the probe's own refusal (22004)",
       CASE WHEN val_key(NULL) = '23502' AND val_key(NULL, 'p:') = '23502'
                 AND val_slot(NULL, 'text', 'score') = '23502'
                 AND val_score(NULL) = '22004' THEN 'ok'
            ELSE 'DISAGREES' END AS verdict;

-- vfdw_val_render is the layer BELOW the per-role NULL rule and reports a null
-- VfdwValue rather than refusing it. That is correct at this layer and is why
-- N-02 and N-03 could not be asserted through it; they are asserted in A.3
-- below, through vfdw_val_from_slot, which is the layer that holds the rule.
SELECT encode(valkey_fdw_test_val_out(NULL::text), 'escape') IS NULL
           AS "render alone reports null rather than refusing it";

-- ===========================================================================
-- A.3  Dispatch through vfdw_val_from_slot
--
-- Everything above this point reaches vfdw_val_render or vfdw_val_build_key,
-- which are the layers below the entry point a statement calls. So the kind
-- dispatch, the per-role NULL rules, the score routing and the unimplemented-
-- kind refusal had no caller anywhere: deleting the vfdw_val_check_score call
-- from vfdw_val_score_column left all 55 score vectors byte-identical, and
-- 'NaN'::numeric would have reached ZADD with a green suite.
--
-- These go through vfdw_val_from_slot over a VfdwValCtx built by the
-- production vfdw_val_ctx_init. valkey_fdw_test_build_key now routes through
-- it too, which re-points the whole K table below, and N-01/K-11 with it.
-- ===========================================================================
WITH v(id, payload, typ, kind, want, why) AS (VALUES
    ('D-01', 'hello'::bytea, 'text',  'value',  'ok:hello',
        'the value arm renders and retains'),
    ('D-02', NULL,           'text',  'value',  '23502',
        'N-02: a null value IS the string row''s absence, so it is refused'),
    ('D-03', 'red',          'text',  'member', 'ok:red',
        'the member arm'),
    ('D-04', NULL,           'text',  'member', '23502',
        'N-03: and a null member likewise'),
    ('D-05', 'alpha',        'text',  'field',  'ok:alpha',
        'a hash field renders like any other value'),
    ('D-06', NULL,           'text',  'field',  'null',
        'but a null FIELD is not an error: an absent field, then an HDEL'),
    ('D-07', '1.5',          'text',  'score',  'ok:1.5',
        'the score arm routes through the validator and passes'),
    ('D-08', 'nan',          'text',  'score',  '22023',
        'and refuses: drop the check_score call and this reads ok:nan'),
    ('D-09', 'Infinity',     'text',  'score',  '22023',
        'including the spelling float8out produces for +inf'),
    ('D-10', NULL,           'text',  'score',  '23502',
        'N-04 reached through the dispatch rather than through the probe'),
    ('D-11', '\x312e3500',   'bytea', 'score',  '22023',
        'a bytea score column feeds the same validator: the trailing NUL is a byte'),
    ('D-12', 'str:a',        'text',  'key',    'ok:str:a',
        'the key arm reaches the builder'),
    ('D-13', 'x',            'text',  'ttl',           '22023',
        'a ttl column of any other type is refused BEFORE the conversion reads'),
    ('D-13b', '10 minutes',  'interval', 'ttl',        'ok:600000',
        'the interval arrives as the milliseconds HPEXPIRE takes'),
    ('D-13c', '1 day',       'interval', 'ttl',        'ok:86400000',
        'a day is 24 hours, the convention EXTRACT(EPOCH FROM ...) uses'),
    ('D-13d', '0',           'interval', 'ttl',        '22023',
        'and a non-positive duration is refused: it would delete the field'),
    ('D-14', 'x',            'text',  'distance',      'XX000',
        'an unimplemented kind is loud; a silent default is how the next kind'),
    ('D-15', 'x',            'text',  'legacy_value',  'XX000',
        'added to the enum acquires accidental semantics, so all three'),
    ('D-16', 'x',            'text',  'dropped',       'XX000', '')
)
SELECT id, coalesce(encode(payload, 'escape'), 'NULL') AS payload, typ, kind,
       val_slot(payload, typ, kind) AS got, want,
       CASE WHEN val_slot(payload, typ, kind) = want THEN 'ok'
            ELSE 'DISAGREES' END AS verdict,
       why
FROM v ORDER BY id;

-- ---------------------------------------------------------------------------
-- D-20: the retained bytes of a row that is no longer the current row.
--
-- vfdw_val_ctx_reset runs once per row in production. This renders A, resets,
-- renders B, and then reads A's bytes back. Both ways of getting
-- vfdw_val_retain wrong - returning its argument, or copying into the scratch
-- instead of into the write buffer - leave A pointing at memory B has already
-- reused, so both read back as B. Every other vector in this file renders one
-- value and stops, which is exactly the shape that cannot see it: replacing
-- the whole body of vfdw_val_retain with "return bytes;" changes nothing
-- anywhere else in the suite.
-- ---------------------------------------------------------------------------
SELECT encode(valkey_fdw_test_val_retain('AAAA'::bytea, 'BBBB'::bytea),
              'escape') AS "D-20 got",
       'AAAA' AS "D-20 want",
       CASE WHEN valkey_fdw_test_val_retain('AAAA'::bytea, 'BBBB'::bytea)
                 = 'AAAA'::bytea THEN 'ok' ELSE 'DISAGREES' END AS verdict;

-- ===========================================================================
-- B.1  What the refusals actually say
--
-- No assertion in this suite used to look at message text, so an invariant I2
-- regression was invisible: a Datum- or Valkey-originated string reaching
-- ereport as a FORMAT string rather than as an argument to "%s" would change
-- no recorded byte until the day it read off the end of the argument list.
-- The score refusal is where such a string is echoed, so it is where the
-- assertion belongs - '%s' in a score must come back as two characters.
-- ===========================================================================
SELECT val_msg($$ SELECT valkey_fdw_test_score('abc'::bytea, true) $$)
    AS "printable text is echoed";
SELECT val_msg($$ SELECT valkey_fdw_test_score('%s %n %99999$s'::bytea, true) $$)
    AS "format directives are data, not directives";
SELECT val_msg($$ SELECT valkey_fdw_test_score('\xff'::bytea, true) $$)
    AS "unprintable bytes are counted, never echoed";
SELECT val_msg($$ SELECT valkey_fdw_test_score('Infinity'::bytea, true) $$)
    AS "infinity is refused as infinite, not as not-a-number";
SELECT val_msg($$ SELECT valkey_fdw_test_build_key('a'::bytea, 'str:') $$)
    AS "a prefix violation names the prefix and nothing of the key";
SELECT val_msg($$ SELECT valkey_fdw_test_build_key(NULL::bytea) $$)
    AS "a null key names the rule that has no null form";
SELECT val_msg($$ SELECT valkey_fdw_test_all_fields_null() $$)
    AS "all mapped columns null";

-- ===========================================================================
-- B.2  A refusal that names bytes this wrapper formatted itself
--
-- Invariant I2 is about what reaches error-reporting code, not about who
-- produced it, and this is the half of it that has nothing to do with
-- libvalkey. A bytea key column carries arbitrary bytes BY DESIGN - that is
-- the documented reason it is allowed - so the duplicate-key refusal is a
-- message this wrapper hands a byte no encoding will accept. Two inserts of
-- the same '\xff' key in one transaction raise it with the key in the DETAIL,
-- and in a UTF-8 database that detail reaches the server log and the wire,
-- where a client_encoding differing from the server encoding makes the
-- conversion run DURING error reporting.
--
-- These are the only vectors in this file that need a reachable server, and
-- none of them sends a byte to it: the refusal comes from the ledger's fold at
-- the statement, long before the pre-commit flush, and BeginForeignModify
-- acquires a connection only to prove the server is there before it binds the
-- transaction's write unit. Every INSERT below is rolled back.
-- ===========================================================================
CREATE SERVER val_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER val_srv;
CREATE FOREIGN TABLE val_dup (k bytea OPTIONS (key 'true'), v text)
    SERVER val_srv OPTIONS (tabletype 'string');
CREATE FOREIGN TABLE val_dup_set (k bytea OPTIONS (key 'true'), m bytea)
    SERVER val_srv OPTIONS (tabletype 'set');

-- A key that IS valid in the database encoding is still named in full. The
-- check substitutes; it does not blanket-suppress, and a unique violation that
-- never says which key collided is the one every DBA has sworn at.
BEGIN;
INSERT INTO val_dup VALUES ('val:k'::bytea, 'one');
SELECT val_msg($$ INSERT INTO val_dup VALUES ('val:k'::bytea, 'two') $$)
    AS "a valid key is named in full";
ROLLBACK;

BEGIN;
INSERT INTO val_dup VALUES ('\xff'::bytea, 'one');
SELECT val_msg($$ INSERT INTO val_dup VALUES ('\xff'::bytea, 'two') $$)
    AS "an invalid key is described, never echoed";
ROLLBACK;

-- Both halves of a set row's identity are user bytes, and both are described
-- rather than echoed. A refusal that checked only the key would still put the
-- member's bytes on the wire.
BEGIN;
INSERT INTO val_dup_set VALUES ('\xff'::bytea, '\xfe'::bytea);
SELECT val_msg($$ INSERT INTO val_dup_set VALUES ('\xff'::bytea, '\xfe'::bytea) $$)
    AS "an invalid member and key are both described";
ROLLBACK;

DROP FOREIGN TABLE val_dup_set;
DROP FOREIGN TABLE val_dup;
DROP USER MAPPING FOR CURRENT_USER SERVER val_srv;
DROP SERVER val_srv;

-- ===========================================================================
-- C  The key builder
--
-- keyprefix filters; it never transforms. The key column carries the complete
-- key on write exactly as it does on read, because the read path keeps the raw
-- key the server returned and refines it against the prefix rather than
-- stripping it. What would be wrong if this did not hold: a write that
-- prepended the prefix would store 'str:str:a' for a table whose own scan
-- already records 'str:a', and a write that accepted an out-of-prefix key
-- would store a row permanently invisible to the table that wrote it, because
-- SCAN MATCH excludes it and the point-lookup refinement drops it.
-- ===========================================================================
WITH v(id, k, prefix, kset, singleton, has_col, want, want_why, why) AS (VALUES
    ('K-01', 'str:a'::bytea, 'str:', NULL,   NULL,   true,  'ok:str:a', 'ok',
        'in prefix: the complete key reaches Valkey unchanged'),
    ('K-02', 'a',            'str:', NULL,   NULL,   true,  '23514', 'prefix',
        'out of prefix: the row would be invisible to its own table'),
    ('K-03', 'str:',         'str:', NULL,   NULL,   true,  'ok:str:', 'ok',
        'exactly the prefix: SCAN MATCH str:* returns it, so a write must too'),
    ('K-04', 'str',          'str:', NULL,   NULL,   true,  '23514', 'prefix',
        'shorter than the prefix: this is why memcmp needs its length guard'),
    ('K-05', '',             'str:', NULL,   NULL,   true,  '23514', 'prefix',
        'refused for the prefix, not for being empty'),
    ('K-06', '',             NULL,   NULL,   NULL,   true,  'ok:', 'ok',
        'the empty string is a legal Valkey key: the builder is length-driven'),
    ('K-07', '\x610062',     NULL,   NULL,   NULL,   true,  'ok:a\000b', 'ok',
        'a NUL-bearing key passes through whole; strlen would write another key'),
    ('K-08', '\x610062',     'a',    NULL,   NULL,   true,  'ok:a\000b', 'ok',
        'the prefix test is (bytes, len): a NUL past the prefix ends nothing'),
    ('K-09', 'a*bc',         'a*b',  NULL,   NULL,   true,  'ok:a*bc', 'ok',
        'the prefix is a literal'),
    ('K-10', 'axbc',         'a*b',  NULL,   NULL,   true,  '23514', 'prefix',
        'and not a glob: the scan escapes the star, so it can never return axbc'),
    ('K-12', 'tags',         NULL,   NULL,   'tags', true,  'ok:tags', 'ok',
        'a singleton key column may restate the option'),
    ('K-13', 'other',        NULL,   NULL,   'tags', true,  '23514',
        'singleton_mismatch',
        'another name would discard the user''s value while reporting success'),
    ('K-14', NULL,           NULL,   NULL,   'tags', true,  'ok:tags', 'ok',
        'or be left unspecified: the only shape COPY tags_k (t) can produce'),
    ('N-11', NULL,           NULL,   NULL,   'tags', false, 'ok:tags', 'ok',
        'or not exist at all: the path that would otherwise index cols[-1]'),
    ('K-16', 'anything',     NULL,   'kset', NULL,   true,  'ok:anything', 'ok',
        'a keyset table has no prefix to satisfy'),
    ('K-17', 'kset',         NULL,   'kset', NULL,   true,  '23514',
        'keyset_collision',
        'but a row may not collide with the table''s own index'),
    ('K-20', 'kset2',        NULL,   'kset', NULL,   true,  'ok:kset2', 'ok',
        'the keyset is one exact name, not a namespace: length equality first'),
    ('K-21', 'kse',          NULL,   'kset', NULL,   true,  'ok:kse', 'ok',
        'and a shorter key is not a collision either'),
    ('K-22', '\x6b73657400', NULL,   'kset', NULL,   true,  'ok:kset\000', 'ok',
        'nor is the name with a NUL after it: five bytes, not four')
)
-- want_why exists because K-02, K-04, K-05, K-13 and K-17 all answer 23514.
-- Without it a builder that refused K-13 through the prefix branch, or K-17
-- through the singleton branch, or that collapsed all three checks into one,
-- would leave this table byte-identical. K-20 to K-22 are the length guard in
-- vfdw_val_is_keyset: turn its `keylen == slen` into a prefix test and all
-- three flip to 23514/keyset_collision.
SELECT id, coalesce(encode(k, 'escape'), 'NULL') AS key,
       coalesce(prefix, '-') AS prefix, coalesce(kset, '-') AS keyset,
       coalesce(singleton, '-') AS singleton, has_col,
       val_key(k, prefix, kset, singleton, has_col) AS got, want,
       val_key_why(k, prefix, kset, singleton, has_col) AS got_why, want_why,
       CASE WHEN val_key(k, prefix, kset, singleton, has_col) = want
                 AND val_key_why(k, prefix, kset, singleton, has_col) = want_why
            THEN 'ok' ELSE 'DISAGREES' END AS verdict,
       why
FROM v ORDER BY id;

-- K-15: a singleton key column is a projection of an option, so UPDATE cannot
-- rename through it. The rule is the same one K-13 asserts - the builder sees a
-- non-matching key and refuses it - and the UPDATE statement that reaches it is
-- the dml suite's.
--
-- K-18 (out(in(bytes)) != bytes makes a re-rendered key miss its own row) and
-- K-19 (a bytea key Const rendered through byteaout names the hex text rather
-- than the key) were read-path defects needing a live server. Both are fixed
-- and asserted in the scan suite instead: vfdw_key_column_pushable refuses a
-- key qual on a column whose rendering is not the key's own bytes, so the
-- clause falls back to the keyspace walk and the executor compares the datum
-- to the datum. R-i1 and R-i2 above are the half of K-18 that lives here -
-- they record what out(in(x)) costs for an int key column, which is exactly
-- why the pushdown now refuses one.

-- ===========================================================================
-- D  The per-argument length cap (VFDW_MAX_ARG_BYTES = 536870912)
--
-- L-01 to L-05 are NOT asserted, and this block says so rather than printing
-- something that would pass whatever the cap were set to.
--
-- The cap is enforced in vfdw_val_render_scratch, but its boundary sits at 512
-- MiB and no probe accepts a claimed length in place of a real datum, so
-- reaching it means constructing a 536870913-byte value: over 1.2 GB resident
-- and minutes of CPU, for one assertion, in every CI job. The two ways to make
-- it testable - a valkey_fdw_test_argcap(bigint) probe, or a debug-only
-- override of the constant - are both design decisions rather than test
-- decisions, so neither was taken here.
--
-- The one thing below that is an assertion: a 400 KB value passes, which
-- proves the check is not refusing everything. It does not prove the boundary
-- is at 512 MiB and must not be read as if it did.
-- ===========================================================================
SELECT length(valkey_fdw_test_val_out(repeat('ab', 200000)))
           AS "400 KB accepted (want 400000)",
       'L-01..L-05 unasserted: no probe reaches the 512 MiB boundary'
           AS "gap";

-- ===========================================================================
-- E  Score syntax
--
-- The validator runs on the RENDERED bytes and never on the column's
-- PostgreSQL type. What would be wrong if it did not: an isnan() test on a
-- float8 Datum passes 'NaN'::numeric and 'inf'::text straight through to a
-- ZADD in phase 2 of the flush, after phase 1 has passed and after earlier
-- actions have already run - which is the partially-applied outcome the whole
-- two-phase design exists to make unreachable.
--
-- The grammar is deliberately a proper SUBSET of what the server accepts. The
-- only place these vectors refuse something Valkey would have taken is the
-- +-infinity family, and that is policy, stated as such. There is no vector
-- where the wrapper accepts something the server rejects, and S-12 is the one
-- that keeps it that way.
-- ===========================================================================
WITH v(id, s, want, want_why, why) AS (VALUES
    ('S-01',  '0'::bytea,  'ok',    'ok',           'the smallest whole number'),
    ('S-02',  '1',         'ok',    'ok',           'int4 renders like this'),
    ('S-03',  '-1',        'ok',    'ok',           'and signed'),
    ('S-04',  '+1',        'ok',    'ok',           'a leading plus: the server takes it too'),
    ('S-05',  '007',       'ok',    'ok',           'leading zeros; the server reads 7'),
    ('S-07a', '0.5',       'ok',    'ok',           'fixed point'),
    ('S-07b', '.5',        'ok',    'ok',           'no integer part, as the server allows'),
    ('S-07c', '1.',        'ok',    'ok',           'no fractional digits, likewise'),
    ('S-07d', '-.5',       'ok',    'ok',           'both at once'),
    ('S-08a', '1e3',       'ok',    'ok',           'scientific'),
    ('S-08b', '1E3',       'ok',    'ok',           'either case'),
    ('S-08c', '1e+3',      'ok',    'ok',           'signed exponent'),
    ('S-08d', '1e-3',      'ok',    'ok',           'and negative'),
    ('S-09',  '1.5e300',   'ok',    'ok',           'large but finite'),
    ('S-10',  '1.7976931348623157e308', 'ok', 'ok',           'DBL_MAX exactly'),
    ('S-11',  '1.8e308',   '22023', 'overflow',     'overflow: ERANGE paired with HUGE_VAL'),
    ('S-13a', '1e-323',    'ok',    'ok',           'subnormal: ERANGE alone is not a refusal'),
    ('S-13b', '5e-324',    'ok',    'ok',           'the smallest subnormal, which Valkey stores'),
    ('S-16',  '1e1000000000000', '22023', 'overflow',     'an exponent no double can hold'),
    ('S-19a', 'inf',       '22023', 'not_finite',   'policy: the server would have accepted it'),
    ('S-19b', '+inf',      '22023', 'not_finite',   'same'),
    ('S-19c', '-inf',      '22023', 'not_finite',   'same'),
    ('S-19d', 'INF',       '22023', 'not_finite',   'case makes no difference to either side'),
    ('S-19e', 'infinity',  '22023', 'not_finite',   'declaring the column text is not a bypass'),
    ('S-22a', 'nan',       '22023', 'not_a_number', 'the server refuses this one too'),
    ('S-22b', 'NAN',       '22023', 'not_a_number', 'in any case'),
    ('S-22c', 'nan(x)',    '22023', 'not_a_number', 'including the payload spelling'),
    ('S-23',  '',          '22023', 'empty',        'empty is not NULL and not a number'),
    ('S-24',  ' 1',        '22023', 'not_a_number', 'leading whitespace, exactly as the server'),
    ('S-25a', '1 ',        '22023', 'trailing',     'trailing anything'),
    ('S-25b', '1\012',     '22023', 'trailing',     'including a newline'),
    ('S-25c', '\0111',     '22023', 'not_a_number', 'and a leading tab'),
    ('S-26',  '1abc',      '22023', 'trailing',     'strtod-and-ignore-the-rest would accept this'),
    ('S-27a', '0x1p3',     '22023', 'trailing',     'hex floats: refused on every server version'),
    ('S-27b', '0x10',      '22023', 'trailing',     'the concrete proof this is a subset'),
    ('S-28',  '1\0002',    '22023', 'trailing',     'a NUL walked as a byte, not as a terminator'),
    ('S-29',  '1\000',     '22023', 'trailing',     'a trailing NUL is a trailing byte'),
    ('S-30a', '1,5',       '22023', 'trailing',     'a decimal comma is not a decimal point'),
    ('S-30b', '--1',       '22023', 'not_a_number', 'one sign only'),
    ('S-30c', '1e',        '22023', 'trailing',     'an incomplete exponent is trailing garbage'),
    ('S-30d', 'e3',        '22023', 'not_a_number', 'and an exponent needs a mantissa'),
    ('S-30e', '.',         '22023', 'not_a_number', 'no digits at all'),
    ('S-30f', '+',         '22023', 'not_a_number', 'nor here'),
    ('S-30g', '-',         '22023', 'not_a_number', 'nor here'),
    ('S-30h', '1_000',     '22023', 'trailing',     'digit separators are not score syntax'),
    ('S-30i', 'abc',       '22023', 'not_a_number', 'plain text'),
    ('S-32',  '\x31',      'ok',    'ok',           'a bytea score column feeds the same validator')
)
-- want_why, not just want. The verdict column used to be printed with nothing
-- to compare it against, so a disagreement between the pre-written vector and
-- the classifier was RECORDED rather than failed - and every 22023 row looked
-- alike whether it was refused for being empty, for trailing bytes or for
-- overflow. The ruling now covers both, so a classifier that answered
-- 'not_a_number' for everything would fail here while leaving the SQLSTATEs
-- untouched.
SELECT id, encode(s, 'escape') AS score, val_score(s) AS got, want,
       val_score_why(s) AS got_why, want_why,
       CASE WHEN val_score(s) = want AND val_score_why(s) = want_why
            THEN 'ok' ELSE 'DISAGREES' END AS ruling,
       why
FROM v ORDER BY id;

-- The same validator reached through a real Datum's rendering, which is the
-- path a zset column actually takes. S-21 is the case that forces it: an
-- isnan() test on a float8 would never see 'NaN'::numeric at all, and S-14 and
-- S-15 arrive as 402 and 401 bytes of decimal digits rather than as anything
-- resembling an exponent.
WITH v(id, rendered, want, want_why, why) AS (VALUES
    ('S-06', valkey_fdw_test_val_out('-0.0'::float8), 'ok', 'ok',
        'signed zero is accepted; the server reads it back as 0 (U-03)'),
    ('S-12', valkey_fdw_test_val_out('1e-320'::float8), 'ok', 'ok',
        'subnormal: a bare-ERANGE rule would refuse what Valkey stores'),
    ('S-14', valkey_fdw_test_val_out('1e-400'::numeric), '22023', 'underflow',
        'underflow to exactly zero, through 402 valid-looking decimal bytes'),
    ('S-15', valkey_fdw_test_val_out('1e400'::numeric), '22023', 'overflow',
        'overflow, through 401; float8 never gets here because 22003 fires first'),
    ('S-17', valkey_fdw_test_val_out('inf'::float8), '22023', 'not_finite',
        'float8_out spells it Infinity; the message must not blame Valkey'),
    ('S-18', valkey_fdw_test_val_out('-inf'::float8), '22023', 'not_finite',
        'and -Infinity'),
    ('S-20', valkey_fdw_test_val_out('nan'::float8), '22023', 'not_a_number',
        'NaN, which the server refuses as well'),
    ('S-21', valkey_fdw_test_val_out('NaN'::numeric), '22023', 'not_a_number',
        'the single case that forces validation on bytes rather than on type')
)
SELECT id,
       CASE WHEN length(rendered) <= 32 THEN encode(rendered, 'escape')
            ELSE encode(substring(rendered from 1 for 6), 'escape')
                 || '...(' || length(rendered) || ' bytes)' END AS rendered,
       val_score(rendered) AS got, want,
       val_score_why(rendered) AS got_why, want_why,
       CASE WHEN val_score(rendered) = want AND val_score_why(rendered) = want_why
            THEN 'ok' ELSE 'DISAGREES' END AS ruling,
       why
FROM v ORDER BY id;

-- ---------------------------------------------------------------------------
-- S-33: +-Infinity gets a verdict of its own, and this is what it buys.
--
-- Valkey stores +inf scores natively, so the READ path returns them: 'ZADD z 1
-- +inf m' comes back as Infinity through a float8 score column and as 'inf'
-- through a text one, with no complaint. The write side refuses both - policy,
-- stated in src/vfdw_score.c and in README.md - so INSERT INTO z SELECT * FROM
-- z meets this refusal on a row the wrapper itself just produced. The one
-- thing that must not happen is calling it "not a number", because Infinity is
-- one, and NaN (which the SERVER refuses) is a different case entirely. Fold
-- not_finite back into not_a_number and every row below changes.
-- ---------------------------------------------------------------------------
WITH v(id, s, want_why) AS (VALUES
    ('S-33a', 'inf'::bytea,       'not_finite'),
    ('S-33b', 'Infinity',         'not_finite'),
    ('S-33c', '-Infinity',        'not_finite'),
    ('S-33d', '+INFINITY',        'not_finite'),
    ('S-33e', 'nan',              'not_a_number'),
    ('S-33f', 'NaN',              'not_a_number'),
    ('S-33g', 'infi',             'not_a_number'),
    ('S-33h', 'infinit',          'not_a_number'),
    ('S-33i', 'infinityy',        'not_a_number'),
    ('S-33j', '1inf',             'trailing')
)
SELECT id, encode(s, 'escape') AS score, val_score_why(s) AS got_why, want_why,
       CASE WHEN val_score_why(s) = want_why THEN 'ok' ELSE 'DISAGREES' END
           AS verdict
FROM v ORDER BY id;

-- Section F of the vectors - what a score becomes after Valkey has stored it
-- as a double - is deliberately absent. R-01 to R-08 are round trips through a
-- live server (1.10 comes back as 1.1, 9007199254740993 as ...92, -0 as 0), so
-- they are dml-suite assertions. Nothing this file can reach observes them -
-- it sends no command to a server - and asserting them here would only be
-- asserting PostgreSQL's own rendering twice.

-- ===========================================================================
-- G  Hash-tag extraction
--
-- Exactly what keyHashSlot does: the first '{', then the first '}' strictly
-- after it, and if either is missing or they are adjacent, the whole key.
--
-- Every vector below is a case where a plausible simplification gives a
-- different answer, and in cluster mode a different node: brace balancing
-- (T-15, T-17), searching for '}' from index 0 (T-14, which computes a
-- negative length), retrying after a degenerate pair (T-10, T-11), and
-- treating the key as a C string (T-24 to T-27).
-- ===========================================================================
WITH v(id, k, want, why) AS (VALUES
    ('T-01', '{a}b'::bytea,     'a'::bytea,        'tag at the start, suffix after'),
    ('T-02', '{a}',             'a',               'nothing after the pair'),
    ('T-03', '}{a}',            'a',               'a closer left of the opener is invisible'),
    ('T-04', '{a}{b}',          'a',               'the first well-formed pair wins'),
    ('T-05', 'a{b}c',           'b',               'the tag need not start at index 0'),
    ('T-06', '{}',              '{}',              'adjacent braces hash the WHOLE key'),
    ('T-07', '{}b',             '{}b',             'the documented {} escape hatch'),
    ('T-08', 'a{}',             'a{}',             'degenerate pair at the end'),
    ('T-09', 'a{}b',            'a{}b',            'position-independent'),
    ('T-10', '{}{tag}',         '{}{tag}',         'a degenerate pair does not fall through'),
    ('T-11', 'foo{}{bar}',      'foo{}{bar}',      'the same trap, as the spec spells it'),
    ('T-12', 'a{b',             'a{b',             'unterminated opener'),
    ('T-13', '{a',              '{a',              'and at the start'),
    ('T-14', 'a}b{c',           'a}b{c',           'closer before opener: no negative length'),
    ('T-15', '{a{b}c}',         'a{b',             'no balancing, no depth counter'),
    ('T-16', '{a{b}',           'a{b',             'the inner closer ends it'),
    ('T-17', '{{a}}',           '{a',              'a tag may itself contain a brace'),
    ('T-18', 'foo{{bar}}zap',   '{bar',            'the spec''s own worked example'),
    ('T-19', 'foo{bar}{zap}',   'bar',             'the second pair is never examined'),
    ('T-20', '{',               '{',               'closer search handed a zero length'),
    ('T-21', '}',               '}',               'no opener at all'),
    ('T-22', '',                '',                'total over the empty key'),
    ('T-23', '{ }',             ' ',               'one byte suffices; nothing is trimmed'),
    ('T-24', 'a\000b',          'a\000b',          'a NUL is an ordinary byte'),
    ('T-25', '{a\000b}c',       'a\000b',          'and legal inside a tag'),
    ('T-26', '{a}\000b',        'a',               'excluded because the tag ended, not because the key did'),
    ('T-27', '{\000}',          '\000',            'one NUL is content: the pair is NOT degenerate'),
    ('T-29', '{user1000}.following', 'user1000',   'the canonical co-location example'),
    ('T-30', 'foo{hash_tag}',   'hash_tag',        'and the documented one')
)
-- Bracketed for the same reason as A.1: T-23's answer is one space byte and
-- T-22's is nothing at all, and an unbracketed column shows both as blank -
-- which is precisely the wrong answer for T-23 (an empty tag would hash to
-- slot 0 instead of 9314).
SELECT id, '[' || encode(k, 'escape') || ']' AS key,
       '[' || encode(valkey_fdw_test_hashtag(k), 'escape') || ']' AS got,
       '[' || encode(want, 'escape') || ']' AS want,
       length(valkey_fdw_test_hashtag(k)) AS got_len,
       CASE WHEN valkey_fdw_test_hashtag(k) IS NOT DISTINCT FROM want THEN 'ok'
            ELSE 'DISAGREES' END AS verdict,
       why
FROM v ORDER BY id;

-- T-28: 500 bytes of tag inside a 1002-byte key, printed as lengths. There is
-- no scratch buffer and no length cap; if either end of the range were off by
-- one this is where it shows, because the neighbouring bytes differ.
SELECT length(valkey_fdw_test_hashtag(
           convert_to('{' || repeat('y', 500) || '}' || repeat('z', 500), 'UTF8')))
           AS "T-28 taglen (want 500)",
       CASE WHEN valkey_fdw_test_hashtag(
                convert_to('{' || repeat('y', 500) || '}' || repeat('z', 500), 'UTF8'))
                = convert_to(repeat('y', 500), 'UTF8')
            THEN 'ok' ELSE 'DISAGREES' END AS verdict;

-- ===========================================================================
-- H.1  CRC-16/XMODEM parameter pinning
--
-- crc16('123456789') = 0x31C3 is the published check value and pins the
-- polynomial, the zero init, the absence of input and output reflection and
-- the zero final xor - all four in one number, independently of any key.
--
-- X-02 is the one that matters second: its raw CRC is 65439, far above 16384,
-- so it is the vector that actually exercises the 14-bit mask. X-01 alone
-- passes with the mask omitted entirely, and X-09's raw CRC is already below
-- the mask, so neither of them can stand in for it.
-- ===========================================================================
WITH v(id, k, want_crc, want_slot, why) AS (VALUES
    ('X-01', '123456789'::bytea, 12739, 12739, 'the published XMODEM check value'),
    ('X-02', 'x',                65439, 16287, 'raw CRC above 16384: this exercises the mask'),
    ('X-03', 'a',                31879, 15495, ''),
    ('X-04', 'ab',               29951, 13567, ''),
    ('X-05', 'abc',              40406,  7638, ''),
    ('X-06', 'foo',              44950, 12182, ''),
    ('X-07', 'hello',            50018,   866, ''),
    ('X-08', 'k1',               61858, 12706, ''),
    ('X-09', 'world',             9059,  9059, 'raw below 16384: the mask is a no-op here'),
    ('X-10', '',                     0,     0, 'the CRC of zero bytes is the init value')
)
SELECT id, encode(k, 'escape') AS key,
       valkey_fdw_test_crc16(k) AS got_crc, want_crc,
       valkey_fdw_test_keyslot(k) AS got_slot, want_slot,
       CASE WHEN valkey_fdw_test_crc16(k) = want_crc
             AND valkey_fdw_test_keyslot(k) = want_slot THEN 'ok'
            ELSE 'DISAGREES' END AS verdict,
       why
FROM v ORDER BY id;

-- ---------------------------------------------------------------------------
-- H.2 to H.5  Slots.
--
-- Read back from a live 6-node Valkey 9.0.5 cluster with CLUSTER KEYSLOT and
-- independently reproduced from a from-scratch bitwise CRC. They are hardcoded
-- here rather than compared against the server because CLUSTER KEYSLOT does
-- not answer on the standalone topology this suite runs against - it replies
-- "ERR This instance has cluster support disabled" - so the live cross-check
-- lives in test/unit/run.sh, where it reports itself as NOT RUN with the
-- server's own reason rather than as a pass, and is a hard failure on the
-- cluster topology. These pin the arithmetic on every topology.
--
-- What would be wrong if they did not hold: in cluster mode a key would be
-- written to a slot other than the one the wrapper declared, which is a
-- MOVED-loop or a silently misplaced row rather than an error.
-- ---------------------------------------------------------------------------
WITH v(id, k, want, why) AS (VALUES
    ('X-20', 'somekey'::bytea,       11058, 'worked example from CLUSTER KEYSLOT'),
    ('X-21', 'foo{hash_tag}',         2515, 'and its hash-tag pair'),
    ('X-22', 'bar{hash_tag}',         2515, 'equal to X-21: the co-location property'),
    ('X-23', '{user1000}.following',  3443, 'the cluster-spec example'),
    ('X-24', '{user1000}.followers',  3443, 'equal to X-23'),
    ('X-25', 'user1000',              3443, 'and to the bare tag: the bytes, not merely a tag'),
    ('X-30', '{a}b',                 15495, 'equals slot(a)'),
    ('X-31', '{a}',                  15495, ''),
    ('X-32', '}{a}',                 15495, ''),
    ('X-33', '{a}{b}',               15495, 'NOT slot(b) = 3300'),
    ('X-34', 'a{b}c',                 3300, 'equals slot(b)'),
    ('X-35', '{}',                   15257, 'NOT 0: the whole key is hashed'),
    ('X-36', '{}b',                   6680, 'NOT slot(b)'),
    ('X-37', 'a{}',                   6082, ''),
    ('X-38', 'a{}b',                 13694, ''),
    ('X-39', '{}{tag}',              11440, 'NOT slot(tag) = 8338'),
    ('X-40', 'foo{}{bar}',            8363, 'NOT slot(bar) = 5061'),
    ('X-41', 'a{b',                  13340, ''),
    ('X-42', '{a',                   10276, ''),
    ('X-43', '{a{b}',                13340, 'equals slot(a{b)'),
    ('X-44', '{a{b}c}',              13340, 'pins the nested case to the exact byte range'),
    ('X-45', '{{a}}',                10276, 'equals slot({a)'),
    ('X-46', 'a}b{c',                13587, 'neither slot(b) nor slot(c)'),
    ('X-47', '{',                     4092, ''),
    ('X-48', '}',                    12090, ''),
    ('X-49', '{bar',                  4015, ''),
    ('X-50', 'foo{{bar}}zap',         4015, 'equals slot({bar)'),
    ('X-51', 'foo{bar}{zap}',         5061, 'equals slot(bar)'),
    ('X-52', '{ }',                   9314, 'equals slot of one space'),
    ('X-53', '{vfdw}:k1',             1465, 'a keyprefix pins a slot: routing depends on this'),
    ('X-53b','{vfdw}:k2',             1465, 'and two keys under it co-locate'),
    ('X-53c','vfdw',                  1465, 'both equal to the bare tag'),
    ('X-60', '\000',                     0, 'CRC init 0 is a fixed point at 0x00'),
    ('X-61', '\000\000',                 0, 'and stays one for any run of NULs'),
    ('X-62', '{\000}',                   0, 'a valid one-byte tag that still lands on 0'),
    ('X-63', '',                         0, 'so slot 0 must never be the unbound sentinel'),
    ('X-64', 'a\000b',                8383, 'NOT slot(a) = 15495, which is what strlen gives'),
    ('X-65', '{a\000b}c',             8383, 'the same tag reached through braces'),
    ('X-66', '{a}\000b',             15495, 'equals slot(a) because the tag ended, not the key'),
    ('X-67', 'ab\000cd',             13676, 'NOT slot(ab) = 13567'),
    ('X-68', '\377\376',              3374, 'high-bit bytes: a signed char index breaks here'),
    ('X-69', 'caf\303\251',           5735, 'and on ordinary UTF-8'),
    ('X-70', '\303\251',             10180, ''),
    ('H5-a', 'str:a',                 7009, 'repo-flavoured keys, for the later dml suite'),
    ('H5-b', 'str:',                  9283, ''),
    ('H5-c', 'str',                   6928, ''),
    ('H5-d', 'p:',                    8000, ''),
    ('H5-e', 'tags',                 14486, ''),
    ('H5-f', 'a*bc',                  8172, ''),
    ('H5-g', 'axbc',                 12098, ''),
    ('H5-h', 'a*b',                   2450, ''),
    ('H5-i', 'tag',                   8338, ''),
    ('H5-j', 'bar',                   5061, ''),
    ('H5-k', 'b',                     3300, ''),
    ('H5-l', 'c',                     7365, ''),
    ('H5-m', 'y',                    12222, ''),
    ('H5-n', 'z',                     8157, ''),
    ('H5-o', ' ',                     9314, '')
)
SELECT id, encode(k, 'escape') AS key,
       valkey_fdw_test_keyslot(k) AS got, want,
       CASE WHEN valkey_fdw_test_keyslot(k) = want THEN 'ok' ELSE 'DISAGREES' END
           AS verdict,
       why
FROM v ORDER BY id;

-- X-71 and X-72: long inputs, printed on their own so the key does not swamp
-- the output. X-72 is the exact-range assertion over a long key - its slot must
-- equal the slot of the 500 y bytes alone (4239) and not that of the whole
-- 1002-byte string.
SELECT valkey_fdw_test_keyslot(convert_to(repeat('x', 1000), 'UTF8'))
           AS "X-71 (want 3195)",
       valkey_fdw_test_keyslot(convert_to(
           '{' || repeat('y', 500) || '}' || repeat('z', 500), 'UTF8'))
           AS "X-72 (want 4239)",
       valkey_fdw_test_keyslot(convert_to(repeat('y', 500), 'UTF8'))
           AS "X-72 bare tag (want 4239)",
       CASE WHEN valkey_fdw_test_keyslot(convert_to(repeat('x', 1000), 'UTF8')) = 3195
                 AND valkey_fdw_test_keyslot(convert_to(
                     '{' || repeat('y', 500) || '}' || repeat('z', 500), 'UTF8')) = 4239
                 AND valkey_fdw_test_keyslot(convert_to(repeat('y', 500), 'UTF8')) = 4239
            THEN 'ok' ELSE 'DISAGREES' END AS verdict;

-- ---------------------------------------------------------------------------
DROP TABLE val_heap;
DROP DOMAIN val_blob_checked;
DROP DOMAIN val_blob;
DROP DOMAIN val_dom;
DROP TYPE val_color;
DROP FUNCTION val_key(bytea, text, text, text, boolean);
DROP FUNCTION val_key_why(bytea, text, text, text, boolean);
DROP FUNCTION val_slot(bytea, text, text, text, text, text);
DROP FUNCTION val_msg(text);
DROP FUNCTION val_score(bytea);
DROP FUNCTION val_score_why(bytea);
DROP FUNCTION val_roundtrip(bytea, text);
DROP FUNCTION val_raises(text);
