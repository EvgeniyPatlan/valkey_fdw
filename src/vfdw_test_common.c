/*-------------------------------------------------------------------------
 *
 * vfdw_test_common.c
 *		The authorisation every diagnostic entry point shares.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_test_common.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "vfdw.h"

#include "catalog/pg_foreign_server.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "utils/acl.h"

#include "vfdw_test_common.h"

/*
 * The server named by an argument, with the privilege the FDW machinery would
 * have checked had the caller reached this keyspace through a foreign table.
 *
 * Neither half of the obvious pair is an authorisation check.
 * GetForeignServerByName is a LOOKUP: it answers for any server in the
 * catalog, and the only question it can fail is whether the name exists.
 * GetUserMapping asks whether a mapping EXISTS for this role and this server,
 * and one CREATE USER MAPPING FOR PUBLIC gives every role in the cluster one.
 * So between them a role holding no grant on anything reaches a keyspace by
 * naming its server: that walks past readonly 'true', past the keyprefix that
 * scopes a table to one part of the keyspace, and past every per-table GRANT,
 * because none of the three is on the path a diagnostic takes.
 *
 * USAGE on the server is the grant to demand because it is what CREATE
 * FOREIGN TABLE against that server already requires. Asking for it here
 * refuses exactly the roles that could not have been given a table to read
 * this keyspace through in the first place.
 */
ForeignServer *
vfdw_test_server(const char *name)
{
	ForeignServer *server = GetForeignServerByName(name, false);
	AclResult	aclresult;

	aclresult = object_aclcheck(ForeignServerRelationId, server->serverid,
								GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FOREIGN_SERVER, name);

	return server;
}

/*
 * The entry points that take a host and a port instead of a server name.
 *
 * Nothing weaker than superuser is correct there, because there is no catalog
 * object to authorise against: the address is an argument, so no grant anyone
 * could hold is a statement about the machine being dialled, and USAGE on some
 * unrelated server does not become one. What these functions hand out is the
 * postmaster's own network reach, aimed by the caller.
 */
void
vfdw_test_require_superuser(void)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to open a Valkey connection by "
						"address"),
				 errdetail("This function connects to an arbitrary host and "
						   "port and is a diagnostic, not part of the "
						   "wrapper's supported surface."),
				 errhint("Define a foreign server and use the pooled probes "
						 "instead.")));
}
