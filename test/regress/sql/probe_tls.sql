-- The keyspace probes, over TLS.
--
-- probe.sql asserts what the probes do; this asserts that they can be used at
-- all on a topology where the alternative cannot. The whole reason the probes
-- are in-process C rather than a shell-out is that `valkey-cli -h valkey`
-- reaches nothing here - this server runs with the plain port disabled, which
-- tls.sql demonstrates directly - and a fixture helper that cannot run on the
-- topology being tested is not a fixture helper.
--
-- Until this file existed that argument was made in a comment in
-- sql/valkey_fdw_test--0.1.sql and tested nowhere, which made it the one claim
-- about the probes with nothing behind it.
--
-- Small on purpose. Every reply shape is already covered on standalone; what
-- is unproven here is reachability, so this carries the cases that a shell
-- fixture could not have carried even if it could connect: a value with an
-- embedded NUL, and a count the server computed.

CREATE SERVER ptls_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379', tls 'true',
             tls_ca_file '/tls/ca.crt', tls_verify 'full');
CREATE USER MAPPING FOR CURRENT_USER SERVER ptls_srv;

-- A round trip whose payload contains a NUL. COPY FROM PROGRAM aborts on one
-- of these outright, so this is not merely inconvenient through a shell - it
-- is unrepresentable.
SELECT count(*) FROM valkey_fdw_test_probe('ptls_srv', 0, 'SET',
                                           'ptls:nul', '\x610062'::bytea);
SELECT encode(val_part, 'hex') AS bytes, octet_length(val_part) AS len
  FROM valkey_fdw_test_probe('ptls_srv', 0, 'GET', 'ptls:nul');

-- The server's own count of those bytes, which the probe cannot fabricate.
SELECT num AS server_says_strlen
  FROM valkey_fdw_test_probe('ptls_srv', 0, 'STRLEN', 'ptls:nul');

-- An empty value is still a key that exists, over TLS as anywhere else.
SELECT count(*) FROM valkey_fdw_test_probe('ptls_srv', 0, 'SET',
                                           'ptls:empty', '');
SELECT val_part IS NULL AS is_sql_null, octet_length(val_part) AS len
  FROM valkey_fdw_test_probe('ptls_srv', 0, 'GET', 'ptls:empty');
SELECT val_part IS NULL AS is_sql_null
  FROM valkey_fdw_test_probe('ptls_srv', 0, 'GET', 'ptls:missing');

-- The key dump, which is what a write suite's gate will use.
SELECT convert_from(key, 'UTF8') AS keyname, keytype
  FROM valkey_fdw_test_keys('ptls_srv', 0, 'ptls:') ORDER BY key;

SELECT num AS deleted FROM valkey_fdw_test_probe('ptls_srv', 0, 'DEL',
                                                 'ptls:nul', 'ptls:empty');
SELECT count(*) AS keys_left
  FROM valkey_fdw_test_keys('ptls_srv', 0, 'ptls:');

DROP SERVER ptls_srv CASCADE;
