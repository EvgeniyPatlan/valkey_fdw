/*-------------------------------------------------------------------------
 *
 * vfdw_test_common.h
 *		Who may drive a diagnostic entry point.
 *
 * The diagnostic functions are spread over six files, split by what each one
 * exercises. Three of them - the probe, the connection and I/O entry points,
 * and the cluster ones - name a Valkey server and reach its keyspace without
 * ever going through a foreign table, so nothing on that path is the FDW's
 * own privilege machinery and the check has to be made here. The other three
 * take no server argument and reach no server: vfdw_testval.c and
 * vfdw_testwbuf.c render Datums and read the write buffer, and vfdw_testkeys.c
 * arrives through vfdw_probe_connect, which is already checked.
 *
 * Declared once rather than written out per file, because a check spelled per
 * file is a check the next entry point is added without.
 *
 * vfdw_testprobe_internal.h is deliberately not this place: it carries what
 * valkey_fdw_test_probe and valkey_fdw_test_keys both need and nothing else,
 * and neither vfdw_testfuncs.c nor vfdw_testcluster.c includes it.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_test_common.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_TEST_COMMON_H
#define VFDW_TEST_COMMON_H

#include "vfdw.h"

#include "foreign/foreign.h"

/*
 * The server named by an argument, once the caller is entitled to it. Every
 * entry point that takes a server name resolves it through here.
 */
extern ForeignServer *vfdw_test_server(const char *name);

/*
 * The gate for the entry points that take a host and a port instead.
 */
extern void vfdw_test_require_superuser(void);

#endif							/* VFDW_TEST_COMMON_H */
