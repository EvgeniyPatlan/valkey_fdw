/*-------------------------------------------------------------------------
 *
 * vfdw_option.c
 *		Option table, validation and parsing for valkey_fdw.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_option.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_option.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#include "commands/defrem.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "utils/builtins.h"


static const char *const vfdw_tabletype_values[] = {
	"string", "hash", "list", "set", "zset", NULL
};

static const char *const vfdw_tlsverify_values[] = {
	"full", "ca", "none", NULL
};

static const char *const vfdw_indextype_values[] = {
	"tag", "numeric", "vector", NULL
};

/*
 * Every option valkey_fdw accepts, in one place.
 *
 * Adding a row here is the only step needed to make an option valid; the
 * validator and the runtime readers both drive off this table. test/unit
 * enumerates it and asserts each entry round-trips, so an entry can never
 * become unreachable the way a hand-written strcmp chain can.
 */
const VfdwOptionDef vfdw_options[] = {
	/* ---- Server ---- */
	{"host", ForeignServerRelationId, VFDW_OPT_STRING, 0, 0, NULL,
		"127.0.0.1", false, VFDW_OPTPRIV_NONE, "Valkey host name or address"},
	{"port", ForeignServerRelationId, VFDW_OPT_INT, 1, 65535, NULL,
		"6379", false, VFDW_OPTPRIV_NONE, "Valkey TCP port"},
	{"unix_socket_path", ForeignServerRelationId, VFDW_OPT_STRING, 0, 0, NULL,
		NULL, false, VFDW_OPTPRIV_NONE, "Unix socket path; overrides host and port"},
	{"cluster", ForeignServerRelationId, VFDW_OPT_BOOL, 0, 0, NULL,
		"false", false, VFDW_OPTPRIV_NONE, "Treat the server as a Valkey Cluster"},
	/*
	 * ROUTING IS NOT IMPLEMENTED, and the description below is worded so that
	 * nobody has to read this comment to find that out. Reads go wherever the
	 * server's own host and port, or the slot map, already point; nothing on
	 * the read path consults this flag. An option whose published description
	 * promises behaviour the code does not have is worse than no option, and
	 * that promise is the part that gets removed - not the option.
	 *
	 * What the flag does do is declare the server read-only, which
	 * vfdw_modify_connect enforces through vfdw_refuse_prefer_replica. That
	 * is why it is not rejected at DDL time as unimplemented: pointing a
	 * foreign server at a replica is deliberate, and the declaration is the
	 * only way this wrapper can be told. Without it, a write plans, executes
	 * every row and dies at PRE_COMMIT on the server's own READONLY, after
	 * the transaction has done all of its work and against a message naming
	 * the script. With it the statement is refused where it was written, with
	 * a hint naming the server. Refusing the option would delete that.
	 *
	 * The refusal has to be a check in PlanForeignModify, where the server is
	 * reachable, and not a bit withheld by vfdw_map_writability - that
	 * function takes a relid and reads TABLE options, so it structurally
	 * cannot see a SERVER option, and IsForeignRelUpdatable would keep
	 * reporting such a table as updatable.
	 *
	 * When routing lands it adds to this, and the description gains a clause;
	 * the write refusal is independent of it and stays as it is.
	 */
	{"prefer_replica", ForeignServerRelationId, VFDW_OPT_BOOL, 0, 0, NULL,
		"false", false, VFDW_OPTPRIV_NONE, "Treat the server as a replica: writes through it are refused"},
	{"tls", ForeignServerRelationId, VFDW_OPT_BOOL, 0, 0, NULL,
		"false", false, VFDW_OPTPRIV_NONE, "Use TLS for the connection"},

	/*
	 * The three file names are superuser-only.
	 *
	 * Each is an absolute path the backend process opens with its own
	 * privileges, not the caller's, on a machine the caller may have no
	 * account on. Owning the catalog entry is not the same authority as
	 * choosing which of the server's files it reads, so the privilege is a
	 * property of the option rather than of the object it hangs off.
	 */
	{"tls_ca_file", ForeignServerRelationId, VFDW_OPT_PATH, 0, 0, NULL,
		NULL, false, VFDW_OPTPRIV_SUPERUSER, "Path to the CA certificate bundle"},
	{"tls_cert_file", ForeignServerRelationId, VFDW_OPT_PATH, 0, 0, NULL,
		NULL, false, VFDW_OPTPRIV_SUPERUSER, "Path to the client certificate"},
	{"tls_key_file", ForeignServerRelationId, VFDW_OPT_PATH, 0, 0, NULL,
		NULL, true, VFDW_OPTPRIV_SUPERUSER, "Path to the client private key"},
	{"tls_sni", ForeignServerRelationId, VFDW_OPT_STRING, 0, 0, NULL,
		NULL, false, VFDW_OPTPRIV_NONE, "Server name sent in the TLS SNI extension"},
	{"tls_verify", ForeignServerRelationId, VFDW_OPT_ENUM, 0, 0, vfdw_tlsverify_values,
		"full", false, VFDW_OPTPRIV_NONE, "Certificate verification strictness"},
	{"connect_timeout_ms", ForeignServerRelationId, VFDW_OPT_INT, 1, 3600000, NULL,
		"5000", false, VFDW_OPTPRIV_NONE, "Connection establishment timeout"},
	{"command_timeout_ms", ForeignServerRelationId, VFDW_OPT_INT, 1, 3600000, NULL,
		"30000", false, VFDW_OPTPRIV_NONE, "Per-command response timeout"},
	{"pipeline_batch", ForeignServerRelationId, VFDW_OPT_INT, 1, 100000, NULL,
		"256", false, VFDW_OPTPRIV_NONE, "Maximum commands in flight when fetching values"},
	{"scan_count", ForeignServerRelationId, VFDW_OPT_INT, 1, 1000000, NULL,
		"1000", false, VFDW_OPTPRIV_NONE, "COUNT hint passed to SCAN and SSCAN"},

	/*
	 * Reader bounds.
	 *
	 * max_reply_elements is a real limit: a multi-bulk reply declares how many
	 * elements follow, and the reader refuses to begin one that claims more
	 * than this. That declaration arrives before any of the elements do, so
	 * without a cap a single lying header commits the backend to an allocation
	 * nobody asked for.
	 *
	 * reader_buffer_bytes is not a limit of any kind, and is named for what it
	 * is: libvalkey's threshold for how much read buffer to keep between
	 * replies rather than return to the allocator. A large value trades memory
	 * for fewer allocations on a scan; it does not bound how large a reply may
	 * be.
	 *
	 * What does bound it is not an option at all. libvalkey has no
	 * maximum-length field, so the ceiling lives in the allocator it calls
	 * through, fixed and out of reach of any statement - see vfdw_io.c, which
	 * explains why a per-server value was the wrong shape for a hook that is
	 * global to the process.
	 */
	{"max_reply_elements", ForeignServerRelationId, VFDW_OPT_INT, 1, 1000000000, NULL,
		"8388608", false, VFDW_OPTPRIV_NONE, "Most elements a multi-bulk reply may declare"},
	{"reader_buffer_bytes", ForeignServerRelationId, VFDW_OPT_INT, 1024, 1073741824, NULL,
		"65536", false, VFDW_OPTPRIV_NONE, "Read buffer retained between replies"},

	/*
	 * Bounds on a transaction's deferred write buffer.
	 *
	 * Writes are held in backend memory until pre-commit, so an unbounded
	 * transaction is an unbounded allocation. Both caps are checked at the row
	 * that crosses them, which is the only point where the error can name what
	 * the user did. They are separate because a million tiny operations and a
	 * hundred enormous ones are different problems.
	 */
	{"write_max_ops", ForeignServerRelationId, VFDW_OPT_INT, 1, 1000000, NULL,
		"10000", false, VFDW_OPTPRIV_NONE, "Most write operations one transaction may defer"},
	{"write_max_bytes", ForeignServerRelationId, VFDW_OPT_INT, 65536, 1073741824, NULL,
		"67108864", false, VFDW_OPTPRIV_NONE, "Most bytes of deferred write data one transaction may hold"},

	/*
	 * Bounds the TRYAGAIN/LOADING retry at pre-commit. Both prove the program
	 * did not run, so a retry is safe rather than hopeful - but it happens
	 * inside COMMIT, where the client is already waiting, so it is bounded
	 * rather than persistent. 0 disables retrying entirely.
	 */
	{"write_retry_count", ForeignServerRelationId, VFDW_OPT_INT, 0, 10, NULL,
		"3", false, VFDW_OPTPRIV_NONE, "Retries for a write the server was too busy to run"},

	/* ---- User mapping ---- */
	{"username", UserMappingRelationId, VFDW_OPT_STRING, 0, 0, NULL,
		NULL, false, VFDW_OPTPRIV_NONE, "ACL user name for AUTH"},
	{"password", UserMappingRelationId, VFDW_OPT_STRING, 0, 0, NULL,
		NULL, true, VFDW_OPTPRIV_NONE, "Password for AUTH"},

	/*
	 * Turning this off is postgres_fdw's superuser-only operation, and for the
	 * same reason: a mapping with no password reaches Valkey as the PostgreSQL
	 * server process, so any Valkey instance that trusts the database host by
	 * address is reachable by whoever holds USAGE on the foreign server. The
	 * refusal in vfdw_conn.c states how to disable the check, which is only
	 * honest advice if disabling it is itself privileged.
	 */
	{"password_required", UserMappingRelationId, VFDW_OPT_BOOL, 0, 0, NULL,
		"true", false, VFDW_OPTPRIV_SUPERUSER_TO_DISABLE,
		"Require a password for non-superuser mappings"},

	/* ---- Foreign table ---- */
	{"database", ForeignTableRelationId, VFDW_OPT_INT, 0, 15, NULL,
		"0", false, VFDW_OPTPRIV_NONE, "Valkey logical database number; not valid in cluster mode"},
	{"tabletype", ForeignTableRelationId, VFDW_OPT_ENUM, 0, 0, vfdw_tabletype_values,
		"string", false, VFDW_OPTPRIV_NONE, "Valkey type the rows of this table are drawn from"},
	{"keyprefix", ForeignTableRelationId, VFDW_OPT_STRING, 0, 0, NULL,
		NULL, false, VFDW_OPTPRIV_NONE, "Literal key prefix scoping the table"},
	{"keyset", ForeignTableRelationId, VFDW_OPT_STRING, 0, 0, NULL,
		NULL, false, VFDW_OPTPRIV_NONE, "Name of a set holding the table's key names"},
	{"singleton_key", ForeignTableRelationId, VFDW_OPT_STRING, 0, 0, NULL,
		NULL, false, VFDW_OPTPRIV_NONE, "Draw all rows from this single key"},
	/*
	 * ACCEPTED AND REFUSED, WHICH IS A POSITION RATHER THAN AN OVERSIGHT.
	 *
	 * search_index and legacy_value here, and ttl, distance and index_type in
	 * the column group below, all validate and then do not do what they name:
	 * the first plan over a legacy_value, ttl or distance table raises 0A000,
	 * and the two search options are simply never consulted - the ordinary key
	 * path answers, no qual reaches an index. They are accepted so that a
	 * table definition can be written ahead of the feature.
	 *
	 * The rule that keeps this from being a broken promise: an option may be
	 * accepted and refused only where a suite asserts the exact refusal and
	 * the README says what it is waiting for. Both halves carry weight. An
	 * unasserted refusal is free to decay into a crash or - worse, because it
	 * looks like an answer - a plausible empty result, with nothing going red;
	 * and a refusal stated nowhere is indistinguishable from the outside from
	 * an option somebody forgot to finish. test/regress/sql/ddl.sql holds all
	 * five: the three refusals to their exact message, and the two search
	 * options to the unchanged answer the key path gives with them set. An
	 * option whose boundary cannot be spelled out there does not belong in
	 * this table.
	 */
	{"search_index", ForeignTableRelationId, VFDW_OPT_STRING, 0, 0, NULL,
		NULL, false, VFDW_OPTPRIV_NONE, "valkey-search index backing this table"},
	{"legacy_value", ForeignTableRelationId, VFDW_OPT_BOOL, 0, 0, NULL,
		"false", false, VFDW_OPTPRIV_NONE, "Expose the legacy (key, value) shape"},
	{"readonly", ForeignTableRelationId, VFDW_OPT_BOOL, 0, 0, NULL,
		"false", false, VFDW_OPTPRIV_NONE, "Reject all write operations on this table"},

	/* ---- Column ---- */
	{"key", AttributeRelationId, VFDW_OPT_BOOL, 0, 0, NULL,
		"false", false, VFDW_OPTPRIV_NONE, "This column holds the Valkey key name"},
	{"field", AttributeRelationId, VFDW_OPT_STRING, 0, 0, NULL,
		NULL, false, VFDW_OPTPRIV_NONE, "Hash field name"},
	{"member", AttributeRelationId, VFDW_OPT_BOOL, 0, 0, NULL,
		"false", false, VFDW_OPTPRIV_NONE, "This column holds the list, set or zset member"},
	{"score", AttributeRelationId, VFDW_OPT_BOOL, 0, 0, NULL,
		"false", false, VFDW_OPTPRIV_NONE, "This column holds the zset score"},
	{"ttl", AttributeRelationId, VFDW_OPT_BOOL, 0, 0, NULL,
		"false", false, VFDW_OPTPRIV_NONE, "This column holds the paired field's time to live"},
	{"distance", AttributeRelationId, VFDW_OPT_BOOL, 0, 0, NULL,
		"false", false, VFDW_OPTPRIV_NONE, "This column receives the vector search score"},
	{"index_type", AttributeRelationId, VFDW_OPT_ENUM, 0, 0, vfdw_indextype_values,
		NULL, false, VFDW_OPTPRIV_NONE, "How the field is indexed in the search index"},

	{NULL, InvalidOid, VFDW_OPT_STRING, 0, 0, NULL, NULL, false,
		VFDW_OPTPRIV_NONE, NULL}
};

const VfdwOptionDef *
vfdw_find_option(const char *name, Oid context)
{
	const VfdwOptionDef *def;

	for (def = vfdw_options; def->name != NULL; def++)
	{
		if (def->context == context && strcmp(def->name, name) == 0)
			return def;
	}
	return NULL;
}

/*
 * Report a rejected option value.
 *
 * The offending text is quoted back so the user can see exactly what was
 * parsed. atoi silently yields 0 on garbage, so port 'abc' becomes the
 * default with nothing said about it; that is the failure mode this exists
 * to prevent. Values on options marked sensitive are redacted rather than
 * echoed.
 */
static void
vfdw_reject(const VfdwOptionDef *def, const char *value, const char *reason)
{
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("invalid value for option \"%s\"", def->name),
			 errdetail("%s", reason),
			 def->sensitive
			 ? errhint("The supplied value is not shown because \"%s\" is sensitive.",
					   def->name)
			 : errhint("Value was: \"%s\"", value)));
}

int
vfdw_parse_int(const VfdwOptionDef *def, const char *value)
{
	char	   *endptr;
	long		result;

	Assert(def->kind == VFDW_OPT_INT);

	if (*value == '\0')
		vfdw_reject(def, value, "an integer is required, but the value is empty");

	errno = 0;
	result = strtol(value, &endptr, 10);

	/* Trailing garbage is an error, not something to ignore. */
	while (*endptr != '\0' && isspace((unsigned char) *endptr))
		endptr++;
	if (*endptr != '\0')
		vfdw_reject(def, value, "an integer is required");

	if (errno == ERANGE || result < (long) def->minval || result > (long) def->maxval)
		vfdw_reject(def, value,
					psprintf("value must be between %d and %d",
							 def->minval, def->maxval));

	return (int) result;
}

bool
vfdw_parse_bool(const VfdwOptionDef *def, const char *value)
{
	bool		result;

	Assert(def->kind == VFDW_OPT_BOOL);

	if (!parse_bool(value, &result))
		vfdw_reject(def, value,
					"a boolean is required (on/off, true/false, yes/no, 1/0)");

	return result;
}

/*
 * Returns the zero-based index of value within def->values.
 */
int
vfdw_parse_enum(const VfdwOptionDef *def, const char *value)
{
	int			i;
	StringInfoData buf;

	Assert(def->kind == VFDW_OPT_ENUM && def->values != NULL);

	for (i = 0; def->values[i] != NULL; i++)
	{
		if (strcmp(def->values[i], value) == 0)
			return i;
	}

	initStringInfo(&buf);
	for (i = 0; def->values[i] != NULL; i++)
		appendStringInfo(&buf, "%s%s", i > 0 ? ", " : "", def->values[i]);

	vfdw_reject(def, value, psprintf("permitted values are: %s", buf.data));
	return -1;					/* unreachable; keeps the compiler quiet */
}

/*
 * Paths must be absolute.
 *
 * The PostgreSQL server resolves a relative path against its own working
 * directory, which is essentially never where the user keeps their
 * certificates. Catching it at DDL time turns a confusing connection-time
 * failure into an immediate, specific one.
 */
const char *
vfdw_parse_path(const VfdwOptionDef *def, const char *value)
{
	Assert(def->kind == VFDW_OPT_PATH);

	if (*value == '\0')
		vfdw_reject(def, value, "a filesystem path is required, but the value is empty");

	if (!is_absolute_path(value))
		vfdw_reject(def, value, "the path must be absolute");

	return value;
}

/*
 * Refuse a restricted option to a caller who may not set it.
 *
 * The rule itself is the priv column of the option table; nothing here names
 * an option, so an option cannot be renamed out from under its own guard.
 */
static void
vfdw_check_option_priv(const VfdwOptionDef *def, const char *value)
{
	/*
	 * THIS SEES THE WHOLE OPTION LIST, NOT THE CHANGE.
	 *
	 * transformGenericOptions hands an FDW validator the merged result of the
	 * statement, so a restricted option that is already set is presented again
	 * by every later ALTER of the same object, whatever that ALTER was for.
	 * The consequence is not subtle and is worth stating rather than being
	 * discovered: once a superuser sets tls_ca_file on a server, a
	 * non-superuser OWNER of that server can no longer run any
	 * ALTER SERVER ... OPTIONS on it at all.
	 *
	 * There is no narrower rule available. The validator is given the new
	 * option list and nothing else - not the old one - so "did this statement
	 * change the restricted option" is a question it cannot ask. Refusing is
	 * the direction that fails safe, and postgres_fdw lives with the same
	 * shape on password_required.
	 *
	 * Fail closed on the enum too. A row carrying a priv this function has
	 * never been taught is refused rather than waved through, so the failure
	 * of an incomplete edit is a denied DDL statement and not a silently
	 * ungoverned option.
	 */
	bool		restricted = true;

	switch (def->priv)
	{
		case VFDW_OPTPRIV_NONE:
			return;
		case VFDW_OPTPRIV_SUPERUSER:
			break;
		case VFDW_OPTPRIV_SUPERUSER_TO_DISABLE:
			restricted = !vfdw_parse_bool(def, value);
			break;
	}

	if (!restricted || superuser())
		return;

	if (def->priv == VFDW_OPTPRIV_SUPERUSER_TO_DISABLE)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to turn off option \"%s\"",
						def->name),
				 errdetail("Only a superuser may set \"%s\" false.", def->name),
				 errhint("Anyone may set it true; only switching it off removes "
						 "a check, which is why only that direction is "
						 "restricted.")));

	ereport(ERROR,
			(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			 errmsg("permission denied to set option \"%s\"", def->name),
			 errdetail("Only a superuser may set \"%s\".", def->name),
			 errhint("This option directs the PostgreSQL server process to act "
					 "with its own privileges rather than the caller's.")));
}

void
vfdw_validate_option(const VfdwOptionDef *def, const char *value)
{
	switch (def->kind)
	{
		case VFDW_OPT_STRING:
			/* Every byte sequence is a legal Valkey key, field or path. */
			break;
		case VFDW_OPT_INT:
			(void) vfdw_parse_int(def, value);
			break;
		case VFDW_OPT_BOOL:
			(void) vfdw_parse_bool(def, value);
			break;
		case VFDW_OPT_ENUM:
			(void) vfdw_parse_enum(def, value);
			break;
		case VFDW_OPT_PATH:
			(void) vfdw_parse_path(def, value);
			break;
	}

	/*
	 * Value first, privilege second. A non-superuser writing
	 * password_required 'nonsense' is told the value is invalid, which is
	 * true, rather than that they lack a privilege they would not have needed
	 * had they written something meaningful.
	 */
	vfdw_check_option_priv(def, value);
}

/*
 * Look an option's default up in the table rather than repeating it here.
 * Defaults stated twice drift, and the two that drift apart are the
 * documented default and the one the code actually applies.
 */
static const char *
vfdw_default(const char *name, Oid context)
{
	const VfdwOptionDef *def = vfdw_find_option(name, context);

	Assert(def != NULL);
	return def->defval;
}

static int
vfdw_default_int(const char *name)
{
	const VfdwOptionDef *def = vfdw_find_option(name, ForeignServerRelationId);

	Assert(def != NULL && def->defval != NULL);
	return vfdw_parse_int(def, def->defval);
}

/*
 * Start from what the option table says, so an option omitted from a server
 * definition behaves exactly as the documented default claims.
 */
static void
vfdw_server_defaults(VfdwServerOptions *out)
{
	memset(out, 0, sizeof(*out));
	out->host = vfdw_default("host", ForeignServerRelationId);
	out->port = vfdw_default_int("port");
	out->connect_timeout_ms = vfdw_default_int("connect_timeout_ms");
	out->command_timeout_ms = vfdw_default_int("command_timeout_ms");
	out->pipeline_batch = vfdw_default_int("pipeline_batch");
	out->scan_count = vfdw_default_int("scan_count");
	out->max_reply_elements = vfdw_default_int("max_reply_elements");
	out->reader_buffer_bytes = vfdw_default_int("reader_buffer_bytes");
	out->write_max_ops = vfdw_default_int("write_max_ops");
	out->write_retry_count = vfdw_default_int("write_retry_count");
	out->write_max_bytes = vfdw_default_int("write_max_bytes");
	out->tls_verify = 0;		/* "full" */
}

/*
 * Apply one recognised server option to the resolved set.
 *
 * Split from the loop that finds it because the two change for different
 * reasons: the loop follows the catalog, while this chain grows by a line
 * every time an option is added. Together they outgrow the length at which a
 * dispatch like this can still be checked option by option, and the per-option
 * clarity is the part worth keeping.
 */
static void
vfdw_apply_server_option(const VfdwOptionDef *def, char *value,
						 VfdwServerOptions *out)
{
	if (strcmp(def->name, "host") == 0)
		out->host = value;
	else if (strcmp(def->name, "port") == 0)
		out->port = vfdw_parse_int(def, value);
	else if (strcmp(def->name, "unix_socket_path") == 0)
		out->unix_socket_path = value;
	else if (strcmp(def->name, "cluster") == 0)
		out->cluster = vfdw_parse_bool(def, value);
	else if (strcmp(def->name, "prefer_replica") == 0)
		out->prefer_replica = vfdw_parse_bool(def, value);
	else if (strcmp(def->name, "tls") == 0)
		out->tls = vfdw_parse_bool(def, value);
	else if (strcmp(def->name, "tls_ca_file") == 0)
		out->tls_ca_file = vfdw_parse_path(def, value);
	else if (strcmp(def->name, "tls_cert_file") == 0)
		out->tls_cert_file = vfdw_parse_path(def, value);
	else if (strcmp(def->name, "tls_key_file") == 0)
		out->tls_key_file = vfdw_parse_path(def, value);
	else if (strcmp(def->name, "tls_sni") == 0)
		out->tls_sni = value;
	else if (strcmp(def->name, "tls_verify") == 0)
		out->tls_verify = vfdw_parse_enum(def, value);
	else if (strcmp(def->name, "connect_timeout_ms") == 0)
		out->connect_timeout_ms = vfdw_parse_int(def, value);
	else if (strcmp(def->name, "command_timeout_ms") == 0)
		out->command_timeout_ms = vfdw_parse_int(def, value);
	else if (strcmp(def->name, "pipeline_batch") == 0)
		out->pipeline_batch = vfdw_parse_int(def, value);
	else if (strcmp(def->name, "scan_count") == 0)
		out->scan_count = vfdw_parse_int(def, value);
	else if (strcmp(def->name, "max_reply_elements") == 0)
		out->max_reply_elements = vfdw_parse_int(def, value);
	else if (strcmp(def->name, "reader_buffer_bytes") == 0)
		out->reader_buffer_bytes = vfdw_parse_int(def, value);
	else if (strcmp(def->name, "write_max_ops") == 0)
		out->write_max_ops = vfdw_parse_int(def, value);
	else if (strcmp(def->name, "write_retry_count") == 0)
		out->write_retry_count = vfdw_parse_int(def, value);
	else if (strcmp(def->name, "write_max_bytes") == 0)
		out->write_max_bytes = vfdw_parse_int(def, value);
}

void
vfdw_read_server_options(List *options, VfdwServerOptions *out)
{
	ListCell   *lc;

	vfdw_server_defaults(out);

	foreach(lc, options)
	{
		DefElem    *elem = (DefElem *) lfirst(lc);
		const VfdwOptionDef *def;

		def = vfdw_find_option(elem->defname, ForeignServerRelationId);
		if (def == NULL)
			continue;			/* belongs to another catalog */

		vfdw_apply_server_option(def, defGetString(elem), out);
	}

}

void
vfdw_read_user_options(List *options, VfdwUserOptions *out)
{
	ListCell   *lc;

	memset(out, 0, sizeof(*out));
	out->password_required = true;

	foreach(lc, options)
	{
		DefElem    *elem = (DefElem *) lfirst(lc);
		const VfdwOptionDef *def;
		char	   *value;

		def = vfdw_find_option(elem->defname, UserMappingRelationId);
		if (def == NULL)
			continue;
		value = defGetString(elem);

		if (strcmp(def->name, "username") == 0)
			out->username = value;
		else if (strcmp(def->name, "password") == 0)
			out->password = value;
		else if (strcmp(def->name, "password_required") == 0)
			out->password_required = vfdw_parse_bool(def, value);
	}
}
