/*-------------------------------------------------------------------------
 *
 * vfdw_vec.h
 *		Vectors, between PostgreSQL's text form and Valkey's bytes.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_vec.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_VEC_H
#define VFDW_VEC_H

#include "vfdw.h"

/*
 * Both directions take a `what` that names the thing being converted - a
 * column, or the query vector - and put it in the error message.
 *
 * Not a convenience. A vector of the wrong length or the wrong shape is a
 * fact about one particular column or one particular expression, and a
 * message that says only "invalid vector" leaves a user with a table of many
 * columns and nothing to look at.
 */
extern char *vfdw_vec_to_text(const char *bytes, size_t len, const char *what);

/*
 * Returns palloc'd bytes and their length; the result is NOT NUL-terminated
 * and must not be measured with strlen. A vector containing 0.0f contains
 * four NUL bytes, which is the ordinary case and not an edge one (invariant
 * I3).
 */
extern char *vfdw_vec_from_text(const char *text, const char *what,
								size_t *lenp);

#endif							/* VFDW_VEC_H */
