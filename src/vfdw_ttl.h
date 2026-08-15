/*
 * vfdw_ttl.h
 *		Per-field time to live: what to ask a server, and whether it can answer.
 *
 * SEPARATE FROM THE SCAN because a ttl column is not read the way every other
 * column is read. Every other column's value arrives inside the reply that
 * fetched the key - HGETALL carries the fields, ZRANGE carries the members -
 * and the row builder takes it from there. A field's expiry is not in any of
 * those replies. It is a second question about the same key, asked with a
 * second command, and the only thing the scan has to know about it is that one
 * key now costs two replies instead of one.
 *
 * SEPARATE FROM vfdw_conn.c because the capability question is this file's
 * subject and not the connection's. What the connection offers is a place to
 * cache the answer for as long as the answer can be trusted, which is the life
 * of the connection.
 *
 * ASKED, NOT INFERRED. The verdict comes from COMMAND INFO rather than from a
 * version string, for the reason vfdw_conn.h gives about the write path: a
 * server that does not have the command cannot answer whatever it calls
 * itself, and one that has it can. Valkey 9 introduced per-field expiry and 8
 * has no such command at all, but that is a fact about two releases rather
 * than the rule, and a fork or a backport would make the version wrong while
 * leaving the question answerable.
 */
#ifndef VFDW_TTL_H
#define VFDW_TTL_H

#include "postgres.h"

#include "vfdw_cmd.h"
#include "vfdw_conn.h"
#include "vfdw_map.h"

/*
 * Whether this connection's server can do per-field expiry, raising if it
 * cannot. Asked of the three verbs actually sent - HPTTL to read, HPEXPIRE and
 * HPERSIST to write - rather than of one taken to imply the others.
 *
 * Raises rather than returning a verdict because there is exactly one thing to
 * do with a no: a table with a ttl column can be neither read from nor written
 * to this server, and every caller would otherwise write the same refusal. The probe runs once per
 * connection and the answer is cached on it.
 */
extern void vfdw_ttl_require(VfdwConn *vconn);

/*
 * HPTTL for one key, naming every ttl column's field.
 *
 * One command per key and not one per column: HPTTL takes a field list, so a
 * table with three ttl columns still costs one reply. The fields are named in
 * map order, which is what makes the reply's positions readable back.
 */
extern void vfdw_ttl_command(VfdwCmd *cmd, const VfdwTableMap *map,
							 const char *key, size_t keylen);

/*
 * Copy the reply's milliseconds into out[], one per ttl column in map order.
 *
 * COPIED, NOT BORROWED, and that is the point of this function existing rather
 * than the row builder reading the reply where it lies. A batch reply stays
 * valid only until the next vfdw_batch_next, and the ttl reply is taken BEFORE
 * the value reply so that the value reply - the large one, the one the tuple
 * is mostly built from - is the one still live while the tuple is built. By
 * then this reply is gone, so what survives has to be these integers.
 *
 * out must hold map->nttl entries.
 */
extern void vfdw_ttl_take(const VfdwTableMap *map, valkeyReply *reply,
						  int64 *out);

/*
 * The server's answer for one field, as a Datum.
 *
 * Returns false when the column is NULL, which covers both of the server's
 * two negative answers and deliberately does not distinguish them: -1 is a
 * field with no expiry and -2 is a field that is not there, and neither is a
 * duration. A field that is absent has already left the row's other columns
 * NULL by the same reasoning.
 */
extern bool vfdw_ttl_datum(int64 ms, Datum *value);

#endif							/* VFDW_TTL_H */
