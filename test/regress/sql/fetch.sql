-- ---------------------------------------------------------------------------
-- Which command reads a hash, and how much it transfers.
--
-- A table mapping two fields of a hundred-field hash used to issue HGETALL and
-- receive all hundred. The saving is not a latency one - HGETALL and HMGET are
-- both a single round trip - it is that HGETALL's cost grows with the KEY's
-- field count, which nothing in the table definition bounds, while HMGET's
-- grows with the TABLE's, which it fixes.
--
-- ASSERTED ON WHAT WAS SENT, not on how long it took. The server counts calls
-- per command, so CONFIG RESETSTAT before a query and INFO commandstats after
-- says exactly which verb ran; a wall-clock assertion would be a measurement
-- of this machine.
-- ---------------------------------------------------------------------------
CREATE SERVER fx_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER fx_srv;

-- One key, two fields this table maps and a hundred it does not.
SELECT num AS fields_set
FROM valkey_fdw_test_probe('fx_srv', 0, VARIADIC
    (ARRAY['HSET', 'fx:1', 'title', 'Alpha', 'body', 'Beta']
     || (SELECT array_agg(x) FROM (
            SELECT unnest(ARRAY['pad' || i, i::text]) AS x
            FROM generate_series(1, 100) i) s))::bytea[]);

CREATE FOREIGN TABLE fx (
    k     text OPTIONS (key 'true'),
    title text OPTIONS (field 'title'),
    body  text OPTIONS (field 'body')
) SERVER fx_srv OPTIONS (tabletype 'hash', keyprefix 'fx:');

-- The same keyspace read as a packed collection, which needs the whole hash
-- and so must still ask for it. Kept beside the other table because "HMGET is
-- used" means little without a case in the same suite where it must not be.
CREATE FOREIGN TABLE fx_packed (k text, v text[]) SERVER fx_srv
    OPTIONS (tabletype 'hash', keyprefix 'fx:', legacy_value 'true');

-- A helper reading one command's call count out of INFO commandstats. Returns
-- 0 rather than NULL for a command that was never called, because the
-- assertions below compare counts and a NULL would make every comparison NULL.
CREATE FUNCTION fx_calls(cmd text) RETURNS int LANGUAGE sql AS $$
    SELECT coalesce((
        SELECT (regexp_match(convert_from(val_part, 'SQL_ASCII'),
                             'cmdstat_' || cmd || ':calls=(\d+)'))[1]::int
        FROM valkey_fdw_test_probe('fx_srv', 0, 'INFO', 'commandstats')), 0);
$$;

-- ---------------------------------------------------------------------------
-- The mapped table asks for its two fields and nothing else.
-- ---------------------------------------------------------------------------
SELECT reply_type AS stats_reset
FROM valkey_fdw_test_probe('fx_srv', 0, 'CONFIG', 'RESETSTAT');

SELECT k, title, body FROM fx ORDER BY k;

SELECT fx_calls('hmget')   > 0 AS asked_for_named_fields,
       fx_calls('hgetall') = 0 AS did_not_ask_for_all;

-- ---------------------------------------------------------------------------
-- The packed table still asks for all of it, because that is what it returns.
-- ---------------------------------------------------------------------------
SELECT reply_type AS stats_reset
FROM valkey_fdw_test_probe('fx_srv', 0, 'CONFIG', 'RESETSTAT');

SELECT k, array_length(v, 1) AS elements FROM fx_packed ORDER BY k;

SELECT fx_calls('hgetall') > 0 AS asked_for_all,
       fx_calls('hmget')   = 0 AS did_not_ask_for_named;

-- ---------------------------------------------------------------------------
-- THE BOUNDARY THAT HMGET CANNOT SEE ON ITS OWN.
--
-- HMGET answers one entry per field asked for and never an empty array, so a
-- key that is gone and a key holding none of these fields both answer all-nil.
-- Those are different answers: the first is a row that must not appear, the
-- second is a row that must. HLEN is asked alongside, which is what separates
-- them - and it is asked for every key, so this pair is the assertion that it
-- is being read rather than merely sent.
-- ---------------------------------------------------------------------------
SELECT num AS other_key
FROM valkey_fdw_test_probe('fx_srv', 0, 'HSET', 'fx:2', 'unmapped', 'x');

-- fx:2 exists and holds no field this table maps: a row, with both NULL.
SELECT k, title IS NULL AS no_title, body IS NULL AS no_body
FROM fx ORDER BY k;

-- A key that does not exist is not a row, however it is reached.
SELECT count(*) AS rows_for_a_missing_key FROM fx WHERE k = 'fx:nope';

-- And the count is the two keys that exist, not one and not three.
SELECT count(*) AS rows FROM fx;

-- ---------------------------------------------------------------------------
-- A POINT LOOKUP ON A KEYSET TABLE.
--
-- `WHERE key = 'x'` used to walk the whole set. The reasoning was that only
-- the server can say whether a named key belongs to the keyset, which is true
-- - and the server says it with SISMEMBER, in constant time. So the key list
-- is built like any other table's and one small reply per named key settles
-- membership, instead of an SSCAN whose cost is the set's size.
-- ---------------------------------------------------------------------------
SELECT num AS members_added
FROM valkey_fdw_test_probe('fx_srv', 0, 'SADD', 'fx:set', 'fx:m1', 'fx:m2');

SELECT num AS member_seeded
FROM valkey_fdw_test_probe('fx_srv', 0, 'HSET', 'fx:m1', 'title', 'InSet');

-- A key that EXISTS in the keyspace and is NOT in the set. It is the case the
-- whole check is for: the value fetch would answer perfectly well, and the row
-- must still not appear.
SELECT num AS stranger_seeded
FROM valkey_fdw_test_probe('fx_srv', 0, 'HSET', 'fx:stranger', 'title', 'Outside');

CREATE FOREIGN TABLE fx_ks (
    k     text OPTIONS (key 'true'),
    title text OPTIONS (field 'title')
) SERVER fx_srv OPTIONS (tabletype 'hash', keyset 'fx:set');

SELECT reply_type AS stats_reset
FROM valkey_fdw_test_probe('fx_srv', 0, 'CONFIG', 'RESETSTAT');

-- The named key is in the set, so it is a row.
SELECT k, title FROM fx_ks WHERE k = 'fx:m1';

-- Asked, and asked WITHOUT walking: sscan is the command whose cost is the
-- set's size, and a point lookup must not issue one.
SELECT fx_calls('sismember') > 0 AS asked_membership,
       fx_calls('sscan')     = 0 AS did_not_walk_the_set;

-- The stranger exists under its own name and is not in the set, so it is not
-- a row - which is what the membership reply decides, since the value fetch
-- behind it answers normally.
SELECT count(*) AS rows_for_a_non_member FROM fx_ks WHERE k = 'fx:stranger';

-- A key in the set that holds nothing is not a row either: membership says
-- the key belongs to this table, not that it exists.
SELECT count(*) AS rows_for_an_absent_member FROM fx_ks WHERE k = 'fx:m2';

-- WITHOUT a key qual the set is still walked, because then there is nothing
-- to check membership OF - SSCAN produces the members itself.
SELECT reply_type AS stats_reset
FROM valkey_fdw_test_probe('fx_srv', 0, 'CONFIG', 'RESETSTAT');

SELECT count(*) AS rows_from_a_full_scan FROM fx_ks;

SELECT fx_calls('sscan')     > 0 AS walked_the_set,
       fx_calls('sismember') = 0 AS asked_no_membership;

-- ---------------------------------------------------------------------------
-- PLAN-TIME ROW ESTIMATES.
--
-- Every table planned at a placeholder until someone ran ANALYZE, and
-- autovacuum never analyzes a foreign table - so a three-member zset and a
-- two-million-member one produced the same plan forever. Two shapes can be
-- counted in one command and now are.
--
-- The estimate is read out of the plan rather than eyeballed from a cost:
-- EXPLAIN (COSTS OFF) hides row counts, and leaving costs on would put this
-- machine's cost constants into an expected file.
-- ---------------------------------------------------------------------------
CREATE FUNCTION fx_plan_rows(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
    plan jsonb;
BEGIN
    EXECUTE 'EXPLAIN (FORMAT json) ' || q INTO plan;
    RETURN (plan -> 0 -> 'Plan' ->> 'Plan Rows')::bigint;
END $$;

SELECT num AS zset_members
FROM valkey_fdw_test_probe('fx_srv', 0, 'ZADD', 'fx:z',
                           '1', 'a', '2', 'b', '3', 'c', '4', 'd');

-- A singleton_key table over that zset, one row per member. Seven would be
-- wrong, 1000 would be wrong, and the server was asked.
CREATE FOREIGN TABLE fx_one (
    m text OPTIONS (member 'true'),
    s text OPTIONS (score 'true')
) SERVER fx_srv OPTIONS (tabletype 'zset', singleton_key 'fx:z');

SELECT fx_plan_rows('SELECT * FROM fx_one') AS estimated_members;

-- SIZE IS NOT ROW COUNT. The same key read as a packed collection is ONE row,
-- and asking the server would have said four. This is the distinction that
-- makes an estimate wrong rather than absent, so it is asserted beside the
-- case it is easily confused with.
CREATE FOREIGN TABLE fx_one_packed (k text, v text[]) SERVER fx_srv
    OPTIONS (tabletype 'zset', singleton_key 'fx:z', legacy_value 'true');

SELECT fx_plan_rows('SELECT * FROM fx_one_packed') AS estimated_packed;

-- A keyset table is its set's size, because each key is one row here.
SELECT fx_plan_rows('SELECT * FROM fx_ks') AS estimated_keyset;

SELECT num AS keyset_grew
FROM valkey_fdw_test_probe('fx_srv', 0, 'SADD', 'fx:set', 'fx:m3', 'fx:m4', 'fx:m5');

-- And it MOVES with the set rather than being a constant that happened to
-- match: a fixed number would have passed the assertion above.
SELECT fx_plan_rows('SELECT * FROM fx_ks') AS estimated_after_growth;

-- A keyprefix table keeps the placeholder. Counting a keyspace means scanning
-- it, which is not a trade worth making at plan time - so ANALYZE stays the
-- answer there, and the README says so.
SELECT fx_plan_rows('SELECT * FROM fx') AS estimated_keyspace;

DROP FUNCTION fx_plan_rows(text);
DROP FUNCTION fx_calls(text);
SELECT num AS keys_removed
FROM valkey_fdw_test_probe('fx_srv', 0, 'DEL', 'fx:1', 'fx:2',
                           'fx:set', 'fx:m1', 'fx:stranger', 'fx:z');

DROP SERVER fx_srv CASCADE;
