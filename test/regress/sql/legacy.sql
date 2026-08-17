-- The packed collection shape: one row per KEY, with the whole collection in
-- one array column.
--
-- WHY THIS SHAPE EXISTS, which is what the assertions below are protecting.
-- Every other shape names each hash field in a column option, so a keyspace of
-- per-tenant hashes whose field sets differ from key to key - or are simply
-- not known when the table is defined - has no expressible mapping at all.
-- This one hands the collection over whole and lets the query decide what it
-- is, which is why the populate_record case here is not a curiosity: it is the
-- reason the code exists, and a shape that stopped supporting it would still
-- pass a test that only counted rows.

CREATE SERVER lg_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER lg_srv;

-- Seeded through the probe rather than through this wrapper's write path: that
-- path refuses this shape, and a fixture built by the code under test proves
-- only that the reader agrees with the writer.
--
-- Deleted first so the seeds report what they created rather than what a
-- previous run left behind.
SELECT valkey_fdw_test_probe('lg_srv', 0, 'DEL', 'lg:h', 'lg:h2', 'lg:l',
                             'lg:s', 'lg:z', 'lgb:l', 'lg:ks', 'lg:k1') IS NOT NULL AS cleaned;
SELECT num AS seeded_hash
FROM valkey_fdw_test_probe('lg_srv', 0, 'HSET', 'lg:h', 'a', '1', 'b', '2');
-- A second hash with a DIFFERENT field set, which is the case no column
-- mapping can express: 'c' is named nowhere in any table definition here.
SELECT num AS seeded_hash2
FROM valkey_fdw_test_probe('lg_srv', 0, 'HSET', 'lg:h2', 'b', 'bee', 'c', '3');
SELECT num AS seeded_list
FROM valkey_fdw_test_probe('lg_srv', 0, 'RPUSH', 'lg:l', 'x', 'y', 'z');
SELECT num AS seeded_set
FROM valkey_fdw_test_probe('lg_srv', 0, 'SADD', 'lg:s', 'alpha', 'beta', 'gamma');
SELECT num AS seeded_zset
FROM valkey_fdw_test_probe('lg_srv', 0, 'ZADD', 'lg:z', '1', 'first',
                           '2', 'second');
-- One list member carrying an embedded NUL and a byte that is not valid UTF-8.
-- Under its own prefix, so the text[] tables above never touch it.
SELECT num AS seeded_bytes
FROM valkey_fdw_test_probe('lg_srv', 0, 'RPUSH', 'lgb:l',
                           '\x61006200ff'::bytea);

-- One table per collection type, all four the same two columns and no column
-- options at all. The key column is the first by position.
CREATE FOREIGN TABLE lg_hash (key text, value text[]) SERVER lg_srv
    OPTIONS (tabletype 'hash', keyprefix 'lg:', legacy_value 'true');
CREATE FOREIGN TABLE lg_list (key text, value text[]) SERVER lg_srv
    OPTIONS (tabletype 'list', keyprefix 'lg:', legacy_value 'true');
CREATE FOREIGN TABLE lg_set (key text, value text[]) SERVER lg_srv
    OPTIONS (tabletype 'set', keyprefix 'lg:', legacy_value 'true');
CREATE FOREIGN TABLE lg_zset (key text, value text[]) SERVER lg_srv
    OPTIONS (tabletype 'zset', keyprefix 'lg:', legacy_value 'true');

-- A column-mapped hash table over the same keys, for the comparisons that
-- follow: what the two shapes must agree about is a property of the keyspace,
-- and asserting it on the packed shape alone would not notice the two drifting
-- apart.
CREATE FOREIGN TABLE lg_cols (
    key text,
    a   text OPTIONS (field 'a')
) SERVER lg_srv OPTIONS (tabletype 'hash', keyprefix 'lg:');

-- ---------------------------------------------------------------------------
-- What the array holds, per table type.
--
-- A hash is its reply in the reply's own order: field, value, field, value.
-- The alternation is the whole point - it is what a pair-consuming function
-- reads - and it is the same order under both protocol versions.
--
-- A list and a set are their members.
--
-- A zset is its MEMBERS and not its scores. Scores are reached by mapping a
-- score column; packing them would make the array's shape depend on which
-- protocol the connection negotiated rather than on the data, because
-- WITHSCORES is answered by RESP2 with member and score alternating and by
-- RESP3 with a nested pair per member.
-- ---------------------------------------------------------------------------
SELECT key, value FROM lg_hash ORDER BY key;
SELECT key, value FROM lg_list ORDER BY key;
-- Sorted inside the array before printing. A set has no order, so the members
-- may arrive in any order and comparing the array as it came would make this
-- assertion a statement about the server's hashing. Three members rather than
-- one on purpose: a single-member set cannot tell "the whole collection" from
-- "the first member", nor one row per key from one row per member, which are
-- exactly the two things this shape can get wrong.
SELECT key, (SELECT array_agg(m ORDER BY m) FROM unnest(value) AS m) AS members
FROM lg_set ORDER BY key;
SELECT key, value FROM lg_zset ORDER BY key;

-- ONE ROW PER KEY, not one per member. The list holds three members and the
-- zset two; deciding this from the table type alone gives one row per member,
-- each carrying a copy of the whole collection, so the row count becomes the
-- member count.
SELECT (SELECT count(*) FROM lg_list) AS list_rows,
       (SELECT count(*) FROM lg_zset) AS zset_rows,
       (SELECT array_length(value, 1) FROM lg_list) AS list_members,
       (SELECT array_length(value, 1) FROM lg_zset) AS zset_members;

-- ---------------------------------------------------------------------------
-- Schema on read, which is the reason the shape exists.
--
-- Neither 'c' nor the shape of lg_doc appears in any table definition: the
-- query supplies the schema, and the two keys below do not even have the same
-- field set. Every other shape in this wrapper requires 'c' to be named in a
-- column option before it can be read at all.
-- ---------------------------------------------------------------------------
CREATE EXTENSION hstore;
CREATE TYPE lg_doc AS (a int, b text, c int);

SELECT key, hstore(value) AS as_hstore FROM lg_hash ORDER BY key;
SELECT key, (populate_record(NULL::lg_doc, hstore(value))).*
FROM lg_hash ORDER BY key;
-- The same reconstruction with nothing but core, and an independent check on
-- the alternation: jsonb_object refuses an array of odd length outright, so a
-- packed hash that had lost or gained one element could not reach this answer
-- however plausible its own printout looked.
SELECT key, jsonb_object(value) AS as_jsonb FROM lg_hash ORDER BY key;

-- ---------------------------------------------------------------------------
-- The element type.
--
-- bytea[] takes Valkey's bytes verbatim, which is the only way a member
-- holding a NUL or a byte sequence that is not valid in the server encoding
-- can be read at all (invariant I3). text[] is checked by the same route every
-- other text value takes, so the same member through a text[] table is a loud
-- refusal rather than a mangled string or a truncated one.
-- ---------------------------------------------------------------------------
CREATE FOREIGN TABLE lg_bytes (key text, value bytea[]) SERVER lg_srv
    OPTIONS (tabletype 'list', keyprefix 'lgb:', legacy_value 'true');
CREATE FOREIGN TABLE lg_bytes_as_text (key text, value text[]) SERVER lg_srv
    OPTIONS (tabletype 'list', keyprefix 'lgb:', legacy_value 'true');

SELECT key,
       array_length(value, 1) AS members,
       octet_length(value[1]) AS member_bytes,
       value[1] = '\x61006200ff'::bytea AS bytes_survived
FROM lg_bytes;

SELECT count(*) FROM lg_bytes_as_text;

-- A domain over the ELEMENT is honoured, per element, by that same route. The
-- constraint is written to reject a member the list actually holds, because a
-- constraint that passes proves only that it was not consulted.
CREATE DOMAIN lg_word AS text CHECK (VALUE <> 'y');
CREATE FOREIGN TABLE lg_elem_domain (key text, value lg_word[]) SERVER lg_srv
    OPTIONS (tabletype 'list', keyprefix 'lg:', legacy_value 'true');
SELECT key, value FROM lg_elem_domain;

-- ---------------------------------------------------------------------------
-- An empty collection is a key that is not there.
--
-- Valkey has no "exists but empty" state, so an empty container reply means
-- the key is gone. A packed table therefore yields NO ROW for such a key -
-- never one row with an empty array - and that is the same answer the mapped
-- shape gives for the same key. The two must agree: a packed table that
-- emitted an empty-array row would make 'SELECT EXISTS (...)' answer true for
-- every key that was ever named.
-- ---------------------------------------------------------------------------
SELECT (SELECT count(*) FROM lg_hash WHERE key = 'lg:gone') AS packed_absent,
       (SELECT count(*) FROM lg_cols WHERE key = 'lg:gone') AS mapped_absent,
       (SELECT count(*) FROM lg_hash WHERE key = 'lg:h')    AS packed_present,
       (SELECT count(*) FROM lg_cols WHERE key = 'lg:h')    AS mapped_present;

-- ---------------------------------------------------------------------------
-- A PACKED ROW IS WRITTEN WHOLE.
--
-- The objection to writing one was that an array of members says nothing about
-- WHICH member a write means. That is true of a write that changes one member,
-- and a packed row's write changes all of them: the array is what the key
-- should hold afterwards. So it folds into an emptying and a rebuild, and the
-- key's contents are exactly the array every time.
-- ---------------------------------------------------------------------------
INSERT INTO lg_hash VALUES ('lg:new', ARRAY['a', '1', 'b', '2']);
SELECT key, value FROM lg_hash WHERE key = 'lg:new';

-- REPLACEMENT, NOT A MERGE, which a shorter array is the only way to show: an
-- update that added and overwrote would leave 'b' behind and read back longer
-- than it was written.
UPDATE lg_hash SET value = ARRAY['a', '9'] WHERE key = 'lg:new';
SELECT key, value FROM lg_hash WHERE key = 'lg:new';

-- Asserted against the server too, because a table agreeing with itself is
-- what a wrong answer also does.
SELECT num AS fields_on_the_server
FROM valkey_fdw_test_probe('lg_srv', 0, 'HLEN', 'lg:new');

-- An empty array means the key holds nothing, and Valkey does not keep an
-- empty collection - so it is gone, which is the same equivalence the mapped
-- shapes have when their last field is nulled.
UPDATE lg_hash SET value = ARRAY[]::text[] WHERE key = 'lg:new';
SELECT count(*) AS rows_after_emptying FROM lg_hash WHERE key = 'lg:new';

-- A list keeps the array's ORDER, which is the property that distinguishes it
-- and the reason the rebuild empties first rather than appending.
INSERT INTO lg_list VALUES ('lg:l2', ARRAY['z', 'y', 'x']);
SELECT key, value FROM lg_list WHERE key = 'lg:l2';

UPDATE lg_list SET value = ARRAY['q', 'r'] WHERE key = 'lg:l2';
SELECT key, value FROM lg_list WHERE key = 'lg:l2';

DELETE FROM lg_list WHERE key = 'lg:l2';
SELECT count(*) AS rows_after_delete FROM lg_list WHERE key = 'lg:l2';

-- A PACKED TABLE OVER A KEYSET, where a delete has to do more than remove the
-- key: the keyset is the table's index of its own keys, so a key left in it
-- after its contents are gone is a row the next scan reports and then cannot
-- fetch.
SELECT num AS keyset_seeded
FROM valkey_fdw_test_probe('lg_srv', 0, 'SADD', 'lg:ks', 'lg:k1');
SELECT num AS keyset_member_seeded
FROM valkey_fdw_test_probe('lg_srv', 0, 'RPUSH', 'lg:k1', 'one', 'two');

CREATE FOREIGN TABLE lg_keyset (key text, value text[]) SERVER lg_srv
    OPTIONS (tabletype 'list', keyset 'lg:ks', legacy_value 'true');

SELECT key, value FROM lg_keyset ORDER BY key;

DELETE FROM lg_keyset WHERE key = 'lg:k1';

-- Gone from the table, and gone from the SET that lists the table's keys.
-- The second is what a delete folded as a member removal would leave behind.
SELECT count(*) AS rows_left FROM lg_keyset;
SELECT num AS keyset_size
FROM valkey_fdw_test_probe('lg_srv', 0, 'SCARD', 'lg:ks');

-- A NULL element is refused rather than skipped or written as empty. Skipping
-- would shorten the collection silently, and no byte string means "absent" to
-- a server that has a member or has not.
INSERT INTO lg_hash VALUES ('lg:bad', ARRAY['a', NULL]);

-- A hash's array alternates field and value, so an odd count names a field
-- with no value. Refused at the statement, where the message can still say
-- which row.
INSERT INTO lg_hash VALUES ('lg:bad', ARRAY['a', '1', 'c']);

-- THE PACKED ZSET IS STILL REFUSED, and now for a reason of its own: its read
-- drops the scores deliberately, so an array written back cannot say what any
-- score should become.
INSERT INTO lg_zset VALUES ('lg:z2', ARRAY['member']);

SELECT c.relname, pg_relation_is_updatable(c.oid, false) AS mask
FROM pg_class c
WHERE c.relname IN ('lg_hash', 'lg_zset', 'lg_bytes', 'lg_cols')
ORDER BY c.relname;

-- ---------------------------------------------------------------------------
-- What the value column may be declared as.
--
-- The column holds many values rather than one, so it is an array, and its
-- elements are the two types the decoder can produce. Each of these is a
-- mistake visible in the CREATE statement, so it is answered at the first plan
-- naming the column - not per row, once the query is already running.
-- ---------------------------------------------------------------------------
CREATE FOREIGN TABLE lg_bad_scalar (key text, value text) SERVER lg_srv
    OPTIONS (tabletype 'hash', keyprefix 'lg:', legacy_value 'true');
SELECT * FROM lg_bad_scalar;

CREATE FOREIGN TABLE lg_bad_elem (key text, value int[]) SERVER lg_srv
    OPTIONS (tabletype 'hash', keyprefix 'lg:', legacy_value 'true');
SELECT * FROM lg_bad_elem;

-- A domain over the ARRAY is refused rather than quietly accepted: the array
-- is assembled from element Datums and never parsed, so this constraint would
-- never be checked, and a constraint that never runs is worse than a table
-- that is not created.
CREATE DOMAIN lg_arr AS text[] CHECK (array_length(VALUE, 1) > 0);
CREATE FOREIGN TABLE lg_bad_domain (key text, value lg_arr) SERVER lg_srv
    OPTIONS (tabletype 'hash', keyprefix 'lg:', legacy_value 'true');
SELECT * FROM lg_bad_domain;

-- A string key holds one value and has no members, so legacy_value and
-- tabletype 'string' contradict each other. Refused as the two table options
-- they are, rather than answered with an array of one element - which is the
-- only shape whose length would say nothing about the data.
--
-- Named together, the validator has both in one option list and refuses the
-- definition, so no such table is ever created.
CREATE FOREIGN TABLE lg_string (key text, value text[]) SERVER lg_srv
    OPTIONS (tabletype 'string', keyprefix 'lg:', legacy_value 'true');

-- The same contradiction, reached the other way and therefore refused
-- somewhere else. tabletype defaults to 'string', so this table names only one
-- of the two options and the validator has nothing to compare; the map applies
-- the default and refuses at plan time.
--
-- Both cases are kept because neither check subsumes the other: delete the
-- validator and the first table is created, delete the map check and this one
-- returns rows.
CREATE FOREIGN TABLE lg_default_string (key text, value text[]) SERVER lg_srv
    OPTIONS (keyprefix 'lg:', legacy_value 'true');
SELECT * FROM lg_default_string;

SELECT valkey_fdw_test_probe('lg_srv', 0, 'DEL', 'lg:h', 'lg:h2', 'lg:l',
                             'lg:s', 'lg:z', 'lgb:l') IS NOT NULL AS cleaned;

DROP SERVER lg_srv CASCADE;
DROP TYPE lg_doc;
DROP DOMAIN lg_word;
DROP DOMAIN lg_arr;
DROP EXTENSION hstore;
