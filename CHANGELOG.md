# Changelog

## 0.2.0

First release.

A PostgreSQL foreign data wrapper for Valkey: read a keyspace as tables, and
write to it inside a PostgreSQL transaction.

### What it does

**Reads.** A table maps onto a string, hash, list, set or sorted set, either
one row per key across a prefix or one table per key. Keys are discovered a
page at a time with `SCAN` and each page's values are fetched in one pipelined
batch. `WHERE key = 'literal'` is a single fetch rather than a traversal, and a
join whose inner side is a foreign table plans as a parameterised key lookup
rather than a scan per outer row. A hash table asks for the fields it maps
rather than the whole hash. `ANALYZE` draws a real sample, so a join against a
Valkey table is planned from a measured row count.

**Writes.** `INSERT`, `UPDATE`, `DELETE` and `COPY FROM`, applied as one unit.
Nothing is sent as the statement executes: DML appends to a transaction-scoped
buffer which folds into per-key preconditions and actions, applied by a single
`EVALSHA` at `PRE_COMMIT`. `ROLLBACK` sends nothing. A scan sees its own
transaction's uncommitted writes. A transaction spanning two hash slots, two
servers or two user mappings is refused whole rather than split.

**Connections.** A pool keyed by (server, user, slot). TLS, ACL username and
password, `prefer_replica`, and a command timeout. Every blocking wait is
interruptible, so `statement_timeout` and `pg_cancel_backend` end a query
against a server that has stopped answering.

**Cluster.** Scans visit every primary; the slot map is a cache and the server
is the authority, so a `MOVED` is followed rather than treated as a key that
vanished.

**Vector search.** A `vector` table over a `valkey-search` index, with KNN
ordering by the pgvector operators `<->`, `<=>` and `<#>`, and numeric and tag
pre-filters pushed into the query. The index's metric, field type, dimension
and element type are verified against the table rather than assumed.

### Requirements

- PostgreSQL 14, 15, 16, 17 or 18
- libvalkey 0.5.0+
- Valkey 8.1+ **to write** — the write path is one Lua program, so a server
  that withholds `EVAL` can be read from and not written to. Such a server is
  refused for writes when the connection is made rather than at commit.
- `valkey-search` for `vector` tables

### Known behaviour worth reading before adopting

- **A scan can return the same key twice**, and this wrapper does not remove
  the second one. That is what `SCAN` is; the README explains why filtering it
  here would cost an unbounded per-scan set to correct something `SELECT
  DISTINCT` already expresses.
- **Writes need scripting.** See above. This is the one thing a wrapper built
  on plain verbs can do that this one cannot.
- **Vector search is refused on a cluster**, at plan time.

### Packages

RPMs and DEBs for el-8, el-9, el-10, amazon-2023, debian-12, debian-13,
ubuntu-24.04 and ubuntu-26.04, against each supported PostgreSQL major.
Amazon Linux 2023 packages PostgreSQL itself and carries 15 through 18, so it
has no 14 build. libvalkey is linked statically and its licence travels with
the binary.

The diagnostic extension `valkey_fdw_test` is deliberately absent from the
packages: it carries entry points that dial a host and port given as
arguments.
