/*-------------------------------------------------------------------------
 *
 * vfdw_map.h
 *		Foreign table shape: which column comes from where.
 *
 * A table shape fixed at (key, value), with tuples built from a two-element
 * C array regardless of how many columns the table actually has, breaks on
 * the third column: BuildTupleFromCStrings reads past the end of that array,
 * so any user able to CREATE FOREIGN TABLE can crash the backend (ledger
 * columns wide).
 *
 * The answer is not a bounds check bolted onto the same design. It is to
 * resolve, once at plan time, a source for every attribute of the relation -
 * and to refuse the table outright if any attribute has none. The executor
 * then fills a natts-sized slot from a mapping that is known complete
 * (invariant I4).
 *
 * Copyright (c) 2026, The valkey_fdw contributors
 *
 * Released under the PostgreSQL License; see the LICENSE file.
 *
 * IDENTIFICATION
 *		src/vfdw_map.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VFDW_MAP_H
#define VFDW_MAP_H

#include "vfdw.h"

#include "access/tupdesc.h"
#include "foreign/foreign.h"
#include "utils/relcache.h"

#include "vfdw_option.h"

typedef enum VfdwTableType
{
	VFDW_TABLE_STRING = 0,
	VFDW_TABLE_HASH,
	VFDW_TABLE_LIST,
	VFDW_TABLE_SET,
	VFDW_TABLE_ZSET,

	/*
	 * A keyspace searched through a valkey-search index rather than walked.
	 *
	 * Its rows come from FT.SEARCH, so it is not a container type the way the
	 * five above are - the table names an index, and the index names the keys.
	 * Nothing reads one yet: every query against it is refused, which is the
	 * state the design calls step 2 and is the point of declaring the shape
	 * before implementing it.
	 */
	VFDW_TABLE_VECTOR
} VfdwTableType;

/*
 * What a column is indexed AS, from the index_type option.
 *
 * A fact about the valkey-search index rather than about the key, which is
 * why it is not one of the column KINDS: a field column is still a field
 * column, and this says what a search may do with it. Only the vector one is
 * consulted today - it is how the KNN matcher tells the indexed vector field
 * from every other field of the same hash - and the other two are what 6.4's
 * pre-filter pushdown will read.
 */
typedef enum VfdwIndexType
{
	VFDW_INDEX_NONE = 0,		/* no index_type option */
	VFDW_INDEX_TAG,
	VFDW_INDEX_NUMERIC,
	VFDW_INDEX_VECTOR
} VfdwIndexType;

typedef enum VfdwColKind
{
	VFDW_COL_DROPPED = 0,		/* attisdropped; never filled */
	VFDW_COL_KEY,				/* the Valkey key name */
	VFDW_COL_VALUE,				/* the whole value, for a string table */
	VFDW_COL_FIELD,				/* one hash field, named by the field option */
	VFDW_COL_MEMBER,			/* a list, set or zset member */
	VFDW_COL_SCORE,				/* a zset score */
	VFDW_COL_POSITION,			/* a list member's index within its list */
	VFDW_COL_TTL,				/* time to live of the paired field */
	VFDW_COL_DISTANCE,			/* vector search score */
	VFDW_COL_LEGACY_VALUE		/* the whole collection, packed into an array */
} VfdwColKind;

struct VfdwPackedElem;

typedef struct VfdwColumn
{
	AttrNumber	attnum;
	VfdwColKind kind;
	VfdwIndexType index_type;
	const char *field;			/* VFDW_COL_FIELD / VFDW_COL_TTL */
	Oid			typid;
	int32		typmod;

	/* Inbound: bytes -> Datum. */
	Oid			typinput;
	Oid			typioparam;

	/*
	 * Outbound: Datum -> bytes. Resolved beside the inbound pair rather than
	 * at BeginForeignModify so the two directions cannot drift: if the rule
	 * for dropped columns or base types is ever changed for one of them it is
	 * visibly changed for the other.
	 */
	Oid			typoutput;
	bool		typisvarlena;

	/*
	 * bytea columns take Valkey's bytes verbatim. Everything else is a text
	 * representation and must be checked against the server encoding before
	 * it becomes a Datum.
	 *
	 * Computed from the BASE type, so a domain over bytea is binary in both
	 * directions. One expression feeds the read path and the write path,
	 * which is what makes a value written verbatim readable verbatim; two
	 * expressions would eventually disagree and mangle it.
	 */
	bool		is_binary;

	/*
	 * True when typid is a domain. The binary branch bypasses the type's
	 * input function, and with it the domain's constraint check, so the read
	 * path has to apply that check itself.
	 */
	bool		is_domain;

	/*
	 * VFDW_COL_FIELD only, and only when the map asks for HMGET: which entry
	 * of that reply is this column's. HMGET answers positionally - values
	 * alone, in the order the fields were asked for - where HGETALL answers
	 * with pairs that carry their own names.
	 */
	int			field_slot;

	/*
	 * VFDW_COL_TTL only: which entry of the HPTTL reply is this column's.
	 *
	 * Assigned where the ttl columns are counted, so the position a field is
	 * ASKED for and the position its answer is READ from are decided by one
	 * walk in one order. Two walks would be two chances to disagree, and the
	 * disagreement would not fail - it would give each column another
	 * column's expiry.
	 */
	int			ttl_slot;

	/* VFDW_COL_LEGACY_VALUE only; NULL for every other kind. */
	struct VfdwPackedElem *packed;
} VfdwColumn;

/*
 * The element a packed collection column is assembled from.
 *
 * A packed table answers with one row per KEY and the whole collection in one
 * array column, so every value that arrives from Valkey is an ELEMENT of that
 * column's type and never a value of it. col describes that element and is
 * what the row builder hands to vfdw_row_datum_from_bytes, so an element takes
 * the same route a scalar column's value takes rather than a second one: a
 * text element is checked against the server encoding, a bytea element keeps
 * its NUL bytes and its unconvertible ones (invariant I3).
 *
 * The three assembly parameters are read from the element type rather than
 * written down as the constants that happen to be right for text and bytea.
 * They are what construct_md_array lays the array out with, and a wrong
 * typalign does not fail: it builds an array whose elements are then read from
 * the wrong offsets.
 */
typedef struct VfdwPackedElem
{
	VfdwColumn	col;
	int16		typlen;
	bool		typbyval;
	char		typalign;
} VfdwPackedElem;

typedef struct VfdwTableMap
{
	Oid			relid;
	int			natts;
	VfdwColumn *cols;			/* natts entries, indexed by attnum - 1 */

	AttrNumber	keyattno;		/* attnum of the key column, or 0 */

	/*
	 * The other roles the write path addresses directly, resolved once here.
	 * Any of them may be InvalidAttrNumber. The modify callbacks must not
	 * re-derive them by walking cols[]: a walk has to pick a winner when two
	 * columns claim one role, and picking silently is how a row gets written
	 * from a column the user did not mean. Duplicates are refused instead.
	 */
	AttrNumber	memberattno;
	AttrNumber	scoreattno;
	AttrNumber	valueattno;

	VfdwTableType tabletype;

	const char *keyprefix;
	const char *keyset;
	const char *singleton_key;
	const char *search_index;
	int			database;
	bool		legacy_value;
	bool		readonly;

	int			nfields;		/* VFDW_COL_FIELD columns */

	/*
	 * VFDW_COL_TTL columns. Both the width of the array vfdw_ttl_take fills
	 * and the count HPTTL is told, which is why it is kept rather than
	 * recounted: the two have to be the same number, and a second walk that
	 * disagreed with the first would misalign every field after it.
	 */
	int			nttl;

	/*
	 * Fetch the named fields with HMGET instead of the whole hash.
	 *
	 * ONE DECISION, READ BY TWO PLACES. The command builder and the row
	 * decoder must agree about which shape the reply has, and the way they
	 * would stop agreeing is by each working it out from the map's other
	 * fields. Deciding once here makes disagreement impossible rather than
	 * unlikely.
	 *
	 * A table mapping two fields of a hundred-thousand-field hash was
	 * transferring all of it. Both commands are one round trip, so there is no
	 * latency threshold to tune between them: HGETALL's cost grows with the
	 * KEY's field count, which this wrapper does not control, and HMGET's with
	 * the TABLE's, which the definition fixes.
	 *
	 * False for a packed table, which is the whole collection by definition,
	 * and for a table that names no field at all - HMGET with no fields is not
	 * a command.
	 */
	bool		hmget;
	int			nreq;			/* fields HMGET asks for, when hmget */
} VfdwTableMap;

/*
 * Resolve and validate the shape of a foreign table.
 *
 * Raises if any attribute has no source, if required options conflict, or if
 * a column option is meaningless for the table type. Called at plan time so
 * a misdescribed table fails before a single command is sent.
 */
extern VfdwTableMap *vfdw_map_build(Relation rel, ForeignTable *table);
extern VfdwTableMap *vfdw_map_build_for_relid(Oid relid);

/*
 * Fill in everything a VfdwColumn derives from its PostgreSQL type.
 *
 * Exposed so that the diagnostic entry points describe a type exactly the way
 * a real table does. A probe that recomputed is_binary for itself would be a
 * second answer to the question the comment on that field says must have one.
 */
extern void vfdw_map_resolve_type(VfdwColumn *col, Oid typid, int32 typmod);

extern const char *vfdw_tabletype_name(VfdwTableType type);

/*
 * The CMD_* bitmask IsForeignRelUpdatable should report for a table, and the
 * reason for whatever it withholds.
 *
 * Deliberately does not build the map and cannot raise; see the definition.
 *
 * detail and hint are set to the reason a table accepts fewer commands than
 * all of them, and to NULL when it accepts all. Both may be NULL if the
 * caller only wants the mask. Returning the reason alongside the mask is not
 * a convenience: the refusal message used to carry one hardcoded explanation,
 * so a table blocked for having a search index was refused with a paragraph
 * about list row identity.
 */
extern int	vfdw_map_writability(Oid relid, const char **detail,
								 const char **hint);
extern int	vfdw_map_writability_for_relid(Oid relid);

#endif							/* VFDW_MAP_H */
