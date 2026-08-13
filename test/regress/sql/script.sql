-- The server-side program: its identity, its vocabulary, and its replies.
--
-- Nothing here runs the program against real data - that is S6, once the
-- flush exists. What is asserted here is everything that must be true BEFORE
-- the first write is ever sent, because each of these failures is silent:
-- a digest that names a different script, a dispatch table that decodes the
-- wrong opcode, a sentinel that reads as the wrong SQLSTATE.

CREATE SERVER sc_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER sc_srv;

-- ---------------------------------------------------------------------------
-- The compiled-in SHA1 is the one the SERVER computes.
--
-- This is the assertion that makes always-EVALSHA-first safe. A digest we
-- computed and then compared against our own computation proves only that
-- SHA1 is deterministic. Asking the server is what catches a trailing NUL
-- hashed by mistake, a text encoding difference, or an editor that rewrote
-- the line endings - each of which produces a valid digest for a script the
-- server has never seen, so every EVALSHA misses and the retry path becomes
-- the normal path without anyone noticing.
-- ---------------------------------------------------------------------------
SELECT valkey_fdw_test_script_sha1() = convert_from(val_part, 'UTF8')
           AS sha1_agrees_with_server
FROM valkey_fdw_test_probe('sc_srv', 0, 'SCRIPT', 'LOAD',
                           valkey_fdw_test_script()::bytea);

-- The digest is 40 lowercase hex characters, so a truncation or an uppercase
-- rendering is a diff rather than a mismatch nobody reads.
SELECT length(valkey_fdw_test_script_sha1()) AS sha1_length,
       valkey_fdw_test_script_sha1() ~ '^[0-9a-f]{40}$' AS sha1_shape;

-- The server can execute it. EVALSHA with no keys and a deliberately empty
-- plan set exercises the decoder's happy path and nothing else - it writes
-- nothing, which is what makes it safe to run here.
SELECT convert_from(val_part, 'UTF8') AS empty_program
FROM valkey_fdw_test_probe('sc_srv', 0, 'EVALSHA',
                           valkey_fdw_test_script_sha1()::bytea,
                           '0', 'V1', '0');

-- ---------------------------------------------------------------------------
-- The C enums and the two Lua dispatch tables are the same vocabulary.
--
-- They are two independent spellings and nothing but this holds them
-- together. An opcode inserted in the middle of the C enum without the same
-- insertion in Lua renames every opcode after it - consistently, silently,
-- and wrongly: the ledger would emit ZADD and the script would run ZREM.
--
-- The tables are parsed out of the script TEXT rather than out of a second C
-- array, because a C-side comparison would compare the enum against itself.
-- ---------------------------------------------------------------------------
CREATE TEMP TABLE sc_lua AS
SELECT 'check'::text AS kind,
       (regexp_matches(body, '\[(\d+)\] = ', 'g'))[1]::int AS op_no
FROM (SELECT substring(valkey_fdw_test_script()
                       FROM 'local CHECK = \{(.*?)\n\}') AS body) c
UNION ALL
SELECT 'action'::text,
       (regexp_matches(body, '\[(\d+)\] = ', 'g'))[1]::int
FROM (SELECT substring(valkey_fdw_test_script()
                       FROM 'local ACTION = \{(.*?)\n\}') AS body) a;

-- Both tables were found and are non-empty. Without this a regexp that
-- stopped matching would leave sc_lua empty and every comparison below would
-- pass by vacuum.
SELECT kind, count(*) AS entries FROM sc_lua GROUP BY kind ORDER BY kind;

-- Every C opcode has a Lua entry and every Lua entry has a C opcode. A FULL
-- JOIN, so a surplus on either side is a row rather than an absence.
SELECT coalesce(c.kind, l.kind) AS kind,
       coalesce(c.op_no, l.op_no) AS op_no,
       c.name AS c_name,
       (l.op_no IS NOT NULL) AS in_lua
FROM valkey_fdw_test_opcodes() c
FULL JOIN sc_lua l ON l.kind = c.kind AND l.op_no = c.op_no
WHERE c.op_no IS NULL OR l.op_no IS NULL
ORDER BY 1, 2;

-- The counts agree, stated positively so the query above is not the only
-- thing standing between a renumbering and a green suite.
SELECT (SELECT count(*) FROM valkey_fdw_test_opcodes() WHERE kind = 'check')
         = (SELECT count(*) FROM sc_lua WHERE kind = 'check') AS check_parity,
       (SELECT count(*) FROM valkey_fdw_test_opcodes() WHERE kind = 'action')
         = (SELECT count(*) FROM sc_lua WHERE kind = 'action') AS action_parity;

-- TYPE_OK is the one check with no dispatch entry: it is applied to every
-- plan directly, before the table is consulted, because it is what makes
-- phase 2 incapable of raising WRONGTYPE. Named here so its absence from the
-- Lua table reads as intent rather than as the omission the parity query
-- would otherwise be reporting.
SELECT name AS check_without_dispatch_entry
FROM valkey_fdw_test_opcodes()
WHERE kind = 'check' AND name = 'TYPE_OK';

-- ---------------------------------------------------------------------------
-- Sentinel decoding, including the word boundaries.
--
-- vfdw_reply_has_prefix is a raw prefix test. Every code is a prefix of a
-- longer string that means something else, so without a boundary check
-- "VFDW1 CONFLICTX" reports a unique violation for something that is not one.
-- The last two vectors are the ones a prefix test gets wrong.
-- ---------------------------------------------------------------------------
SELECT v AS reply, (c).verdict, (c).detail
FROM (VALUES
        ('VFDW1 OK 3'),
        ('VFDW1 CONFLICT 2 wb:k1 check 2'),
        ('VFDW1 MISSING 1 wb:g1'),
        ('VFDW1 WRONGTYPE 1 wb:x list'),
        ('VFDW1 BADPROTO 0 - version'),
        ('VFDW1 INTERNAL 2 wb:k 3 OOM'),
        -- No detail: the code ends the reply.
        ('VFDW1 CONFLICT'),
        -- Not ours at all.
        ('WRONGTYPE Operation against a key holding the wrong kind of value'),
        ('NOSCRIPT No matching script'),
        (''),
        -- Ours, but a code this build does not know: BADPROTO, not UNKNOWN.
        ('VFDW1 NEWCODE 1 k'),
        -- The boundary cases. A raw prefix test calls both of these CONFLICT.
        ('VFDW1 CONFLICTX 1 k'),
        ('VFDW1 OKAY 1 k'),
        -- A prefix that is not ours on a boundary.
        ('VFDW10 CONFLICT 1 k')
     ) AS t(v),
     LATERAL (SELECT valkey_fdw_test_script_classify(v::bytea)) AS s(c);

-- A NUL inside the detail survives classification: replies are bytes, and a
-- classifier that took a cstring would truncate here.
SELECT (c).verdict, length((c).detail) AS detail_len
FROM (SELECT valkey_fdw_test_script_classify(
               'VFDW1 CONFLICT 1 a'::bytea || '\x00'::bytea || 'b'::bytea)) AS s(c);


-- ---------------------------------------------------------------------------
-- The golden ARGV vector.
--
-- src/vfdw_script_encode.c is the only writer of this format and the Lua
-- program the only reader. A change to either that the other does not match
-- produces a BADPROTO from inside a commit, which is the worst place to find
-- out. Recorded here, the same change is a diff in this file.
--
-- The digest renders as a placeholder: it moves whenever the script text
-- changes, including for a comment, and a golden vector that churns on every
-- edit stops being read. The real digest is asserted above, against the
-- server.
-- ---------------------------------------------------------------------------
CREATE FOREIGN TABLE sc_str (k text OPTIONS (key 'true'), v text)
    SERVER sc_srv OPTIONS (tabletype 'string', keyprefix 'sc:');
CREATE FOREIGN TABLE sc_ks (k text OPTIONS (key 'true'), v text)
    SERVER sc_srv OPTIONS (tabletype 'string', keyset 'sc:index');
CREATE FOREIGN TABLE sc_zs (k text OPTIONS (key 'true'),
                            m text OPTIONS (member 'true'),
                            s text OPTIONS (score 'true'))
    SERVER sc_srv OPTIONS (tabletype 'zset', keyprefix 'scz:');

SELECT valkey_fdw_test_poke('sc_srv', 'sc:ren', 'x');
SELECT count(*) FROM valkey_fdw_test_probe('sc_srv', 0, 'SADD', 'sc:index', 'sc:gone');

-- One INSERT, one keyset INSERT, one rename and one zset member: between them
-- they cover every shape of argument the encoder can emit - data, a key index
-- for a keyset, and a key index for a RENAME source.
BEGIN;
INSERT INTO sc_str VALUES ('sc:a', 'one');
INSERT INTO sc_ks  VALUES ('sc:b', 'two');
UPDATE sc_str SET k = 'sc:renamed' WHERE k = 'sc:ren';
INSERT INTO sc_zs  VALUES ('scz:z', 'm1', '1.5');
SELECT ordinal, kind, convert_from(value, 'UTF8') AS value
FROM valkey_fdw_test_script_program() ORDER BY ordinal;

-- ---------------------------------------------------------------------------
-- W2: every key index the encoder emits is within KEYS.
--
-- Keys travel as 1-based indices because the shebang makes the server enforce
-- the declared-key rule. An index past the end is a nil in Lua - SISMEMBER
-- would see a missing argument and RENAME a nil key, both a long way from the
-- cause. Checked here as a property of the emitted program rather than only
-- as the C-side elog, because the elog fires on a disagreement between two
-- walks and this catches a wrong rule in both of them.
-- ---------------------------------------------------------------------------
WITH prog AS (
    SELECT ordinal, convert_from(value, 'UTF8') AS v
    FROM valkey_fdw_test_script_program()
), nkeys AS (
    SELECT v::int AS n FROM prog WHERE ordinal = 2
)
SELECT (SELECT n FROM nkeys) AS numkeys,
       -- KEYS occupy ordinals 3 .. 2+n, so every one of them must exist and
       -- the ARGV that follows must start with the version marker.
       (SELECT count(*) FROM prog
         WHERE ordinal BETWEEN 3 AND 2 + (SELECT n FROM nkeys)) AS keys_present,
       (SELECT v FROM prog
         WHERE ordinal = 3 + (SELECT n FROM nkeys)) AS first_argv;

ROLLBACK;

-- With an empty buffer the program is a well-formed no-op rather than
-- something the script has to special-case: nplans 0 and no keys at all.
SELECT ordinal, convert_from(value, 'UTF8') AS value
FROM valkey_fdw_test_script_program() ORDER BY ordinal;


-- ---------------------------------------------------------------------------
-- The encoder and the program agree, end to end.
--
-- Everything above compares each side against a recorded expectation. This is
-- the only assertion that puts them together: the exact bytes the encoder
-- produces are handed to the server, which decodes them with the real script.
-- A disagreement the golden vector cannot see - an opcode whose argument
-- order differs between the two, a key index off by one - surfaces here as a
-- BADPROTO instead of at someone's COMMIT.
--
-- This WRITES, which nothing else in this suite does. That is the point: the
-- checks have to pass and the actions have to land, and until now no phase-1
-- check had ever refused anything and no phase-2 action had ever run.
-- ---------------------------------------------------------------------------
BEGIN;
INSERT INTO sc_str VALUES ('sc:e1', 'hello');
INSERT INTO sc_ks  VALUES ('sc:e2', 'indexed');
INSERT INTO sc_zs  VALUES ('scz:e', 'mem', '2.5');

SELECT convert_from(val_part, 'UTF8') AS applied
FROM valkey_fdw_test_probe('sc_srv', 0,
       VARIADIC (SELECT array_agg(CASE WHEN ordinal = 1
                                       THEN valkey_fdw_test_script_sha1()::bytea
                                       ELSE value END ORDER BY ordinal)
                 FROM valkey_fdw_test_script_program()));
ROLLBACK;

-- The actions really landed, and the keyset was maintained inside the same
-- execution as the write it indexes.
SELECT convert_from(key,'UTF8') AS key, keytype
FROM valkey_fdw_test_keys('sc_srv', 0, 'sc:') ORDER BY 1;
-- Scalar commands, not ZRANGE ... WITHSCORES: libvalkey leaves reply->str
-- NULL for every aggregate element, so a nested reply renders as NULLs and
-- would read as "the zset is empty" whether it is or not.
SELECT convert_from(val_part,'UTF8') AS zset_member
FROM valkey_fdw_test_probe('sc_srv', 0, 'ZRANGE', 'scz:e', '0', '-1');
SELECT dbl AS zset_score
FROM valkey_fdw_test_probe('sc_srv', 0, 'ZSCORE', 'scz:e', 'mem');

-- Re-running the SAME program must now be refused by phase 1, not applied
-- twice: the keys it required absent are present because it created them.
-- This is the check half, and it is the first time one has ever refused.
BEGIN;
INSERT INTO sc_str VALUES ('sc:e1', 'hello');
SELECT convert_from(val_part, 'UTF8') AS refused_second_time
FROM valkey_fdw_test_probe('sc_srv', 0,
       VARIADIC (SELECT array_agg(CASE WHEN ordinal = 1
                                       THEN valkey_fdw_test_script_sha1()::bytea
                                       ELSE value END ORDER BY ordinal)
                 FROM valkey_fdw_test_script_program()));
ROLLBACK;

-- And a type mismatch is refused by TYPE_OK rather than reaching a write.
-- sc:index is the keyset, so it is a SET under a key this string table is
-- entitled to address - a real collision rather than a contrived one.
BEGIN;
INSERT INTO sc_str VALUES ('sc:index', 'not a set');
SELECT convert_from(val_part, 'UTF8') AS refused_wrong_type
FROM valkey_fdw_test_probe('sc_srv', 0,
       VARIADIC (SELECT array_agg(CASE WHEN ordinal = 1
                                       THEN valkey_fdw_test_script_sha1()::bytea
                                       ELSE value END ORDER BY ordinal)
                 FROM valkey_fdw_test_script_program()));
ROLLBACK;

-- ---------------------------------------------------------------------------
-- The program itself carries no user data and no dynamic verb.
--
-- scripts/lint.sh enforces the write verbs being literal; this asserts the
-- shape a reader can check from SQL. Both matter: the lint gate is what
-- fails a refactor, and this is what a reviewer reads.
-- ---------------------------------------------------------------------------
SELECT left(valkey_fdw_test_script(), 6) AS shebang,
       valkey_fdw_test_script() LIKE '%PHASE 1%'  AS has_phase1,
       valkey_fdw_test_script() LIKE '%PHASE 2%'  AS has_phase2,
       strpos(valkey_fdw_test_script(), 'PHASE 1')
         < strpos(valkey_fdw_test_script(), 'PHASE 2') AS phases_in_order;

DROP FOREIGN TABLE sc_str, sc_ks, sc_zs;
DROP USER MAPPING FOR CURRENT_USER SERVER sc_srv;
DROP SERVER sc_srv;
