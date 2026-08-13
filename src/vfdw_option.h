/*-------------------------------------------------------------------------
 *
 * vfdw_option.h
 *		Data-driven option definitions for valkey_fdw.
 *
 * Invariant I7: options are data, not code. One table drives validation,
 * defaults, and every runtime read, so a rule cannot be silently disabled by
 * a typo in a strcmp chain.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_option.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_OPTION_H
#define VFDW_OPTION_H

#include "vfdw.h"

#include "nodes/pg_list.h"

typedef enum VfdwOptKind
{
	VFDW_OPT_STRING,
	VFDW_OPT_INT,
	VFDW_OPT_BOOL,
	VFDW_OPT_ENUM,

	/*
	 * A filesystem path, which must be absolute. The server resolves these
	 * relative to its own working directory, so a relative path is almost
	 * never what the user meant and fails much later, at connection time,
	 * with a confusing error.
	 */
	VFDW_OPT_PATH
} VfdwOptKind;

/*
 * Who may set an option.
 *
 * This lives in the table beside the option it governs because of invariant
 * I7: options are data, not code. A privilege rule written as an if in the
 * validator would be exactly the strcmp chain I7 exists to prevent - rename
 * the option in the table and the guard goes quietly dead, still compiling,
 * still passing, protecting nothing.
 */
typedef enum VfdwOptPriv
{
	VFDW_OPTPRIV_NONE = 0,

	/* Only a superuser may set it at all. */
	VFDW_OPTPRIV_SUPERUSER,

	/*
	 * Anyone may set it true; only a superuser may set it false.
	 *
	 * The asymmetry is deliberate: the restricted direction is the one that
	 * REMOVES a check. Turning the check on grants nobody anything, so it
	 * needs no privilege; turning it off is the operation that hands a role
	 * reach it did not already have.
	 */
	VFDW_OPTPRIV_SUPERUSER_TO_DISABLE
} VfdwOptPriv;

typedef struct VfdwOptionDef
{
	const char *name;

	/*
	 * Catalog the option may appear on: ForeignServerRelationId,
	 * UserMappingRelationId, ForeignTableRelationId, or AttributeRelationId
	 * for per-column options.
	 */
	Oid			context;
	VfdwOptKind kind;

	/* Inclusive bounds; VFDW_OPT_INT only. */
	int			minval;
	int			maxval;

	/* NULL-terminated permitted values; VFDW_OPT_ENUM only. */
	const char *const *values;

	/* Documentation default, or NULL when the option has none. */
	const char *defval;

	/*
	 * Redact the value from error messages, log output and EXPLAIN. Set for
	 * anything credential-bearing.
	 */
	bool		sensitive;

	/*
	 * Enforced by vfdw_validate_option, which every DDL path already reaches,
	 * so an option cannot acquire a privilege rule that only some of the ways
	 * of setting it respect.
	 */
	VfdwOptPriv priv;

	const char *summary;
} VfdwOptionDef;

/*
 * The option table, terminated by an entry with a NULL name. Exposed so that
 * tests can enumerate it and assert every entry is reachable from both the
 * validator and the runtime readers.
 */
extern const VfdwOptionDef vfdw_options[];

extern const VfdwOptionDef *vfdw_find_option(const char *name, Oid context);
extern void vfdw_validate_option(const VfdwOptionDef *def, const char *value);

/* Parsers shared by the validator and the runtime readers. */
extern int	vfdw_parse_int(const VfdwOptionDef *def, const char *value);
extern bool vfdw_parse_bool(const VfdwOptionDef *def, const char *value);
extern int	vfdw_parse_enum(const VfdwOptionDef *def, const char *value);
extern const char *vfdw_parse_path(const VfdwOptionDef *def, const char *value);

/*
 * Resolved option sets.
 *
 * Read once per connection rather than per command, from the same table that
 * validates them, so a value can never be accepted at DDL time and then
 * interpreted differently at runtime.
 */
typedef struct VfdwServerOptions
{
	const char *host;
	int			port;
	const char *unix_socket_path;
	bool		cluster;
	const char *cluster_nodes;
	bool		prefer_replica;
	bool		tls;
	const char *tls_ca_file;
	const char *tls_cert_file;
	const char *tls_key_file;
	const char *tls_sni;
	int			tls_verify;		/* index into the tls_verify enum */
	int			connect_timeout_ms;
	int			command_timeout_ms;
	int			pipeline_batch;
	int			scan_count;
	int			max_reply_elements;
	int			reader_buffer_bytes;
	int			write_max_ops;
	int			write_max_bytes;
	int			write_retry_count;
} VfdwServerOptions;

typedef struct VfdwUserOptions
{
	const char *username;
	const char *password;
	bool		password_required;
} VfdwUserOptions;

extern void vfdw_read_server_options(List *options, VfdwServerOptions *out);
extern void vfdw_read_user_options(List *options, VfdwUserOptions *out);

#endif							/* VFDW_OPTION_H */
