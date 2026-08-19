-- Who may reach the keyspace, and by which route.
--
-- Every other suite in this tree runs as the regression superuser, which
-- passes each check below without ever touching it. That makes this the one
-- file where a gate that holds and a gate that was never written look
-- different.
--
-- Two things make the difference worth a file of its own. PostgreSQL grants
-- EXECUTE on a new function to PUBLIC, so a diagnostic that reaches a Valkey
-- server is executable by every role that can log in unless something revokes
-- it. And resolving a foreign server BY NAME is a catalog lookup rather than
-- an authorisation: GetForeignServerByName answers for any server in the
-- catalog, and GetUserMapping asks only whether a mapping exists, which one
-- CREATE USER MAPPING FOR PUBLIC gives every role in the cluster. Between
-- them, a role holding no grant on anything reaches a keyspace by naming its
-- server - past readonly, past the keyprefix that scopes a table to one part
-- of the keyspace, and past every per-table GRANT.
--
-- So the shape asserted here is one role scoped to one key prefix by one
-- grant, followed by an enumeration of every other door out of that prefix.
-- Routes closed by PostgreSQL itself - ownership of the table, CREATE on a
-- schema, USAGE on the wrapper - are asserted beside the ones this code
-- closes, because "cannot read that key" is a claim about all of them at once
-- and a reader should not have to hold half of it in their head.

-- PostgreSQL 15 revoked CREATE on schema public from PUBLIC; 14 still grants
-- it. One of the routes enumerated below is "create a foreign table of your
-- own", and on 14 that route is open for a reason which has nothing to do
-- with this wrapper - so the suite would report a door as closed on one major
-- and open on another while the code under test was identical.
--
-- Stated here rather than inherited from whichever default the server shipped
-- with. A precondition a test depends on and does not write down is one that
-- changes underneath it.
REVOKE CREATE ON SCHEMA public FROM PUBLIC;

CREATE SERVER priv_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR CURRENT_USER SERVER priv_srv;

-- The second server exists to be named by a role that was never granted USAGE
-- on it. Its mapping is FOR PUBLIC, and that is the point rather than a
-- convenience: one such mapping gives every role in the cluster a mapping, so
-- a refusal below cannot be a missing mapping wearing a privilege error's
-- clothes. password_required is waived for the same reason - with it in place
-- an ungated probe would be stopped by the connection layer instead, and the
-- suite would pass while proving nothing.
CREATE SERVER priv_other FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
CREATE USER MAPPING FOR PUBLIC SERVER priv_other
    OPTIONS (password_required 'false');

-- Two keys under two prefixes. The second is the one nothing below may reach.
SELECT valkey_fdw_test_poke('priv_srv', 'priv:allowed:k1'::bytea,
                            'alpha'::bytea) AS wrote_allowed;
SELECT valkey_fdw_test_poke('priv_srv', 'priv:secret:k1'::bytea,
                            'classified'::bytea) AS wrote_secret;

CREATE FOREIGN TABLE priv_allowed_t (k text, v text) SERVER priv_srv
    OPTIONS (keyprefix 'priv:allowed:');
CREATE FOREIGN TABLE priv_secret_t (k text, v text) SERVER priv_srv
    OPTIONS (keyprefix 'priv:secret:');

-- Both keys are really there, read from the seat that may see everything.
-- Without this the refusals below would be indistinguishable from an empty
-- keyspace, which is the shape a privilege suite passes in while asserting
-- nothing at all.
SELECT k, v FROM priv_allowed_t ORDER BY k;
SELECT k, v FROM priv_secret_t ORDER BY k;

CREATE ROLE priv_reader NOSUPERUSER;
GRANT USAGE ON FOREIGN SERVER priv_srv TO priv_reader;
CREATE USER MAPPING FOR priv_reader SERVER priv_srv
    OPTIONS (password_required 'false');
GRANT SELECT ON priv_allowed_t TO priv_reader;

-- ---------------------------------------------------------------------------
-- The one route that is meant to work, and then every other one.
-- ---------------------------------------------------------------------------
SET ROLE priv_reader;

SELECT k, v FROM priv_allowed_t ORDER BY k;

-- A key outside the table's prefix is not in the table. Fetching it would
-- fill the key column with the very value the recheck compares against, and
-- the row would pass a filter meant to exclude it.
SELECT count(*) AS by_exact_key FROM priv_allowed_t WHERE k = 'priv:secret:k1';

-- The pattern route reaches the same place: a LIKE refines the prefix rather
-- than replacing it, so an intersection that is empty ends the scan.
SELECT count(*) AS by_pattern FROM priv_allowed_t WHERE k LIKE 'priv:secret:%';

-- The table over the other prefix exists and was not granted.
SELECT k, v FROM priv_secret_t ORDER BY k;

-- Repointing the granted table at the other prefix needs ownership of it.
ALTER FOREIGN TABLE priv_allowed_t OPTIONS (SET keyprefix 'priv:secret:');

-- A table of its own needs CREATE on a schema. USAGE on the server, which
-- this role does hold, is the other half and is not enough alone - which is
-- what makes USAGE safe to hand to a role that is meant to see one prefix.
CREATE FOREIGN TABLE priv_escape_t (k text, v text) SERVER priv_srv
    OPTIONS (keyprefix 'priv:secret:');

-- A server of its own needs USAGE on the wrapper.
CREATE SERVER priv_escape FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');

-- And the diagnostics, which reach a keyspace without a foreign table at all,
-- are not executable by PUBLIC: sql/valkey_fdw_test--0.1.sql revokes each one
-- as it creates it, and valkey_fdw_test.control keeps the whole extension out
-- of an install that will never run a suite.
SELECT * FROM valkey_fdw_test_probe('priv_srv', 0, 'GET', 'priv:secret:k1');
SELECT * FROM valkey_fdw_test_keys('priv_srv', 0, 'priv:');
SELECT valkey_fdw_test_poke('priv_srv', 'priv:secret:k1'::bytea, NULL);
SELECT valkey_fdw_test_pooled_ping('priv_srv');

RESET ROLE;

-- ---------------------------------------------------------------------------
-- A role that does hold EXECUTE on the diagnostics.
--
-- Deliberately not the reader above. The refusals there all come from the
-- function ACL, and a suite that only ever sees that one cannot tell whether
-- the entry points check anything themselves; granting the reader EXECUTE
-- would instead make its containment a statement about a grant nobody makes.
-- So a second role holds EXECUTE and no foreign table, and what is asserted
-- about it is the gate underneath: USAGE on the server it names, which is the
-- same grant CREATE FOREIGN TABLE against that server already requires.
-- ---------------------------------------------------------------------------
CREATE ROLE priv_prober NOSUPERUSER;
GRANT USAGE ON FOREIGN SERVER priv_srv TO priv_prober;
CREATE USER MAPPING FOR priv_prober SERVER priv_srv
    OPTIONS (password_required 'false');
GRANT EXECUTE ON FUNCTION valkey_fdw_test_probe(text, int, bytea[])
    TO priv_prober;
GRANT EXECUTE ON FUNCTION valkey_fdw_test_keys(text, int, bytea)
    TO priv_prober;
GRANT EXECUTE ON FUNCTION valkey_fdw_test_ping(text, int, int, int)
    TO priv_prober;

SET ROLE priv_prober;

-- Where the grant was made the gate opens. PING rather than a key read: this
-- line exists to show that the refusals below are the missing grant, and not
-- the function ACL, the mapping, or a server nothing can reach.
SELECT reply_type, convert_from(val_part, 'UTF8') AS body
  FROM valkey_fdw_test_probe('priv_srv', 0, 'PING');

-- Where it was not, naming the server is not enough.
SELECT reply_type FROM valkey_fdw_test_probe('priv_other', 0, 'PING');
SELECT count(*) FROM valkey_fdw_test_keys('priv_other', 0, 'priv:');

-- An entry point that takes a host and a port has no catalog object to
-- authorise against: an address is an argument, so no grant anyone could hold
-- is a statement about the machine being dialled, and USAGE on some unrelated
-- server does not become one. What it hands out is the postmaster's own
-- network reach, aimed by the caller, so nothing weaker than superuser is
-- correct.
SELECT valkey_fdw_test_ping('valkey', 6379, 1000, 1000);

RESET ROLE;

-- ---------------------------------------------------------------------------
-- The classification a caller can branch on.
--
-- The messages above are for people and each names the check that fired;
-- SQLSTATE is what a program sees, and every route out of the prefix has to
-- arrive as the same insufficient_privilege rather than as a mixture of codes
-- that a caller would have to enumerate.
-- ---------------------------------------------------------------------------
CREATE FUNCTION priv_sqlstate(stmt text) RETURNS text
LANGUAGE plpgsql AS $$
BEGIN
    EXECUTE stmt;
    RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE;
END $$;

SET ROLE priv_reader;
SELECT route, priv_sqlstate(stmt) AS sqlstate
FROM (VALUES
    ('select the ungranted table',
     $$SELECT * FROM priv_secret_t$$),
    ('alter the granted table',
     $$ALTER FOREIGN TABLE priv_allowed_t
           OPTIONS (SET keyprefix 'priv:secret:')$$),
    ('create a foreign table',
     $$CREATE FOREIGN TABLE priv_escape_t (k text, v text) SERVER priv_srv
           OPTIONS (keyprefix 'priv:secret:')$$),
    ('create a server',
     $$CREATE SERVER priv_escape FOREIGN DATA WRAPPER valkey_fdw$$),
    ('probe the keyspace',
     $$SELECT * FROM valkey_fdw_test_probe('priv_srv', 0, 'GET',
                                           'priv:secret:k1')$$)
) AS t(route, stmt)
ORDER BY route;
RESET ROLE;

SET ROLE priv_prober;
SELECT route, priv_sqlstate(stmt) AS sqlstate
FROM (VALUES
    ('probe on an ungranted server',
     $$SELECT * FROM valkey_fdw_test_probe('priv_other', 0, 'PING')$$),
    ('keys on an ungranted server',
     $$SELECT * FROM valkey_fdw_test_keys('priv_other', 0, 'priv:')$$),
    ('ping by host and port',
     $$SELECT valkey_fdw_test_ping('valkey', 6379, 1000, 1000)$$)
) AS t(route, stmt)
ORDER BY route;
RESET ROLE;

REVOKE EXECUTE ON FUNCTION valkey_fdw_test_probe(text, int, bytea[])
    FROM priv_prober;
REVOKE EXECUTE ON FUNCTION valkey_fdw_test_keys(text, int, bytea)
    FROM priv_prober;
REVOKE EXECUTE ON FUNCTION valkey_fdw_test_ping(text, int, int, int)
    FROM priv_prober;

-- ---------------------------------------------------------------------------
-- Options whose privilege is a property of the option, not of the object.
--
-- Owning a catalog entry is not the same authority as choosing which of the
-- server's files the backend process opens with its own privileges, on a
-- machine the owner may have no account on. Nor as waiving the rule that a
-- non-superuser mapping carries a password, which is the rule that stops a
-- role borrowing the PostgreSQL process's network identity.
-- ---------------------------------------------------------------------------
CREATE ROLE priv_owner NOSUPERUSER;
GRANT USAGE ON FOREIGN DATA WRAPPER valkey_fdw TO priv_owner;
CREATE SERVER priv_owned FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');
ALTER SERVER priv_owned OWNER TO priv_owner;
CREATE USER MAPPING FOR priv_owner SERVER priv_owned;

SET ROLE priv_owner;

-- The owner may set what carries no privilege of its own, so the refusals
-- below are specific rather than "the owner may alter nothing".
ALTER SERVER priv_owned OPTIONS (ADD connect_timeout_ms '250');

ALTER SERVER priv_owned OPTIONS (ADD tls_ca_file '/tmp/x.pem');

ALTER USER MAPPING FOR CURRENT_USER SERVER priv_owned
    OPTIONS (ADD password_required 'false');

-- The direction that only adds a check is open to anyone; it is switching the
-- check off that is restricted, and the refusal follows the value rather than
-- the ADD or SET spelling.
ALTER USER MAPPING FOR CURRENT_USER SERVER priv_owned
    OPTIONS (ADD password_required 'true');
ALTER USER MAPPING FOR CURRENT_USER SERVER priv_owned
    OPTIONS (SET password_required 'false');

RESET ROLE;

-- A superuser may waive it, which is what makes the refusal in vfdw_conn.c
-- honest when it says how.
ALTER USER MAPPING FOR priv_owner SERVER priv_owned
    OPTIONS (SET password_required 'false');

-- ---------------------------------------------------------------------------
-- What a restricted option costs the owner afterwards.
--
-- transformGenericOptions hands a validator the merged result of the
-- statement rather than the change, so a restricted option that is already
-- set is presented again by every later ALTER of the same object, whatever
-- that ALTER was for. Once a superuser sets tls_ca_file, the non-superuser
-- owner can no longer alter anything on that server - including the timeout
-- they were free to set a moment ago.
--
-- This is asserted rather than left in a comment because it is a consequence
-- worth failing on. The validator is given the new option list and not the
-- old one, so "did this statement change the restricted option" is a question
-- it cannot ask; if a later PostgreSQL ever lets it, this block goes red and
-- the rule can be narrowed on purpose instead of by accident.
-- ---------------------------------------------------------------------------
ALTER SERVER priv_owned OPTIONS (ADD tls_ca_file '/tmp/x.pem');

SET ROLE priv_owner;

ALTER SERVER priv_owned OPTIONS (SET connect_timeout_ms '500');

RESET ROLE;

-- And with it gone the owner has their server back.
ALTER SERVER priv_owned OPTIONS (DROP tls_ca_file);

SET ROLE priv_owner;

ALTER SERVER priv_owned OPTIONS (SET connect_timeout_ms '500');

RESET ROLE;

-- What actually landed. Read back rather than inferred: a refusal that also
-- failed to refuse the write would look identical above.
SELECT unnest(srvoptions) AS server_option FROM pg_foreign_server
WHERE srvname = 'priv_owned' ORDER BY 1;
SELECT unnest(umoptions) AS mapping_option FROM pg_user_mappings
WHERE srvname = 'priv_owned' ORDER BY 1;

-- ---------------------------------------------------------------------------
-- The split between the two extensions, asserted rather than assumed.
--
-- pg_regress loads both, so "after CREATE EXTENSION valkey_fdw alone the
-- probes are undefined" cannot be reached here by installing less. It is
-- reached by removing the half that carries them and looking. The catalog
-- answer comes first, because that is the one that fails the day an entry
-- point is added to the wrong file.
-- ---------------------------------------------------------------------------
SELECT count(*) AS diagnostics_outside_the_test_extension
FROM pg_proc p
     LEFT JOIN pg_depend d ON d.classid = 'pg_proc'::regclass
                          AND d.objid = p.oid AND d.deptype = 'e'
     LEFT JOIN pg_extension e ON e.oid = d.refobjid
WHERE p.proname LIKE 'valkey!_fdw!_test!_%' ESCAPE '!'
  AND e.extname IS DISTINCT FROM 'valkey_fdw_test';

-- And what the wrapper's own extension carries, none of which reaches a
-- Valkey server by itself.
SELECT p.proname
FROM pg_proc p
     JOIN pg_depend d ON d.classid = 'pg_proc'::regclass AND d.objid = p.oid
                     AND d.deptype = 'e'
     JOIN pg_extension e ON e.oid = d.refobjid
WHERE e.extname = 'valkey_fdw'
ORDER BY p.proname;

DROP EXTENSION valkey_fdw_test;

SELECT to_regprocedure('valkey_fdw_test_probe(text,int,bytea[])') IS NULL
           AS probe_undefined,
       to_regprocedure('valkey_fdw_test_ping(text,int,int,int)') IS NULL
           AS ping_undefined;

-- The wrapper is untouched by that: one shared library, two sets of catalog
-- entries, and a scan still works with the diagnostics gone.
SELECT valkey_fdw_version() > 0 AS wrapper_still_loaded;
SELECT k, v FROM priv_allowed_t ORDER BY k;

CREATE EXTENSION valkey_fdw_test;
SELECT to_regprocedure('valkey_fdw_test_probe(text,int,bytea[])') IS NOT NULL
           AS probe_back;

-- ---------------------------------------------------------------------------
SELECT valkey_fdw_test_poke('priv_srv', 'priv:allowed:k1'::bytea, NULL)
    AS deleted_allowed;
SELECT valkey_fdw_test_poke('priv_srv', 'priv:secret:k1'::bytea, NULL)
    AS deleted_secret;

DROP FOREIGN TABLE priv_allowed_t;
DROP FOREIGN TABLE priv_secret_t;
DROP USER MAPPING FOR priv_reader SERVER priv_srv;
DROP USER MAPPING FOR priv_prober SERVER priv_srv;
DROP USER MAPPING FOR CURRENT_USER SERVER priv_srv;
DROP USER MAPPING FOR PUBLIC SERVER priv_other;
DROP USER MAPPING FOR priv_owner SERVER priv_owned;
DROP SERVER priv_srv;
DROP SERVER priv_other;
DROP SERVER priv_owned;
REVOKE USAGE ON FOREIGN DATA WRAPPER valkey_fdw FROM priv_owner;
DROP ROLE priv_reader;
DROP ROLE priv_prober;
DROP ROLE priv_owner;
DROP FUNCTION priv_sqlstate(text);
