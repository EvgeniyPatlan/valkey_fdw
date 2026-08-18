-- Table shape resolution.
--
-- Building every tuple from a two-element array regardless of the relation's
-- width means a third column reads past the end of that array - any user who
-- can CREATE FOREIGN TABLE can crash the backend. Here every attribute must
-- resolve to a source at plan time, so a table that cannot be filled is
-- refused instead of being scanned.

CREATE SERVER map_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER map_srv;

-- Shape resolution happens while planning, so EXPLAIN is what exercises it -
-- and it exercises nothing else. Selecting instead would make every case
-- depend on what the server happens to hold, which is how a suite ends up
-- measuring the previous run rather than its own fixture.

-- One column: the key alone is a legitimate table.
CREATE FOREIGN TABLE map_key_only (k text) SERVER map_srv;
EXPLAIN (COSTS OFF) SELECT * FROM map_key_only;

-- Two columns: key and value, no options needed.
CREATE FOREIGN TABLE map_two (k text, v text) SERVER map_srv;
EXPLAIN (COSTS OFF) SELECT * FROM map_two;

-- Three columns on a string table. A string table supplies a key and one
-- value, so with TWO spare columns it is not the third that is wrong - it is
-- that the table does not say WHICH of them is the value. Both are therefore
-- named, and the default that map_two relies on is deliberately not applied:
-- picking one would make a typo look like it worked. This is the exact shape
-- that crashes a two-element tuple builder.
CREATE FOREIGN TABLE map_three (k text, v text, extra text) SERVER map_srv;
EXPLAIN (COSTS OFF) SELECT * FROM map_three;

-- The same width is fine once every column says where it comes from.
CREATE FOREIGN TABLE map_three_hash (
    k     text,
    name  text OPTIONS (field 'name'),
    email text OPTIONS (field 'email')
) SERVER map_srv OPTIONS (tabletype 'hash');
EXPLAIN (COSTS OFF) SELECT * FROM map_three_hash;

-- Width is not the constraint; having a source is.
CREATE FOREIGN TABLE map_wide (
    k text,
    f1 text OPTIONS (field 'f1'), f2 text OPTIONS (field 'f2'),
    f3 text OPTIONS (field 'f3'), f4 text OPTIONS (field 'f4'),
    f5 text OPTIONS (field 'f5'), f6 text OPTIONS (field 'f6'),
    f7 text OPTIONS (field 'f7'), f8 text OPTIONS (field 'f8')
) SERVER map_srv OPTIONS (tabletype 'hash');
EXPLAIN (COSTS OFF) SELECT * FROM map_wide;

-- A dropped column leaves a gap in the descriptor that must be skipped, not
-- counted as unmapped.
ALTER FOREIGN TABLE map_wide DROP COLUMN f4;
EXPLAIN (COSTS OFF) SELECT * FROM map_wide;

-- A list or set leaves exactly one meaning for a spare column, so it need not
-- be spelled out. A zset leaves two - member and score - and must be.
CREATE FOREIGN TABLE map_set_default (k text, m text) SERVER map_srv
    OPTIONS (tabletype 'set');
EXPLAIN (COSTS OFF) SELECT * FROM map_set_default;

CREATE FOREIGN TABLE map_zset_ambiguous (k text, a text, b text) SERVER map_srv
    OPTIONS (tabletype 'zset');
EXPLAIN (COSTS OFF) SELECT * FROM map_zset_ambiguous;

-- A singleton table names its key in its options, so no column carries it and
-- none is claimed by default: the lone column here is the member, not the key.
CREATE FOREIGN TABLE map_singleton_set (m text) SERVER map_srv
    OPTIONS (tabletype 'set', singleton_key 's');
EXPLAIN (COSTS OFF) SELECT * FROM map_singleton_set;

-- A key column is still permitted, and gets the fixed name.
CREATE FOREIGN TABLE map_singleton_keyed (
    k text OPTIONS (key 'true'),
    m text OPTIONS (member 'true')
) SERVER map_srv OPTIONS (tabletype 'set', singleton_key 's');
EXPLAIN (COSTS OFF) SELECT * FROM map_singleton_keyed;

-- Being a singleton does not excuse a column from having a source. Two spare
-- columns on a singleton set leave the same ambiguity map_three has, so both
-- are named for the same reason.
CREATE FOREIGN TABLE map_singleton_extra (m text, x text) SERVER map_srv
    OPTIONS (tabletype 'set', singleton_key 's');
EXPLAIN (COSTS OFF) SELECT * FROM map_singleton_extra;

-- ---------------------------------------------------------------------------
-- Definitions that must be refused.
-- ---------------------------------------------------------------------------

CREATE FOREIGN TABLE map_two_keys (
    a text OPTIONS (key 'true'),
    b text OPTIONS (key 'true')
) SERVER map_srv;
EXPLAIN (COSTS OFF) SELECT * FROM map_two_keys;

-- Every column claimed by something other than the key leaves nothing to be
-- the key, and a non-singleton table cannot be read without one.
CREATE FOREIGN TABLE map_no_key (
    m text OPTIONS (member 'true')
) SERVER map_srv OPTIONS (tabletype 'set');
EXPLAIN (COSTS OFF) SELECT * FROM map_no_key;

CREATE FOREIGN TABLE map_conflicting_col (
    k text,
    v text OPTIONS (field 'f', score 'true')
) SERVER map_srv OPTIONS (tabletype 'zset');
EXPLAIN (COSTS OFF) SELECT * FROM map_conflicting_col;

-- score belongs to zset tables only.
CREATE FOREIGN TABLE map_score_on_hash (
    k text,
    s double precision OPTIONS (score 'true')
) SERVER map_srv OPTIONS (tabletype 'hash');
EXPLAIN (COSTS OFF) SELECT * FROM map_score_on_hash;

-- A ttl column has to say which field's lifetime it reports.
CREATE FOREIGN TABLE map_ttl_no_field (
    k text,
    t interval OPTIONS (ttl 'true')
) SERVER map_srv OPTIONS (tabletype 'hash');
EXPLAIN (COSTS OFF) SELECT * FROM map_ttl_no_field;

-- distance needs a table type it could have come from. A search_index on a
-- hash does not make one: the index is where the search happens, and the
-- table type is what says a search happens at all.
CREATE FOREIGN TABLE map_distance_no_index (
    k text,
    d double precision OPTIONS (distance 'true')
) SERVER map_srv OPTIONS (tabletype 'hash');
EXPLAIN (COSTS OFF) SELECT * FROM map_distance_no_index;

-- field columns are meaningless on a set.
CREATE FOREIGN TABLE map_field_on_set (
    k text,
    f text OPTIONS (field 'f')
) SERVER map_srv OPTIONS (tabletype 'set');
EXPLAIN (COSTS OFF) SELECT * FROM map_field_on_set;

-- The three key-discovery strategies are alternatives. Documenting that and
-- then accepting the combination anyway means silently ignoring one.
--
-- Refused at CREATE, not at the first SELECT: the validator sees a table's
-- whole option list at once, so it can answer this, and answering it later
-- meant the table existed for a while - long enough for
-- pg_relation_is_updatable, which reads options and never builds a map, to
-- call it fully writable. There is no EXPLAIN below because there is no table.
CREATE FOREIGN TABLE map_singleton_and_prefix (k text, v text) SERVER map_srv
    OPTIONS (singleton_key 's', keyprefix 'p:');

CREATE FOREIGN TABLE map_prefix_and_keyset (k text, v text) SERVER map_srv
    OPTIONS (keyprefix 'p:', keyset 'ks');

-- ALTER goes through the same validator over the merged list, which is the
-- path that would otherwise let a valid table become an invalid one.
CREATE FOREIGN TABLE map_alter_conflict (k text, v text) SERVER map_srv
    OPTIONS (keyprefix 'p:');
ALTER FOREIGN TABLE map_alter_conflict OPTIONS (ADD keyset 'ks');
EXPLAIN (COSTS OFF) SELECT * FROM map_alter_conflict;

-- Legacy shape is exactly two columns. Width is checked before support is,
-- so a malformed legacy table is reported as malformed.
CREATE FOREIGN TABLE map_legacy_wide (k text, v text[], x text) SERVER map_srv
    OPTIONS (tabletype 'hash', legacy_value 'true');
EXPLAIN (COSTS OFF) SELECT * FROM map_legacy_wide;

-- The well-formed shape the width check above is measured against. It plans,
-- which is the half of that pair worth stating: "wide is refused" says nothing
-- unless narrow is accepted, and a check that refuses everything passes the
-- first assertion.
CREATE FOREIGN TABLE map_legacy (k text, v text[]) SERVER map_srv
    OPTIONS (tabletype 'hash', legacy_value 'true');
EXPLAIN (COSTS OFF) SELECT * FROM map_legacy;

-- ---------------------------------------------------------------------------
-- Shapes that are designed and validated but not yet read.
--
-- Their options parse and their definitions are coherent, so nothing earlier
-- rejects them - but no code fills their columns. Returning NULL would be a
-- plausible empty result, which is the failure this wrapper exists to avoid,
-- so they are refused at plan time until the phase that implements them.
--
-- The list is shorter than it was. ttl reads, and distance is no longer a
-- column waiting on a phase but a column that belongs to a table type: it is
-- what a vector search returns, so asking for one from a hash is a shape
-- error, and the whole vector table is what is refused instead.
-- ---------------------------------------------------------------------------
CREATE FOREIGN TABLE map_ttl (
    k text,
    f text      OPTIONS (field 'f'),
    t interval  OPTIONS (ttl 'true', field 'f')
) SERVER map_srv OPTIONS (tabletype 'hash');
EXPLAIN (COSTS OFF) SELECT * FROM map_ttl;

-- The shape error, which names the table type rather than the phase - a
-- search_index on the table does not make a hash able to report a distance.
CREATE FOREIGN TABLE map_distance (
    k text,
    d double precision OPTIONS (distance 'true')
) SERVER map_srv OPTIONS (tabletype 'hash', search_index 'idx');
EXPLAIN (COSTS OFF) SELECT * FROM map_distance;

-- A field column on a vector table is accepted: a vector search answers with
-- the hash the index indexed, so its fields are the row's columns. The table
-- is refused for being unimplemented, not for the column being wrong, which
-- is the distinction this pair is here to hold.
CREATE FOREIGN TABLE map_vector_field (
    k text,
    f text OPTIONS (field 'f')
) SERVER map_srv OPTIONS (tabletype 'vector', search_index 'idx');
EXPLAIN (COSTS OFF) SELECT * FROM map_vector_field;

DROP SERVER map_srv CASCADE;
