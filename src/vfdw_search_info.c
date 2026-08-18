/*-------------------------------------------------------------------------
 *
 * vfdw_search_info.c
 *		Asking the index what it is, before ranking anything by it.
 *
 * Split from src/vfdw_search.c because it answers a different question and
 * the two together outgrow the file-length gate. That file builds and reads
 * the search; this one decides whether the search is the one the query meant.
 *
 * WHY THIS EXISTS AT ALL. A KNN query names a field and a k. It does NOT name
 * a distance metric - the index carries that, fixed at FT.CREATE - so a table
 * queried with <=> against an index built with DISTANCE_METRIC L2 returns k
 * rows, in order, with a distance column filled in, and every one of those
 * numbers is a Euclidean distance labelled as a cosine one. Nothing fails.
 * Nothing looks wrong. That is the single most dangerous shape in this whole
 * path, and the only defence is to ask.
 *
 * The same reasoning covers the other three: a field the index does not hold
 * as a vector cannot be ranked by, a query vector of the wrong dimension is a
 * different point in a different space, and an element type other than
 * FLOAT32 means src/vfdw_vec.c is encoding to a layout the index does not
 * use. Each is a wrong answer with no symptom.
 *
 * THE SERVER IS THE AUTHORITY, which is invariant I8's reasoning applied to a
 * second cache. The alternative was to have the table declare its index's
 * metric and dimension in options, and then two statements of one fact would
 * drift - with the wrong one winning silently, because the server never
 * objects.
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_search_info.c
 *
 *-------------------------------------------------------------------------
 */
#include "vfdw_search.h"

#include "vfdw_cmd.h"
#include "vfdw_error.h"
#include "vfdw_row.h"

/*
 * FT.INFO answers with pairs, and some of its values are themselves pairs.
 * The shape this file walks, for the parts it reads:
 *
 *		attributes -> [ attr, attr, ... ]
 *		attr       -> [ identifier <name> ... type <TAG|VECTOR|NUMERIC>
 *		                index [ dimensions <n> distance_metric <M>
 *		                        data_type <T> ... ] ]
 *
 * Every level is the same alternating name/value array, so every level is
 * walked with vfdw_scan_hash_lookup - the same function that finds a field in
 * a hash - rather than with a parser of its own.
 */

/*
 * A nested value: the child reply a name maps to, rather than its bytes.
 *
 * vfdw_scan_hash_lookup returns bytes, which is right for a leaf and useless
 * for a subtree. This is the same walk stopping one step earlier.
 */
static const valkeyReply *
vfdw_info_sub(const valkeyReply *reply, const char *name)
{
	size_t		i;
	size_t		nlen = strlen(name);

	if (reply == NULL ||
		(reply->type != VALKEY_REPLY_ARRAY && reply->type != VALKEY_REPLY_MAP))
		return NULL;

	for (i = 0; i + 1 < reply->elements; i += 2)
	{
		const valkeyReply *k = vfdw_reply_child(reply, i);

		if (k->str != NULL && k->len == nlen &&
			memcmp(k->str, name, nlen) == 0)
			return vfdw_reply_child(reply, i + 1);
	}
	return NULL;
}

/*
 * A leaf value as a palloc'd string, or NULL.
 *
 * Copied rather than pointed at, because it goes into an ereport and a
 * libvalkey-owned string must never do that (invariant I2). NUL-terminated
 * here because everything read through it is an identifier or a keyword -
 * never a document's bytes.
 */
static char *
vfdw_info_str(const valkeyReply *reply, const char *name)
{
	const char *data;
	size_t		len;

	if (reply == NULL ||
		(reply->type != VALKEY_REPLY_ARRAY && reply->type != VALKEY_REPLY_MAP))
		return NULL;

	if (!vfdw_scan_hash_lookup(reply, name, &data, &len) || data == NULL)
		return NULL;

	return pnstrdup(data, (Size) len);
}

/*
 * An integer leaf, or -1.
 *
 * FT.INFO spells some of its numbers as integers and some as strings -
 * dimensions arrives as an integer, num_docs as a string - so only the
 * integer spelling is read here and anything else answers -1. A parser for
 * the other spelling would be a second number syntax for the sake of a field
 * this file does not read, and -1 is a value the caller already has to
 * handle: an index whose dimension could not be read is one the server will
 * object to itself.
 */
static int64
vfdw_info_int(const valkeyReply *reply, const char *name)
{
	const valkeyReply *v = vfdw_info_sub(reply, name);

	if (v == NULL || v->type != VALKEY_REPLY_INTEGER)
		return -1;

	return v->integer;
}

/*
 * The attribute entry for one field name, or NULL if the index has no such
 * field.
 *
 * "identifier" is the field of the hash; "attribute" is the alias a query
 * uses. They are the same string unless FT.CREATE gave an AS, and this
 * matches on the identifier because that is what the table's field option
 * names - the hash's own field, which is also what the ordinary key path
 * would read.
 */
static const valkeyReply *
vfdw_info_attribute(valkeyReply *info, const char *field)
{
	const valkeyReply *attrs = vfdw_info_sub(info, "attributes");
	size_t		i;

	if (attrs == NULL || attrs->type != VALKEY_REPLY_ARRAY)
		return NULL;

	for (i = 0; i < attrs->elements; i++)
	{
		const valkeyReply *attr = vfdw_reply_child(attrs, i);
		char	   *id = vfdw_info_str(attr, "identifier");

		if (id != NULL && strcmp(id, field) == 0)
		{
			pfree(id);
			return attr;
		}
		if (id != NULL)
			pfree(id);
	}
	return NULL;
}

/*
 * Refuse a query whose index does not hold this field as a vector.
 *
 * index_type 'vector' on the column said it should; this says whether it
 * does. The two disagreeing is a table description that no longer matches the
 * index it names - which nothing else would report, because a search over a
 * TAG field simply returns something.
 */
static const valkeyReply *
vfdw_search_vector_attr(const VfdwKnnScan *knn, valkeyReply *info)
{
	const valkeyReply *attr = vfdw_info_attribute(info, knn->field);
	char	   *type;

	if (attr == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("index \"%s\" has no field \"%s\"",
						knn->index, knn->field),
				 errdetail("The column declares index_type 'vector' and names "
						   "that field, and the index does not index it."),
				 errhint("Check the field option against the index's SCHEMA, "
						 "or re-create the index.")));

	type = vfdw_info_str(attr, "type");
	if (type == NULL || strcmp(type, "VECTOR") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("field \"%s\" of index \"%s\" is not a vector",
						knn->field, knn->index),
				 errdetail("The index holds it as %s, which cannot be ranked "
						   "by distance.",
						   type != NULL ? type : "something this wrapper "
						   "could not read")));

	return vfdw_info_sub(attr, "index");
}

/*
 * The two facts about how the vectors are laid out: how wide an element is,
 * and how many of them there are.
 *
 * Both are refused rather than converted around. FLOAT32 is what
 * src/vfdw_vec.c encodes to and decodes from, so another element type is not
 * a conversion this wrapper gets wrong but one it does not do - the bytes
 * would be read as the wrong number of elements of the wrong width. And a
 * vector of the wrong length is a point in a different space; the server
 * would object to that one itself, but its message names neither the column
 * nor the count, and the arithmetic is done here anyway.
 */
static void
vfdw_search_check_layout(const VfdwKnnScan *knn, const valkeyReply *idx,
						 size_t qbytes)
{
	char	   *dtype = vfdw_info_str(idx, "data_type");
	int64		dims;

	if (dtype == NULL || strcmp(dtype, "FLOAT32") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("index \"%s\" stores %s vectors, which this wrapper "
						"does not encode", knn->index,
						dtype != NULL ? dtype : "unreadable"),
				 errdetail("Only TYPE FLOAT32 is implemented.")));

	dims = vfdw_info_int(idx, "dimensions");
	if (dims > 0 && qbytes != (size_t) dims * 4)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("the query vector has %zu dimensions and index \"%s\" "
						"has " INT64_FORMAT, qbytes / 4, knn->index, dims),
				 errdetail("Both are counted in FLOAT32 elements.")));
}

void
vfdw_search_verify(const VfdwKnnScan *knn, valkeyReply *info, size_t qbytes)
{
	const valkeyReply *idx;
	char	   *metric;

	if (vfdw_reply_is_error(info))
		vfdw_reply_expect(info, VFDW_RTYPE(VALKEY_REPLY_ARRAY) |
						  VFDW_RTYPE(VALKEY_REPLY_MAP), "FT.INFO");

	idx = vfdw_search_vector_attr(knn, info);

	/*
	 * THE CHECK THIS FILE EXISTS FOR. The operator chose a metric and the
	 * index has one; nothing in the protocol makes them agree, and a
	 * disagreement changes every number and every ordering while failing
	 * nothing.
	 */
	metric = vfdw_info_str(idx, "distance_metric");
	if (metric == NULL || strcmp(metric, knn->metric) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("the ORDER BY operator measures %s and index \"%s\" "
						"measures %s", knn->metric, knn->index,
						metric != NULL ? metric : "something this wrapper "
						"could not read"),
				 errdetail("A KNN query does not carry a metric: the index "
						   "applies its own, so this query would be answered "
						   "with distances of the wrong kind, in order, with "
						   "nothing to show for it."),
				 errhint("Use <-> for L2, <=> for COSINE and <#> for IP, "
						 "matching the index's DISTANCE_METRIC.")));

	vfdw_search_check_layout(knn, idx, qbytes);
}
