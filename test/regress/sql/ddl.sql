-- DDL surface: servers, user mappings, column-mapped tables, and the legacy
-- compatibility shape. Phase 0 establishes that the definitions are accepted
-- and stored; later phases assert what they do.

CREATE SERVER vk FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379', connect_timeout_ms '2000');

CREATE USER MAPPING FOR CURRENT_USER SERVER vk
    OPTIONS (username 'fdw_app', password 'app_pw');

-- The primary model: one column per hash field, typed.
CREATE FOREIGN TABLE docs (
    id        text OPTIONS (key 'true'),
    title     text OPTIONS (field 'title'),
    category  text OPTIONS (field 'category', index_type 'tag'),
    year      int  OPTIONS (field 'year', index_type 'numeric'),
    body      bytea OPTIONS (field 'body'),
    body_ttl  interval OPTIONS (field 'body', ttl 'true')
) SERVER vk
  OPTIONS (tabletype 'hash', keyprefix 'doc:', search_index 'docs_idx');

-- The legacy packed-array shape, for migration.
CREATE FOREIGN TABLE legacy_hash (
    key   text,
    value text[]
) SERVER vk
  OPTIONS (tabletype 'hash', keyprefix 'h:', legacy_value 'true');

-- Sorted-set table with an explicit score column.
CREATE FOREIGN TABLE leaderboard (
    member text OPTIONS (member 'true'),
    score  double precision OPTIONS (score 'true')
) SERVER vk
  OPTIONS (tabletype 'zset', singleton_key 'scores:global');

-- Options round-trip through the catalog unchanged.
SELECT c.relname,
       (SELECT array_agg(o ORDER BY o) FROM unnest(ft.ftoptions) o) AS options
FROM pg_foreign_table ft
JOIN pg_class c ON c.oid = ft.ftrelid
ORDER BY c.relname;

SELECT c.relname, a.attname,
       (SELECT array_agg(o ORDER BY o)
          FROM pg_options_to_table(a.attfdwoptions) t(k, v), unnest(ARRAY[k||'='||v]) o
       ) AS column_options
FROM pg_attribute a
JOIN pg_class c ON c.oid = a.attrelid
WHERE c.relname IN ('docs', 'leaderboard')
  AND a.attnum > 0
  AND a.attfdwoptions IS NOT NULL
ORDER BY c.relname, a.attnum;

-- ---------------------------------------------------------------------------
-- Rejections at DDL time.
-- ---------------------------------------------------------------------------
CREATE FOREIGN TABLE bad_tabletype (k text, v text) SERVER vk
    OPTIONS (tabletype 'stream');

CREATE FOREIGN TABLE bad_database (k text, v text) SERVER vk
    OPTIONS (database '99');

CREATE FOREIGN TABLE bad_column_opt (
    k text OPTIONS (keyname 'true')
) SERVER vk;

CREATE FOREIGN TABLE bad_index_type (
    k text OPTIONS (field 'f', index_type 'geo')
) SERVER vk;

DROP FOREIGN TABLE docs, legacy_hash, leaderboard;
DROP USER MAPPING FOR CURRENT_USER SERVER vk;
DROP SERVER vk;

-- ---------------------------------------------------------------------------
-- Writability.
--
-- Without IsForeignRelUpdatable, core reports every foreign table as
-- read-only and refuses an INSERT with a message about foreign tables in
-- general. Answering per table means the refusal names this table and its
-- reason, and that information_schema agrees with the error.
--
-- The callback must never raise and never build the table map: the rewriter
-- calls it for every foreign table it touches and information_schema.tables
-- calls it for every relation, so a map built here would refuse a json or
-- legacy_value table and break an unrelated query rather than a write.
-- ---------------------------------------------------------------------------
CREATE SERVER w_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER w_srv;

CREATE FOREIGN TABLE w_string (k text, v text) SERVER w_srv;
CREATE FOREIGN TABLE w_list (k text, m text OPTIONS (member 'true'))
    SERVER w_srv OPTIONS (tabletype 'list');
CREATE FOREIGN TABLE w_readonly (k text, v text) SERVER w_srv
    OPTIONS (readonly 'true');
CREATE FOREIGN TABLE w_json (k text, a text OPTIONS (field '$.a'))
    SERVER w_srv OPTIONS (tabletype 'json');
CREATE FOREIGN TABLE w_legacy (k text, v text[]) SERVER w_srv
    OPTIONS (tabletype 'hash', legacy_value 'true');

-- A table whose reader is not implemented for a reason that has nothing to do
-- with row identity, and one whose column is not implemented at all. Both used
-- to report a fully writable mask, because writability was decided from table
-- options alone: w_ttl cannot even be SELECTed and information_schema called
-- it insertable, updatable and deletable.
CREATE FOREIGN TABLE w_search (k text, v text) SERVER w_srv
    OPTIONS (search_index 'idx');
CREATE FOREIGN TABLE w_ttl (
    k text,
    f text     OPTIONS (field 'f'),
    t interval OPTIONS (field 'f', ttl 'true')
) SERVER w_srv OPTIONS (tabletype 'hash');

-- A list row has no identity that survives its neighbours moving, so it takes
-- inserts and deletes but not updates.
SELECT c.relname,
       pg_relation_is_updatable(c.oid, false) AS mask
FROM pg_class c
WHERE c.relname LIKE 'w\_%'
ORDER BY c.relname;

-- information_schema asks the same question for every relation, including the
-- two whose reader is not implemented. This must simply answer.
SELECT table_name, is_insertable_into
FROM information_schema.tables
WHERE table_name LIKE 'w\_%'
ORDER BY table_name;

-- The refusals themselves.
INSERT INTO w_string VALUES ('k', 'v');
UPDATE w_list SET m = 'x';
INSERT INTO w_readonly VALUES ('k', 'v');
INSERT INTO w_json VALUES ('k', 'v');

-- Each refusal must say why THIS table is refused. The reason used to be one
-- hardcoded errdetail, so a string table blocked for carrying a search index
-- was turned away with a paragraph about list row identity - a confidently
-- wrong answer, which is worse than a generic one. Compare the DETAIL here
-- with the one UPDATE w_list produced above: they must differ.
INSERT INTO w_search VALUES ('k', 'v');

-- A ttl column is refused while the map is built, before writability is even
-- consulted, so this reports the column and not the write. That is the more
-- useful of the two answers and is why the map is built first.
INSERT INTO w_ttl VALUES ('k', 'v', NULL);

-- The mask must agree with the refusal rather than contradict it. This is the
-- pair that used to disagree: 28 - insertable, updatable and deletable -
-- reported through information_schema for two tables that raise on every
-- actual use, one of which cannot even be SELECTed.
SELECT c.relname,
       pg_relation_is_updatable(c.oid, false) AS mask
FROM pg_class c
WHERE c.relname IN ('w_search', 'w_ttl')
ORDER BY c.relname;

DROP SERVER w_srv CASCADE;
