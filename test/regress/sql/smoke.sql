-- Extension loads, links against libvalkey, and registers a well-formed
-- wrapper. This is the floor: if this suite fails, nothing else is meaningful.

SELECT valkey_fdw_version() > 0 AS version_is_positive;

-- Printed as a shape rather than a literal so the suite does not have to be
-- re-recorded every time the pinned libvalkey moves.
SELECT valkey_fdw_libvalkey_version() ~ '^[0-9]+\.[0-9]+\.[0-9]+$'
    AS libvalkey_version_well_formed;

SELECT fdwhandler <> 0 AS has_handler,
       fdwvalidator <> 0 AS has_validator
FROM pg_foreign_data_wrapper
WHERE fdwname = 'valkey_fdw';

-- A scan that finds nothing returns no rows and no error.
CREATE SERVER smoke_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
-- No password: the standalone test server has none configured, and sending
-- one would fail AUTH. That failure is correct behaviour, but it belongs in
-- the acl suite rather than here.
CREATE USER MAPPING FOR CURRENT_USER SERVER smoke_srv;

-- Scoped to a prefix nothing else writes. Scanning the whole keyspace here
-- would make this assertion true only against a pristine server, so it would
-- pass on a fresh container and report the previous run's leftovers on the
-- next one.
CREATE FOREIGN TABLE smoke_t (k text, v text) SERVER smoke_srv
    OPTIONS (keyprefix 'vfdw:smoke:');

-- Nothing there yet.
SELECT * FROM smoke_t;

-- The line above is this suite's only exercise of the scan path, and on its
-- own it cannot fail: a scan that returned nothing under every circumstance -
-- a cursor loop that never advanced, a discovery step that never issued SCAN,
-- a tuple builder that never filled a slot - produces exactly those bytes.
-- That matters more here than anywhere else, because harness.sh runs smoke and
-- nothing else on the cluster and search topologies, so on those this file is
-- the whole of what is asserted about reading a row.
--
-- So the suite writes its own key and reads it back. valkey_fdw_test_poke
-- rather than valkey-cli: the topologies this file has to run on are exactly
-- the ones with no fixture helper available.
--
-- The key carries a hash tag, and that is not decoration. One of those
-- topologies is the 6-node cluster, where 'valkey' is node 1 and owns slots
-- 0-5460; a key hashing anywhere else would be answered with MOVED and this
-- file would fail on a topology nobody was looking at. The slot is asserted
-- rather than assumed, so changing the tag is caught here.
SELECT valkey_fdw_test_keyslot('vfdw:smoke:{vfdw}k'::bytea) AS slot_must_be_low;

SELECT valkey_fdw_test_poke('smoke_srv', 'vfdw:smoke:{vfdw}k'::bytea,
                            'v'::bytea) AS wrote;
SELECT k, v FROM smoke_t;

-- And removes it, so the next run's keyspace is not a function of this one.
SELECT valkey_fdw_test_poke('smoke_srv', 'vfdw:smoke:{vfdw}k'::bytea,
                            NULL) AS deleted;
SELECT count(*) AS rows_after_delete FROM smoke_t;

DROP FOREIGN TABLE smoke_t;
DROP USER MAPPING FOR CURRENT_USER SERVER smoke_srv;
DROP SERVER smoke_srv;
