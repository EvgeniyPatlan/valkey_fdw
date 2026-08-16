-- ---------------------------------------------------------------------------
-- A list member's position.
--
-- LRANGE order is real - it is the one property that distinguishes a list from
-- a set - and until a column recorded it, nothing in an emitted tuple did. A
-- SQL reader had no way to restore the order the list is IN, because rows come
-- back in whatever order the scan produced them and ORDER BY had nothing to
-- name.
--
-- scan_count is set to 2 so the keyspace below spans several SCAN pages. That
-- is the part worth testing: the position counter is per key and reset per
-- key, and the way it breaks is by surviving a page refill and numbering the
-- second page's members from where the first left off.
-- ---------------------------------------------------------------------------
CREATE SERVER ps_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379', scan_count '2');
CREATE USER MAPPING FOR CURRENT_USER SERVER ps_srv;

CREATE FOREIGN TABLE ps (
    key text     OPTIONS (key 'true'),
    m   text     OPTIONS (member 'true'),
    pos int      OPTIONS (position 'true')
) SERVER ps_srv OPTIONS (tabletype 'list', keyprefix 'ps:');

-- Members deliberately NOT in alphabetical order, so that ORDER BY pos and
-- ORDER BY m are different answers. Sorted input would let a wrapper that
-- ignored position entirely produce the right-looking result.
SELECT num AS pushed_a
FROM valkey_fdw_test_probe('ps_srv', 0, 'RPUSH', 'ps:a', 'delta', 'alpha', 'charlie');

SELECT num AS pushed_b
FROM valkey_fdw_test_probe('ps_srv', 0, 'RPUSH', 'ps:b', 'zulu', 'yankee');

-- ZERO-BASED, matching LINDEX, LSET and LRANGE. A column numbered from 1 would
-- make `WHERE pos = 1` mean the second member to the server and the first to
-- whoever wrote the query.
SELECT key, pos, m FROM ps WHERE key = 'ps:a' ORDER BY pos;

-- THE ORDER IS RESTORED, and it is not the order the members sort in.
SELECT string_agg(m, ',' ORDER BY pos) AS by_position,
       string_agg(m, ',' ORDER BY m)   AS by_value
FROM ps WHERE key = 'ps:a';

-- EACH KEY STARTS AGAIN AT ZERO. A counter that ran on across keys would give
-- the second list positions 3 and 4, which is the mistake this pairing exists
-- to catch - and it would still pass an ORDER BY pos assertion made about one
-- key alone.
SELECT key, min(pos) AS first, max(pos) AS last, count(*) AS members
FROM ps GROUP BY key ORDER BY key;

-- Across a page boundary. scan_count is 2 and there are six keys, so the scan
-- refills several times; every key's members must still be numbered from zero
-- within that key.
SELECT num AS pushed_c FROM valkey_fdw_test_probe('ps_srv', 0, 'RPUSH', 'ps:c', 'c0', 'c1');
SELECT num AS pushed_d FROM valkey_fdw_test_probe('ps_srv', 0, 'RPUSH', 'ps:d', 'd0', 'd1');
SELECT num AS pushed_e FROM valkey_fdw_test_probe('ps_srv', 0, 'RPUSH', 'ps:e', 'e0', 'e1');
SELECT num AS pushed_f FROM valkey_fdw_test_probe('ps_srv', 0, 'RPUSH', 'ps:f', 'f0', 'f1');

SELECT count(*) AS keys_numbered_from_zero
FROM (SELECT key, min(pos) AS lo FROM ps GROUP BY key) g
WHERE g.lo = 0;

-- The member at a position is the member the server has there, asserted
-- against the server rather than against this table: agreeing with itself is
-- what a wrong answer does too.
SELECT (SELECT m FROM ps WHERE key = 'ps:a' AND pos = 1) AS from_table,
       (SELECT encode(val_part, 'escape')
        FROM valkey_fdw_test_probe('ps_srv', 0, 'LINDEX', 'ps:a', '1'))
           AS from_server;

-- ---------------------------------------------------------------------------
-- Shapes refused.
-- ---------------------------------------------------------------------------
-- Only a list has positions. A set has no order at all, and a zset's order is
-- its scores, which a score column already reports.
CREATE FOREIGN TABLE ps_set (
    key text OPTIONS (key 'true'),
    m   text OPTIONS (member 'true'),
    pos int  OPTIONS (position 'true')
) SERVER ps_srv OPTIONS (tabletype 'set', keyprefix 'ps:');
SELECT * FROM ps_set;

-- A position is a number, and taking it as text would hand back a decimal
-- string nothing declared the width of.
CREATE FOREIGN TABLE ps_text (
    key text  OPTIONS (key 'true'),
    m   text  OPTIONS (member 'true'),
    pos text  OPTIONS (position 'true')
) SERVER ps_srv OPTIONS (tabletype 'list', keyprefix 'ps:');
SELECT * FROM ps_text;

-- A column names one source.
CREATE FOREIGN TABLE ps_two (
    key text OPTIONS (key 'true'),
    m   text OPTIONS (member 'true', position 'true')
) SERVER ps_srv OPTIONS (tabletype 'list', keyprefix 'ps:');

-- ---------------------------------------------------------------------------
-- Writing.
-- ---------------------------------------------------------------------------
-- A NEW KEY, because an INSERT into a list creates the key: appending to one
-- that already exists is refused by the precondition that says so, and using
-- an existing key here would make every assertion below fire on that instead
-- of on what it claims to test.
--
-- Positions are assigned in insertion order, from zero. This is the write
-- direction's half of the read assertion above: the rows go in in an order,
-- and come back numbered by it.
INSERT INTO ps VALUES ('ps:new', 'n0', NULL), ('ps:new', 'n1', NULL);
SELECT key, pos, m FROM ps WHERE key = 'ps:new' ORDER BY pos;

-- NAMING A POSITION IS REFUSED. A position is where a member sits rather than
-- a property of it: another session's push or removal moves every position
-- after its own, so a write addressing a member by index is addressing it by a
-- name that can change underneath.
--
-- On a key that does not exist, deliberately. The same statement against an
-- existing key is refused first for being an insert over a live key, and would
-- have asserted that rule twice over instead of this one.
INSERT INTO ps VALUES ('ps:fresh', 'x', 0);

-- Reached only through INSERT, because UPDATE on a list table is refused
-- earlier and for a different reason - a list row has no identity that
-- survives its neighbours moving. Both messages are asserted so that neither
-- can quietly start standing in for the other.
UPDATE ps SET pos = 0 WHERE key = 'ps:new' AND m = 'n0';

-- The refusal did not take the rest of the write path with it: DELETE removes
-- by value, which needs no position at all, and the survivor renumbers.
DELETE FROM ps WHERE key = 'ps:new' AND m = 'n0';
SELECT key, pos, m FROM ps WHERE key = 'ps:new' ORDER BY pos;

SELECT num AS keys_removed
FROM valkey_fdw_test_probe('ps_srv', 0, 'DEL', 'ps:a', 'ps:b', 'ps:c', 'ps:d',
                           'ps:e', 'ps:f', 'ps:new', 'ps:fresh');

DROP SERVER ps_srv CASCADE;
