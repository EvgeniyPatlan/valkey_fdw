-- ---------------------------------------------------------------------------
-- Per-field time to live, on a server that does not have it.
--
-- SCOPED TO VALKEY 8 AND EARLIER by suites_for_topology. The feature itself is
-- asserted in ttl.sql, which runs on 9 and later; this file asserts the only
-- thing that can be true here, which is that the wrapper says so plainly.
--
-- It exists because the alternative to a clear refusal is not an error - it is
-- a column of NULLs. HPTTL on a server without it is an unknown command, and a
-- read path that shrugged at that would report every field as having no
-- expiry, which is a plausible answer and a wrong one. That is the failure
-- this wrapper exists to avoid, so it is asserted rather than assumed.
-- ---------------------------------------------------------------------------
CREATE SERVER tta_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER tta_srv;

CREATE FOREIGN TABLE tta (
    key text        OPTIONS (key 'true'),
    a   text        OPTIONS (field 'a'),
    t   interval    OPTIONS (ttl 'true', field 'a')
) SERVER tta_srv OPTIONS (tabletype 'hash', keyprefix 'tta:');

SELECT num AS seeded
FROM valkey_fdw_test_probe('tta_srv', 0, 'HSET', 'tta:k', 'a', 'av');

-- REFUSED, AND WITH ROWS PRESENT. The key exists and its field has a value, so
-- a wrapper that answered here would answer with real-looking rows carrying a
-- NULL expiry - which is why the fixture is seeded before the refusal is
-- asserted rather than after.
SELECT * FROM tta;

-- The refusal is about the column and not the table, so the same keyspace read
-- without that column still works. A capability refusal that took the whole
-- table with it would be a bigger claim than the server made.
CREATE FOREIGN TABLE tta_novel (
    key text OPTIONS (key 'true'),
    a   text OPTIONS (field 'a')
) SERVER tta_srv OPTIONS (tabletype 'hash', keyprefix 'tta:');

SELECT key, a FROM tta_novel ORDER BY key;

-- Planning does not contact the server, so the refusal belongs to execution
-- and EXPLAIN is expected to succeed on either server. Asserted so that moving
-- the check earlier is a visible change rather than a silent one.
EXPLAIN (COSTS OFF) SELECT * FROM tta;

-- The write refusal is decided from the catalogue alone, so it reads the same
-- on every server and says the same thing here as it does on 9.
INSERT INTO tta VALUES ('tta:new', 'av', NULL);

SELECT num AS keys_removed
FROM valkey_fdw_test_probe('tta_srv', 0, 'DEL', 'tta:k');

DROP SERVER tta_srv CASCADE;
