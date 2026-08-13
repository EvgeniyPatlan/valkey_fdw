-- The option table is the single source of truth for what valkey_fdw
-- accepts. These tests assert that what the code exposes, what the validator
-- accepts, and what is documented all agree.

-- Shape of the table, by context.
SELECT context, count(*) AS options
FROM valkey_fdw_options()
GROUP BY context
ORDER BY context;

-- Full inventory. A diff here means an option was added, removed, or had its
-- default, its redaction or who may set it changed, all of which are
-- user-visible and should be deliberate.
SELECT context, name, type, default_value, sensitive, requires_superuser
FROM valkey_fdw_options()
ORDER BY context, name;

-- Anything credential-bearing must be marked sensitive so it is redacted
-- from error text and EXPLAIN.
SELECT bool_and(sensitive) AS credentials_marked_sensitive
FROM valkey_fdw_options()
WHERE name IN ('password', 'tls_key_file');

-- ---------------------------------------------------------------------------
-- Invariant I7: EVERY option the code knows about is reachable from the
-- validator. A validator branch guarded by strcmp(name, "singleton_key ") -
-- a trailing space - is dead code, and takes whatever conflict checks it
-- holds down with it in silence.
--
-- The names below are written out here rather than read from
-- valkey_fdw_options(). A loop that took each name from the code and handed
-- it straight back to the validator was a tautology: both resolve against the
-- same static vfdw_options table, so a name always matched itself. Renaming
-- the entry to "singleton_key " - the exact defect this block exists to
-- catch - left its output unchanged. Naming the options independently is
-- what turns that typo into a failure here: the validator is asked for
-- "singleton_key", the code no longer has one, and the call raises.
--
-- The cost is that adding an option means editing this list. That is the
-- point; the inventory above already records the code's answer, and these two
-- disagreeing is the signal.
--
-- Each name carries a value of the right shape, since an option with no
-- default still has to be validated with something. Paths must be absolute
-- and enums must name a member they accept.
-- ---------------------------------------------------------------------------
CREATE TEMP TABLE option_manifest(context text, name text, sample text);
INSERT INTO option_manifest VALUES
    ('column',       'distance',            'true'),
    ('column',       'field',               'f1'),
    ('column',       'index_type',          'tag'),
    ('column',       'key',                 'true'),
    ('column',       'member',              'true'),
    ('column',       'score',               'true'),
    ('column',       'ttl',                 'true'),
    ('server',       'cluster',             'true'),
    ('server',       'command_timeout_ms',  '1000'),
    ('server',       'connect_timeout_ms',  '1000'),
    ('server',       'host',                'valkey'),
    ('server',       'max_reply_elements',  '1000'),
    ('server',       'pipeline_batch',      '16'),
    ('server',       'port',                '6379'),
    ('server',       'prefer_replica',      'true'),
    ('server',       'reader_buffer_bytes', '65536'),
    ('server',       'scan_count',          '100'),
    ('server',       'tls',                 'true'),
    ('server',       'tls_ca_file',         '/tls/ca.crt'),
    ('server',       'tls_cert_file',       '/tls/client.crt'),
    ('server',       'tls_key_file',        '/tls/client.key'),
    ('server',       'tls_sni',             'valkey'),
    ('server',       'tls_verify',          'full'),
    ('server',       'unix_socket_path',    '/tmp/valkey.sock'),
    ('server',       'write_max_bytes',     '1048576'),
    ('server',       'write_max_ops',       '100'),
    ('table',        'database',            '0'),
    ('table',        'keyprefix',           'p:'),
    ('table',        'keyset',              'ks'),
    ('table',        'legacy_value',        'true'),
    ('table',        'readonly',            'true'),
    ('table',        'search_index',        'idx'),
    ('table',        'singleton_key',       's'),
    ('table',        'tabletype',           'hash'),
    ('user_mapping', 'password',            'x'),
    ('user_mapping', 'password_required',   'true'),
    ('user_mapping', 'username',            'u');

-- Both directions, so a rename shows up as one row missing and one extra
-- rather than as a count that happens to match. No rows is the pass.
SELECT coalesce(m.context, o.context) AS context,
       coalesce(m.name, o.name) AS name,
       CASE WHEN m.name IS NULL THEN 'the code has it, this file does not'
            ELSE 'this file has it, the code does not' END AS discrepancy
FROM option_manifest m
FULL JOIN valkey_fdw_options() o
       ON o.context = m.context AND o.name = m.name
WHERE m.name IS NULL OR o.name IS NULL
ORDER BY 1, 2;

DO $$
DECLARE
    r        record;
    ctx      oid;
    failures text := '';
    tested   int := 0;
BEGIN
    FOR r IN SELECT * FROM option_manifest ORDER BY context, name
    LOOP
        ctx := CASE r.context
                   WHEN 'server'       THEN 'pg_foreign_server'::regclass::oid
                   WHEN 'user_mapping' THEN 'pg_user_mapping'::regclass::oid
                   WHEN 'table'        THEN 'pg_foreign_table'::regclass::oid
                   WHEN 'column'       THEN 'pg_attribute'::regclass::oid
               END;
        IF ctx IS NULL THEN
            failures := failures || format('%s: unmapped context %s; ',
                                           r.name, r.context);
            CONTINUE;
        END IF;

        BEGIN
            PERFORM valkey_fdw_validator(ARRAY[r.name || '=' || r.sample], ctx);
            tested := tested + 1;
        EXCEPTION WHEN OTHERS THEN
            failures := failures || format('%s (%s): %s; ',
                                           r.name, r.context, SQLERRM);
        END;
    END LOOP;

    IF failures <> '' THEN
        RAISE EXCEPTION 'options unreachable from validator: %', failures;
    END IF;
    RAISE NOTICE 'every option named in this file is accepted by the validator (% of them)',
                 tested;
END $$;

DROP TABLE option_manifest;

-- The count is printed separately so that an option added without a validator
-- branch, or removed, is a diff here as well as in the inventory above.
SELECT count(*) AS options_total FROM valkey_fdw_options();

-- ---------------------------------------------------------------------------
-- Combination rules the validator enforces, which single-option acceptance
-- cannot reach.
--
-- The three key-discovery options are alternatives. This used to be checked
-- only when the table map was built, which is the first SELECT - so the table
-- could be created, and pg_relation_is_updatable, which reads options and
-- never builds a map, called it fully writable.
-- ---------------------------------------------------------------------------
SELECT valkey_fdw_validator(ARRAY['singleton_key=s', 'keyprefix=p:'],
                            'pg_foreign_table'::regclass::oid);
SELECT valkey_fdw_validator(ARRAY['keyprefix=p:', 'keyset=ks'],
                            'pg_foreign_table'::regclass::oid);
SELECT valkey_fdw_validator(ARRAY['singleton_key=s', 'keyset=ks'],
                            'pg_foreign_table'::regclass::oid);

-- One of them alone is fine, which is what makes the refusals above specific.
DO $$
BEGIN
    PERFORM valkey_fdw_validator(ARRAY['keyprefix=p:'],
                                 'pg_foreign_table'::regclass::oid);
    PERFORM valkey_fdw_validator(ARRAY['keyset=ks'],
                                 'pg_foreign_table'::regclass::oid);
    PERFORM valkey_fdw_validator(ARRAY['singleton_key=s'],
                                 'pg_foreign_table'::regclass::oid);
    RAISE NOTICE 'each key-discovery option is accepted on its own';
END $$;

-- ---------------------------------------------------------------------------
-- Rejections. Each of these is a value a lax parser accepts silently.
--
-- Full verbosity throughout: the DETAIL says why the value was rejected and
-- the HINT echoes what was actually parsed. Those two lines are the whole
-- point of rejecting rather than defaulting, so they are asserted, not
-- suppressed.
-- ---------------------------------------------------------------------------

-- Non-numeric port. atoi() silently yields 0 on garbage, and a 0 port then
-- defaults to 6379, connecting somewhere the user never asked for.
CREATE SERVER bad_port_text FOREIGN DATA WRAPPER valkey_fdw OPTIONS (port 'abc');

-- Out of range in both directions.
CREATE SERVER bad_port_low  FOREIGN DATA WRAPPER valkey_fdw OPTIONS (port '0');
CREATE SERVER bad_port_high FOREIGN DATA WRAPPER valkey_fdw OPTIONS (port '70000');

-- Trailing garbage after a valid prefix: '6379x' is not 6379.
CREATE SERVER bad_port_junk FOREIGN DATA WRAPPER valkey_fdw OPTIONS (port '6379x');

-- Empty value.
CREATE SERVER bad_port_empty FOREIGN DATA WRAPPER valkey_fdw OPTIONS (port '');

-- Unknown option, with a hint listing what is actually valid here.
CREATE SERVER bad_unknown FOREIGN DATA WRAPPER valkey_fdw OPTIONS (hostname 'x');

-- Enum and boolean values outside their domains.
CREATE SERVER bad_enum FOREIGN DATA WRAPPER valkey_fdw OPTIONS (tls_verify 'maybe');
CREATE SERVER bad_bool FOREIGN DATA WRAPPER valkey_fdw OPTIONS (cluster 'perhaps');

-- Option valid on another catalog must not be accepted here.
CREATE SERVER bad_context FOREIGN DATA WRAPPER valkey_fdw OPTIONS (tabletype 'hash');

-- A relative path resolves against the server's working directory, not the
-- user's, so it is rejected at DDL time rather than failing much later at
-- connection time.
CREATE SERVER bad_relpath FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (tls_ca_file 'certs/ca.crt');

-- Redaction: tls_key_file is sensitive, so the rejected value must not be
-- echoed back the way a non-sensitive option's is. Compare the HINT here
-- with the one from bad_relpath above.
CREATE SERVER bad_keypath FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (tls_key_file 'relative/secret.key');

-- Duplicate names, reached by calling the validator directly. PostgreSQL
-- normalises the DDL path, so this is the path that can actually carry a
-- duplicate.
SELECT valkey_fdw_validator(ARRAY['port=6379', 'port=6380'],
                            'pg_foreign_server'::regclass::oid);

-- ---------------------------------------------------------------------------
-- Acceptance, to prove the rejections above are specific rather than blanket.
--
-- The validator returns void, so success is the absence of an exception; a
-- DO block reports that explicitly instead of printing a meaningless NULL
-- comparison.
-- ---------------------------------------------------------------------------
DO $$
BEGIN
    PERFORM valkey_fdw_validator(
        ARRAY['host=valkey', 'port=6379', 'tls=on', 'tls_verify=ca',
              'tls_ca_file=/tls/ca.crt', 'connect_timeout_ms=250',
              'pipeline_batch=1', 'unix_socket_path=/tmp/valkey.sock'],
        'pg_foreign_server'::regclass::oid);
    RAISE NOTICE 'server options accepted';

    PERFORM valkey_fdw_validator(
        ARRAY['tabletype=zset', 'keyprefix=doc:', 'legacy_value=false',
              'database=15', 'readonly=on'],
        'pg_foreign_table'::regclass::oid);
    RAISE NOTICE 'table options accepted';

    PERFORM valkey_fdw_validator(
        ARRAY['key=true', 'field=embedding', 'index_type=vector'],
        'pg_attribute'::regclass::oid);
    RAISE NOTICE 'column options accepted';

    PERFORM valkey_fdw_validator(
        ARRAY['username=app', 'password=secret', 'password_required=off'],
        'pg_user_mapping'::regclass::oid);
    RAISE NOTICE 'user mapping options accepted';
END $$;
