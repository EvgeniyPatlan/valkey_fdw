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

-- ---------------------------------------------------------------------------
-- Writing an expiry.
-- ---------------------------------------------------------------------------
-- ONE ROW SETTING BOTH a field and its lifetime, which is the case the action
-- order exists for: HPEXPIRE on a field that is not there yet answers -2 and
-- sets nothing, so an expiry applied before the HSET that creates its field
-- would be lost exactly here and nowhere else.
INSERT INTO tt VALUES ('tt:new', 'av', 'bv', interval '10 minutes', NULL);

SELECT key, a, b,
       a_ttl > interval '9 minutes' AS a_in_window,
       b_ttl IS NULL AS b_unexpiring
FROM tt WHERE key = 'tt:new';

-- NULL PERSISTS, and does not expire the field now. The field must still be
-- there afterwards - that is the whole distinction, and a wrapper that read
-- NULL as "expire immediately" would pass an assertion that only checked the
-- expiry had gone.
UPDATE tt SET a_ttl = NULL WHERE key = 'tt:new';

SELECT key, a, a_ttl IS NULL AS expiry_removed
FROM tt WHERE key = 'tt:new';

-- Months and days convert with PostgreSQL's own 30-day, 24-hour convention,
-- the arithmetic EXTRACT(EPOCH FROM ...) uses.
UPDATE tt SET a_ttl = interval '1 day' WHERE key = 'tt:new';

SELECT key,
       a_ttl > interval '23 hours' AS at_least_a_day,
       a_ttl <= interval '24 hours' AS at_most_a_day
FROM tt WHERE key = 'tt:new';

-- A DURATION IN THE PAST IS REFUSED rather than applied. HPEXPIRE with a
-- deadline already gone deletes the field, so this would remove data as a side
-- effect of a statement that reads like it sets a property.
UPDATE tt SET a_ttl = interval '0' WHERE key = 'tt:new';
UPDATE tt SET a_ttl = interval '-5 minutes' WHERE key = 'tt:new';

-- The refusal left the row alone, which is the half of it worth asserting: a
-- refusal that had already applied part of the statement would be worse than
-- no refusal.
SELECT key, a, a_ttl > interval '23 hours' AS still_a_day
FROM tt WHERE key = 'tt:new';

-- ROLLBACK sends nothing, expiries included.
BEGIN;
UPDATE tt SET a_ttl = interval '5 minutes' WHERE key = 'tt:new';
ROLLBACK;

SELECT key, a_ttl > interval '23 hours' AS unchanged_by_rollback
FROM tt WHERE key = 'tt:new';

-- Setting a field's value does not disturb its lifetime: the statement said
-- nothing about the expiry, so nothing about the expiry is sent.
UPDATE tt SET b = 'bv2' WHERE key = 'tt:new';

SELECT key, b, a_ttl > interval '23 hours' AS ttl_survived_a_field_write
FROM tt WHERE key = 'tt:new';

-- information_schema must agree with what the table now accepts. It reported
-- NO while every write raised; it has to report YES now that they do not.
SELECT c.relname, ist.is_insertable_into
FROM information_schema.tables ist
JOIN pg_class c ON c.relname = ist.table_name
WHERE c.relname = 'tt' AND ist.table_schema = 'public';

DELETE FROM tt WHERE key = 'tt:new';
SELECT count(*) AS rows_after_delete FROM tt WHERE key = 'tt:new';

SELECT num AS keys_removed
FROM valkey_fdw_test_probe('tt_srv', 0, 'DEL', 'tt:k', 'tt:plain', 'tt:new');

DROP SERVER tt_srv CASCADE;
