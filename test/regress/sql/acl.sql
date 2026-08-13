-- Authentication and authorisation against a server with ACLs.
--
-- The default user is disabled on this topology, so a connection that fails
-- to AUTH cannot silently fall back to unrestricted access. That makes each
-- failure below distinguishable: no credentials, wrong credentials, and valid
-- credentials without the permission needed are three different answers, and
-- a client that reports them as one error is telling the operator nothing.

CREATE TEMP TABLE vkout(line text);

CREATE FUNCTION vk(cmd text) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    DELETE FROM vkout;
    EXECUTE format('COPY vkout FROM PROGRAM %L',
                   'valkey-cli -h valkey --user fdw_admin --pass admin_pw '
                   || cmd || ' >/dev/null 2>&1; echo ok');
END $$;

SELECT vk('FLUSHALL');
SELECT vk('SET vfdw:a alpha');
SELECT vk('SET vfdw:b beta');

CREATE SERVER acl_srv FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');

-- ---------------------------------------------------------------------------
-- Valid credentials.
-- ---------------------------------------------------------------------------
CREATE USER MAPPING FOR CURRENT_USER SERVER acl_srv
    OPTIONS (username 'fdw_app', password 'app_pw');

SELECT valkey_fdw_test_pooled_ping('acl_srv') AS authenticated;

-- fdw_app may read the test keyspace, so a scan works end to end.
CREATE FOREIGN TABLE acl_t (k text, v text) SERVER acl_srv
    OPTIONS (keyprefix 'vfdw:');
SELECT k, v FROM acl_t ORDER BY k;

DROP FOREIGN TABLE acl_t;
DROP USER MAPPING FOR CURRENT_USER SERVER acl_srv;

-- ---------------------------------------------------------------------------
-- No credentials at all.
--
-- The default user is off, so the server answers NOAUTH rather than serving
-- the connection unauthenticated.
-- ---------------------------------------------------------------------------
CREATE USER MAPPING FOR CURRENT_USER SERVER acl_srv;
SELECT valkey_fdw_test_pooled_ping('acl_srv');
DROP USER MAPPING FOR CURRENT_USER SERVER acl_srv;

-- ---------------------------------------------------------------------------
-- Wrong password, and an unknown user.
--
-- Both must report an authentication failure, and neither may echo the
-- credential that was tried: the password option is sensitive, so it stays
-- out of the message, the detail and the hint.
-- ---------------------------------------------------------------------------
CREATE USER MAPPING FOR CURRENT_USER SERVER acl_srv
    OPTIONS (username 'fdw_app', password 'wrong_pw');
SELECT valkey_fdw_test_pooled_ping('acl_srv');
DROP USER MAPPING FOR CURRENT_USER SERVER acl_srv;

CREATE USER MAPPING FOR CURRENT_USER SERVER acl_srv
    OPTIONS (username 'no_such_user', password 'app_pw');
SELECT valkey_fdw_test_pooled_ping('acl_srv');
DROP USER MAPPING FOR CURRENT_USER SERVER acl_srv;

-- A password with no username authenticates as the default user, which is
-- disabled here.
CREATE USER MAPPING FOR CURRENT_USER SERVER acl_srv
    OPTIONS (password 'app_pw');
SELECT valkey_fdw_test_pooled_ping('acl_srv');
DROP USER MAPPING FOR CURRENT_USER SERVER acl_srv;

-- ---------------------------------------------------------------------------
-- Authenticated but not permitted.
--
-- fdw_noperm can connect and nothing else. Authorisation failure must not be
-- reported as authentication failure - the operator's fix is different.
-- ---------------------------------------------------------------------------
CREATE USER MAPPING FOR CURRENT_USER SERVER acl_srv
    OPTIONS (username 'fdw_noperm', password 'noperm_pw');

SELECT valkey_fdw_test_pooled_ping('acl_srv') AS noperm_can_connect;

CREATE FOREIGN TABLE noperm_t (k text, v text) SERVER acl_srv
    OPTIONS (keyprefix 'vfdw:');
SELECT k, v FROM noperm_t ORDER BY k;
DROP FOREIGN TABLE noperm_t;

DROP USER MAPPING FOR CURRENT_USER SERVER acl_srv;

-- ---------------------------------------------------------------------------
-- A non-superuser may not connect without a password.
--
-- postgres_fdw's rule, and the reason for it: the server would otherwise
-- authenticate using whatever ambient identity the PostgreSQL process has -
-- a peer-authenticated socket, a client certificate - which the SQL-level
-- user never presented and cannot be held responsible for.
-- ---------------------------------------------------------------------------
CREATE ROLE acl_nosuper LOGIN;
GRANT USAGE ON FOREIGN SERVER acl_srv TO acl_nosuper;
CREATE USER MAPPING FOR acl_nosuper SERVER acl_srv OPTIONS (username 'fdw_app');

-- EXECUTE as well as USAGE, or this asserts nothing. The diagnostic functions
-- are revoked from PUBLIC, so without the grant this role is refused at the
-- function ACL and never reaches the password rule below - which would leave
-- the rule's own refusal untested while the suite still went green.
GRANT EXECUTE ON FUNCTION valkey_fdw_test_pooled_ping(text) TO acl_nosuper;

SET SESSION AUTHORIZATION acl_nosuper;
SELECT valkey_fdw_test_pooled_ping('acl_srv');
RESET SESSION AUTHORIZATION;

-- Only a superuser may waive it, and then the connection is attempted and
-- fails on its own merits rather than being refused locally.
ALTER USER MAPPING FOR acl_nosuper SERVER acl_srv
    OPTIONS (ADD password_required 'false');
SET SESSION AUTHORIZATION acl_nosuper;
SELECT valkey_fdw_test_pooled_ping('acl_srv');
RESET SESSION AUTHORIZATION;

DROP USER MAPPING FOR acl_nosuper SERVER acl_srv;
REVOKE USAGE ON FOREIGN SERVER acl_srv FROM acl_nosuper;
DROP ROLE acl_nosuper;

-- ---------------------------------------------------------------------------
-- The classification a client can actually branch on.
--
-- The messages above are for people; SQLSTATE is for programs. Distinct codes
-- are the whole point of mapping Valkey's error prefixes: a caller retrying
-- on a bad password must not also retry on a missing permission. Reporting
-- every one of these as a bare ERRCODE_FDW_ERROR makes that impossible.
-- ---------------------------------------------------------------------------
CREATE FUNCTION ping_sqlstate(srv text) RETURNS text
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM valkey_fdw_test_pooled_ping(srv);
    RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE;
END $$;

CREATE SERVER acl_codes FOREIGN DATA WRAPPER valkey_fdw
    OPTIONS (host 'valkey', port '6379');

CREATE USER MAPPING FOR CURRENT_USER SERVER acl_codes;
SELECT ping_sqlstate('acl_codes') AS no_credentials;
DROP USER MAPPING FOR CURRENT_USER SERVER acl_codes;

CREATE USER MAPPING FOR CURRENT_USER SERVER acl_codes
    OPTIONS (username 'fdw_app', password 'wrong_pw');
SELECT ping_sqlstate('acl_codes') AS wrong_password;
DROP USER MAPPING FOR CURRENT_USER SERVER acl_codes;

CREATE USER MAPPING FOR CURRENT_USER SERVER acl_codes
    OPTIONS (username 'fdw_noperm', password 'noperm_pw');
CREATE FOREIGN TABLE codes_t (k text, v text) SERVER acl_codes
    OPTIONS (keyprefix 'vfdw:');

DO $$
DECLARE
    n bigint;
BEGIN
    SELECT count(*) INTO n FROM codes_t;
    RAISE NOTICE 'no error';
EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'insufficient permission: %', SQLSTATE;
END $$;

DROP SERVER acl_codes CASCADE;
DROP FUNCTION ping_sqlstate(text);

DROP SERVER acl_srv CASCADE;
DROP FUNCTION vk(text);
