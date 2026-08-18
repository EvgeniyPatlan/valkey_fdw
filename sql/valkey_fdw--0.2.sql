/* valkey_fdw--0.1.sql */

\echo Use "CREATE EXTENSION valkey_fdw" to load this file. \quit

CREATE FUNCTION valkey_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION valkey_fdw_validator(text[], oid)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER valkey_fdw
  HANDLER valkey_fdw_handler
  VALIDATOR valkey_fdw_validator;

CREATE FUNCTION valkey_fdw_version()
RETURNS int
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

COMMENT ON FUNCTION valkey_fdw_version()
IS 'valkey_fdw code version';

CREATE FUNCTION valkey_fdw_libvalkey_version()
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

COMMENT ON FUNCTION valkey_fdw_libvalkey_version()
IS 'Version of the libvalkey client this module is running against';

/*
 * The full option table, exposed so that documentation and tests can be
 * checked against what the code actually accepts rather than against a
 * hand-maintained list.
 */
CREATE FUNCTION valkey_fdw_options(
    OUT name        text,
    OUT context     text,
    OUT type        text,
    OUT default_value text,
    OUT sensitive   boolean,
    OUT requires_superuser boolean)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

COMMENT ON FUNCTION valkey_fdw_options()
IS 'Every option valkey_fdw accepts, with its context, type and default';

