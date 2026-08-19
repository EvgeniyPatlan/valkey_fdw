-- Reading a transaction's own uncommitted writes.
--
-- Writes are buffered until pre-commit, so without the overlay a transaction
-- would not merely fail to see its own INSERT - it would read the PRE-write
-- state and present it as current, which is worse than an error.

SELECT valkey_fdw_test_server('ov_srv');

CREATE FOREIGN TABLE ov (k text OPTIONS (key 'true'), v text)
    SERVER ov_srv OPTIONS (tabletype 'string', keyprefix 'ov:');
CREATE FOREIGN TABLE ov2 (k text OPTIONS (key 'true'), v text)
    SERVER ov_srv OPTIONS (tabletype 'string', keyprefix 'ow:');

-- Seeded through the probe: these are rows the SERVER already has, which is
-- what the overlay must be layered over.
SELECT valkey_fdw_test_poke('ov_srv', 'ov:s1', 'server1');
SELECT valkey_fdw_test_poke('ov_srv', 'ov:s2', 'server2');

-- ---------------------------------------------------------------------------
-- The three verdicts: inject, replace, hide.
-- ---------------------------------------------------------------------------
BEGIN;
INSERT INTO ov VALUES ('ov:new', 'inserted');
UPDATE ov SET v = 'replaced' WHERE k = 'ov:s1';
DELETE FROM ov WHERE k = 'ov:s2';
SELECT k, v FROM ov ORDER BY k;
ROLLBACK;

-- And none of it survived the rollback, so what the overlay showed was the
-- transaction's own view rather than a write that had escaped.
SELECT k, v FROM ov ORDER BY k;

-- ---------------------------------------------------------------------------
-- Visibility is PostgreSQL's own CommandId rule.
--
-- INSERT ... SELECT is ONE command, so its own output is invisible to its own
-- scan and the statement terminates. A wrapper that showed the write to the
-- reader that produced it would loop until the buffer cap fired - which is
-- the failure this rule exists to prevent, and it would look like a hang.
-- ---------------------------------------------------------------------------
BEGIN;
INSERT INTO ov SELECT 'ov:c' || k, v FROM ov;
SELECT count(*) AS rows_after_insert_select FROM ov;
-- Two commands: the second sees the first.
INSERT INTO ov VALUES ('ov:two', 'x');
SELECT count(*) AS rows_after_second_command FROM ov;
ROLLBACK;

-- ---------------------------------------------------------------------------
-- A key created and then deleted in one transaction is not injected.
--
-- The tail iterator walks INSERTs, so this is the case where injecting from
-- the operation log alone gives the wrong answer: the row must be checked
-- against the overlay's own verdict, not merely found in the log.
-- ---------------------------------------------------------------------------
BEGIN;
INSERT INTO ov VALUES ('ov:tmp', 'x');
SELECT count(*) AS visible_with_tmp FROM ov;
DELETE FROM ov WHERE k = 'ov:tmp';
SELECT count(*) AS visible_after_delete FROM ov;
SELECT k FROM ov ORDER BY k;
ROLLBACK;

-- An UPDATE of a row this transaction created shows the newest value, not the
-- first: the overlay walks its per-key list backwards.
BEGIN;
INSERT INTO ov VALUES ('ov:twice', 'first');
UPDATE ov SET v = 'second' WHERE k = 'ov:twice';
SELECT k, v FROM ov WHERE k = 'ov:twice';
ROLLBACK;

-- ---------------------------------------------------------------------------
-- The overlay is per relation.
--
-- Two tables on one server with different prefixes must not see each other's
-- buffered rows; their tupledescs need not even match, so a shared overlay
-- would store one table's tuple into the other's slot.
-- ---------------------------------------------------------------------------
BEGIN;
INSERT INTO ov  VALUES ('ov:a', 'in ov');
INSERT INTO ov2 VALUES ('ow:a', 'in ov2');
SELECT 'ov' AS tbl, count(*) FROM ov
UNION ALL
SELECT 'ov2', count(*) FROM ov2
ORDER BY 1;
ROLLBACK;

-- ---------------------------------------------------------------------------
-- A rescan replays the injected rows rather than skipping them.
--
-- The seen-set and the tail iterator both have to reset, or the second pass
-- of a nested loop silently drops every buffered row - the same shape,
-- arriving through the overlay instead of through the cursor.
-- ---------------------------------------------------------------------------
BEGIN;
INSERT INTO ov VALUES ('ov:r1', 'x'), ('ov:r2', 'y');
SET LOCAL enable_material = off;
SET LOCAL enable_memoize = off;
SET LOCAL enable_hashjoin = off;
SET LOCAL enable_mergejoin = off;
-- Each row of the outer side rescans the inner, so an injected row that were
-- emitted only once would make this count short.
SELECT count(*) AS nested_loop_pairs FROM ov a, ov b;
-- Stated separately so the count above is compared against something, rather
-- than merely recorded: a rescan that dropped injected rows would leave the
-- two disagreeing instead of both shrinking together.
SELECT (SELECT count(*) FROM ov) * (SELECT count(*) FROM ov) AS expected_square;
ROLLBACK;

-- ---------------------------------------------------------------------------
-- A savepoint rollback that discards buffered rows also discards their
-- visibility.
-- ---------------------------------------------------------------------------
BEGIN;
INSERT INTO ov VALUES ('ov:keep', 'k');
SAVEPOINT sp;
INSERT INTO ov VALUES ('ov:drop', 'd');
SELECT count(*) AS with_both FROM ov;
ROLLBACK TO SAVEPOINT sp;
SELECT count(*) AS after_savepoint_rollback FROM ov;
SELECT k FROM ov ORDER BY k;
ROLLBACK;

-- ---------------------------------------------------------------------------
-- A read-only transaction pays nothing, and the keyspace is where it started.
-- ---------------------------------------------------------------------------
SELECT k, v FROM ov ORDER BY k;

DROP FOREIGN TABLE ov, ov2;
DROP USER MAPPING FOR CURRENT_USER SERVER ov_srv;
DROP SERVER ov_srv;
