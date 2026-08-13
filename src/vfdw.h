/*-------------------------------------------------------------------------
 *
 * vfdw.h
 *		Shared declarations for valkey_fdw.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_H
#define VFDW_H

#include "postgres.h"

#if PG_VERSION_NUM < 160000
#error "valkey_fdw requires PostgreSQL 16 or later"
#endif
#if PG_VERSION_NUM >= 190000
#error "valkey_fdw has not been validated against this PostgreSQL major version"
#endif

/*
 * Code version. Leading digits track the SQL API major version; the last two
 * digits are the minor. 0.1 -> 1.
 */
#define VFDW_CODE_VERSION 1

/*
 * Non-returning function declarations.
 *
 * PostgreSQL 18 replaced the trailing pg_attribute_noreturn() with a leading
 * pg_noreturn, so the attribute moved to the other end of the declaration.
 * Declaring through both spellings keeps one source working across 16 to 18.
 */
#if PG_VERSION_NUM >= 180000
#define VFDW_NORETURN		pg_noreturn
#define VFDW_NORETURN_TAIL
#else
#define VFDW_NORETURN
#define VFDW_NORETURN_TAIL	pg_attribute_noreturn()
#endif

/*
 * Error reporting.
 *
 * Invariant I2: nothing reaches error-reporting code unless it is known valid
 * in the database encoding, whoever produced it. A string libvalkey owns - a
 * reply body, a context's errstr - is one source of such bytes; the bytes this
 * wrapper formats itself are another, and the rule does not distinguish them.
 * A bytea key column carries arbitrary bytes by design, so a refusal that
 * echoed the key verbatim would write a raw 0xff to a UTF-8 database's log and
 * put it on the wire, where a client_encoding differing from the server
 * encoding makes the conversion run during error reporting. Callers copy into
 * palloc'd memory with the length, pass the copy through vfdw_safe_text, and
 * hand the result here. Invariant I1: nothing is freed on the way out of an
 * error path - a connection is discarded by the transaction callback in
 * src/vfdw_conn_xact.c and a reply by the reset callback its batch registers
 * on the surrounding memory context, never inline before an ereport.
 *
 * The detail argument is always passed through a "%s" format, never used as
 * a format string itself, so that bytes the wrapper did not write itself can
 * never be interpreted as format directives.
 */
extern void vfdw_ereport(int elevel, int sqlerrcode,
						 const char *msg, const char *detail);

#endif							/* VFDW_H */
