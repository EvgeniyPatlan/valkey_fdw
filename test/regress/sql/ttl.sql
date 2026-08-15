-- ---------------------------------------------------------------------------
-- Per-field time to live, on a server that has it.
--
-- SCOPED TO VALKEY 9 AND LATER by suites_for_topology, because that is where
-- HPTTL exists. The other half of this subject - what a server WITHOUT it is
-- told - is ttl_absent.sql, and between them every supported server is
-- asserted about. Neither suite is skipped anywhere.
--
-- What is deliberately NOT asserted here is an exact remaining duration. The
-- server counts down in real time, so any expected file naming a number would
-- be a clock race; what is asserted is the shape of the answer - that a field
-- with an expiry reports one inside the window it was given, that a field
-- without one reports NULL, and that those are different.
-- ---------------------------------------------------------------------------
CREATE SERVER tt_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER tt_srv;

CREATE FOREIGN TABLE tt (
    key text        OPTIONS (key 'true'),
    a   text        OPTIONS (field 'a'),
    b   text        OPTIONS (field 'b'),
    a_ttl interval  OPTIONS (ttl 'true', field 'a'),
    b_ttl interval  OPTIONS (ttl 'true', field 'b')
) SERVER tt_srv OPTIONS (tabletype 'hash', keyprefix 'tt:');

SELECT num AS seeded
FROM valkey_fdw_test_probe('tt_srv', 0, 'HSET', 'tt:k', 'a', 'av', 'b', 'bv');

-- One field is given an expiry and the other is not, in the same key. That
-- pairing is the assertion: a table whose ttl columns all reported the same
-- thing would pass a test that read one number and copied it to every column.
SELECT num AS expired_a
FROM valkey_fdw_test_probe('tt_srv', 0, 'HPEXPIRE', 'tt:k', '600000',
                           'FIELDS', '1', 'a');

-- a has one, b does not, and the two columns say so independently.
SELECT key, a, b,
       a_ttl IS NOT NULL AS a_has_ttl,
       b_ttl IS NOT NULL AS b_has_ttl
FROM tt ORDER BY key;

-- Inside the window it was given, and not merely non-NULL. A read that took
-- the server's number as seconds, or as microseconds, would still be NOT NULL
-- here; only a bound catches the unit.
SELECT key,
       a_ttl > interval '9 minutes'  AS not_too_small,
       a_ttl <= interval '10 minutes' AS not_too_large
FROM tt ORDER BY key;

-- THE COLUMNS ARE NOT INTERCHANGEABLE. Expire b as well, to a plainly
-- different duration, and each column must report its own field: a reply read
-- positionally but assembled in another order would pass every assertion above
-- and fail this one.
SELECT num AS expired_b
FROM valkey_fdw_test_probe('tt_srv', 0, 'HPEXPIRE', 'tt:k', '60000',
                           'FIELDS', '1', 'b');

SELECT key, a_ttl > b_ttl AS a_outlives_b FROM tt ORDER BY key;

-- A key whose fields have no expiry at all, so the NULL above is not an
-- artefact of this one key.
SELECT num AS seeded_plain
FROM valkey_fdw_test_probe('tt_srv', 0, 'HSET', 'tt:plain', 'a', 'av');

-- NULLNESS, never the durations themselves. They count down in real time, so
-- an expected file naming one would pass once and fail on the next run.
SELECT key, a,
       a_ttl IS NULL AS a_ttl_null,
       b_ttl IS NULL AS b_ttl_null
FROM tt ORDER BY key;

-- A field that is not there reports NULL rather than an error: absent and
-- unexpiring are both absences, and the row's other columns already say so.
SELECT key, b IS NULL AS b_missing, b_ttl IS NULL AS b_ttl_missing
FROM tt WHERE key = 'tt:plain';

-- ---------------------------------------------------------------------------
-- Shapes refused, which do not depend on the server.
-- ---------------------------------------------------------------------------
-- A ttl column reads one field's expiry, so it must name the field.
CREATE FOREIGN TABLE tt_nofield (key text, t interval OPTIONS (ttl 'true'))
    SERVER tt_srv OPTIONS (tabletype 'hash', keyprefix 'tt:');
SELECT * FROM tt_nofield;

-- Only a hash has fields.
CREATE FOREIGN TABLE tt_list (
    key text,
    m   text OPTIONS (member 'true'),
    t   interval OPTIONS (ttl 'true', field 'a')
) SERVER tt_srv OPTIONS (tabletype 'list', keyprefix 'tt:');
SELECT * FROM tt_list;

-- A duration is an interval. Taken as text this would return the server's
-- millisecond count, which is a number the user did not ask for in a unit
-- nothing declared.
CREATE FOREIGN TABLE tt_text (
    key text,
    a   text OPTIONS (field 'a'),
    t   text OPTIONS (ttl 'true', field 'a')
) SERVER tt_srv OPTIONS (tabletype 'hash', keyprefix 'tt:');
SELECT * FROM tt_text;

-- Writes are refused whole, and the reason names the expiry rather than the
-- table type: reading a ttl works, setting one is what has not landed.
INSERT INTO tt VALUES ('tt:new', 'av', 'bv', NULL, NULL);
UPDATE tt SET a = 'x' WHERE key = 'tt:k';
DELETE FROM tt WHERE key = 'tt:k';

-- information_schema must agree with those refusals rather than contradict
-- them; a table that raises on every write and advertises itself as writable
-- is the disagreement this pair exists to catch.
SELECT c.relname, ist.is_insertable_into
FROM information_schema.tables ist
JOIN pg_class c ON c.relname = ist.table_name
WHERE c.relname = 'tt' AND ist.table_schema = 'public';

SELECT num AS keys_removed
FROM valkey_fdw_test_probe('tt_srv', 0, 'DEL', 'tt:k', 'tt:plain');

DROP SERVER tt_srv CASCADE;
