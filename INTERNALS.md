# valkey_fdw internals

This document is for people changing this code. `README.md` says what the
wrapper does and how to use it; this says how it works and, wherever the two
differ, why it was built that way rather than the obvious way.

Almost every decision here has a reason, and a surprising number of those
reasons are a specific failure that was hit once. Where a design looks more
elaborate than it needs to be, the elaboration is usually load-bearing and the
comment above it says what it is carrying. **Read the file header before moving
code between files.** They are not decorative; several invariants are stated in
a `.h` and enforced in a `.c` that does not restate them.

## Contents

| Section | What it covers |
|---|---|
| [Shape of the thing](#shape-of-the-thing) | The layering, and what each layer may depend on |
| [Invariants](#invariants) | I1–I8 and W1–W3: what they mean and where they are enforced |
| [Connections, authentication and transaction integration](#connections-authentication-and-transaction-integration) | The pool, TLS, AUTH, and what happens at a transaction boundary |
| [I/O, command construction and error classification](#io-command-construction-and-error-classification) | Interruptible waits, batching, reply ownership, the error taxonomy |
| [Options, table shape and row identity](#options-table-shape-and-row-identity) | The option table, the column map, row identity |
| [The read path](#the-read-path-planning-scanning-and-row-production) | Access-path choice, the page loop, the overlay |
| [The write path](#the-write-path-buffering-and-folding) | The write buffer and the fold into a per-key plan |
| [The atomic apply](#the-atomic-apply-the-lua-program-and-the-flush) | The Lua program, the flush, and the four outcomes |
| [Cluster routing, and values on the wire](#cluster-routing-and-values-on-the-wire) | The slot map, the fan-out, Datum rendering |
| [How this project is tested](#how-this-project-is-tested) | Topologies, gates, the mutation manifest |
| [Known documentation drift](#known-documentation-drift) | Comments that do not match the code |

---

## Shape of the thing

The split is by concern, and the lower layers deliberately have no planner or
executor dependencies so they can be exercised directly by the diagnostic
functions without going through a query.

```
valkey_fdw.c    handler and validator registration — the only file the
                server links against by name

  ── options and shape ──────────────────────────────────────────────
vfdw_option.c   the option table; one source of truth for the validator
                and every runtime reader
vfdw_map.c      table and column shape resolution
vfdw_writable.c answers "may this table be written" without building a
                map and without raising
vfdw_rowid.c    row identity per table shape

  ── transport ──────────────────────────────────────────────────────
vfdw_conn*.c    connection pool keyed (serverid, userid, slot) plus node
                identity; TLS, ACL auth, xact and subxact callbacks
vfdw_tls.c      the transport libvalkey does not provide: real hostname
                verification
vfdw_io.c       interruptible I/O: WaitLatchOrSocket + CHECK_FOR_INTERRUPTS
vfdw_cmd.c      binary-safe argv construction, pipelining, reply decoding
vfdw_error.c    context errors and reply errors, mapped to SQLSTATEs

  ── values ─────────────────────────────────────────────────────────
vfdw_val.c      Datum → bytes;  vfdw_row.c is its exact inverse
vfdw_score.c    zset score handling
vfdw_slot.c     CRC-16 and hash-tag extraction

  ── read path ──────────────────────────────────────────────────────
vfdw_plan.c     access-path choice (planner)
vfdw_scan*.c    row production (executor)
vfdw_overlay.c  what a scan sees of its own transaction's uncommitted writes
vfdw_analyze.c  ANALYZE, driving the same producer a query does

  ── write path ─────────────────────────────────────────────────────
vfdw_modify.c   the FdwRoutine modify callbacks
vfdw_wbuf.c     transaction-scoped write buffer (ops appended, nothing freed)
vfdw_ledger*.c  folds the buffer into one plan per key
vfdw_script*.c  the Lua program, its identity, and its wire encoding
vfdw_flush*.c   applies the whole transaction at PRE_COMMIT, as one unit

  ── cluster ────────────────────────────────────────────────────────
vfdw_cluster.c  the slot map: a cache, never an authority
*_cluster.c     hang the cluster half of scan and flush off the standalone path

  ── diagnostics ────────────────────────────────────────────────────
vfdw_test*.c    SQL-callable probes, in a SEPARATE superuser-only extension
```

Two shapes are worth internalising before reading further.

**Reads discover then fetch.** A scan finds keys a page at a time and fetches a
whole page's values in one pipelined batch. It never issues a round trip per
row. On a cluster it visits every primary, and an exhausted node is not an
exhausted scan.

**Writes are deferred entirely.** Nothing reaches the server as a statement
executes. DML appends to a transaction-scoped buffer, which folds into
preconditions and actions per key, applied by a single `EVALSHA` at
`PRE_COMMIT`. `ROLLBACK` sends nothing at all. A transaction spanning two
servers, two user mappings or two hash slots is refused whole rather than split.

---

## Invariants

These are the rules that make a whole class of defect unreachable rather than
fixed case by case. The I-series numbers rules that hold everywhere; the
W-series numbers rules that hold of the write path alone. The table below is
where I1–I8 and W1–W3 are defined, and it is the statement of record for all
eleven: an invariant the tree names by number and this table does not define is
a mistake in one place or the other.

I8 is repeated by number in `src/vfdw_cluster.h`, and W1–W2 in
`src/vfdw_script.h` and `src/vfdw_script_encode.c`, beside the code that keeps
them. W3 is stated in words rather than by number where it binds — the file
headers of `src/vfdw_modify.h` and `src/vfdw_flush.h`, and the comment above
`vfdw_modify_connect` — because the call sites it constrains are the ones that
have to state what they may not do. The register of closed defects numbers its
own rows with letters that overlap these, so a bare `W5` beside an assertion
names a register row rather than an invariant; the invariants are the eleven
below and nothing else.

| | Rule | Enforced by |
|---|---|---|
| **I1** | Nothing is freed, reset or closed on the way out of an error path | Convention, plus the batch's context reset callback (`src/vfdw_cmd.c:156`) and the xact callbacks |
| **I2** | No byte string reaches error-reporting code unless it is known valid in the database encoding | `vfdw_safe_text` (`src/vfdw_error.c`); `scripts/lint.sh` bans a non-literal `ereport` format |
| **I3** | Lengths travel with data. No `strlen` on Valkey bytes; NULs are data | `vfdw_cmd_add_bytes`, reply handling throughout |
| **I4** | Tuples are built from a `natts`-sized array | `vfdw_map_build` resolves every attribute or refuses the table |
| **I5** | "No tuple this call" and "scan finished" are distinct states | `vfdw_scan_fetch`, `vfdw_scan_cluster_advance`, `vfdw_scan_retry_pass` |
| **I6** | Every blocking wait is interruptible | `vfdw_io_wait` — all socket waits go through it |
| **I7** | Options are data, not code | `vfdw_options[]` drives validator and readers alike |
| **I8** | The slot map is a cache; the server is the authority | `src/vfdw_cluster.h`; nothing raises on an unplaced slot |
| **W1** | The script's write verbs are literal `server.pcall` calls, and phase 1 contains no write | `scripts/lint.sh` counts them |
| **W2** | Every key index the encoder emits is in range | `vfdw_script_encode.c` asserts it where the cause is visible |
| **W3** | Nothing reaches the wire before the pre-commit flush. A connection is *acquired* by a modify callback — AUTH and HELLO included, because they are part of acquiring a cold one — and no command is sent through it until `vfdw_flush_pre_commit` | `src/vfdw_modify.h` and `src/vfdw_flush.h` state the two sides of it; `test/isolation/specs/write_conflict.spec` asserts a transaction's writes are invisible to another session until it commits |

**I1 is about behaviour, not about a mechanism.** It was originally written as
"no `PG_TRY` appears in `vfdw_flush.c`", which was a proxy for the real rule and
turned out to be too strong — a `PG_TRY` that frees nothing and only decides
which SQLSTATE an error carries breaks nothing. The rule is that no resource is
unwatched, freed, reset or closed before an `ereport`, because a cleanup round
trip on the way out can itself fail and destroy the real diagnosis.

Several comments in the tree describe connections as owned by a
`ResourceOwner`. **No ResourceOwner API is used anywhere in this repository.**
The behaviour I1 describes does hold, but the mechanisms are transaction
callbacks (`src/vfdw_conn_xact.c`) for connections and
`MemoryContextRegisterResetCallback` (`src/vfdw_cmd.c:156`) for replies.
`src/vfdw_conn.h:17` states this correctly and explains why: connections
deliberately outlive transactions, and a ResourceOwner would destroy them at
the wrong time. See [Known documentation drift](#known-documentation-drift).

---

## Connections, authentication and transaction integration

Everything that reaches a Valkey server passes through one pooled `VfdwConn`. The pool is deliberately small in surface — four entry points to acquire, one to release, two callbacks at transaction boundaries — and the reasoning behind almost every line of it is a failure mode of the obvious implementation, which opens a connection during planning, another for the scan and another for the modify, and closes none of them when a statement raises (`src/vfdw_conn.h:8`). A socket belongs to no PostgreSQL cleanup mechanism by default, so each aborted statement leaks a descriptor. Here a connection is opened once per identity and lives for the backend, which bounds descriptors by the number of distinct mappings instead of by the number of statements, and removes the per-statement connect, AUTH and SELECT round trips.

The component is four files: `src/vfdw_conn.c` (open, authenticate, negotiate, acquire), `src/vfdw_conn_pool.c` (which entry serves a request), `src/vfdw_conn_xact.c` (what happens at transaction boundaries) and `src/vfdw_conn_internal.h` (the shared struct). `src/vfdw_xact.c` owns registration and ordering; `src/vfdw_tls.c` owns the transport.

### The pool key, and why node identity is not in it

`VfdwConnKey` is `(serverid, userid, slot)` (`src/vfdw_conn_internal.h:29`). `slot` is a connection index, not a cluster hash slot — a collision of words that predates the cluster work and is called out in both the struct comment and the file header of `src/vfdw_conn_pool.c`. Slot 0 answers every sequential statement, so the common case remains one connection reused for the life of the backend; a second slot exists only while an earlier reader still holds one, which a nested loop over two foreign tables on the same server produces. The ceiling is `VFDW_MAX_CONN_SLOTS`, 32 (`src/vfdw_conn_internal.h:115`); exceeding it raises `ERRCODE_TOO_MANY_CONNECTIONS` from `vfdw_conn_no_slots` (`src/vfdw_conn_pool.c:120`), whose wording notes that a cluster reaches the ceiling sooner because a fan-out scan holds one connection per primary.

Cluster node identity — `node_host[VFDW_MAX_NODE_HOST]` and `node_port` — lives on the entry rather than in the key (`src/vfdw_conn_internal.h:62`). A host name cannot join a `HASH_BLOBS` key without being hashed into a fixed field, and the entry-side alternative buys something: repointing a slot at a different node has to be an explicit act, and that act closes the socket first (`vfdw_conn_claim`, `src/vfdw_conn_pool.c:96`). Reusing a socket whose peer is not the one the caller asked for is a wrong answer rather than an error, which is exactly the failure routing exists to prevent. The host is a fixed-size array rather than a pointer because a hash entry outlives any memory context the name could plausibly have been allocated in.

`vfdw_conn_take_free_slot` (`src/vfdw_conn_pool.c:134`) walks slots 0..31 for the identity, creating entries as it goes, and skips leased ones. An unleased entry already pointing at the requested node is reused as it stands. Otherwise the entry is remembered as a candidate, and a candidate with **nothing open** is preferred over one holding a socket for another node (`src/vfdw_conn_pool.c:180`). That preference is what makes pooling work across nodes at all: without it every node falls back to the same first free slot and evicts the previous node's connection, and the comment records the measurement that caught it — a second visit to three primaries opened three more sockets. A `NULL` host means "the server's own configured endpoint" and matches an entry whose `node_host` is empty (`vfdw_conn_node_matches`, `src/vfdw_conn_pool.c:70`), so a plain server does not repoint its one connection on every call.

Because `hash_search(HASH_ENTER)` fills only the key, `vfdw_conn_init_entry` (`src/vfdw_conn_pool.c:37`) initialises the rest. The comment on `script_loaded` there is worth reading before deleting anything that looks redundant: fields cleared only in `vfdw_conn_close` are read uninitialised the first time an identity is used, because close never runs for an entry that has never been opened.

The hash table itself is created in `vfdw_conn_init_pool` (`src/vfdw_conn.c:58`) with `HASH_ELEM | HASH_BLOBS` and no `HASH_CONTEXT`, so it lands in `TopMemoryContext` and outlives every transaction — which is the point, since connections deliberately outlive transactions.

Three acquisition entry points differ only in what they permit:

| Entry point | Permits | Endpoint |
|---|---|---|
| `vfdw_get_connection` | non-cluster servers only | server's own host/port or Unix socket |
| `vfdw_get_connection_cluster` | cluster servers | as above (the seed) |
| `vfdw_get_connection_node` | cluster servers | the named node, overriding both |

The cluster refusal (`src/vfdw_conn.c:506`) sits on acquisition rather than on the socket open so that permission is opt-in per call site. An unrouted scan against a cluster does not fail — it returns whatever keys the node it happened to reach holds — so the refusal is the safe default and each path opts in once it can route.

### Leasing, and what actually owns a connection

A `valkeyContext` is one reply stream, and replies arrive in the order their commands were sent. Two readers pipelining onto one context therefore interleave and each takes whichever reply arrives next, including the other's; where the shapes differ that surfaces as a protocol error, and where they agree — two string tables — it silently returns the wrong value (`src/vfdw_conn_internal.h:79`). Rather than demultiplex a shared stream, a reader takes the connection to itself. Every acquisition leases, and the lease records `GetCurrentSubTransactionId()` in `lease_subid`.

`vfdw_release_connection` (`src/vfdw_conn.c:457`) clears both fields and nothing else. It is called when a reader finishes — `vfdw_scan_end` and `vfdw_scan_state_close` after draining the batch (`src/vfdw_scan.c:768`, `:798`) — and deliberately not on an error path. A scan that raised leaves its lease held, and the transaction callback releases it along with everything else. That is invariant I1: nothing is freed, reset or closed on the way out of a failure.

Not every caller holds a lease for long. `vfdw_modify_connect` (`src/vfdw_modify.c:192`) acquires only to prove the server is reachable and to read the options the write path will use, then gives the lease straight back — invariant W3 says the write path issues no command outside the pre-commit flush, so there is no reply stream to protect, and holding one lease per DML statement would make an INSERT routed into 33 partitions fail with a message wrong in both nouns.

**On ResourceOwner ownership.** Several comments in the tree describe pooled connections and replies as belonging to a ResourceOwner. No ResourceOwner API is used anywhere in this repository. `src/vfdw_conn.h:17` states the actual design: transaction integration is by xact callback, precisely because connections outlive transactions and a ResourceOwner would destroy them at the wrong time. What a transaction end has to guarantee is narrower — that no connection is left mid-conversation. Reply lifetime is handled separately, by `MemoryContextRegisterResetCallback` on the batch context (`src/vfdw_cmd.c:156`), which is what lets scan and flush code raise from anywhere without a `PG_TRY`. See "Unverified" below.

### Opening: limits, TLS, AUTH, RESP3

`vfdw_conn_open` (`src/vfdw_conn.c:370`) runs a fixed sequence, and the order carries meaning:

1. Connect, through `vfdw_io_connect`. A node endpoint from the slot map overrides host, port and suppresses the Unix socket path — that path names one local server and cannot reach a cluster's other members (`src/vfdw_conn.c:382`).
2. Transfer ownership to the entry immediately, before any command can fail, and set `in_conversation` for the whole of setup. If anything below raises, the entry is found mid-conversation at transaction end and closed there (I1).
3. `vfdw_io_set_limits` **before anything can be read** (`src/vfdw_conn.c:407`). A multi-bulk header declares how many elements follow before any of them arrive, so an unbounded reader commits to whatever the far end claims — and the far end is not always the server the operator believes it is. The bounds come from `reader_buffer_bytes` and `max_reply_elements`.
4. TLS, if enabled: `vfdw_tls_attach` then `vfdw_conn_force_handshake`.
5. AUTH, then HELLO.

Every setup command goes through `vfdw_conn_command` (`src/vfdw_conn.c:105`), which builds a deadline from `command_timeout_ms`, sends, reads, and raises on an error reply. It sets `in_conversation` itself and **saves and restores** rather than clearing (`src/vfdw_conn.c:128`). The comment records why: SELECT was once sent after the flag had already been cleared, so a cancel mid-command left the connection in the pool with an unread reply and the next statement received it. The restore is not in a `PG_FINALLY` and must not be — an error longjmps past it, leaving the flag set, which is the outcome that gets the connection discarded.

`vfdw_conn_force_handshake` (`src/vfdw_conn.c:153`) exists because a non-blocking libvalkey context reports TLS setup as successful even when `SSL_connect` returned `WANT_READ`/`WANT_WRITE`; the handshake actually completes during the first real read or write. Without a forced `PING` round trip, a rejected certificate would surface as whatever command ran next — "authentication failed" — sending the reader to the wrong place. Its `PG_CATCH` replaces the I/O-shaped error with OpenSSL's reason and the specific verification result when there is one, and re-throws when there is not. It does not release the connection: the pool entry already owns it.

`vfdw_conn_authenticate` (`src/vfdw_conn.c:210`) sends nothing when no password is configured; otherwise two-argument AUTH when `username` is set (Valkey 6+ ACL users) and one-argument otherwise. The failure message deliberately does not name the credential, because it would land in both the server log and the client's error output.

`vfdw_conn_negotiate_resp3` (`src/vfdw_conn.c:285`) sends `HELLO 3`. RESP3 is worth asking for — `HGETALL` returns a typed map and zset scores real doubles, rather than a flat array to re-pair and re-parse — but it cannot be a requirement. The distinction that matters is between a server *declining* the protocol and HELLO *failing*, and it has to be drawn on the reply body. `vfdw_conn_resp3_declined` (`src/vfdw_conn.c:250`) recognises exactly two answers as declines: a `NOPROTO` prefix, and an `ERR` reply containing "unknown command" from a pre-6.0 server. The second is searched rather than compared, because the server appends the arguments it did not understand, and the search is bounded by `reply->len` rather than by a terminator (invariant I3). Everything else — `NOPERM`, `LOADING`, `MISCONF` — means setup did not happen and is raised, because a connection whose setup was refused must not enter the pool believing itself to be an old server. The downgrade is then recorded from the reply *shape*: `resp3` is true only when the reply is a `VALKEY_REPLY_MAP` (`src/vfdw_conn.c:317`).

`vfdw_conn_select_db` (`src/vfdw_conn.c:321`) is a no-op when the database already matches, refuses any non-zero database on a cluster, and otherwise sets `database = -1` *before* the round trip (`src/vfdw_conn.c:360`). If the round trip is cancelled the connection is somewhere unknown; recording the new value on success would leave the entry claiming the old database, and recording it beforehand would claim the new one. A connection that lies about its database reads the wrong keyspace rather than failing. In practice `vfdw_conn_command` marks it mid-conversation anyway, so it will be discarded; the two together mean no surviving connection can be wrong.

### password_required, and its superuser gate

`vfdw_conn_check_password_required` (`src/vfdw_conn.c:431`) runs on every acquisition, after options are read and before the socket is opened. It raises unless one of three things holds: the option is false, a password is configured, or `superuser_arg(user->userid)` is true for the role the mapping is being used on behalf of. This is postgres_fdw's rule, adopted for the same reason: without it, any role holding USAGE on a foreign server can reach anything the PostgreSQL process itself can reach — a Valkey instance trusting the database host by address, for instance.

The gate has a second half in the option table. `password_required` carries `VFDW_OPTPRIV_SUPERUSER_TO_DISABLE` (`src/vfdw_option.c:188`), meaning anyone may set it true and only a superuser may set it false. The asymmetry is the point: the restricted direction is the one that removes a check. The rule lives in the table beside the option rather than as an `if` in the validator because of invariant I7 — a privilege guard written as a `strcmp` chain goes quietly dead when the option is renamed. `vfdw_check_option_priv` (`src/vfdw_option.c:360`) also fails closed on an unrecognised privilege value, and its comment records a consequence worth knowing before someone reports it as a bug: `transformGenericOptions` hands the validator the merged option list, not the delta, so once a superuser sets a restricted option on a server, a non-superuser owner can no longer run any `ALTER SERVER ... OPTIONS` on it.

### TLS: what libvalkey does not do

`src/vfdw_tls.c` exists for one reason, stated in its header: as of libvalkey 0.5.0, `valkeyTLSOptions.server_name` is wired into `SSL_set_tlsext_host_name` only — that is SNI — and `X509_VERIFY_PARAM_set1_host` is never called. The result is chain validation without identity checking, so a certificate signed by a trusted CA but issued for a different host is accepted. That is precisely what hostname verification exists to catch, so `tls_verify = 'full'` cannot be built on libvalkey's helper. The wrapper therefore builds the `SSL_CTX` and `SSL` itself and hands the finished session to `valkeyInitiateTLS`, the entry point libvalkey documents for callers managing their own context.

`vfdw_tls_make_context` (`src/vfdw_tls.c:212`) sets a TLS 1.2 floor rather than inheriting the platform default; loads `tls_ca_file` when given and otherwise falls back to the system trust store whenever verification is on (silently trusting nothing would reject every certificate for a reason the user cannot see); loads a client certificate and requires `tls_key_file` alongside it; and installs `SSL_VERIFY_PEER` unless `tls_verify` is `none`. `vfdw_tls_set_identity` (`src/vfdw_tls.c:282`) sends SNI for the peer name (`tls_sni` if set, otherwise `host`), skipping it for an IP literal since SNI carries names only, and under `full` pins verification to that name with `X509_VERIFY_PARAM_set1_host` or `set1_ip_asc`, with `X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS` so that `*.example.com` is acceptable and `w*.example.com` is not. `tls_ca_file`, `tls_cert_file` and `tls_key_file` are superuser-only options.

The second thing this module does is make failures legible. A TLS failure reaches the caller through libvalkey as a generic I/O error whose `errstr` derives from `errno`, and `errno` is 0 when the cause was certificate verification — so the message reads "Success" (`src/vfdw_tls.h:36`). The real reason is only in OpenSSL's error queue, retrieved by `vfdw_tls_take_error`. And OpenSSL's queue says only "certificate verify failed", which is one string for a wrong hostname, an untrusted CA and an expired certificate. The specific result exists only inside the verification callback, so `vfdw_tls_verify_cb` (`src/vfdw_tls.c:71`) captures the **first** failure into a backend-lifetime static. First rather than last, because under `tls_verify = 'none'` the callback still runs and its answer is ignored, and recording the last would let a vaguer later error overwrite the specific one. `vfdw_tls_verify_reason` **takes** the value rather than reading it, so one handshake's reason cannot be reported against the next, and always sets a hint matched to the check that refused — a single hint for every failure named `tls_ca_file` and the hostname, which are exactly what is correct when a certificate has merely expired.

Ownership discipline in this file mirrors the connection layer's: `vfdw_tls_fail` (`src/vfdw_tls.c:186`) copies the message, releases `ssl` and `ctx`, then reports, because `ereport` does not return. After `SSL_new` takes its reference the context is dropped immediately (`src/vfdw_tls.c:350`), leaving one fewer thing to unwind on every path below.

### Invalidation

`ALTER SERVER` and `ALTER USER MAPPING` must take effect without asking the user to reconnect, so `vfdw_conn_inval_callback` (`src/vfdw_conn.c:602`) is registered on `FOREIGNSERVEROID` and `USERMAPPINGOID`. It **marks** rather than closes: the callback may fire inside a transaction that is still using the connection. Each entry records the syscache hash values of its own server and mapping at acquisition time (`src/vfdw_conn.c:496`), and only entries matching the invalidated hash are marked — a `hashvalue` of 0 is the cache-reset signal and marks everything. Without the per-entry hashes, creating an unrelated foreign server would drop every open connection in the backend.

A marked entry is closed at whichever comes first: the next acquisition, which closes it inline and clears the flag before reopening (`src/vfdw_conn.c:490`), or the transaction sweep. The inline close is the reason `vfdw_conn_peek` exists — see below.

### Transaction and subtransaction integration

There is exactly one `RegisterXactCallback` and one `RegisterSubXactCallback` in the extension, both in `src/vfdw_xact.c:111`. `src/vfdw_xact.h` gives the reason: `RegisterXactCallback` prepends and `CallXactCallbacks` walks head-first, so a callback registered *before* another runs *after* it. Registering the write callback beside the connection callback would make the order between "flush the buffer" and "sweep the connections" depend on which lazy registration ran first in this backend — which in turn depends on whether the backend's first valkey_fdw statement was a SELECT or an INSERT. `src/vfdw_conn.c` therefore registers nothing and exports `vfdw_conn_xact` and `vfdw_conn_subxact`, which `vfdw_xact_callback` calls in a written order: buffer first, connections second, so a connection sweep can never run before the work it would have carried.

`vfdw_xact_callback` (`src/vfdw_xact.c:36`) has no `default:` arm, deliberately — the `XactEvent` enum is closed, and an exhaustive switch turns a new event in a future major into a compiler warning rather than a silent fall-through past the flush.

| Event | Buffer / flush | Pool |
|---|---|---|
| `PRE_COMMIT` | `vfdw_flush_pre_commit()` — the whole transaction applied as one `EVALSHA` | no-op |
| `PARALLEL_PRE_COMMIT` | `elog(ERROR)` if the buffer is non-empty | no-op |
| `PRE_PREPARE` | refuses with `ERRCODE_FEATURE_NOT_SUPPORTED` if anything is buffered | no-op |
| `PREPARE` | `elog(ERROR)` if a buffer survived (unreachable by construction) | no-op |
| `COMMIT`, `PARALLEL_COMMIT` | asserts the flush ran, then resets buffer and flush state | sweep |
| `ABORT`, `PARALLEL_ABORT` | resets buffer and flush state; sends nothing | sweep |

`vfdw_conn_xact` (`src/vfdw_conn_xact.c:25`) returns immediately for every pre-event, so the connection is untouched while the flush is using it, and acts only on the four commit/abort events. For each open entry it closes anything that is `in_conversation` or `invalidated`, and clears every lease unconditionally. No reader survives the transaction, so a lease still held belongs to a scan that raised before returning it; releasing it here rather than in the scan's error path is what keeps I1.

**Why PRE_COMMIT specifically.** At `XACT_EVENT_PRE_COMMIT` the transaction is still `TRANS_INPROGRESS`, the ResourceOwner and memory contexts are intact, an `ereport` still aborts the transaction, and — the load-bearing property — interrupts are not yet held. Every other transaction event runs inside `HOLD_INTERRUPTS`, where `CHECK_FOR_INTERRUPTS` is a no-op and invariant I6 (every blocking wait is interruptible) cannot be met. So no byte may reach the wire from any of them. `src/vfdw_xact.h:33` is explicit that this rule has no test and no lint on purpose: a structural grep over these switch arms passes trivially the moment the code is factored into helpers, so it would be a rule that could only ever be green. Review is the enforcement. The callback is re-entrant across a failed flush — an `ereport` at PRE_COMMIT re-enters as `XACT_EVENT_ABORT`, and `vfdw_wbuf_reset()` assumes nothing about how far the flush got.

Before acquiring anything, the flush calls `vfdw_conn_peek` (`src/vfdw_flush.c:605`). `PreCommit_Portals` is expected to have dropped every portal by then; this turns the expectation into a checked precondition. It cannot be written in terms of `vfdw_get_connection`, because that closes an invalidated connection inline and would free the very `valkeyContext` a live scan batch is caching — a guard phrased on its return value could only run after the destructive part, turning a wrong answer into a use-after-free. `vfdw_conn_peek` (`src/vfdw_conn.c:680`) therefore only looks: it never creates, opens, closes or reads options, and it scans all 32 slots for the identity looking for one that is open and mid-conversation.

**Subtransactions.** `vfdw_conn_subxact` (`src/vfdw_conn_xact.c:94`) filters for `SUBXACT_EVENT_ABORT_SUB` itself — which subtransaction events matter to a connection is knowledge about connections, so the filter stays in the connection file rather than moving into the sequencer. Its two halves behave differently and the asymmetry is the whole content of the function. Closing a mid-conversation connection is unconditional across all entries, because the reply to whatever was in flight is still coming whoever sent it. Releasing leases is *not*: only a lease whose `lease_subid` equals `mySubid` is released. Releasing every lease would break a live outer scan — `SELECT f(x) FROM t` with an EXCEPTION block inside `f` runs subtransactions to completion and failure while the scan on `t` is open and rightly holding its connection, and handing that connection to a second reader is exactly the reply-stream aliasing the lease prevents.

The narrower rule was itself a fix. Without any subtransaction release, a caught error leaked a slot: the scan raised, I1 left the lease alone, the EXCEPTION block swallowed the error, and nothing gave the slot back until end of transaction. Forty caught failures in one statement consumed all 32 slots, and every attempt afterwards reported "too many concurrent Valkey scans" in place of whatever had actually gone wrong.

**Mid-conversation discard, end to end.** `vfdw_conn_begin`/`vfdw_conn_end` bracket a conversation and are called by the batch layer, not by scan or flush code directly: `vfdw_batch_begin` marks the connection (`src/vfdw_cmd.c:159`) and `vfdw_batch_end` drains every outstanding reply before clearing the flag (`src/vfdw_cmd.c:253`). Draining is what makes "clean" a fact rather than a hope — replies nobody read would otherwise sit in the stream and be mistaken for the next statement's. On any unwind the flag is left set, and the transaction or subtransaction callback closes the connection. `vfdw_conn_close` (`src/vfdw_conn.c:42`) frees the context but leaves the pool entry in place with its key and node identity, resetting `resp3`, `database` and `script_loaded` — reopening is cheap relative to getting reuse wrong, and a new server has never seen our Lua program whatever the old one had. That last field is a per-connection cache consulted by the flush (`src/vfdw_flush.c:401`) to decide whether to pipeline a `SCRIPT LOAD` ahead of the `EVALSHA`, and cleared again when the server answers `NOSCRIPT` (`src/vfdw_flush.c:664`).

### Principal functions

| Function | File | Responsible for |
|---|---|---|
| `vfdw_get_connection` | `src/vfdw_conn.c:521` | Acquire for a non-cluster server; refuses `cluster 'true'` |
| `vfdw_get_connection_cluster` | `src/vfdw_conn.c:527` | Acquire from a caller that can route across a cluster |
| `vfdw_get_connection_node` | `src/vfdw_conn.c:533` | Acquire a connection to one named cluster node |
| `vfdw_release_connection` | `src/vfdw_conn.c:457` | Return a lease; never called on an error path |
| `vfdw_conn_take_free_slot` | `src/vfdw_conn_pool.c:134` | Pick the entry that serves a request, and lease it |
| `vfdw_conn_claim` | `src/vfdw_conn_pool.c:96` | Repoint a free entry at a node, closing its socket first |
| `vfdw_conn_close` | `src/vfdw_conn.c:42` | Drop the socket and everything that described it, keeping the entry |
| `vfdw_conn_open` | `src/vfdw_conn.c:370` | Connect, bound the reader, TLS, AUTH, HELLO — in that order |
| `vfdw_conn_command` | `src/vfdw_conn.c:105` | One setup command with a deadline, under the in-conversation flag |
| `vfdw_conn_force_handshake` | `src/vfdw_conn.c:153` | Force lazy TLS to complete so failures are reported as TLS failures |
| `vfdw_conn_authenticate` | `src/vfdw_conn.c:210` | One- or two-argument AUTH, without naming the credential on failure |
| `vfdw_conn_negotiate_resp3` | `src/vfdw_conn.c:285` | `HELLO 3`, with the decline-versus-failure distinction |
| `vfdw_conn_check_password_required` | `src/vfdw_conn.c:431` | The non-superuser-needs-a-password refusal |
| `vfdw_conn_select_db` | `src/vfdw_conn.c:321` | Move the logical database, or refuse it on a cluster |
| `vfdw_conn_peek` | `src/vfdw_conn.c:680` | Non-destructive "is anything mid-conversation" probe for pre-commit |
| `vfdw_conn_inval_callback` | `src/vfdw_conn.c:602` | Mark entries whose server or mapping changed |
| `vfdw_conn_xact` | `src/vfdw_conn_xact.c:25` | Close dirty or invalidated connections and clear all leases |
| `vfdw_conn_subxact` | `src/vfdw_conn_xact.c:94` | Close dirty connections; release only this subtransaction's leases |
| `vfdw_xact_callback` | `src/vfdw_xact.c:36` | Sequence buffer work ahead of pool work at every transaction event |
| `vfdw_xact_ensure_registered` | `src/vfdw_xact.c:105` | The single registration site for both callbacks |
| `vfdw_tls_attach` | `src/vfdw_tls.c:321` | Build the SSL_CTX and SSL, then hand the session to libvalkey |
| `vfdw_tls_set_identity` | `src/vfdw_tls.c:282` | SNI, and the hostname pinning libvalkey does not perform |
| `vfdw_tls_verify_reason` | `src/vfdw_tls.c:126` | Take the specific X509 failure and the hint that matches it |
| `vfdw_tls_take_error` | `src/vfdw_tls.c:153` | Pull OpenSSL's queued reason into palloc'd memory |

---

## I/O, command construction and error classification

Three files sit between the rest of the wrapper and the socket: `src/vfdw_io.c` moves bytes and waits, `src/vfdw_cmd.c` builds commands and owns replies, `src/vfdw_error.c` turns a failure into a SQLSTATE. Nothing above them calls libvalkey's blocking helpers, allocates a reply, or reads `errstr` directly. That concentration is what lets three separate invariants — I6 (every wait is interruptible), I3 (lengths travel with data) and I2 (nothing unchecked reaches error reporting) — be enforced in one place each rather than at every call site.

### Every blocking wait is interruptible

Invariant I6 is stated in `src/vfdw_io.h:6-14` and implemented entirely by `vfdw_io_wait` (`src/vfdw_io.c:65-105`). The mechanism has four parts, and all four are needed:

The socket is non-blocking. `vfdw_io_connect` sets `VALKEY_OPT_NONBLOCK` (`src/vfdw_io.c:186`), and the comment there records that this is not a performance choice: libvalkey only treats `EWOULDBLOCK` as benign when `VALKEY_BLOCK` is clear, so a blocking context would turn every short read into a hard I/O error.

Bytes move through the incremental primitives. `vfdw_io_flush` loops on `valkeyBufferWrite` until it reports `done` (`src/vfdw_io.c:368-383`); `vfdw_io_get_reply` alternates `valkeyGetReplyFromReader` and `valkeyBufferRead` (`src/vfdw_io.c:385-408`). The parse attempt deliberately precedes the wait — a complete reply may already be sitting in the reader from an earlier read, which is exactly what makes pipelining work.

The waiting in between is `WaitLatchOrSocket(MyLatch, …, conn->fd, timeout_ms, PG_WAIT_EXTENSION)` with `WL_LATCH_SET | WL_EXIT_ON_PM_DEATH` plus the socket event and, when a deadline exists, `WL_TIMEOUT` (`src/vfdw_io.c:86-91`). A bare `poll()` on the socket would see only the socket. Waking on `MyLatch` is what makes a cancel or terminate request land during a wait, and `WL_EXIT_ON_PM_DEATH` is what stops the backend outliving the postmaster as an orphan holding a Valkey connection. This code never constructs or caches a `WaitEventSet` of its own; PostgreSQL's own wrapper is called once per wait. The `#if PG_VERSION_NUM >= 180000` include of `storage/waiteventset.h` (`src/vfdw_io.c:21-23`) exists only because the declarations moved between headers.

`CHECK_FOR_INTERRUPTS()` is called both before the wait and immediately after `ResetLatch` (`src/vfdw_io.c:73`, `:100`). The second one is the load-bearing call: the latch may have been set by a cancel, and acting on it here is what converts "the latch woke us" into "the query aborts". It was measured: deleting that `CHECK_FOR_INTERRUPTS` takes the TAP run from 7 wallclock seconds to 37, because the 30-second server-side block runs to completion. `test/tap/t/001_cancel.pl` covers a blocking read, a scan mid-flight and a cancel during the pre-commit flush; `test/regress/sql/io.sql` asserts the same property from one session by checking that `statement_timeout` cancels a 30-second `BLPOP` in under ten seconds, elapsed time and all. A test asserting only the SQLSTATE would pass with a blocking `recv()` underneath.

I6 depends on interrupts not being held, which is why the write path flushes at `XACT_EVENT_PRE_COMMIT` and nowhere else: every other transaction event runs inside `HOLD_INTERRUPTS`, where `CHECK_FOR_INTERRUPTS` is a no-op and the invariant cannot be satisfied, so no byte may reach the wire from those arms (`src/vfdw_xact.h:23-36`, `src/vfdw_flush.c:6-12`).

`test/spike/v1_nonblocking_io.c` is the standalone program that established the four libvalkey behaviours this design rests on, none of which the public headers state. It stands `poll()` in for `WaitLatchOrSocket`, since the question it answers is about the library rather than about PostgreSQL's event loop.

### Deadlines

A deadline is an absolute `TimestampTz`, produced by `vfdw_io_deadline(timeout_ms)` (`src/vfdw_io.c:29-35`), with `0` meaning "no deadline". `src/vfdw_io.h:34-38` gives the reason for making them absolute rather than per-call: a multi-round-trip operation must not be able to extend its own budget one wait at a time. `vfdw_io_wait` recomputes the remaining milliseconds on entry and raises `ERRCODE_CONNECTION_FAILURE` if the budget is already spent, before waiting at all (`src/vfdw_io.c:75-84`); a `WL_TIMEOUT` return raises the same error.

Two callers arm deadlines. `vfdw_conn_command` computes one per command from `command_timeout_ms` (`src/vfdw_conn.c:109`, `:288`), and `vfdw_batch_begin` computes one per batch (`src/vfdw_cmd.c:151`). The connect path has its own budget from `connect_timeout_ms`, which also becomes libvalkey's `connect_timeout` (`src/vfdw_io.c:188-207`) — `io.sql` pins that against an unroutable address, so the wrapper's deadline fires rather than the kernel's much longer one.

### Connecting

`vfdw_io_connect` is the single funnel every connection passes through, including the diagnostic entry points in `src/vfdw_testfuncs.c` that are handed a host and a port and never look at a foreign server. A non-blocking connect returns before the handshake finishes, so `vfdw_io_complete_connect` waits for writability and then reads `SO_ERROR` — writability alone is not success (`src/vfdw_io.c:115-155`). Afterwards it clears `conn->err` and `errstr`, because any error libvalkey recorded while the connect was in flight is stale and would make the first `valkeyBufferWrite` fail its own "this context has already seen an error" check.

This is the one region where a connection is released inline, and the reason is ownership: until the context is handed to the pool, nothing else has a reference to it, so a failure that did not close it would leak the descriptor outright. `vfdw_io_fail_and_close` (`src/vfdw_io.c:46-56`) fixes the order — copy the message, release, then report — because reporting first leaves unreachable code after a longjmp and releasing first leaves the message pointing into freed memory. That is also why `vfdw_io_complete_connect` contains the one `PG_TRY` in this layer (`src/vfdw_io.c:121-132`): it exists solely to close the half-built socket on the way out of the writability wait. Once `vfdw_conn_open` assigns the context to a pool entry (`src/vfdw_conn.c:396`), invariant I1 takes over and nothing is closed inline again.

### The allocation ceiling

libvalkey's reader offers two settings, applied by `vfdw_io_set_limits` (`src/vfdw_io.c:358-366`) from `max_reply_elements` and `reader_buffer_bytes` at connection open (`src/vfdw_conn.c:407`). Only one of them is a limit. `maxelements` refuses a multi-bulk header declaring more elements than allowed, before any element arrives (`vendor/libvalkey/src/src/read.c:540`, raising `VALKEY_ERR_PROTOCOL` "Multi-bulk length out of range"). `maxbuf` is a retention threshold — the size above which an empty read buffer is returned to the allocator rather than kept (`read.c:782`) — which is why the option is named `reader_buffer_bytes` and documented as not a limit (`src/vfdw_option.c:125-145`).

Neither bounds a single bulk string. One bulk string is one element, and it is allocated from the length its own header declares; libvalkey exposes no maximum-length setting. A peer answering `GET` with a gigabyte header therefore gets a gigabyte of backend memory before any wrapper code sees a byte — and under `tls 'false'` or `tls_verify 'none'` that peer need not be the server the operator configured.

The only hook the library offers is its allocator, so the bound lives there. `vfdw_io_install_alloc_ceiling` installs four wrappers over malloc/calloc/realloc/strdup (`src/vfdw_io.c:230-340`); each refuses by returning `NULL`, which libvalkey converts into `VALKEY_ERR_OOM` internally and which `vfdw_errcode_for_context` already reports as `ERRCODE_OUT_OF_MEMORY` (53200). Refusal is a return and nothing else — the library unwinds its own half-built objects and I1 leaves the connection to the transaction callback. The calloc wrapper compares as a quotient rather than a product, since the product is what would overflow. `strdup` is built on the malloc wrapper so there is one place the ceiling applies; its `strlen` is not an I3 violation because libvalkey only duplicates host names and option strings through it, never reply bytes.

**The ceiling is fixed rather than configurable, and the reason is recorded because it is a hardening change that had to be undone.** It was a server option first. `valkeySetAllocators` replaces one process-global struct of function pointers (`vendor/libvalkey/src/src/alloc.c:38-53`), so a per-server value makes a process-global weapon out of a tuning knob. Server options are read before the password check and before the cluster refusal (`src/vfdw_conn.c:502-513`), so any role holding `USAGE` on a server defined with a small value could lower the ceiling for every other server in the backend merely by *attempting* a connection that was then refused. A hardening change that hands an unprivileged caller a cross-server denial of service is worse than the unbounded allocation it was meant to prevent.

Arming happens lazily from `vfdw_io_connect` (`src/vfdw_io.c:171`) rather than from a reader of options, because arming from an options reader left the diagnostic entry points — which read no server at all — outside the bound.

Two limits are stated honestly and both are real. It bounds *every* libvalkey allocation, not only a reply: the command buffer and the reply structs come through the same wrappers, so it is wider than its name. And it is approximate in size, because the reader's buffer is an sds string that grows greedily — doubling below 1 MB and by 1 MB above it (`vendor/libvalkey/src/src/sds.c:217-223`) — so a reply somewhat under the ceiling can still ask for a step over it.

Because a bound no statement can reach is a bound no statement can prove, `vfdw_io_set_alloc_ceiling_for_test` exists (`src/vfdw_io.c:351-356`) behind the superuser-only `valkey_fdw_test` extension. `io.sql` lowers it to 64 KiB, reads a 256 KiB value and asserts SQLSTATE 53200, then restores it and reads the same value in full — the second read is what proves the ceiling refused the first one and not the data. The setting is process-global and outlives the statement, exactly like the thing it moves, so the suite is responsible for putting it back.

### Binary-safe command construction

A `VfdwCmd` (`src/vfdw_cmd.h:46-53`) is a parallel `argv` / `arglens` pair with a count, issued through `valkeyAppendCommandArgv`. Arguments are never C strings on the wire, so a key or value containing a NUL byte travels whole instead of being truncated at the first zero — invariant I3: a reply passed on as `reply->str`, to C string APIs that take no length, loses everything past the first NUL. `io.sql` round-trips `\x610062`, an all-NUL value and invalid UTF-8 to pin it.

`vfdw_cmd_ensure` grows both vectors in one function (`src/vfdw_cmd.c:70-87`); the comment gives the reason plainly — an argument whose length lives in a differently-sized array than its pointer is the shape of every off-by-one in this kind of code. Three appenders express intent rather than convenience:

| Appender | For | Note |
|---|---|---|
| `vfdw_cmd_add_bytes` | anything from Valkey, a `bytea`, or a text Datum | length is supplied by the caller |
| `vfdw_cmd_add_cstr` | our own literals and NUL-terminated text we own | the one legitimate `strlen` in this file |
| `vfdw_cmd_add_int` | integers | formats into a 24-byte buffer in the command's context and passes the returned length |

`add_bytes` stores the pointer, not a copy (`src/vfdw_cmd.c:93`). The bytes must stay live until `vfdw_batch_add` returns, at which point `valkeyAppendCommandArgv` has formatted them into the connection's output buffer (`vendor/libvalkey/src/src/valkey.c:1221-1238`). `scripts/lint.sh:307-310` notes every `strlen` in `src/` for review against I3 rather than banning it, since a handful of uses are legitimate and each states why at its site.

### The batch: queueing, flushing and reply ownership

`VfdwBatch` (`src/vfdw_cmd.c:23-39`) is a pipeline over one connection. `vfdw_batch_begin` reads `pipeline_batch` and `command_timeout_ms` from the connection's options, registers a reset callback on the caller's memory context, and marks the connection in conversation.

`vfdw_batch_add` queues a command and auto-flushes once `pipeline_batch` commands are queued (`src/vfdw_cmd.c:182-202`). Without that bound, a scan over a large keyspace would queue every command before sending any, holding the whole request set in the output buffer and the whole reply set in the reader. `vfdw_batch_next` releases the previous reply, flushes anything still queued — a reply cannot arrive for a command still sitting in the output buffer — and takes the next reply in queue order (`src/vfdw_cmd.c:210-234`). `vfdw_batch_end` drains everything still outstanding before declaring the connection clean, because unread replies would be paired with the *next* statement's commands.

**Callers never free a reply.** The batch owns `current`; it stays valid exactly until the next `vfdw_batch_next` or `vfdw_batch_end`. This is not a convenience, it is a leak-proofing measure: a caller that raises partway through handling a reply cannot leak it, because it never owned it.

`vfdw_batch_flushes` (`src/vfdw_cmd.c:176-180`) exists solely so the `pipeline_batch` option has an observable consequence. A probe that reports how many values matched returns the same number at every depth, so it cannot distinguish a batch honouring the configured bound from one ignoring it. `io.sql` asserts exact flush counts — 71 at `pipeline_batch '7'` and 1 at the default 256, for 500 commands — because those are the wrapper's own arithmetic and not the server's.

One consequence deserves stating: `vfdw_io_flush` waits only for writability and never drains replies while it waits, which has the shape of a deadlock — fill the server's output buffer, the server stops reading, our writes never complete. It is not one, and `io.sql` records why: Valkey ships `client-output-buffer-limit normal 0 0 0`, so the server buffers replies in its own memory and keeps reading; an operator who sets a finite limit gets a closed connection rather than a stall, which arrives as a connection error. The suite asserts a 200,000-command pipeline completes. `src/vfdw_testkeys.c:44-50` chunks its own probe pipeline for the same reason.

### The reset callback that makes PG_TRY unnecessary

`vfdw_batch_cleanup` (`src/vfdw_cmd.c:131-141`) is registered with `MemoryContextRegisterResetCallback` on the context the batch was created in. An error unwinding past the batch resets that context, which runs the callback, which returns the held reply to libvalkey. No caller writes cleanup code on an error path, which is invariant I1 in its most direct form.

The other half matters as much: the callback does **not** clear the connection's conversation flag. `vfdw_conn_end` is reached only from `vfdw_batch_end` on the success path (`src/vfdw_cmd.c:253`), so a connection abandoned mid-flight is still flagged `in_conversation`, and `vfdw_conn_xact`'s abort branch closes it rather than handing on a reply stream at an unknown offset (`src/vfdw_conn_xact.c:54-56`). Memory is reclaimed by the context; connection safety is decided at transaction end.

The batch is allocated in a dedicated context wherever it is created repeatedly. `vfdw_scan_cluster_attach` (`src/vfdw_scan_cluster.c:143-167`) ends and rebuilds the batch on every node advance and every redirect, resetting `batch_cxt` in between; allocating batches in the scan context left a struct and a reset callback per node per pass for the life of the query.

### Reply shape

`vfdw_reply_expect(reply, allowed, what)` (`src/vfdw_cmd.c:296-328`) takes a mask built from `VFDW_RTYPE(t)` — `1 << t`, with libvalkey's reply types running 1 to 14. It refuses a `NULL` reply, converts a server error reply into an `ereport` carrying the server's own classified message, and otherwise reports the arriving shape by name via `vfdw_reply_type_name`. It handles the error case through `vfdw_error_reply_text` and deliberately does **not** free the reply, because here the batch owns it — the comment at `src/vfdw_cmd.c:308-314` records that keeping the copy, the NULL-body guard and the encoding check in one shared function is what stops this site from disagreeing with `src/vfdw_error.c`, which is how it came to lack the NULL guard its sibling had.

It validates the **top level only**. Anything walking into an aggregate must guard its own elements: `vfdw_reply_child` (`src/vfdw_cmd.c:283-294`) reads `elements` before indexing `element[]` and refuses a NULL child, because the hash lookup, the zset pair decode and the cluster shard parse all index with arithmetic derived from a count the server supplied. Writing an integer reply into a fixed-size buffer is a heap overflow of exactly that shape, and a scan reaching it takes the backend down mid-query. `vfdw_scan_expect_string` (`src/vfdw_scan.c:187-199`) is the same discipline for element types — an integer element read as `(str, len) = (NULL, 0)` would be sent back to the server as an empty cursor. `src/vfdw_testprobe.c:171-216` exercises the arity guard directly with a fabricated reply, since nothing arriving over the network can produce it.

### The error taxonomy

Errors are classified along two axes, and the split is the taxonomy.

**Context errors** are transport failures libvalkey recorded on the `valkeyContext`. `vfdw_errcode_for_context` (`src/vfdw_error.c:20-37`) maps them:

| libvalkey error | SQLSTATE |
|---|---|
| `VALKEY_ERR_IO`, `VALKEY_ERR_EOF`, `VALKEY_ERR_TIMEOUT` | `ERRCODE_CONNECTION_FAILURE` |
| `VALKEY_ERR_PROTOCOL` | `ERRCODE_PROTOCOL_VIOLATION` |
| `VALKEY_ERR_OOM` | `ERRCODE_OUT_OF_MEMORY` |
| anything else | `ERRCODE_FDW_ERROR` |

`vfdw_error_from_context` (`src/vfdw_error.c:205-220`) copies `errstr` *before* reporting. `errstr` lives inside the context, and a later change that released the connection anywhere on this path would turn the report into a read of freed memory. The connection is not released here; see I1.

**Reply errors** are the server's own `-ERR` frames, which carry a machine-readable prefix. `vfdw_errcode_for_reply` (`src/vfdw_error.c:68-86`) translates it, so a client gets a code worth branching on instead of one generic FDW error:

| Prefix | SQLSTATE | Meaning to a client |
|---|---|---|
| `WRONGTYPE` | `ERRCODE_DATATYPE_MISMATCH` | the key holds another type — a schema problem |
| `WRONGPASS` | `ERRCODE_INVALID_PASSWORD` | credentials |
| `NOAUTH` | `ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION` | not authenticated |
| `NOPERM` | `ERRCODE_INSUFFICIENT_PRIVILEGE` | ACL refusal |
| `NOPROTO` | `ERRCODE_FEATURE_NOT_SUPPORTED` | RESP3 unavailable |
| `LOADING`, `MASTERDOWN`, `CLUSTERDOWN` | `ERRCODE_CANNOT_CONNECT_NOW` | transient — retry is meaningful |
| `BUSY` | `ERRCODE_LOCK_NOT_AVAILABLE` | a script is running |
| `OOM` | `ERRCODE_OUT_OF_MEMORY` | server-side memory limit |
| `READONLY` | `ERRCODE_READ_ONLY_SQL_TRANSACTION` | writing to a replica |
| `MISCONF` | `ERRCODE_CONFIG_FILE_ERROR` | server configuration |
| `CROSSSLOT` | `ERRCODE_FEATURE_NOT_SUPPORTED` | keys span hash slots |
| `EXECABORT` | `ERRCODE_TRANSACTION_ROLLBACK` | transaction abandoned |

**Matching is on a word boundary**, in both the classifier (`src/vfdw_error.c:80-82`) and `vfdw_reply_has_prefix` (`src/vfdw_error.c:88-122`): the prefix must be followed by a space or end the body. The recorded reason is not tidiness. A raw prefix test answers true for a code that merely *starts* with another — `WRONGTYPEX` for `WRONGTYPE` — and the two consumers disagree about what that costs. The classifier would hand such a body a generic SQLSTATE, while the scan's skippable rule (`src/vfdw_scan.c:382-388`) would drop the row without a word. The looser of the two tests was therefore the one deciding whether data went missing. `vfdw_script_classify` (`src/vfdw_script.h:80-96`) matches the same way on `(str, len)` rather than on a `valkeyReply`, because the write program's verdict has usually been copied out of a batch-owned reply already.

**`vfdw_safe_text` is where I2 is enforced** (`src/vfdw_error.c:147-164`). It returns a palloc'd copy when the bytes verify against the database encoding, and otherwise a description of their length: `a N-byte value that is not valid in encoding X`. An `ERROR` is one of the few places a value reaches both the client and the server log without passing through the read path's `pg_verifymbstr`, and where `client_encoding` differs from the server encoding the conversion runs *during* error reporting. The rule is about bytes and not about ownership, which is the part most easily got wrong: Valkey echoes the client's own arguments back inside some error messages, and a `bytea` key column carries arbitrary bytes by design, so a refusal that echoed a key verbatim would put a raw `0xff` into a UTF-8 database's log (`src/vfdw.h:48-67`, `src/vfdw_refuse.c:108-145`, `src/vfdw_flush.c:117-138`). `vfdw_val_printable` in `src/vfdw_score.c` is deliberately *not* a second copy of this — it answers the narrower question of whether text not yet proved to be a number is worth echoing, is stricter (printable ASCII, at most 64 bytes), and must not be relaxed into this one.

`vfdw_error_reply_text` (`src/vfdw_error.c:166-203`) is the single function that combines the three steps, so they cannot be half-applied. It classifies on the **raw** bytes and returns the **checked** text, so a message whose tail is unreportable still yields `WRONGTYPE` rather than a generic code. Its empty-body guard tests both `str == NULL` and `len == 0`, and the length half is not redundant: libvalkey allocates an empty string for a `-\r\n` frame, so the NULL test alone let an empty message through and the user got a bare `DETAIL:` with nothing after it. This was found by fabricating a frame that had been recorded as impossible to produce — the reason given was that no real server sends one, which was true and beside the point. `scripts/mutate.py` keeps it honest: reverting the length test must turn `resp` red on the `fault` topology.

Three raise helpers differ only in what they do with the reply, and choosing among them is entirely a question of who owns it:

| Helper | Reply ownership |
|---|---|
| `vfdw_error_from_context` | no reply; does not touch the connection |
| `vfdw_error_from_reply` | caller (or batch) keeps it; nothing is freed |
| `vfdw_error_from_reply_free` | takes ownership: copies, frees, then reports, in that order |

The ordering inside `vfdw_error_from_reply_free` (`src/vfdw_error.c:242-259`) is the whole point of its existing — reporting first never returns, so the reply would leak; releasing first leaves the message pointing into freed memory. Encapsulating it means no caller has to get it right.

Every message finally goes through `vfdw_ereport` (`src/valkey_fdw.c:92-107`), which passes both `msg` and `detail` as arguments to `"%s"` and never as format strings, so bytes the wrapper did not write cannot be read as format directives. `scripts/lint.sh:42` bans a non-literal first argument to `errmsg`, `errdetail`, `errhint` or `errcontext` outright.

### Principal functions

| Function | File | Responsibility |
|---|---|---|
| `vfdw_io_connect` | `src/vfdw_io.c` | the one path every connection takes: non-blocking connect, deadline, ceiling arming |
| `vfdw_io_wait` (static) | `src/vfdw_io.c` | the only blocking wait in the extension; latch, socket, postmaster death, deadline |
| `vfdw_io_deadline` | `src/vfdw_io.c` | turn a timeout in milliseconds into an absolute deadline |
| `vfdw_io_flush` | `src/vfdw_io.c` | drain the output buffer without blocking |
| `vfdw_io_get_reply` | `src/vfdw_io.c` | parse-then-wait loop returning one reply; caller takes ownership |
| `vfdw_io_install_alloc_ceiling` | `src/vfdw_io.c` | install the process-global allocation bound into libvalkey |
| `vfdw_io_set_limits` | `src/vfdw_io.c` | apply `max_reply_elements` and `reader_buffer_bytes` to a reader |
| `vfdw_cmd_add_bytes` / `_add_cstr` | `src/vfdw_cmd.c` | append an argument with its length; the I3 boundary |
| `vfdw_batch_begin` / `_end` | `src/vfdw_cmd.c` | open and close a pipeline, including the reset callback and the drain |
| `vfdw_batch_add` | `src/vfdw_cmd.c` | queue a command, auto-flushing at `pipeline_batch` |
| `vfdw_batch_next` | `src/vfdw_cmd.c` | release the previous reply, flush, return the next in order |
| `vfdw_reply_expect` | `src/vfdw_cmd.c` | assert a reply's top-level shape, or raise with the server's message |
| `vfdw_reply_child` | `src/vfdw_cmd.c` | bounds-checked access to an aggregate element (an index taken on trust is a heap overflow) |
| `vfdw_errcode_for_context` | `src/vfdw_error.c` | transport failure to SQLSTATE |
| `vfdw_errcode_for_reply` | `src/vfdw_error.c` | server error prefix to SQLSTATE, on a word boundary |
| `vfdw_reply_has_prefix` | `src/vfdw_error.c` | word-boundary prefix test for callers that handle a code themselves |
| `vfdw_safe_text` | `src/vfdw_error.c` | the I2 gate: bytes to text an error may carry |
| `vfdw_error_reply_text` | `src/vfdw_error.c` | copy, guard, encoding-check and classify a reply body in one call |
| `vfdw_error_from_reply_free` | `src/vfdw_error.c` | raise from a reply the caller owns, in the only safe order |

---

## Options, table shape and row identity

Three questions have to be answered before this wrapper sends a single command: what a `CREATE SERVER`/`CREATE FOREIGN TABLE` statement is allowed to say, what each column of a foreign table is a view of, and — for a write — what names the row that a DML statement is talking about. All three are settled in the catalog layer, at DDL time or at plan time, and never per row in the executor. That placement is the point: the defects it forecloses are per-row reads off the end of a fixed array and silently ignored options, and neither is reachable if the shape is resolved once and refused when it does not resolve.

### The option table is the only description of an option

Every option the wrapper accepts is one row of `vfdw_options[]` (`src/vfdw_option.c:53`), a NULL-name-terminated array. Invariant **I7** — options are data, not code — is stated at the top of `src/vfdw_option.h` and is the reason the array exists: a rule expressed as a branch in a `strcmp` chain goes quietly dead when the option is renamed, still compiling and still passing. A validator branch guarded on `"singleton_key "` with a trailing space is the whole failure: it matches nothing, the conflict checks behind it never run, and no build or test says so; the regression suite has a block dedicated to catching that typo (`test/regress/sql/options.sql`).

A row is a `VfdwOptionDef` (`src/vfdw_option.h:69`):

| Field | Meaning |
|---|---|
| `name` | The option name as written in `OPTIONS (...)`. |
| `context` | The catalog OID the option may appear on: `ForeignServerRelationId`, `UserMappingRelationId`, `ForeignTableRelationId`, or `AttributeRelationId` for a per-column option. Lookup is by (name, context), so the same name in two contexts would be two independent rows. |
| `kind` | `VFDW_OPT_STRING`, `_INT`, `_BOOL`, `_ENUM` or `_PATH`. Selects the parser and, through `vfdw_option_kind_label`, the type reported to SQL. |
| `minval` / `maxval` | Inclusive bounds, `VFDW_OPT_INT` only. Checked on every parse, so the runtime reader cannot accept a value the validator rejected. |
| `values` | NULL-terminated permitted values, `VFDW_OPT_ENUM` only. `vfdw_parse_enum` returns the index, which is what gets stored (`tls_verify` is an `int` index into this array, `tabletype` is cast to `VfdwTableType`). |
| `defval` | The documented default as a string, or NULL for no default. The runtime defaults are read back out of this field rather than restated — see below. |
| `sensitive` | Redact the value from error text, log output and EXPLAIN. `vfdw_reject` prints the offending value only when this is false (`src/vfdw_option.c:251`). |
| `priv` | Who may set it. See the next subsection. |
| `summary` | The published one-line description, surfaced through `valkey_fdw_options()`. |

**The validator.** `valkey_fdw_validator` (`src/valkey_fdw.c:216`) receives the option list and the context OID from core. For each element it does three things and nothing else: look the name up in the table (an unknown name is refused with a hint built by `vfdw_option_hint`, which enumerates the same array, so the hint cannot drift from what is accepted); refuse a duplicate name uniformly, because detection done per option, by asking whether that option has a value yet, is defeated by a falsy one — `database '0'` twice; and call `vfdw_validate_option`, which dispatches on `kind` and then applies the privilege rule. The one cross-option rule that a validator can enforce, `vfdw_check_key_options`, runs last so that an unknown or malformed option is reported before a combination rule fires on values that are themselves nonsense.

**The runtime readers.** `vfdw_read_server_options` and `vfdw_read_user_options` (`src/vfdw_option.c:551`, `:581`) walk a catalog option list, resolve each name in the same table, and parse with the same parsers the validator used — which is what makes it impossible for a value to be accepted at DDL time and interpreted differently later. `vfdw_map_read_table_options` and `vfdw_map_read_column_options` (`src/vfdw_map.c:171`, `:111`) do the same for table and column options. Any name that does not resolve in the reader's context is skipped rather than raising, since a reader is handed one object's list.

Defaults are not restated in the readers. `vfdw_server_defaults` (`src/vfdw_option.c:476`) fills each field by looking the option up and parsing its own `defval`, through `vfdw_default`/`vfdw_default_int`. The reason is recorded in the comment above them and is worth repeating: a default restated in a reader is a second copy of the same fact, and the copy the documentation describes then drifts from the one the code applies. `vfdw_apply_server_option` (`:503`) is split out of the reading loop because the two change for different reasons — the loop follows the catalog, the dispatch grows a line per option — and together they would exceed the 60-line function gate.

**The SQL surface.** `valkey_fdw_options()` (`src/valkey_fdw.c:332`) materialises the table as six columns matching the `OUT` list in `sql/valkey_fdw--0.1.sql:40`. It exists so the suites can assert against the code's own answer instead of a transcription: `test/unit/run.sh` fails the build when an option is missing from the README table, when the README names an option the code does not accept, when a `sensitive` option carries a default, and when the README's Superuser column disagrees with `priv`. `test/regress/expected/options.out` records the full inventory, so adding or changing an option shows up as a reviewable diff.

Two places deliberately do not drive off the table, and both are documented where they sit: `src/vfdw_writable.c` compares option names as literals because it must not raise (below), and `vfdw_check_key_options` carries its own three-name array of the mutually exclusive key options.

### The privilege field, and what a merged option list costs

`VfdwOptPriv` (`src/vfdw_option.h:51`) has three values. `VFDW_OPTPRIV_NONE` is unrestricted. `VFDW_OPTPRIV_SUPERUSER` marks the three TLS file names: each is an absolute path the backend opens with its own privileges on a host the caller may have no account on, so the privilege belongs to the option rather than to ownership of the object it hangs off. `VFDW_OPTPRIV_SUPERUSER_TO_DISABLE` marks `password_required`: anyone may turn the check on, only a superuser may turn it off, because only the off direction hands a role reach it did not have.

`vfdw_check_option_priv` (`src/vfdw_option.c:359`) names no option — it switches on `priv` alone, so an option cannot be renamed out from under its own guard — and it fails closed: a `priv` value the switch has never been taught leaves `restricted` true, so an incomplete edit produces a denied DDL statement rather than an ungoverned option. Value validation runs before the privilege check (`vfdw_validate_option`, `:418`), so a non-superuser writing `password_required 'nonsense'` is told the value is invalid, which is the truth, rather than being refused a privilege they would not have needed.

The consequence worth knowing before you meet it: `transformGenericOptions` hands the validator the **merged** option list, not the delta. A restricted option that is already set is presented again by every later `ALTER` of the same object, whatever that `ALTER` was for. So once a superuser sets `tls_ca_file` on a server, a non-superuser who owns that server can no longer run any `ALTER SERVER ... OPTIONS` on it at all. There is no narrower rule available — the validator is never shown the old list, so "did this statement change the restricted option" is not a question it can ask — and refusing is the direction that fails safe. postgres_fdw lives with the same shape on `password_required`.

`prefer_replica` is the option to read if you want the reasoning style of this table in one place (`src/vfdw_option.c:65`–`93`). Routing is not implemented and nothing on the read path consults the flag; the response was to narrow the published description to what is true, not to delete the option, because declaring a foreign server read-only is the only way this wrapper can be told that it points at a replica. Enforcement is `vfdw_refuse_prefer_replica` (`src/vfdw_refuse.c:75`), called from `vfdw_modify_connect` (`src/vfdw_modify.c:207`) against the options the acquisition actually used. It cannot be a bit withheld by `vfdw_map_writability`, which takes a relid and reads *table* options and so structurally cannot see a server option.

### Resolving a foreign table into a column map

`vfdw_map_build` (`src/vfdw_map.c:607`) turns a `Relation` plus its `ForeignTable` into a `VfdwTableMap` (`src/vfdw_map.h:102`): `natts` columns, indexed by `attnum - 1`, plus the table-level options and the attnums of the roles the write path addresses directly. The header of `src/vfdw_map.h` states why it exists rather than a bounds check: build every tuple from a two-element C array regardless of the relation's width, and a third column makes `BuildTupleFromCStrings` read past the end of that array, so any user who can `CREATE FOREIGN TABLE` can crash the backend. The answer is invariant **I4** — resolve a source for every attribute once, at plan time, and refuse the table if any attribute has none, after which the executor fills a `natts`-sized slot from a mapping known to be complete.

The build runs in a fixed order, and the order carries meaning:

1. `vfdw_map_read_table_options` — table options, then the three pairwise key-option conflict checks as a second line of defence for a catalog row nothing validated.
2. Per attribute: a dropped column gets `kind = VFDW_COL_DROPPED` and `attnum = InvalidAttrNumber` and is never touched again; everything else gets its type resolved and its column options read.
3. `vfdw_map_assign_defaults` — give every remaining attribute a source, or refuse.
4. `vfdw_map_check_types` — reject kinds that cannot mean anything for this table type.
5. `vfdw_map_index_roles` — record the member/score/value attnums and refuse duplicates.
6. `vfdw_map_check_implemented` — refuse shapes whose reader has not landed. Deliberately last, so a table that is both malformed and unimplemented is reported as malformed, which is the more useful of the two answers.

`vfdw_map_build_for_relid` (`:649`) is the same thing from a relid, opening the relation with `NoLock` because every caller is already holding one.

**Column kinds** (`src/vfdw_map.h:48`):

| Kind | Source of the value | Requires |
|---|---|---|
| `VFDW_COL_KEY` | the Valkey key name of the row | at most one per table |
| `VFDW_COL_VALUE` | the whole string value | `tabletype 'string'` |
| `VFDW_COL_FIELD` | one hash field, named by `field` | `tabletype 'hash'` |
| `VFDW_COL_MEMBER` | a list, set or zset member | `tabletype 'list'`, `'set'` or `'zset'` |
| `VFDW_COL_SCORE` | the zset score | `tabletype 'zset'` |
| `VFDW_COL_TTL` | time to live of the field it names | `tabletype 'hash'`, and the column must also carry `field` |
| `VFDW_COL_DISTANCE` | vector search score | the `search_index` option |
| `VFDW_COL_LEGACY_VALUE` | the legacy `value text[]` column | `legacy_value 'true'` |
| `VFDW_COL_DROPPED` | nothing; `attisdropped`, or provisionally "unclaimed" during the build | — |

`vfdw_row_fill_column` (`src/vfdw_row.c:237`) fills key, value, field, member and score today; ttl and distance are refused at plan time by `vfdw_map_check_implemented`, as are `legacy_value` tables. Refusing rather than returning NULL is a policy the project applies everywhere: a plausible empty result is the failure mode it exists to avoid.

**One column, one source.** `vfdw_map_check_single_source` (`:89`) counts the claims a column makes and refuses more than one, because silently preferring one over another makes a typo look as though it worked. The single exception is encoded in the count itself: `field` does not count as a claim when `ttl` is set, since a ttl column is required to name the field whose expiry it reports.

**Assigning defaults** (`vfdw_map_assign_defaults`, `:389`). `vfdw_map_survey` first locates a declared key column — refusing two — and counts the columns still unclaimed, ignoring dropped ones. Then:

- A `legacy_value` table takes the fixed positional `(key, value)` shape and returns; it is a separate shape rather than a special case threaded through the normal one, because it exists only for migration.
- A `singleton_key` table skips key claiming entirely: its identity is the option, so no column has to carry the key. A column may still declare `key 'true'` and will be filled with that fixed name, which is what lets such a table join its siblings on that column.
- Otherwise the first unclaimed column becomes the key (`vfdw_map_assign_key`), which is what makes a two-column table work with no options at all. A table with no key column and no `singleton_key` is refused with a hint naming both ways out.
- A single remaining column takes the one meaning its table type leaves for it (`vfdw_map_default_kind`, `:49`): a string table's spare column is the value, a list's or set's is the member. A hash's spare column could be any field and a zset's could be member or score, so those imply nothing and must be declared. The same function drives the refusal text, so the rule and its explanation cannot drift apart.
- Anything still unclaimed is an error that names **every** unsourced column (`vfdw_map_report_unsourced`, `:282`). With two spare columns on a string table the first one is not the one that is wrong — the table simply does not say which is the value — and blaming one sends the reader to the wrong line.

**Type resolution.** `vfdw_map_resolve_type` (`:531`) fills both I/O directions of a column in one place: `typinput`/`typioparam` inbound, `typoutput`/`typisvarlena` outbound, plus `is_binary` and `is_domain`. They are resolved together because they are one decision — whichever type the read path reads through, the write path must write through, or a value cannot survive a round trip. `is_binary` is taken from the **base** type, so a domain over `bytea` is byte-verbatim in both directions; with `atttypid` alone its bytes would be pushed back through `pg_verifymbstr` and `bytea_in`. `is_domain` exists because the binary branch bypasses the input function and with it the domain's constraint check, which the read path then has to apply itself. The function is exported so the diagnostic probes describe a type through exactly the same call a real table does.

**Role index.** `vfdw_map_index_roles` (`:572`) records `memberattno`, `scoreattno` and `valueattno` and refuses a table that names any role twice. The read path would not care — it would fill both columns with the same bytes — but on the write path two member columns holding different values describe two different rows and no rule chooses between them. This check is at the first plan and **not** at `CREATE`, and the comment above it corrects an earlier version that claimed otherwise: the validator runs once per catalog object with that object's own options, so when it runs for a column it sees that column's options and nothing else, and "two columns claim the same role" is not a question a single column's option list can answer.

### Three key-discovery options, at most one of them

A table finds its keys in exactly one of three ways, and the options are alternatives:

| Option | What the scan does |
|---|---|
| `singleton_key` | one fixed key; `vfdw_scan_plan` returns `VFDW_SCAN_SINGLETON` and no qual may displace it (`src/vfdw_plan.c:565`) |
| `keyprefix` | `SCAN MATCH <prefix>*`, the prefix escaped as a literal (`vfdw_plan_scan_pattern`, `:510`); the key column carries the complete key including the prefix, so `keyprefix` filters and never transforms |
| `keyset` | `SSCAN` over the named set; no `MATCH` is available and no key list is built, since only the server can say whether a named key is in the set |
| none | `SCAN` over the keyspace, narrowed only by whatever a qual supplies |

Documenting the exclusivity and then not enforcing it — the branch meant to check it left unreachable — accepts the combination and ignores one of the options silently at runtime. Here it is refused in three places for three different reasons. `vfdw_check_key_options` (`src/valkey_fdw.c:181`) is the one that matters: a table's whole option list arrives at the validator at once, so the mistake is caught at `CREATE` and at `ALTER`, where it was made. `vfdw_map_read_table_options` (`src/vfdw_map.c:220`) repeats it pairwise for a catalog row nothing validated. `vfdw_map_writability` counts them (`src/vfdw_writable.c:79`) so that such a table is not advertised as updatable through `information_schema` — before the validator check existed, a table naming two of them could be created and only failed at its first `SELECT`.

### `vfdw_map_writability` answers without building a map and without raising

`IsForeignRelUpdatable` is called by the rewriter for every foreign table it touches and by `information_schema.tables` for every relation in the database. `vfdw_map_build` refuses a `json` or `legacy_value` table by design, and the regression database is deliberately left holding several of those — so a map built here would break the next suite's first look at `information_schema` rather than the write it was asked about. That constraint is what `src/vfdw_writable.c` exists for, and its file header says so: it parses the same options a second time with parsers that report failure instead of raising, and the file-length gate makes the duplication visible rather than buried.

`vfdw_map_read_writability` (`:53`) collects the table options it needs into a small struct; anything unrecognised is treated as a table this wrapper will not write to, which is the safe answer. `vfdw_map_write_block` (`:134`) returns the first reason the table accepts no write at all, or NULL:

- `readonly 'true'`;
- `legacy_value` — no reader and no writer;
- a `search_index`, which routes the table to the unimplemented search phase;
- more than one key-discovery option, so no key can be resolved for a row;
- a column reading `ttl` or `distance` (`vfdw_map_has_unimplemented_column`, `:95`). This one walks `ATTNUM` in the syscache directly rather than through the relcache, because it must not open a relation and must not raise; attnums are contiguous, so the first miss is the end. Before it existed, writability was decided from table options alone and a hash table with a ttl column was advertised through `information_schema` as fully writable while it could not even be `SELECT`ed.

Otherwise the mask is `INSERT|UPDATE|DELETE`, except for `tabletype 'list'`, which gets `INSERT|DELETE` with its own reason: a list row has no identity that survives its neighbours moving — `LREM` removes by value and a concurrent push shifts every position — so there is nothing an `UPDATE` could name. The recorded masks are in `test/regress/expected/ddl.out:136` (28 = full, 24 = list, 0 = blocked).

The reason travels back with the mask through the `detail`/`hint` out-parameters, and that is not a convenience. The refusal message used to carry one hardcoded explanation, so a table blocked for having a search index was refused with a paragraph about list row identity. `vfdw_refuse_unwritable` (`src/vfdw_refuse.c:21`) puts whatever comes back into the `errdetail`, after handling `readonly` with its own message and SQLSTATE 25006. It runs at plan time (`vfdw_modify_plan`) **and** again at `BeginForeignModify`, because a plan cached before an `ALTER FOREIGN TABLE` must not bypass either gate, and once more from `src/vfdw_copy.c:83`, which has no plan to read.

### Row identity per table shape

`vfdw_rowid_shape` (`src/vfdw_rowid.c:39`) is the identity table below expressed as code, and it is exhaustive over `VfdwTableType` with no `default` arm — one would let the next table type added to the enum inherit whatever rule happened to be written last, silently, where a missing arm is a compiler warning.

| Shape | Identity | Junk injected |
|---|---|---|
| `string`, `hash` | the key | `vfdw_key_aN` |
| `list`, `set`, `zset` | (key, member) | `vfdw_key_aN`, `vfdw_member_aM` |
| any shape with `singleton_key` | the option itself supplies the key | key junk omitted; member junk still injected for the collection types |

`want_key` is false whenever the table has a `singleton_key`, even when it also declares a `key 'true'` column, and `keyattno` may then legitimately be `InvalidAttrNumber` (`src/vfdw_rowid.h:42`). The reason is that a junk `Var` over a column filled with a constant would let a row name a key the table does not own.

`vfdw_rowid_add_update_targets` is registered as `AddForeignUpdateTargets` and injects each wanted junk column as a `Var` over a **real** table column, named `vfdw_<role>_a<attnum>` by `vfdw_rowid_junk_name`. The embedded attnum mirrors core's `ctidN` convention and is not decoration: two valkey tables attached as partitions of one parent may carry their key column at different attnums, and `add_row_identity_var` raises *conflicting uses of row-identity name* when one name maps to two different `Var`s. That collision is the only behavioural, mutation-testable property of the naming scheme, since `AddForeignUpdateTargets` and `BeginForeignModify` necessarily use the same builder and a test comparing one against the other would be a tautology. The callback is never reached for `INSERT` — core does not call it — so there is no `CMD_INSERT` branch to get wrong. Building the map here is safe and deliberate: this is a plan-time path, unlike `IsForeignRelUpdatable`, so a shape the reader refuses is refused for its own reason rather than with a message about writes. `src/vfdw_rowid.c:27` also records that `optimizer/appendinfo.h` must be included by name, because `add_row_identity_var` is not reached transitively through `optimizer/planmain.h` and the resulting build failure reads as a missing prototype for a function that plainly exists.

For a `DELETE`, the callback additionally asks for a whole-row `Var` (`:144`). A `DELETE` has no new row, so without it a `DELETE ... RETURNING` gets an all-null tuple — core stores one, the row count is right, every column is NULL — which is the plausible-empty-result failure this project refuses everywhere. Core adds a whole-row junk column of its own for a foreign table only when row triggers exist, and a deferred write has no remote `RETURNING` to fetch the old row from, so the row must be captured locally at scan time. It is requested unconditionally on `DELETE` rather than only when `RETURNING` is present, because `root->parse->returningList` is the *query's* list and a `DELETE` inside a CTE or a rule can acquire one later in planning; one unused whole-row `Var` costs a projection core was doing anyway.

The junk names then travel in the plan rather than being rebuilt: `vfdw_modify_junk_names` (`src/vfdw_modify.c:100`) stores exactly two `String`s, in `{key, member}` order, at `VFDW_MOD_PRIV_JUNK_NAMES`, either of which may be empty when the shape wants no column for that role. Always two, so position carries the role and no reader has to guess what a one-element list held. `vfdw_rowid_resolve` (`src/vfdw_rowid.c:190`) resolves them against the subplan targetlist at `BeginForeignModify`; a name the plan promised and the executor cannot find is an `elog`, following postgres_fdw's precedent, because it is an internal inconsistency and not a user error. `vfdw_rowid_find_wholerow` is the exception — a plan that never asked for the whole row will not use it, so a missing one is `InvalidAttrNumber` and the delete callback falls back to the slot core gave it. `vfdw_rowid_read` (`:210`) raises on a NULL identity for the same reason: a scan that produced a row it cannot name is a bug, not a condition an application should branch on.

### What the handler registers

`valkey_fdw_handler` (`src/valkey_fdw.c:110`) builds the `FdwRoutine` with `makeNode`, which zeroes it, so every callback not assigned there is NULL.

| Group | Registered |
|---|---|
| Scan planning | `GetForeignRelSize`, `GetForeignPaths`, `GetForeignPlan` (all in `src/valkey_fdw.c`) |
| Scan execution | `BeginForeignScan`, `IterateForeignScan`, `ReScanForeignScan`, `EndForeignScan` → `src/vfdw_scan.c` |
| Reporting | `ExplainForeignScan`, `ExplainForeignModify`, `AnalyzeForeignTable`, `IsForeignRelUpdatable` |
| Modify | `AddForeignUpdateTargets` → `src/vfdw_rowid.c`; `PlanForeignModify`, `BeginForeignModify`, `ExecForeignInsert/Update/Delete`, `EndForeignModify` → `src/vfdw_modify.c` |
| COPY, tuple routing, batching | `BeginForeignInsert`, `EndForeignInsert`, `ExecForeignBatchInsert`, `GetForeignModifyBatchSize` |

`GetForeignRelSize` resolves the table's shape before anything else and hangs the map on `baserel->fdw_private`, so a column with no source is reported once at plan time instead of becoming a per-row out-of-bounds read; `reltuples` below zero is replaced with the round constant `VFDW_DEFAULT_TUPLES`, kept obviously invented so that an `EXPLAIN` shows it was never measured, and selectivity is left to the planner's own machinery. `GetForeignPlan` keeps every restriction clause on the scan node, including a key equality that became a single fetch, so a mistake in the access-path choice cannot return rows that do not satisfy the query. `AnalyzeForeignTable` hands back `vfdw_scan_acquire_sample_rows` with `totalpages = 0`, because Valkey has no pages and inventing a number would feed a fabricated `relpages` back into the planner.

`BeginForeignInsert` is registered specifically because `CopyFrom` and tuple routing reach `ExecForeignInsert` through it rather than through `BeginForeignModify`; while it was absent the wrapper had to refuse rather than dereference a NULL `ri_FdwState`, and `vfdw_refuse_no_modify_state` survives as the guard for a fourth entry point added later.

Nothing else is assigned: no join or upper-relation pushdown, no direct modify (`PlanDirectModify` and its family), no `ExecForeignTruncate`, no `ImportForeignSchema`, no row marks or `RefetchForeignRow`, no `RecheckForeignScan`, and none of the parallel-scan callbacks. `ExecForeignTruncate`'s absence is a decision rather than an omission: it runs at *statement* time and is gated only on the pointer being non-NULL — `IsForeignRelUpdatable` is never consulted — so registering it and applying immediately would produce a write that survives `ROLLBACK`, the exact defect the whole write path exists to prevent, while deferring it would require enumerating the victim set at flush time, unbounded for both `keyprefix` and `keyset` tables. Users get core's *cannot truncate foreign table* and the reasoning in the README, because a wrapper that registered nothing cannot attach an `errhint`.

### Principal functions

| Function | File | Responsibility |
|---|---|---|
| `vfdw_find_option` | `src/vfdw_option.c` | The (name, context) lookup every validator and reader goes through. |
| `vfdw_validate_option` | `src/vfdw_option.c` | Parse by kind, then apply the privilege rule; the single DDL-time gate. |
| `vfdw_check_option_priv` | `src/vfdw_option.c` | Enforce `priv` without naming an option, failing closed on an unknown value. |
| `vfdw_read_server_options` / `vfdw_read_user_options` | `src/vfdw_option.c` | Resolve a server's or mapping's options, defaulting from the table's own `defval`. |
| `valkey_fdw_validator` | `src/valkey_fdw.c` | The registered validator: unknown names, duplicates, per-option validation, key-option exclusivity. |
| `vfdw_check_key_options` | `src/valkey_fdw.c` | The only place the three key-discovery options can be refused at DDL time. |
| `valkey_fdw_options` | `src/valkey_fdw.c` | Expose the option table to SQL, which is what makes the drift checks possible. |
| `vfdw_map_build` / `vfdw_map_build_for_relid` | `src/vfdw_map.c` | Resolve and validate a foreign table's shape, or refuse it. |
| `vfdw_map_assign_defaults` | `src/vfdw_map.c` | Give every attribute a source; the function I4 rests on. |
| `vfdw_map_resolve_type` | `src/vfdw_map.c` | Everything a column derives from its PostgreSQL type, both I/O directions at once. |
| `vfdw_map_index_roles` | `src/vfdw_map.c` | Record member/score/value attnums and refuse a role claimed twice. |
| `vfdw_map_writability` | `src/vfdw_writable.c` | The `CMD_*` mask plus the reason for whatever it withholds, without building a map and without raising. |
| `vfdw_refuse_unwritable` | `src/vfdw_refuse.c` | Turn that mask into the statement's refusal, at plan time and again at Begin. |
| `vfdw_rowid_shape` | `src/vfdw_rowid.c` | Which junk columns a shape needs, and over which real columns. |
| `vfdw_rowid_add_update_targets` | `src/vfdw_rowid.c` | `AddForeignUpdateTargets`: inject those `Var`s, plus the whole-row column for `DELETE`. |
| `vfdw_rowid_resolve` / `vfdw_rowid_read` | `src/vfdw_rowid.c` | Find the junk attnos in the subplan and read a row's identity from a slot. |
| `valkey_fdw_handler` | `src/valkey_fdw.c` | The `FdwRoutine`; the authoritative list of what is and is not implemented. |

---

## The read path: planning, scanning and row production

The read path is split down the middle at the plan/execute boundary. `src/vfdw_plan.c` decides *how* a scan will reach Valkey — once per query, with no executor state in sight — and `src/vfdw_scan.c` and its siblings do the reaching. The two communicate only through the plan's `fdw_private`, which has three fixed slots with fixed types so that reading one never depends on which strategy wrote it (`src/vfdw_scan.h:51`): the strategy, the key list, and the `SCAN MATCH` glob.

The split is deliberate. Access-path choice changes for planner reasons; page and reply bookkeeping changes for protocol and topology reasons; and the row producer (`src/vfdw_row.c`) changes when RESP2 and RESP3 disagree about a shape. Keeping them apart is also what lets ANALYZE drive the producer with no plan and no executor node at all.

### Access-path selection

`vfdw_scan_plan` (`src/vfdw_plan.c:551`) picks one of three strategies, in this order.

| Strategy | Chosen when | What the executor does | What ends it |
|---|---|---|---|
| `VFDW_SCAN_SINGLETON` | the table has `singleton_key` | fetch that one key | the single page is spent |
| `VFDW_SCAN_KEYS` | a non-keyset table has a key restriction that survives the pushability gate | fetch the named keys; on a cluster, partitioned per primary | every node's subset is done and no redirect is owed |
| `VFDW_SCAN_KEYSPACE` | everything else, including every keyset table | `SCAN` with `MATCH`/`TYPE`, or `SSCAN` over the keyset, a page at a time | cursor 0 on the last primary, and no redirect is owed |

The safety property that makes this cheap to get wrong is set up one level above, in `vfdwGetForeignPlan`: every restriction clause is kept on the scan node (`src/valkey_fdw.c:459`, `:465`). Nothing is pushed down in the sense of being *removed* from the plan. A key equality that becomes a point fetch is still rechecked by the executor, and Valkey answers by exact key so the recheck costs nothing. A narrowing that is too wide therefore costs reads and returns the right answer.

There is exactly one way that argument fails, and it is why table scoping is handled separately from qual analysis. The key column of the emitted row is filled with the *key that was fetched* (`src/vfdw_row.c:250`). So if a table confined by `keyprefix` ever fetched a key outside that prefix, the resulting row would carry a key column that satisfies the very recheck meant to exclude it. Two places close this:

- `vfdw_keys_refine` (`src/vfdw_plan.c:317`) drops any named key that does not begin with the table's `keyprefix`, and drops repeats — `IN ('a','a')` names one row, and fetching twice would duplicate it. Both are correctness, not speed.
- The singleton path returns before any qual is examined (`src/vfdw_plan.c:565`). A singleton table already names its key and no clause may displace it; letting `key = 'other'` through would read a different key and then fill the key column with the value the recheck tests against.

A keyset table never gets a key list (`src/vfdw_plan.c:577`). Whether a named key belongs to the set is a question only the server can answer, so the scan walks the set with `SSCAN` and lets the clause filter — bounded by the set rather than by the keyspace.

Two smaller rules in the same file: only the first key restriction found is used (`src/vfdw_plan.c:356`), because a second would have to be *intersected* to narrow further and remains a filter regardless; and `VFDW_MAX_KEY_LIST` (1000, `src/vfdw_plan.c:47`) is tested against the array's own declared element count *before* `deconstruct_array` runs (`src/vfdw_plan.c:282`), so refusing a million-element `IN` list does not first cost a million-element materialisation.

### Which key columns may be pushed down, and why it is a whitelist

The pushdown replaces "walk the keyspace and keep the keys whose decoded value equals the constant" with "fetch `out(constant)`". That is sound only when `out(constant)` is the *only* byte string the read path decodes to the constant — and the read path decodes a key through the column's **input** function. The requirement is therefore `out(in(K)) == K` for every key the table might hold: a property of the type, not of the constant. `vfdw_key_column_pushable` (`src/vfdw_plan.c:152`) admits `getBaseType(typid)` of `TEXTOID` or `VARCHAROID` and nothing else.

Two families fail injectivity, and the second is why a round-trip check on the *constant* is not a substitute:

- **bytea.** The read path never calls `bytea_in`; it stores the server's bytes verbatim (`src/vfdw_row.c:40`). A plan key rendered by `byteaout` is the hex text `\x6e756c...`. The two disagree about which bytes the column names, so the fetch asks for a key that does not exist — or, on a `keyprefix` table, is dropped by `vfdw_keys_refine` and the scan ends with an empty list and zero rows.
- **Types whose output canonicalises** — `int4`, `numeric`, `timestamp`, `json`, `bpchar`. A key literally named `007` under an `int4` key column is returned by a full scan as `7`, and `WHERE k = 7` would fetch a key named `7`. Checking `in(out(c)) == c` passes here and still loses the row, because the condition is about keys the *table holds*, which plan time cannot enumerate.

`text` and `varchar` are named rather than derived because no catalog property means "the output function reproduces the input bytes" — `typcategory 'S'` includes `bpchar`, which pads. The file notes that `src/vfdw_val.c` argues at length *against* whitelists; the distinction is that rendering a Datum is a question the type answers for itself and getting it wrong costs a round trip, while injectivity is not exposed by the type system and getting it wrong loses rows.

The gate is consulted on the equality path and the LIKE path (`src/vfdw_plan.c:362`, `:443`) so the rule is stated once, even though it cannot currently fire on the LIKE path — `OID_TEXT_LIKE_OP` over a bare `Var` already implies a text column.

Around that gate sit the clause-shape checks, all in `vfdw_keys_from_opexpr` (`src/vfdw_plan.c:194`) and `vfdw_keys_from_saop` (`src/vfdw_plan.c:240`):

- `vfdw_is_key_var` (`src/vfdw_plan.c:64`) validates `varno` against the relation being planned and bounds `varattno` to `1..natts` before comparing it to `keyattno`. Without either check the same code indexes the tuple descriptor with an unvalidated `varattno`, and a `Var` belonging to some other relation of the query is taken for this table's key.
- Equality is the *type's own* `eq_opr` from the type cache (`src/vfdw_plan.c:82`), not a hardcoded proc OID.
- The commuted form `'literal' = key` is accepted (`src/vfdw_plan.c:208`); a clause written that way round is the same point lookup.
- A non-deterministic collation refuses the pushdown (`src/vfdw_plan.c:99`). Under one, two distinct byte strings compare equal, so the named key is not the only key satisfying the clause. The comment records the symptom that motivated it: the answer depended on whether the constant folded — `k = 'NDQ2'` got a point lookup and no rows, `k = (SELECT 'NDQ2'::text)` got a keyspace scan and the row.
- `IN` arrives as `ScalarArrayOpExpr`; only `useOr` is taken, since `ALL` is a conjunction that one key satisfies only degenerately, and NULL array elements are skipped because `key = NULL` names nothing.

One consequence worth knowing before changing anything here: `vfdw_keys_refine` compares *rendered spellings* with `strncmp`/`strcmp`. That is correct only because the gate has already excluded every column whose rendering is not the key's own bytes. If the pushdown is ever taught to carry `(bytes, len)` through `fdw_private` — which also forces `fdw_private` to stop holding keys as `String` and `vfdw_scan_set_keys` to stop taking their length with `strlen` (`src/vfdw_scan.c:583`) — this comparison must become the length-guarded `memcmp` its write-side counterpart already uses.

### LIKE prefix extraction and glob escaping

`vfdw_plan_find_like_prefix` (`src/vfdw_plan.c:437`) matches `OID_TEXT_LIKE_OP` by OID — which is what PostgreSQL's own index machinery does for LIKE, there being no type-cache entry to ask — over a bare key `Var` and a non-NULL `Const`, under a deterministic collation. ILIKE is excluded because its prefix is not a byte prefix.

`vfdw_like_prefix` (`src/vfdw_plan.c:397`) walks the pattern to the first `%` or `_`, treating backslash as escaping the next character and stopping at a trailing backslash. The backslash convention is also how a custom `ESCAPE` arrives: the parser rewrites `LIKE p ESCAPE e` into `like_escape(p, e)`, and constant-folding that yields a pattern in the backslash convention. No literal prefix means no narrowing.

`vfdw_plan_scan_pattern` (`src/vfdw_plan.c:510`) combines the LIKE prefix with the table's `keyprefix` and returns the glob the executor will send:

- a keyset table gets `NULL` — `SSCAN` walks a set and accepts no `MATCH`;
- both restrictions must hold, so the longer wins, but only when it genuinely *extends* the other (`src/vfdw_plan.c:528`). Divergent prefixes can be satisfied by no key at all, and the table's own prefix is kept so the impossibility is left to the recheck rather than encoded in a pattern;
- the chosen prefix is a literal, not a pattern, so `vfdw_escape_glob` (`src/vfdw_plan.c:490`) backslash-escapes `*`, `?`, `[`, `]` and `\` before `*` is appended. Without it a table scoped to `a[b]:` would match an entirely different set of keys than its definition names.

This function is exported because ANALYZE needs the table-level pattern with no plan to read it from (`src/vfdw_scan.c:673`). The executor never rebuilds the glob — `vfdw_scan_build_scan` sends `state->pattern` as it stands (`src/vfdw_scan.c:149`), so there is only one place that can be wrong about which keys a table contains.

### The page loop and the pipelined value fetch

`vfdw_scan_build_scan` (`src/vfdw_scan.c:117`) emits either `SSCAN <set> <cursor> COUNT n` or `SCAN <cursor> [MATCH g] COUNT n TYPE <t>`. The server-side `TYPE` filter matters: fetching every key and discarding the mismatches client-side is both wasteful and the mechanism by which a wrong-type key at a page boundary truncates the result. `SSCAN` accepts neither `MATCH` nor `TYPE`, so a keyset table's wrong-type members can only be filtered when their values come back — which is the sole reason `WRONGTYPE` is a skippable reply at all.

`vfdw_scan_take_page` (`src/vfdw_scan.c:231`) insists on a two-element reply, treats a cursor of exactly `"0"` as "this node's traversal is complete" by setting `state->cursor = NULL`, and takes the key array. Every element goes through `vfdw_scan_expect_string` (`src/vfdw_scan.c:187`) because `vfdw_reply_expect` validates only the *top* level of a reply: an integer or nil element would otherwise be read as `(NULL, 0)` and an empty cursor sent back to the server. Keys are copied with `palloc` + `memcpy`, never `pnstrdup` (`src/vfdw_scan.c:222`): `pnstrdup` does `strnlen` first, so a key containing a NUL would be copied short while `keylens` still recorded its full length — invariant I3.

`vfdw_scan_queue_page` (`src/vfdw_scan.c:308`) then queues one value command per key of the page — `GET`, `HGETALL`, `LRANGE 0 -1`, `SMEMBERS`, or `ZRANGE 0 -1 WITHSCORES` by table type (`src/vfdw_scan.c:62`) — into the pipelined batch, which flushes every `pipeline_batch` commands. Fetching N values costs `ceil(N / pipeline_batch)` round trips rather than N, which is the single largest performance difference from a per-key client. Replies are taken in order with `vfdw_batch_next`, and the batch owns each one until the next take, so no caller frees a reply and no error path leaks one.

Three memory contexts carry the scan (`src/vfdw_scan_internal.h:40`): `scan_cxt` for the whole scan (plan keys, cursor, seen-set keys — everything that must survive a rescan), `page_cxt` reset per page, and `batch_cxt` existing purely so a rescan can *reset* the batch rather than abandon it. `vfdw_batch_begin` registers a reset callback on whatever context it is given, so beginning each pass in `scan_cxt` left a struct plus a callback behind per pass, and a foreign scan on the inner side of a nested loop grew both memory and the reset-callback chain with the outer row count.

`vfdw_scan_fetch` (`src/vfdw_scan.c:493`) is the producer. Per call it: emits the next member if a collection reply is mid-unpack; refills when the current page's keys are used up; otherwise takes the next key **and its reply, always in that order**; consults the overlay; records a redirect; skips or raises; and finally either holds a collection or fills a single row. The reply must be taken before the overlay is consulted, because the reply is positionally paired with the key — consulting the overlay *instead of* taking it would leave the pipeline one reply out of step for the rest of the scan.

`vfdw_scan_refill` (`src/vfdw_scan.c:325`) has two shapes. For a keyed or singleton strategy it calls `vfdw_scan_keys_page`, and if that yields nothing, tries a redirect retry pass. For a keyspace scan it loops on `vfdw_scan_next_page` **until a page actually yields keys** — an empty page is not the end of a scan, and returning on one is precisely how the tail of a result set gets dropped.

### "No tuple this call" versus "scan finished"

This is invariant I5 and it is the spine of the whole executor half. The states that mean *no tuple now, keep going* are numerous:

- an empty page from `SCAN`/`SSCAN`;
- a cursor of 0 on one primary — `vfdw_scan_cluster_advance` (`src/vfdw_scan_cluster.c:189`) is the only thing that may end a keyspace scan on a cluster;
- a node that owns none of the plan's keys (`src/vfdw_scan_cluster.c:364`) — the same trap reached by a different route;
- a key that expired, held the wrong type, or answered with an empty container;
- a `MOVED`/`ASK` redirect, which is a row that *moved*, not a row that is *absent*;
- an overlay `HIDE`, and an overlay tail entry already seen.

The scan is over only when `vfdw_scan_drain_tail` returns a cleared slot (`src/vfdw_scan_overlay.c:216`), which requires that the cursor is exhausted on the last primary, nothing is in flight, no redirect is owed, and the overlay's tail is spent. There is one further exit that ends a scan: `vfdw_scan_fetch` returns the cleared slot if `vfdw_batch_next` hands back `NULL` (`src/vfdw_scan.c:523`), which happens only when the batch has nothing outstanding while keys remain — a defensive path, and the one exit that does not run the overlay tail.

Redirects are chased with a bounded budget of three passes (`src/vfdw_scan_cluster.c:124`). `vfdw_scan_retry_pass` (`src/vfdw_scan_cluster.c:292`) distinguishes "nothing owed a pass" from "owed a pass and out of budget": the second raises SQLSTATE 40001 with a hint to check `CLUSTER INFO`. These were once one test, and a scan that gave up chasing keys ended exactly like a scan with nothing left to chase — rows missing, nothing logged. The pass also rebuilds the node list from a refreshed map, because a slot may have moved onto a primary this scan has never heard of. Routing within a pass uses `state->pass_map`, a snapshot captured when the pass began (`src/vfdw_scan_internal.h:116`): routing off the live map offers a key to both its stale and its fresh owner, and it comes back twice.

### Which replies are skippable and which now raise

| Reply to a value fetch | Treatment |
|---|---|
| `NIL` | skip, count as skipped (`src/vfdw_scan.c:385`) |
| `MOVED` / `ASK` error | recorded for the retry pass, then skipped (`src/vfdw_scan.c:364`, `:386`) |
| `WRONGTYPE` error | skip, count (`src/vfdw_scan.c:387`) |
| empty array/map/set on a non-string table | skip, count — the key is absent (`src/vfdw_scan.c:432`) |
| `STATUS` | raise (`src/vfdw_scan.c:397`) |
| any other error reply | raise, under the SQLSTATE its prefix implies (`src/vfdw_cmd.c:305`) |
| any other unexpected shape | raise, naming the shape that arrived |

Admitting a broader class of errors made a `NOPERM` or an `OOM` into one more skipped key and a result set silently short of it — the same truncation a nil or wrong-type reply at a page boundary produces, arriving through the value fetch instead of through the cursor. A `STATUS` is refused rather than skipped because none of `GET`, `HGETALL`, `LRANGE`, `SMEMBERS`, `ZRANGE` has one among its answers, so one arriving means the reply stream is at an offset this scan cannot account for. The `WRONGTYPE` test is unconditional even though only keyset tables can normally produce one, since `SCAN ... TYPE` filters the rest server-side.

The absent-container rule (`src/vfdw_scan.c:432`) is the subtlest of these, and its boundary is exact. Only `GET` reports a missing key with a nil; `HGETALL` on a missing key answers with an empty map under RESP3 and an empty array under RESP2, and the collection commands answer with empty containers. The collection types survived that by accident — zero elements produce zero rows — but a hash table has no member loop, so `vfdw_scan_fill` emitted one row with the key column set to the name that was asked for and every mapped field NULL. `SELECT EXISTS (SELECT 1 FROM h WHERE k = <anything>)` answered true, an `IN` list returned a row per named key present or not, and a keyset hash table with a stale member returned a phantom all-NULL row that ANALYZE then counted. That is I5 run backwards: "no tuple now" turned into a tuple. The boundary must stay where it is — a hash that exists but holds only fields this table does not map answers with a *non-empty* reply and legitimately produces a row of NULLs. Emptiness of the reply, not emptiness of the row, is what says the key is absent.

### The overlay: a scan's view of its own transaction's writes

Writes are buffered until pre-commit (see the write-path section), so without an overlay a transaction could not read what it had just written, and — worse — would read the pre-write state and call it current. `src/vfdw_overlay.c` answers "what has this transaction done to this key"; `src/vfdw_scan_overlay.c` is what a scan does with the answer.

**Visibility is PostgreSQL's own rule.** An operation is visible to a scan only when its `CommandId` is strictly older than the scan's `es_snapshot->curcid`, captured once in `vfdw_scan_begin` (`src/vfdw_scan.c:705`) and tested at `src/vfdw_overlay.c:226`. That is what stops `INSERT INTO t SELECT * FROM t` from re-reading its own output: one command, so its writes carry that very cid and stay invisible. `INSERT; SELECT` is two commands and the second sees the first.

The index is keyed by `(relid, key bytes)` — two foreign tables may map one Valkey key with different column sets, and a scan of one must not see the other's buffered row, whose tuple descriptor would not even match. Each entry holds *every* operation on that key in buffer order rather than only the latest (`src/vfdw_overlay.c:103`), because "latest" is a function of the asking command: a scan with an older `curcid` must see the state as of its own command. The index is rebuilt only when `vfdw_wbuf_generation()` has moved (`src/vfdw_overlay.c:148`) — that generation advances exactly when the operation list changes, including on a savepoint rollback. `vfdw_overlay_active` is deliberately *not* per relation (`src/vfdw_overlay.c:181`): answering "has this transaction written to *this* table" would need the index, and building it is the cost the call exists to avoid. An empty buffer is the case that matters and the common one, and the check is asked per fetch, because a cursor opened before the first write would otherwise never consult the overlay.

`vfdw_overlay_check` walks the key's operation list **backwards** to the newest operation the asking command may see (forwards would find the oldest, which is the state before the transaction did anything) and returns:

| Verdict | Meaning | What the scan does |
|---|---|---|
| `VFDW_OVL_HIDE` | the newest visible op is a delete | no tuple this call; `continue` (`src/vfdw_scan.c:531`) |
| `VFDW_OVL_REPLACE` | the newest visible op carries a whole row | store the buffered `HeapTuple` and return it |
| `VFDW_OVL_PASS` | nothing visible, or an op with no stored tuple | the server's reply stands |

`vfdw_scan_apply_overlay` (`src/vfdw_scan_overlay.c:186`) also marks the key seen for *every* key it checks, whatever the verdict, so the tail cannot re-emit a key the server's own pages already produced. A `REPLACE` stores the tuple with `shouldFree = false`: it belongs to the write buffer and outlives the slot, and freeing it would leave the ledger and the flush pointing at released memory. No synthetic `valkeyReply` is ever manufactured — a fake reply would land in `state->cur_reply`, which is dereferenced across `Iterate` calls, and a `ROLLBACK TO SAVEPOINT` rebuilding the overlay would free it under an open cursor.

Note the ordering in `vfdw_scan_fetch`: the verdict is consumed at `src/vfdw_scan.c:526`, *before* the redirect note and the skippable tests. A key this transaction has hidden or rewritten is answered from the buffer and never reaches the redirect bookkeeping, which is coherent — in both cases the transaction's own version of the row is the answer — but it is not stated in a comment.

When the server's keys run out, `vfdw_scan_drain_tail` initialises the tail iterator and emits the keys this transaction created that the server does not have yet. `vfdw_overlay_iter_next` (`src/vfdw_overlay.c:257`) walks the buffer for `INSERT` operations on this relation and asks `vfdw_overlay_check` for the verdict rather than re-implementing the visibility test. That is a scar: the rule was once written in both places, and a mutation that disabled the `CommandId` test in `check()` left the whole suite green because the copy in this loop still enforced it. Delegating also answers the two questions the loop would otherwise track itself — a key created then deleted (`HIDE`), and a key created then updated (whose newest tuple, not the original insert's, is what must be emitted).

The seen-set (`src/vfdw_scan_overlay.c:79`) is keyed by key **bytes**, not by entry ordinal: a rebuild renumbers every entry, so an ordinal-keyed set held across a savepoint rollback would silently skip or repeat rows. The iterator carries the buffer generation and restarts if it moved (`src/vfdw_overlay.c:266`), and restarting is safe precisely because the seen-set is byte-keyed. The set's key bytes are copied into a context of its own — they arrive from `page_cxt`, which is reset per page — and `vfdw_scan_rescan` resets that context whole (`src/vfdw_scan.c:742`); dropping only the pointer leaked the table and every key in it, once per rescan, unbounded in the outer row count of a nested loop.

`vfdw_scan_rescan` (`src/vfdw_scan.c:710`) restarts from the beginning rather than from wherever the last pass stopped — cursor, `started`, page keys, seen-set, tail flag, collection cursor and counters all reset, and the batch is rebuilt. Resetting only a row counter leaves a rescan replaying whatever page the scan happened to be holding, or, with a pushed-down qual, producing nothing at all.

### Building a tuple for a table of any width

`vfdw_scan_fill` (`src/vfdw_row.c:220`) clears the slot, sets all `natts` attributes null, then walks the map's `natts` columns and fills each from its own kind. This is invariant I4, stated structurally. Assume instead that every table is `(key, value)` and build tuples from a two-element array regardless of the relation's width, and a third column reads past the end of that array, so any user who can `CREATE FOREIGN TABLE` can crash the backend. Here the shape is resolved once at plan time (`src/valkey_fdw.c:393`) and a column with no source is a definition error reported then, so the executor fills a `natts`-sized slot from a mapping already known complete.

Per column (`src/vfdw_row.c:237`): `VFDW_COL_KEY` takes the key bytes; `VFDW_COL_VALUE` takes a string reply; `VFDW_COL_FIELD` looks the field up in the hash reply; `VFDW_COL_MEMBER` and `VFDW_COL_SCORE` locate the current element. Anything unmatched simply stays NULL, which is how a hash holding none of the mapped fields legitimately yields a row of NULLs.

The protocol version shows through in two places, and both are *detected* rather than assumed. `HGETALL` is a map under RESP3 and a flat alternating array under RESP2, so `vfdw_scan_hash_lookup` (`src/vfdw_row.c:112`) walks pairs and accepts either container. `ZRANGE ... WITHSCORES` is a flat alternating array under RESP2 and an array of two-element pairs under RESP3, so `vfdw_scan_member_at` (`src/vfdw_row.c:155`) inspects the first element's type, and `vfdw_scan_member_stride` (`src/vfdw_row.c:199`) returns 2 only for the flat form. Reading one layout as the other yields the wrong number of rows with null members. Every index into `element[]` goes through `vfdw_reply_child`, which checks the declared count before dereferencing — an index taken on the server's word is a heap overflow, and a scan reaching it takes the backend down mid-query.

Bytes become a Datum in `vfdw_row_datum_from_bytes` (`src/vfdw_row.c:34`), the exact inverse of `src/vfdw_val.c`:

- a `bytea` column (computed from the *base* type, so a domain over bytea counts) takes the bytes verbatim into a `bytea` Datum. Because that branch bypasses the type's input function it also bypasses a domain's constraints, so `domain_check` is applied explicitly, with a per-attribute `DomainIOData` cache in `VfdwRowCtx` to avoid a syscache lookup and an expression compile per value;
- everything else is a text representation and is checked with `pg_verifymbstr` before `InputFunctionCall`. Passing these bytes straight into a text datum instead admits invalidly-encoded data into the database. There is deliberately no mirror of this check on the write side: outbound bytes came from the server and are already in the server encoding.

Lengths travel with the data throughout (I3); nothing here treats a value as a C string, so a value containing a NUL arrives whole. On the read side a NULL data pointer is used as the NULL signal (`src/vfdw_row.c:93`), which is safe only because libvalkey never returns a NULL `str` for a zero-length bulk — the write side has no such guarantee and must not copy the idiom.

### ANALYZE reuses the same producer

`vfdw_scan_acquire_sample_rows` (`src/vfdw_analyze.c:66`) creates a scan state with `vfdw_scan_state_create`, drives `vfdw_scan_fetch` to exhaustion, and closes it with `vfdw_scan_state_close`. Those three are exported from `src/vfdw_scan.h` for exactly this purpose, with `VfdwScanState` left opaque.

The reuse is the point. A sampler of its own would be free to disagree with the scan about what the table contains, and statistics describing a different table than the one being read are worse than no statistics — every page rule, skip rule and overlay rule above applies identically to the sample. Valkey offers no way to sample a keyspace, so the whole table is read once and reservoir-sampled down to `targrows`; the compensation for that cost is that `totalrows` is a count rather than an extrapolation (`src/vfdw_analyze.c:107`). `totalpages` is reported as zero (`src/valkey_fdw.c:561`) because inventing one would feed a fabricated `relpages` back into the planner, and until someone runs ANALYZE a foreign table gets the round placeholder `VFDW_DEFAULT_TUPLES` (`src/valkey_fdw.c:89`) — round so that an EXPLAIN makes it obvious the number was invented. Without any of this, a row estimate invented from the key count is what a join with a Valkey table on one side gets chosen from.

Two differences from an executor-driven scan follow from ANALYZE having no plan. It gets the table-level glob only — `vfdw_plan_scan_pattern(map, NULL)` (`src/vfdw_scan.c:673`) — with the singleton strategy applied from the table options, and it never calls `vfdw_scan_adopt_plan`, so the strategy stays `VFDW_SCAN_KEYSPACE` for everything else. It also never assigns `overlay_cid`; the state is zero-initialised (`src/vfdw_scan.c:633`), and the visibility test admits an operation only when `op->cid < curcid`, so with `curcid` zero no buffered write is visible and no tail row is injected. ANALYZE inside a transaction that has already done DML therefore samples the server's state alone. That falls out of the code; no comment records whether it is intended.

EXPLAIN reads the same plan (`src/valkey_fdw.c:483`): the strategy name comes from `fdw_private` so it is visible without executing, the match glob or key count says how much of the keyspace will be walked, and under `ANALYZE` the skipped count and the `SCAN` round-trip count are added. The round-trip count exists because it is the only externally visible consequence of `scan_count` — the same rows come back in the same order at every page size, so without it the option's only test would be that its value parses.

### Principal functions

| Function | File | Responsible for |
|---|---|---|
| `vfdw_scan_plan` | `src/vfdw_plan.c` | choosing the strategy and encoding it, the key list and the glob into `fdw_private` |
| `vfdw_key_column_pushable` | `src/vfdw_plan.c` | the injectivity gate: may a restriction on this key column become a named fetch at all |
| `vfdw_plan_find_keys` | `src/vfdw_plan.c` | finding the first key equality or `IN` among the clauses and turning it into a key list |
| `vfdw_keys_refine` | `src/vfdw_plan.c` | dropping duplicates and keys outside the table's `keyprefix` |
| `vfdw_plan_find_like_prefix` | `src/vfdw_plan.c` | extracting the literal prefix a `LIKE` pattern requires |
| `vfdw_plan_scan_pattern` | `src/vfdw_plan.c` | combining table prefix and LIKE prefix into an escaped `SCAN MATCH` glob; also used by ANALYZE |
| `vfdw_scan_strategy_name` | `src/vfdw_plan.c` | the strategy label EXPLAIN prints, derived from the plan alone |
| `vfdw_scan_state_create` | `src/vfdw_scan.c` | everything a scan needs except the access path; shared by the executor and ANALYZE |
| `vfdw_scan_fetch` | `src/vfdw_scan.c` | the producer: one tuple, or an empty slot only when the scan is genuinely over |
| `vfdw_scan_refill` | `src/vfdw_scan.c` | getting more replies in flight, or reporting that there are none left |
| `vfdw_scan_build_scan` | `src/vfdw_scan.c` | the `SCAN`/`SSCAN` for the next page, including `MATCH`, `COUNT` and the server-side `TYPE` filter |
| `vfdw_scan_queue_page` | `src/vfdw_scan.c` | pipelining one value command per key of the current page |
| `vfdw_scan_reply_is_skippable` / `vfdw_scan_reply_is_absent` | `src/vfdw_scan.c` | the exact boundary between "advance past this key" and "raise" |
| `vfdw_scan_rescan` | `src/vfdw_scan.c` | restarting a scan from the beginning, including the seen-set and the batch |
| `vfdw_scan_apply_overlay` | `src/vfdw_scan_overlay.c` | applying this transaction's verdict for one scanned key to the slot |
| `vfdw_scan_drain_tail` | `src/vfdw_scan_overlay.c` | emitting buffered inserts the server does not have; the only place a scan ends |
| `vfdw_scan_mark_seen` / `vfdw_scan_already_seen` | `src/vfdw_scan_overlay.c` | the byte-keyed set that stops a key being emitted twice |
| `vfdw_overlay_check` | `src/vfdw_overlay.c` | the single statement of the command-id visibility rule, and the PASS/HIDE/REPLACE verdict |
| `vfdw_overlay_iter_next` | `src/vfdw_overlay.c` | walking the buffer's inserts for the tail, delegating visibility back to `check` |
| `vfdw_scan_keys_page` | `src/vfdw_scan_cluster.c` | one page for a strategy whose keys the plan named, partitioned per primary |
| `vfdw_scan_cluster_advance` | `src/vfdw_scan_cluster.c` | moving to the next primary; returning false is the only thing that may end a keyspace scan |
| `vfdw_scan_retry_pass` | `src/vfdw_scan_cluster.c` | re-fetching redirected keys, and raising rather than going quiet when the budget is spent |
| `vfdw_scan_fill` | `src/vfdw_row.c` | building a tuple for a table of any width from a `natts`-sized map |
| `vfdw_row_datum_from_bytes` | `src/vfdw_row.c` | bytes to a Datum of the column's declared type, binary or encoding-checked |
| `vfdw_scan_acquire_sample_rows` | `src/vfdw_analyze.c` | ANALYZE's reservoir sample, drawn through the scan's own producer |

---

## The write path: buffering and folding

### Nothing is sent as a statement executes

An `INSERT` against a Valkey foreign table sends no Valkey command. Neither does an `UPDATE`, a `DELETE`, or a `COPY`. Every row a statement produces is rendered into memory and appended to a transaction-scoped buffer; the whole transaction is applied by one `EVALSHA` at `XACT_EVENT_PRE_COMMIT`, and `ROLLBACK` sends nothing at all. That rule is invariant W3, defined in the invariants table above, and `src/vfdw_modify.h` states the module's side of it: the modify callbacks perform no I/O beyond acquiring the pooled connection.

The reason is that Valkey has no way to undo. A wrapper that wrote as it executed would have to leave the keyspace half-modified whenever a later row raised, whenever a statement-level trigger failed, or whenever the user typed `ROLLBACK` — and would then report a row count for writes that no longer stood. Deferring turns the whole transaction into a single server-side program whose checks and writes cannot be separated by the network (`src/vfdw_script.h`). It also fixes where errors are allowed to appear: `src/vfdw_xact.h` records why `PRE_COMMIT` is the only legal wire point — the transaction is still `TRANS_INPROGRESS`, interrupts are not held, the ResourceOwner and contexts are intact, and an `ereport` still aborts. `COMMIT`, `ABORT`, `PREPARE` and `ABORT_SUB` all run inside `HOLD_INTERRUPTS`, where `CHECK_FOR_INTERRUPTS` is a no-op and invariant I6 cannot be satisfied, so no network I/O may happen there. There is deliberately no lint for that rule, because a structural grep over the switch arms would pass the moment the code were factored into helpers.

The one thing a DML statement does touch is the connection pool, and only to prove the server usable. `vfdw_modify_connect` (`src/vfdw_modify.c:192`) acquires a lease, reads the options that acquisition actually used, refuses `prefer_replica`, binds the write unit, and hands the lease straight back (`src/vfdw_modify.c:214`). The point is that an unreachable host, a wrong password, a rejected certificate or an ACL denial is reported at the DML statement rather than at `COMMIT`. Keeping the lease was tried and is wrong: the scan holds a lease because it owns a reply stream, this callback owns none, and holding one would cost a connection slot per DML statement — an `INSERT` routed into 33 foreign partitions would fail with "too many concurrent Valkey scans on one server", a message wrong in both nouns. The absence of any `VfdwConn *` in `VfdwModifyState` is the visible consequence. `vfdw_conn_select_db` is deliberately *not* called here: W3 grants acquisition, and AUTH and HELLO are part of acquiring a cold connection, but `SELECT` is a command, and the flush reissues it anyway on whatever slot it takes.

`vfdw_modify_begin` returns early under `EXEC_FLAG_EXPLAIN_ONLY` (`src/vfdw_modify.c:236`). Without it a plain `EXPLAIN` of an `INSERT` would open a connection and bind the transaction's unit, so `EXPLAIN` of an insert into a second database would raise `0A000`. `ExplainForeignModify` therefore prints from `fdw_private` only, and says what the plan will do: `deferred, one atomic unit, flushed at commit`.

Two consequences of deferral are visible as refusals elsewhere. `PREPARE TRANSACTION` is refused whenever the buffer is non-empty (`src/vfdw_xact.c:52`), because Valkey has no two-phase commit and the wrapper cannot hold a prepared write. And `INSERT ... ON CONFLICT` is refused at plan time (`src/vfdw_refuse.c:59`): `ExecForeignInsert` must return a row before the pre-commit flush can know the row will be skipped, so the reported count would be wrong in exactly the case `ON CONFLICT` asks about.

### The write buffer

The buffer is one `AllocSet` context under `TopMemoryContext`, created lazily by `vfdw_wbuf_context` (`src/vfdw_wbuf.c:108`) and reset — never deleted — from the transaction callback. It is not `TopTransactionContext` for three stated reasons: that gives no control over savepoint-scoped release, `AtCommit_Memory` resets it out from under us, and a named context makes an oversized `COPY` buffer legible in `MemoryContextStats` instead of looking like a leak. It uses `ALLOCSET_START_SMALL_SIZES` so a three-row transaction pays a 1 kB block and a `COPY` grows to 8 MB blocks; `ALLOCSET_DEFAULT_SIZES` would charge every small transaction 8 kB against a cap whose minimum is 65536. Creating the context also registers the transaction callbacks, so no buffered state can exist without them.

Operations hang on a doubly-linked list with a tail pointer (`src/vfdw_wbuf.h:13`). Append is O(1) and `ROLLBACK TO SAVEPOINT` walks backwards from the tail — correct only because levels along the live list are non-decreasing, which the two subtransaction arms maintain. A handler that appended out of level order would break the backwards walk silently, so the property is written down rather than left to be worked out again from the failure.

**Nothing is ever freed.** Truncation unlinks and leaves the chunks in the context (`vfdw_wbuf_unlink_tail`, `src/vfdw_wbuf.c:354`). An open cursor's slot may hold a pointer into a dropped operation's `HeapTuple`, so returning that memory would be a use-after-free that only shows up under a cursor. Nothing in the context is ever `pfree`'d or `repalloc`'ed either, which is what makes the byte counter monotonic by construction rather than by an argument about AllocSet free lists — `repalloc` of a chunk over `allocChunkLimit` frees the old block and `mem_allocated` would drop.

Because the flush runs long after the statement is gone, every byte an operation points at must live in this context: the new row's `HeapTuple` copied with `ExecCopySlotHeapTuple` (`src/vfdw_modify.c:364`), converted column bytes via `VfdwValCtx.dest`, hash field names copied by `vfdw_modify_retain_name` (`src/vfdw_render.c:39`) because `col->field` points into a `VfdwTableMap` that dies at `EndForeignModify`, and the table's keyset name retained on the operation itself (`src/vfdw_modify.c:394`).

#### The two caps, and where each is enforced

| Counter | What it is | Bounded by | Checked in |
|---|---|---|---|
| `live_ops` | operations currently on the list; decremented by truncation | `write_max_ops` (default 10000) | `vfdw_wbuf_append` only (`src/vfdw_wbuf.c:320`) |
| `alloc_bytes` | `MemoryContextMemAllocated` minus the baseline captured at reset; monotonic, never decremented | `write_max_bytes` (default 64 MiB, minimum 65536) | `vfdw_wbuf_op_new` (`:282`) **and** `vfdw_wbuf_append` (`:322`) |

They are separate because a million tiny operations and a hundred enormous ones are different problems (`src/vfdw_option.c:155`). The byte counter is read from the allocator rather than maintained by hand: a hand-rolled counter can only ever under-count, an under-count is silent, and no test can distinguish it from a smaller workload. It counts chunk headers and size-class rounding, which is what actually bounds memory — and because it has block granularity, tests may assert relations between readings but never an absolute value.

The byte cap is checked twice, and the check in `vfdw_wbuf_op_new` is the one that actually bounds the context. A row is rendered into the buffer *between* `op_new` and `append`, and a row refused partway through rendering — a NULL key, a non-finite score, a value that will not cast — keeps the bytes it had already retained and never reaches `append`. With the check only at `append`, a `LOOP ... EXCEPTION ... END` over failing rows grew the buffer without limit while no cap ever fired. Charging at allocation bounds the total at the cap plus the one row in flight, whatever happens to it.

Both byte tests read "have we already allocated more than the cap", not "would we". By the time `append` runs the row's bytes are in the context, so the error fires on the row that crossed the cap and cannot be defeated by a row that was refused after allocating. The counter is never given back on `ROLLBACK TO SAVEPOINT` — the `errhint` says so in as many words (`src/vfdw_wbuf.c:260`) — because a design that returned budget would let the same exception loop allocate without bound while the cap read as untouched. The two cap errors share SQLSTATE 54000 and carry two different `errmsg` literals deliberately: a test asserting only the SQLSTATE would pass with the checks swapped.

A separate ceiling, `VFDW_MAX_ARG_BYTES` (512 MiB, `src/vfdw_val.h:56`), bounds one *argument* rather than the transaction. `write_max_bytes` does not subsume it: it is settable to 1 GiB, so a single 600 MB value would slip under it and reach the wire, where an over-length bulk argument is a protocol error that closes the connection — classifiable at pre-commit only as `08007` for the whole transaction.

#### Ordering, stamps and the generation counter

Each row is rendered in refusal-cost order — key, score, member, payload (`src/vfdw_modify.c:485`) — so the common refusals leave nothing retained. Retention is per column, not per row, so a row refused by a *later* column's rule leaves the columns already accepted behind, and those bytes are counted; `src/vfdw_val.h:117` records that this is required rather than tolerated, for the same monotonicity reason.

`vfdw_wbuf_append` stamps each operation with `GetCurrentTransactionNestLevel()` and `GetCurrentCommandId(true)` (`src/vfdw_wbuf.c:317`). The command id is what lets the overlay tell two statements apart. The `true` is honestly annotated as currently redundant rather than load-bearing — `standard_ExecutorStart` and `CopyFrom` already mark the command id used — and the comment says plainly that the write-path design's claim to the contrary is the part that is wrong.

The append also bumps a generation counter. Bumping it only on truncation was a real defect: the overlay caches its index against this number, so an index built during a `DELETE`'s own scan stayed "fresh" after the `DELETE` was appended, and a key created and then deleted in one transaction stayed visible.

#### Subtransactions

`vfdw_wbuf_subxact` (`src/vfdw_wbuf.c:410`) handles four events, all as explicit arms so the switch is exhaustive by intent rather than by omission. At both `ABORT_SUB` and `PRE_COMMIT_SUB` the nest level is still the child's; core pops it afterwards.

- **`ABORT_SUB`** unlinks operations from the tail while `op->level >= curlevel`, bumps the generation if anything went, re-evaluates the binding, re-folds the ledger and drops the overlay. It runs inside `HOLD_INTERRUPTS`, so the work is pure memory: no allocation, no syscall, no catalog access, bounded by `write_max_ops`.
- **`PRE_COMMIT_SUB`** lowers the surviving suffix's levels to `curlevel - 1` and promotes the unit binding on the same rule. A committed savepoint's work belongs to its parent; without the promotion the unit would keep a level that no longer exists and the next abort at that depth would drop a binding it did not create.
- **`COMMIT_SUB`** and **`START_SUB`** do nothing, because operations carry their own level and `PRE_COMMIT_SUB` has already promoted the suffix.

The binding is discarded only when the buffer is empty *and* the binding's own level is at or above the aborting one (`vfdw_wbuf_rebind_after_truncate`, `src/vfdw_wbuf.c:392`). Emptiness alone was a live defect: the binding is taken at `BeginForeignModify` before the first row is buffered, so any subtransaction aborting in between — most commonly a plpgsql `BEGIN ... EXCEPTION` inside an expression the statement is still evaluating — found the buffer empty and dropped a binding its own statement was about to use, and the next row died on `vfdw_wbuf_op_new`'s `elog`. The emptiness test stays because a binding promoted out of a committed savepoint can outlive its level while operations still reference it. The reduction is only valid while there is one unit per transaction, and the comment says so: if more than one is ever admitted, the binding must be recomputed from the survivors.

`vfdw_wbuf_reset` (`src/vfdw_wbuf.c:470`) assumes nothing about how far a flush got, so `COMMIT` and `ABORT` can both call it and a double reset behaves like a single one. It drops the ledger *before* resetting the context, because the ledger points into the buffer and a ledger that outlived one reset would be read against the next transaction's bytes.

### The write unit, and why a spanning transaction is refused whole

A `VfdwWriteUnit` (`src/vfdw_wbuf.h:175`) is the one Valkey identity a transaction's writes may address: server, user mapping, logical database, plus the caps and the nest level the binding was taken at. `vfdw_wbuf_bind` (`src/vfdw_wbuf.c:198`) fills it on the first call and compares on every later one, raising one of three distinct messages. Three literals rather than one parameterised message, so that a recorded expected file can tell them apart rather than only seeing `0A000`; the user-mapping message names the two *roles* rather than the two mapping OIDs, because an OID is unstable across runs and a refusal nobody can assert is a refusal nobody notices losing.

The comparison is on `umid`, not on the SQL user: the mapping determines the credentials that reach Valkey, so two SQL users sharing a `PUBLIC` mapping are one identity and must not be refused. `userid` is carried separately because the flush re-acquires with `GetUserMapping(userid, serverid)`, which takes the SQL user.

The refusal follows from the shape of the flush rather than from caution. The transaction is applied by one Lua program on one connection: `SELECT` is per-connection, credentials are per-connection, and a second server is a second connection. The alternative — several programs applied in sequence — makes partial application a normal outcome, which is the single thing the design works hardest to prevent. The same argument is made at length for hash slots in `src/vfdw_flush_cluster.c`, and reaches the same answer: refuse, with a hint that names hash tags, because a refusal a user cannot act on is just a wall.

The caps are taken from the first bind and are never re-read (`src/vfdw_wbuf.h:196`). Re-reading them per statement would let an `ALTER SERVER` tighten a cap under a transaction that had already exceeded it. They come from the options of the acquisition `vfdw_modify_connect` actually used, so a plan cached before an `ALTER SERVER` cannot disagree with them, and the bind happens *after* the `prefer_replica` refusal so a refused statement leaves no unit behind.

Slots are not part of the binding. `unit.hashslot` is set to 0 and never compared; each operation's slot is computed and stored (`src/vfdw_modify.c:491`) and `vfdw_flush_cluster_slot` (`src/vfdw_flush_cluster.c:106`) walks the buffer at flush time, checking each operation's key, its `oldkey` when the row is being renamed, and the table's keyset — because checking only the key would let a cross-slot unit through as single-slot and the server would answer `CROSSSLOT` from inside the program, after earlier writes in it had applied. That walk runs before the connection is taken, so a refusal costs no round trip.

### The fold

The buffer is an ordered log of what SQL did. The ledger is what the server has to be told: for each key, what must already be true, and the ordered list of writes that follow. One fold has two consumers — the script encoder and the overlay — so that what a `SELECT` sees inside the transaction and what `COMMIT` applies cannot disagree by construction (`src/vfdw_ledger.h`).

`vfdw_ledger_note` (`src/vfdw_ledger.c:476`) folds one operation. Each modify callback calls it immediately *after* `vfdw_wbuf_append`, so the incremental path and `vfdw_ledger_rebuild_from_buffer` walk the same sequence and must produce the same ledger; `test/regress/sql/wbuf.sql:727` asserts the equality across a `ROLLBACK TO SAVEPOINT` rather than assuming it. Folding after the append matters on the error path too: the fold may raise 23505, and by then the operation is already linked, so the abort that follows either resets the buffer or truncates and re-folds it.

The ledger has its own memory context (`src/vfdw_ledger.c:85`), not the buffer's, because it is rebuilt from scratch on every truncation and rebuilding into the buffer's context would pile a dead copy onto `write_max_bytes` each time a savepoint rolled back. It points *into* the buffer for all key and value bytes and copies none of them, which is why `vfdw_wbuf_reset` drops both together.

Keys and members are arbitrary bytes of arbitrary length, so neither `HASH_BLOBS` (fixed width) nor `HASH_STRINGS` (stops at the first NUL) can key the two dynahash tables; the entry key is a (pointer, length) pair with the hash and match functions at `src/vfdw_ledger.c:47`.

#### One plan per key

`vfdw_ledger_plan_for` (`src/vfdw_ledger.c:242`) returns the plan for a key, creating it on first touch and reporting through `*created` whether this call made it. That flag — not "the action list is empty" — is what the precondition rule turns on, because the second operation of a delete-then-insert pair follows a first operation that legitimately emitted actions. Every plan is created carrying a `TYPE_OK` check as its first check, which is what makes phase 2 incapable of raising `WRONGTYPE`: the realistic runtime error against a keyspace this wrapper does not own is refused in phase 1, where refusing is still free. A plan also accumulates the OIDs of every relation that contributed to it, because two foreign tables may legitimately map one key — a hash with disjoint field sets is the normal case — and an error naming only the last one seen sends the reader to the wrong table definition.

#### Preconditions come from the first operation

`vfdw_ledger_precondition` (`src/vfdw_ledger.c:358`) returns immediately unless this is the key's first touch. A first `INSERT`, or a rename's destination, sets `require = KEY_ABSENT`, appends a `CHK_KEY_ABSENT` and marks the key `CREATED`; anything else sets `KEY_PRESENT`, appends `CHK_KEY_PRESENT` and marks it `LIVE`.

The rule is what makes intra-transaction and cross-transaction uniqueness the same question. The check the server runs at flush is the same one the fold enforces in memory, so a second `INSERT` of a key raises 23505 at the statement rather than becoming a last-writer-wins overwrite that reports two rows and leaves one.

Taking the precondition from *every* operation would be wrong in both directions. A transaction that deletes a key and then inserts it is legal SQL; the actions are the `DEL` already recorded followed by the create, and `require` must stay `KEY_PRESENT` — demanding absence would fail the flush for doing exactly what SQL asked. And a rename's destination is a key this transaction is creating whatever statement produced it: treating it as an update would demand the new name already exist, and would also let a rename onto a live key destroy it silently, since Valkey's `RENAME` overwrites.

#### The duplicate rule and its exemptions

`vfdw_ledger_check_duplicate` (`src/vfdw_ledger.c:403`) fires only for an `INSERT` that is not the first touch of its key, and only when the plan's state is not `DELETED`. `CREATED` means this transaction made the key and `LIVE` means it existed and this transaction touched it; both refuse a second `INSERT`, `DELETED` does not.

Which shapes it applies to is narrower than the key level suggests: the check returns early for everything except `string` and `json`. The other shapes are covered one level down, where the identity actually lives.

| Shape | Where uniqueness is decided | Mechanism |
|---|---|---|
| string, json | `vfdw_ledger_check_duplicate` | key plan state |
| hash | `vfdw_ledger_fold_hash` | `(key, field)` member entry, `present` |
| set, zset | `vfdw_ledger_fold_member` | `(key, member)` entry, `present` |
| list | not checked | duplicate values are the point of a list |

Per-(key, sub) state is created by `vfdw_ledger_member` (`src/vfdw_ledger.c:289`) and initialised field by field rather than with a `memset`, because dynahash has already written the key into the leading bytes. Every field must be listed: `present_removals` was missed once, and the uninitialised read surfaced as a recorded `LVALUE_COUNT_GE` of 1 on PostgreSQL 16 and 17 and 0 on 18.

The field and member preconditions follow the key rule one level down, with one exception that is easy to get wrong: on a plan whose state is `CREATED`, no field or member precondition is emitted at all (`src/vfdw_ledger_fold.c:60` and `:119`). The key cannot already hold the field, so an absence check is dead weight on every row of a bulk load and a presence check is actively wrong — requiring presence there is how a rename came to demand that its own destination already be populated. The in-memory ledger still catches a repeat, which is where a repeat actually comes from.

For hashes, a NULL field is recorded rather than dropped: it becomes an `HDEL` at flush, while a field the statement did not assign is simply not in the array. Collapsing the two would make an `UPDATE` that nulls a field indistinguishable from one that leaves it alone (`src/vfdw_wbuf.h:96`). The all-fields-null refusal is scoped to `INSERT` (`src/vfdw_render.c:112`): for an `INSERT` it is right, because a row that sets no mapped field would create nothing and reporting one row inserted would be a lie; for an `UPDATE` clearing every mapped field is a legitimate request, and refusing it left no way to express it.

Lists replace presence with a count. A `DELETE` first consumes an occurrence this transaction itself pushed, and only when there is none does it emit `CHK_LVALUE_COUNT_GE` with an incremented `present_removals` (`src/vfdw_ledger_fold.c:210`): only the removals the transaction has not itself pushed need to exist on the server beforehand, and counting them all would refuse a transaction that pushed a value and then removed it.

#### Renames

`vfdw_ledger_is_rename` (`src/vfdw_ledger.c:418`) is an `UPDATE` whose `oldkey` differs from its `key`. `vfdw_ledger_fold_rename` emits `ACT_RENAME` carrying the old name, swaps the keyset entries, and then creates *a second plan for the old key carrying only a precondition* — a plan with checks and no actions. That indirection is necessary because a plan's checks are all about its own key and there is no opcode for "some other key is present". Both keys being in the plan set is also what makes the cluster slot walk refuse a cross-slot rename in both directions, rather than deleting without creating.

A rename is used rather than a delete-and-recreate because `RENAME` preserves whatever the table does not map, which recreation would destroy.

Member renames are handled in `vfdw_ledger_fold_member` (`src/vfdw_ledger_fold.c:162`) as a remove followed by an add, in that order. The order is correct only because the two names differ, and the comment says so precisely because a later change that made them equal would otherwise delete the row silently. The identity whose prior existence the precondition is about is the one the row *had* — `oldmember`, not `member`; asking for the new name to be present would refuse every rename.

#### Keyset maintenance

When a table has a `keyset` index, `vfdw_ledger_keyset` (`src/vfdw_ledger.c:338`) emits `KSET_ADD`/`KSET_REM` alongside the write it indexes, so index and data are in the same atomic unit. Leaving the keyset for the user to keep current is how it drifts out of step with the keys it names. The keyset name is retained on the operation rather than looked up when the fold runs, because the fold also runs from a subtransaction abort callback where opening a relation is not allowed — it takes a lock on a resource owner being torn down and the backend dies. It is the only thing the fold ever wanted the table map for.

#### Re-folding after a truncation

`vfdw_ledger_rebuild_from_buffer` (`src/vfdw_ledger.c:524`) drops the ledger and re-folds the surviving log, passing `rel = NULL` and touching no catalog for the reason above. It guards against re-entering itself: the fold can raise, but re-folding a log already in the buffer must not, since every operation in it was accepted once. The guard exists rather than the assumption because such a raise would arrive during an abort, where a second error replaces the first and the user is told about the wrong thing.

#### The vocabulary

Checks and actions are opcodes from `src/vfdw_opcode.h`, shared by the ledger and the Lua program that decodes them through two dispatch tables. The numbering is wire-visible: append only, never renumber, because a reordered enum and an unchanged script disagree silently, each doing something valid for the wrong opcode. Diagnostics and recorded expected files carry names rather than numbers for the same reason — a file full of small integers is one renumbering away from being wrong and still passing. `VFDW_REQ_ANY` is not "no check": it is the state of a plan carrying only its `TYPE_OK`, and it exists so a later reader cannot mistake "nothing recorded" for "absence required".

The encoder walks the ledger in first-touch plan order (`src/vfdw_script_encode.c:262`), and `vfdw_flush_pre_commit` reads the plan count (`src/vfdw_flush.c:720`).

### The refusal gates

`src/vfdw_refuse.c` holds the write path's refusals in one file, cut there because a refusal is the one part of the write path with no state: each function reads its arguments, decides, and either returns or raises. Nothing from it may move into `src/vfdw_wbuf.c`, which is pinned as pure memory that touches no connection.

| Gate | Where it runs | What it refuses |
|---|---|---|
| `vfdw_refuse_unwritable` | `vfdw_modify_plan` (`:133`), `vfdw_modify_begin` (`:255`), `vfdw_modify_begin_insert` (`src/vfdw_copy.c:83`) | `readonly` tables (25006), and any command outside the shape's writability mask (0A000) |
| `vfdw_refuse_on_conflict` | `vfdw_modify_plan` (`:134`) | `ON CONFLICT` (only `DO NOTHING` reaches it; `DO UPDATE` dies earlier in `infer_arbiter_indexes`) |
| `vfdw_refuse_prefer_replica` | `vfdw_modify_connect` | writing through a server configured to prefer a replica |
| `vfdw_wbuf_bind` | first row of each statement | a second server, user mapping or database in one transaction |
| `vfdw_wbuf_op_new` / `vfdw_wbuf_append` | per row | the two caps (54000), and an unbound unit (`elog`) |
| `vfdw_refuse_duplicate_key` / `_member` | the fold, per row | a key or member this transaction already created (23505) |
| `vfdw_refuse_no_modify_state` | each Exec callback | unreachable today; kept because the alternative is dereferencing NULL |
| `vfdw_flush_cluster_slot` | pre-commit, before the connection | a unit spanning two hash slots |

Two details are worth carrying forward. `vfdw_refuse_unwritable` takes its `errdetail` and `errhint` back from `vfdw_map_writability` along with the mask, because a single hardcoded detail explained every refusal with the list row-identity paragraph — so a table blocked for carrying a search index was refused for a reason that had nothing to do with it. And the duplicate messages name the key, not just the table: the bytes may be arbitrary (a `bytea` key column is documented to carry them), so they go through `vfdw_safe_text` and then `"%s"` under invariant I2, parenthesised in core's `Key (col)=(val)` spelling because when the bytes cannot be shown the substitution is itself a phrase naming an encoding, and quotes around it would nest.

The mask itself is per shape: a `list` table gets `INSERT|DELETE` only (`src/vfdw_writable.c:197`), since a list row has no identity that survives its neighbours moving. `vfdw_modify_update` therefore keeps an `elog` for a list reaching it (`src/vfdw_modify.c:563`) — loud rather than a silent wrong write if the mask ever stops being true.

### COPY, tuple routing and batch insert

`COPY FROM` and tuple routing into a foreign partition reach `ExecForeignInsert` through `BeginForeignInsert`, not `BeginForeignModify`. They bring no plan, no `fdw_private` and no subplan, so `src/vfdw_copy.c` derives from the relation everything the modify path reads from the plan. That is sound only because this path is `INSERT`: there are no junk row-identity columns, and every mapped column is assigned by definition. Both facts are restated at the derivation, because neither holds for the general modify path. Registering these callbacks (`src/valkey_fdw.c:139`) is what makes `COPY` work at all; while `BeginForeignInsert` was absent, `CopyFrom` reached `ExecForeignInsert` with `ri_FdwState` NULL and the wrapper had to refuse rather than dereference it.

The differences from plain DML:

| | `BeginForeignModify` | `BeginForeignInsert` |
|---|---|---|
| parent context | `estate->es_query_cxt` | `mtstate->ps.state->es_query_cxt`, or `CurrentMemoryContext` when `mtstate` is NULL — it is NULL for `COPY FROM` and non-NULL for tuple routing, and dereferencing it unconditionally crashes the commoner path |
| junk columns | resolved from `fdw_private` | all three explicitly `InvalidAttrNumber`, rather than relying on the zeroed allocation |
| `target_attrs` | the planner's `updatedCols` for `UPDATE`, every assignable attribute otherwise; converted on first use | every mapped attnum, built directly in the buffer context |
| EXPLAIN | `vfdw_modify_explain` prints from `fdw_private` | no plan node, nothing to print |

Everything after that is shared. `vfdw_modify_connect` runs identically, so `COPY` binds the same unit and pays the same caps, and `vfdw_modify_batch_insert` loops over the same single-row `vfdw_modify_insert` rather than carrying its own render-and-append logic — a batch loop with its own copy is how the two drift and a `COPY` starts writing something an `INSERT` would not. It never reduces `*numSlots`, because every row is accepted or the statement raises, and a short return would mean silently dropping rows.

`vfdw_modify_batch_size` returns a modest constant of 100 rather than something derived from `pipeline_batch`: nothing reaches the wire, so a batch saves callback overhead and no round trips. It drops to 1 when the statement needs per-row work core cannot batch — `RETURNING` has to project each row and a before/after insert row trigger has to see each one — because reporting more makes core skip rows silently.

One asymmetry to keep in mind: `target_attrs` is built lazily in the modify path (`src/vfdw_modify.c:386`) but eagerly here. Lazily, because paying for it at `Begin` charged every DML statement against `write_max_bytes` whether or not it buffered anything — an `UPDATE` matching no rows, or a statement refused before its first row, left a bitmap behind for the rest of the transaction.

### Principal functions

| Function | File | Responsible for |
|---|---|---|
| `vfdw_modify_plan` | `src/vfdw_modify.c` | plan-time refusals, and the four `fdw_private` slots (junk names, target attnums, table type, key source) |
| `vfdw_modify_begin` | `src/vfdw_modify.c` | per-statement state, table map, value context, junk resolution; early return under EXPLAIN-only |
| `vfdw_modify_connect` | `src/vfdw_modify.c` | proving the server usable, refusing `prefer_replica`, binding the unit, returning the lease |
| `vfdw_modify_insert` / `_update` / `_delete` | `src/vfdw_modify.c` | rendering one row into an operation, appending it, folding it |
| `vfdw_modify_render_payload` | `src/vfdw_render.c` | the value column, or every mapped hash field including NULLs |
| `vfdw_modify_retain_name` | `src/vfdw_render.c` | copying a field or keyset name into the buffer context |
| `vfdw_wbuf_context` | `src/vfdw_wbuf.c` | the backend-lifetime buffer context; registering the transaction callbacks |
| `vfdw_wbuf_bind` | `src/vfdw_wbuf.c` | binding the transaction's one Valkey identity, or refusing a second |
| `vfdw_wbuf_op_new` | `src/vfdw_wbuf.c` | allocating a zeroed operation and enforcing the byte cap at allocation |
| `vfdw_wbuf_append` | `src/vfdw_wbuf.c` | stamping level and command id, enforcing both caps, linking, bumping the generation |
| `vfdw_wbuf_subxact` | `src/vfdw_wbuf.c` | savepoint truncation and promotion, rebinding, triggering the re-fold |
| `vfdw_wbuf_reset` | `src/vfdw_wbuf.c` | dropping ledger, overlay and buffer together at end of transaction |
| `vfdw_ledger_note` | `src/vfdw_ledger.c` | folding one operation into its key's plan |
| `vfdw_ledger_plan_for` | `src/vfdw_ledger.c` | first-touch plan creation and the `*created` flag the precondition rule turns on |
| `vfdw_ledger_precondition` | `src/vfdw_ledger.c` | the first-operation rule: `KEY_ABSENT` for a create, `KEY_PRESENT` otherwise |
| `vfdw_ledger_check_duplicate` | `src/vfdw_ledger.c` | the key-level duplicate refusal and its shape exemptions |
| `vfdw_ledger_fold_rename` | `src/vfdw_ledger.c` | `RENAME`, keyset swap, and the source key's pure-precondition plan |
| `vfdw_ledger_keyset` | `src/vfdw_ledger.c` | keyset index maintenance inside the same unit |
| `vfdw_ledger_fold_hash` / `_member` / `_list` | `src/vfdw_ledger_fold.c` | the per-shape fold and its `(key, field)` / `(key, member)` / occurrence-count rules |
| `vfdw_ledger_rebuild_from_buffer` | `src/vfdw_ledger.c` | re-folding the surviving log after truncation, without catalog access |
| `vfdw_refuse_unwritable` | `src/vfdw_refuse.c` | `readonly` and the per-shape writability mask, with the reason that goes with it |
| `vfdw_refuse_duplicate_key` / `_member` | `src/vfdw_refuse.c` | the 23505 the fold raises, naming the offending bytes safely |
| `vfdw_modify_begin_insert` | `src/vfdw_copy.c` | the plan-less entry point for `COPY FROM` and tuple routing |
| `vfdw_modify_batch_size` / `vfdw_modify_batch_insert` | `src/vfdw_copy.c` | how many rows core may hand over, and buffering them through the single-row path |
| `vfdw_flush_cluster_slot` | `src/vfdw_flush_cluster.c` | the one slot the unit writes, or the cross-slot refusal |

---

## The atomic apply: the Lua program and the flush

A transaction's writes never travel as they execute. DML appends to the write buffer, the ledger folds the buffer into one plan per key, and at `XACT_EVENT_PRE_COMMIT` the whole set is encoded into a single `EVALSHA` and applied by one server-side program. That is what makes the unit atomic without `MULTI`: the program runs as one server-side execution, so no other client's command can separate a check from the write it guards.

### Two phases, and the single exit (W1)

The program is one compile-time C literal (`src/vfdw_script.c:47`–`174`). It runs in three movements, and the order is load-bearing.

It first decodes every plan out of `ARGV` before running anything (`src/vfdw_script.c:62`–`95`). A decode failure after a write would be partial application; here nothing has been written, so a malformed argument list is a `BADPROTO` refusal. The decoder also insists it consumed exactly the arguments it was sent — `if i ~= #ARGV + 1` at `src/vfdw_script.c:96` — so a truncated or over-long vector is refused rather than silently half-read.

Phase 1 (`src/vfdw_script.c:100`–`139`) runs every check and writes nothing; phase 2 (`src/vfdw_script.c:141`–`172`) writes and checks nothing. A script that interleaved them could refuse the fourth plan after applying the first three, which is exactly the outcome this design exists to make unreachable. Phase 1 has a single exit: a check closure returns a code string, the loop returns `fail(...)` on the first one, and nothing has been written.

The type check is not in the dispatch table. `TYPE_OK` is a check opcode the ledger puts on every plan as its first check (`src/vfdw_ledger.c:277`–`283`), but the script's `CHECK` table has `[2] = nil` and the dispatch loop skips op 2 explicitly. Instead each plan gets an unconditional `TYPE` call at `src/vfdw_script.c:126`–`129`. Doing it inline, before the table is consulted, is what makes phase 2 incapable of raising `WRONGTYPE` — the realistic error against a keyspace this wrapper does not own is refused where refusing is still free.

**W1** is the property that the phase split is checkable. Phase 2 dispatches through closures whose verbs are literal string constants — `server.pcall('SET', k, a[1])` — rather than taking the verb from `ARGV`. A verb read from data would be shorter, would work identically, and would make the gate vacuous, because there would be no write verb left in the file to grep for. `scripts/lint.sh:260`–`305` enforces three things: every one of the eleven distinct write verbs appears as a literal inside a `server.pcall`; both phase markers exist and `PHASE 1` precedes `PHASE 2`; and no `server.pcall` appears between the two markers. The third check takes grep's exit status apart rather than testing it for zero (`scripts/lint.sh:296`–`303`), because a grep that failed to run would otherwise read as "phase 1 is clean" — the two other checks fail in the safe direction and are left alone.

Phase 2 can still fail: an action opcode with no table entry, or a `pcall` that returns an error table, produces `fail('INTERNAL', ...)` (`src/vfdw_script.c:164`–`170`). That is outcome 4 below, and by construction it means the check set was incomplete. On success the program returns a status reply `VFDW1 OK <nplans>` (`src/vfdw_script.c:174`).

### Keys travel as indices, never as names (W2)

Every key argument inside `ARGV` is a 1-based index into `KEYS`, never a key name: `RENAME`'s source, and the keyset name for `KSET_ABSENT` / `KSET_PRESENT` / `KSET_ADD` / `KSET_REM`. The reason is the `#!lua` shebang, which makes the server enforce the declared-key rule — a name smuggled through `ARGV` would be an undeclared key, accepted on a single node right up until a slot migrates and then refused.

`src/vfdw_script_encode.c` owns the mapping and is the only place that needs to get it right. Which argument of a step is a key is answered by two small tables, `vfdw_check_key_arg` and `vfdw_action_key_arg` (`src/vfdw_script_encode.c:146`–`171`), rather than by a condition at each emit site: the encoder and the two Lua dispatch tables have to agree opcode by opcode, and a rule spelled out once can be read against them.

Interning runs in two passes (`src/vfdw_script_encode.c:262`–`265`). Plan keys go in first, in plan order, so a plan's own key index is its ordinal and needs no lookup; only the extras are searched, and a rename whose source is another plan's key resolves to that plan rather than being declared twice. The two passes are not an optimisation — `numkeys` is written to the command before any `KEYS` element and long before any `ARGV`, so the whole key set has to be known before a single byte is emitted.

**W2** — every emitted index is within `KEYS` — is asserted where the cause is visible. `src/vfdw_script_encode.c:232`–`236` fails loudly if a step's key argument is not in the table, and `src/vfdw_script_encode.c:285`–`286` re-checks the plan's own ordinal, because the two loops that make that true are far enough apart to be edited alone. An out-of-range index would be `nil` in Lua, which `SISMEMBER` sees as a missing argument and `RENAME` as a nil key — failures a long way from their cause.

### The wire encoding and the golden vector

The layout is specified beside the program that decodes it (`src/vfdw_script.c:27`–`46`) and written by one function:

```
EVALSHA <sha1> <numkeys> <key…>            -- keys, deduplicated, plan keys first
ARGV:   "V1" <nplans>
        per plan: keyidx ttype require nchecks nactions
                  nchecks  × (checkop  nargs arg…)
                  nactions × (actionop nargs arg…)
```

The opcodes are the integers of `VfdwCheckOp` and `VfdwActionOp`. That numbering is wire-visible, which is why `src/vfdw_opcode.h:40`–`48` says append only and never renumber: a reordered enum and an unchanged script disagree silently, each doing something valid for the wrong opcode — the ledger emits `ZADD` and the script runs `ZREM`. The Lua tables are written with an explicit `[0] =` first entry for the same reason; Lua indexes from 1, and a bare list would shift every opcode by one.

Because the encoder is the only writer of this format and the program the only reader, a change to either that the other does not match surfaces as a `BADPROTO` from inside somebody's `COMMIT`. The golden vector exists to move that discovery to review time. `valkey_fdw_test_script_program()` (`src/vfdw_testwbuf.c:576`) re-runs the encoder against the current buffer and returns one row per command element; `test/regress/sql/script.sql` records the vector for a transaction chosen to cover every argument shape the encoder can emit — plain data, a key index for a keyset, and a key index for a `RENAME` source. The digest is rendered as the fixed placeholder `<sha1>` (`src/vfdw_testwbuf.c:594`–`598`) because it moves whenever the script text changes, comments included, and a golden vector that churns on every edit stops being read.

The same suite holds the assertions the vector cannot make: the two Lua dispatch tables are parsed out of the script *text* and full-joined against `valkey_fdw_test_opcodes()`, so a renumbering on either side is a row rather than an absence; and the encoded program is handed to the real server, so an argument-order disagreement the recorded bytes cannot see arrives as a `BADPROTO` in the suite instead of at a commit.

### Script identity, and the NOSCRIPT retry

`vfdw_script_sha1()` (`src/vfdw_script.c:192`–`229`) computes the digest once per backend into a static buffer. It hashes `sizeof(literal) - 1` (`src/vfdw_script.c:216`): the trailing NUL is not part of the script, and hashing it yields a digest the server never agrees with — off by exactly one byte is the kind of mistake that presents as a server problem. The digest is computed rather than hard-coded because a literal is one edit away from naming a script that is not this one, and `EVALSHA` would then either miss on every attempt or, on a server where another client loaded a script with that digest, run something else. The suite closes the loop by comparing the compiled-in value against what the server's own `SCRIPT LOAD` returns; agreeing with ourselves would prove only that SHA1 is deterministic.

Loading is pipelined rather than probed. `vfdw_flush_attempt` (`src/vfdw_flush.c:395`) queues `SCRIPT LOAD` ahead of the `EVALSHA` in the same batch whenever the connection's `script_loaded` flag is unset, so the cold case costs no extra round trip and the flag is set only after an outcome of applied (`src/vfdw_flush.c:443`–`444`). The flag is cleared on connection open (`src/vfdw_conn.c:55`), on pool entry creation (`src/vfdw_conn_pool.c:57`) and in the subtransaction abort sweep (`src/vfdw_conn_xact.c:123`) — all three are needed, because `hash_search(HASH_ENTER)` fills only the key, so a field cleared only in close reads uninitialised for a brand-new identity and would skip a load the server never saw.

If the load itself errors, it is reported as itself (`src/vfdw_flush.c:434`–`438`) and the phase is marked proven clean: the two commands execute in order, so a failed load means the `EVALSHA` behind it necessarily saw `NOSCRIPT` and nothing ran. A `NOPERM` where scripting is denied by ACL is far more useful than the `NOSCRIPT` it causes.

### Classifying the reply

`vfdw_script_classify` (`src/vfdw_script.c:272`) reads one reply body. It takes `(str, len)` rather than a NUL-terminated string because a reply is Valkey's memory with its own length (I3), and matching is done on a word boundary by `vfdw_script_word_at` (`src/vfdw_script.c:259`): without it, `VFDW1 CONFLICTX` classifies as `CONFLICT` and reports a unique violation for something else entirely. The sentinel space is ours to extend, so an old build must refuse to guess at a code it does not know — and our own prefix with an unrecognised code returns `BADPROTO`, not `UNKNOWN` (`src/vfdw_script.c:317`–`324`), because it means this build and the loaded script disagree. The detail is `pnstrdup`'d because the caller's reply is batch-owned and the next take invalidates it, and it is copied as *bytes* without an encoding check — I2 is about what reaches error reporting, which is `vfdw_flush_detail`'s job (`src/vfdw_flush.c:131`–`138`), since the script's `fail()` concatenates the key into every message and a bytea key column carries arbitrary bytes by design.

The flush's own classifier is two functions. `vfdw_flush_classify` (`src/vfdw_flush.c:318`) handles the non-error path: a NULL reply is indeterminate; the reply must be a `STATUS` reply, checked through `vfdw_reply_expect` with a mask built by `VFDW_RTYPE` (`src/vfdw_flush.c:344`) — the comment records that OR-ing raw type constants instead produced a mask matching nothing and rejected the program's own success reply *after* it had written, which is the one outcome this file exists to prevent. The status must classify as `OK`, and its detail must equal the decimal plan count exactly (`src/vfdw_flush.c:371`–`375`). That is a comparison against a number we already know, not a parse: `atoi` reports no failure of its own — it is why a port written `abc` silently becomes the default — and here it would have read `VFDW1 OK` followed by garbage as an honest zero-plan reply.

`vfdw_flush_classify_error` (`src/vfdw_flush.c:280`) handles error replies in a fixed order: `NOSCRIPT` and then `TRYAGAIN`/`LOADING` return retryable outcomes; `VFDW1 INTERNAL` raises outcome 4; any other `VFDW1` code raises a sentinel; a foreign error whose prefix appears in the proof list (`src/vfdw_flush.c:235`–`238`) is raised with the server's own message and the phase marked proven clean; anything else is indeterminate.

### The four outcomes, and which way classification degrades

| # | Outcome | SQLSTATE | Reached by |
|---|---|---|---|
| 1 | Applied | — | `VFDW1 OK <n>` as a status reply, `n` equal to the plan count |
| 2 | Not applied | 23505 / 40001 / 42804 / XX000 | A phase-1 sentinel (`CONFLICT` / `MISSING` / `WRONGTYPE` / `BADPROTO`), a failure strictly before the send, or a server prefix proving pre-execution rejection |
| 3 | Indeterminate | 08007 | Any I/O failure, deadline, cancel or unexpected shape once the `EVALSHA` has been handed to the socket |
| 4 | Partially applied | XX000 | `VFDW1 INTERNAL` — the program failed after it had begun writing |

The mapping from verdict to SQLSTATE lives with the flush (`src/vfdw_flush.c:161`–`201`) rather than with the script, because "what does this mean to SQL" is a different question from "what did the server say".

The governing rule is that classification degrades toward *unknown* and never toward a false *applied*. Two mechanisms implement it. First, every unrecognised shape or status in `vfdw_flush_classify` falls to `vfdw_flush_error_indeterminate` rather than being interpreted. Second, `vfdw_flush_rethrow_indeterminate` (`src/vfdw_flush.c:484`) re-raises as 08007 the transport failures that are raised *beneath* the flush — a closed connection or a malformed frame comes from `vfdw_io`/`vfdw_cmd` and never reaches the classifier, so before this existed it arrived carrying the transport's own 08006 and an application switching on 08007 missed it. What it must not swallow is enumerated at `src/vfdw_flush.c:494`–`503`: errors raised before the send or already proven clean, our own sentinels, a query cancel, an admin shutdown, anything at FATAL or above, and a pending die. Each is re-thrown because it is either more true or more specific than 08007. The original message becomes the errdetail so the cause is not lost behind "the outcome is unknown".

The hint attached to outcome 3 names three states, not two (`src/vfdw_flush.c:148`–`157`). "In full or not at all" would be a positive claim that outcome 4 contradicts, and the message must not promise what a possibly-incomplete precondition set cannot deliver. `test/regress/sql/wfault.sql` drives the fault proxy through the reply path and asserts the negative property directly: three of its five faulted transactions leave their key in Valkey while PostgreSQL rolls back, and the suite records that rather than asserting it away, because the durability boundary is one-directional by design.

### Why PRE_COMMIT is the only legal point

At `XACT_EVENT_PRE_COMMIT` the transaction is still `TRANS_INPROGRESS`, the ResourceOwner and memory contexts are intact, an `ereport` still aborts, and interrupts are not held — `HOLD_INTERRUPTS` is roughly sixty lines later in `CommitTransaction`. Every other transaction event runs inside that hold, where `CHECK_FOR_INTERRUPTS` is a no-op and invariant I6 cannot be satisfied, so no byte may reach the wire from any of them. There is deliberately no lint for the rule (`src/vfdw_xact.h:23`–`40`): a structural grep over the switch arms passes trivially the moment the code is factored into helpers, so it is a rule that could only ever be green.

Registration is through the single callback in `src/vfdw_xact.c:36`–`49`, which calls the flush before the connection sweep so a sweep can never run before the work it would have carried. The switch has no `default`, so a new event in a future major is a compiler warning rather than a silent fall-through past the flush. `PRE_PREPARE` refuses a non-empty buffer outright — Valkey has no two-phase commit — and `PARALLEL_PRE_COMMIT` `elog`s if a parallel worker somehow holds one.

`vfdw_flush_pre_commit` (`src/vfdw_flush.c:684`) returns immediately on an empty buffer, touching no connection. Otherwise it installs the context callback, acquires the connection, creates `flush_cxt` as a child of the write buffer's context — so an error path releases it with the buffer's own reset in the ABORT branch rather than needing a `PG_FINALLY` — and runs the retry loop. `vfdw_flush_acquire` (`src/vfdw_flush.c:592`) checks `vfdw_conn_peek` *before* `GetForeignServer` and the connection call, turning `PreCommit_Portals`' expectation that every portal is gone into a checked precondition; the ordering matters because `vfdw_get_connection` closes an invalidated connection inline, freeing the very context a live scan batch caches, so a guard phrased in terms of its return value could only run after the destructive part.

Nothing is cleaned up on the way out (I1). The `PG_TRY` at `src/vfdw_flush.c:729` frees nothing and closes nothing — its only job is deciding the SQLSTATE. The `EVALSHA` reply is batch-owned and released by the batch's context reset callback on any unwind; the batch is deliberately left open on every raising path (`src/vfdw_flush.c:386`–`393`), which leaves the connection flagged mid-conversation so `vfdw_conn_xact`'s abort branch discards a reply stream whose offset nobody knows.

### The error-context callback

`vfdw_flush_errcontext` (`src/vfdw_flush.c:104`–`111`) is an `ErrorContextCallback` installed for the whole flush region. It fires for *every* error raised inside that region — ours, libvalkey's, a query cancel, a `transaction_timeout`, an OOM — without a `PG_TRY` and without intercepting anything. That is what it can do that error handling on our own paths cannot: a 57014 raised by core at `COMMIT` still carries a line saying whether the write may have landed.

It reads one variable, `vfdw_flush_phase`, which has three values rather than two. `VFDW_FLUSH_PROVEN_CLEAN` exists because a reply can prove the program did not run after the command was sent, and without that third state the context line would say "may or may not have been applied" directly above a hint saying "no changes were applied" — a reader would rightly believe the more alarming of the two. The phase is raised to `SENT` after the commands are queued but before the batch necessarily pushes them (`src/vfdw_flush.c:418`–`422`), which is the conservative direction: everything above that line is pure local work and provably not applied.

### The retry budget

`vfdw_flush_run` (`src/vfdw_flush.c:623`) loops until the unit applies or a budget runs out. Both retryable outcomes prove the program did not run — `NOSCRIPT` because there was nothing to run, `TRYAGAIN` and `LOADING` because they are the server saying "not now" rather than "no" — so going round again cannot double-apply. Each attempt opens its own batch and therefore gets a fresh deadline, and the phase is reset to `BEFORE_SEND` so a failure during the retry still reports honestly.

The two budgets differ deliberately. `NOSCRIPT` gets exactly one forced reload however large `write_retry_count` is (`src/vfdw_flush.c:649`–`666`): a second `NOSCRIPT` for a program sent in the same round trip is not a transient condition, it is another client running `SCRIPT FLUSH` in a loop, and retrying it is only a slower failure. `TRYAGAIN` and `LOADING` are bounded by `write_retry_count` — default 3, range 0 to 10 (`src/vfdw_option.c:171`), where 0 disables retrying entirely — and exhausting it raises 40001 with a hint naming the option, because the retry happens inside `COMMIT` with the client already waiting. `vfdw_flush_retries()` and its siblings are backend-lifetime counters that are never reset, so a suite reads them as deltas (`src/vfdw_flush.h:36`–`43`).

### The cross-slot refusal on a cluster

One program on one connection is the whole basis of the guarantee, and a cluster breaks the premise: keys in different hash slots live on different primaries, and a single `EVAL` may touch only one slot. A cross-slot transaction therefore cannot be one program. The choice made in `src/vfdw_flush_cluster.c` is to refuse it whole. The alternative — one program per node, applied in sequence — makes partial application a normal outcome, which is the single thing the write path works hardest to prevent, and it would have to be documented as such rather than shipped quietly.

`vfdw_flush_cluster_slot` (`src/vfdw_flush_cluster.c:105`) walks the write buffer and, for each operation, folds in three keys rather than one: `key`, `oldkey` and `keyset` (`src/vfdw_flush_cluster.c:113`–`123`). Checking only the first would let a rename or a keyset-indexed table through as single-slot, and the server would answer `CROSSSLOT` from *inside* the program, after the earlier writes in it had already applied — outcome 4, reached by the exact route this check closes. The refusal is `ERRCODE_FEATURE_NOT_SUPPORTED`, names both keys and both slots through `vfdw_safe_text` (I2 applies: both are bytes a bytea key column put in the buffer), and hints at hash tags, because a refusal a user cannot act on is just a wall.

The ordering in `vfdw_flush_connect` (`src/vfdw_flush.c:542`–`589`) matters as much as the check. The `cluster` option is read from the catalog rather than from a connection, so the refusal happens before anything is taken from the pool: it costs no round trip, cannot leave a connection mid-conversation, and keeps no pointer live across the `ereport` inside the enclosing `PG_TRY`. Only once a slot is known is the map consulted, and an unclaimed slot is not grounds to refuse — I8 says the map is a cache and the server is the authority, so the discovery connection is used and whatever `MOVED` comes back is reported. Note that the flush does not invalidate the slot map on a `MOVED`; `vfdw_cluster_invalidate` is called only from the scan path (`src/vfdw_scan_cluster.c:279`).

`test/regress/sql/cluster.sql` (slice 5) exercises both halves: a single-slot transaction applies and lands on the owning node, and a cross-slot one is refused with *neither* key left behind — the second assertion being the only one that would catch a check placed after the first write.

### Principal functions

| Function | File | Responsibility |
|---|---|---|
| `vfdw_script_text` | `src/vfdw_script.c` | Returns the compile-time Lua literal, for `SCRIPT LOAD` and for the suite |
| `vfdw_script_sha1` | `src/vfdw_script.c` | The digest the server knows the program by, computed once per backend |
| `vfdw_script_classify` | `src/vfdw_script.c` | Turns one reply body into a `VfdwScriptVerdict`, matching codes on a word boundary |
| `vfdw_script_encode` | `src/vfdw_script_encode.c` | Folds the ledger into one `EVALSHA` command; the only writer of the wire format |
| `vfdw_check_key_arg` / `vfdw_action_key_arg` | `src/vfdw_script_encode.c` | The single statement of which argument of an opcode is a key index (W2) |
| `vfdw_flush_pre_commit` | `src/vfdw_flush.c` | The whole flush: precondition, connection, context callback, retry loop |
| `vfdw_flush_acquire` / `vfdw_flush_connect` | `src/vfdw_flush.c` | Quiescence check, then the connection the program runs on — cluster-routed if the server is one |
| `vfdw_flush_run` | `src/vfdw_flush.c` | The retry loop and its two distinct budgets |
| `vfdw_flush_attempt` | `src/vfdw_flush.c` | One batch: optional `SCRIPT LOAD`, the `EVALSHA`, and the phase transition to SENT |
| `vfdw_flush_classify` / `vfdw_flush_classify_error` | `src/vfdw_flush.c` | Reply to outcome, including the plan-count comparison and the proof-prefix list |
| `vfdw_flush_errcontext` | `src/vfdw_flush.c` | The `errcontext` line every error inside the flush carries |
| `vfdw_flush_rethrow_indeterminate` | `src/vfdw_flush.c` | Re-raises post-send transport failures as 08007, and re-throws everything more specific |
| `vfdw_flush_cluster_slot` | `src/vfdw_flush_cluster.c` | The unit's single slot, or the cross-slot refusal |
| `valkey_fdw_test_script_program` | `src/vfdw_testwbuf.c` | Re-runs the encoder and returns the command element by element; the golden vector's source |

---

## Cluster routing, and values on the wire

This section covers two things that meet at the same place: how a key is turned into a node, and how a Datum is turned into the bytes that key names. They meet because the slot is computed from the rendered key bytes — `vfdw_val_slot` is in `src/vfdw_slot.c`, which shares a header with the Datum-to-bytes code (`src/vfdw_val.h`) precisely because a caller needs both to write one row.

### I8: the map is a cache, the server is the authority

Invariant I8 is stated in `src/vfdw_cluster.h:6`, not in a `.c`, because it is a rule about the type rather than about one function. Every routing decision the slot map informs must be allowed to be wrong, because during a reshard it will be. The consequence that shapes the code is negative: nothing in the routing layer may raise on a slot it cannot place. An unknown owner means "ask a node and expect to be redirected", which is exactly what a client with no map at all does.

That rule shows up in three places, and each of them is a place where refusing would have been the easier code to write:

- `vfdw_cluster_route` (`src/vfdw_cluster.c:404`) returns NULL for an unclaimed slot rather than erroring. `owner[]` is `int16` with `-1` meaning unclaimed, and unclaimed is a real cluster state — a partly formed or mid-reshard cluster genuinely has slots nobody serves (`src/vfdw_cluster.h:55`).
- A keyed lookup keeps an unplaced key in *every* node's subset instead of dropping it (`src/vfdw_scan_cluster.c:235`). Dropping it would turn a resharding window into missing rows, which is a wrong answer with no error attached.
- The flush sends a unit whose slot nobody claims to whichever node answered discovery, and lets the MOVED come back (`src/vfdw_flush.c:580`).

The probe `valkey_fdw_test_cluster_route` reports host and port as NULL for an unclaimed slot for the same reason (`src/vfdw_testcluster.c:118`) — the state has to be representable in the diagnostic, or the diagnostic cannot show it.

### Discovery, cache lifetime, and invalidation

Discovery is one `CLUSTER SHARDS` (`src/vfdw_cluster.c:278`). There is deliberately no `CLUSTER SLOTS` fallback: it would be parsing code no supported image ever executes, and unexecuted parsing code is what this project refuses to ship (`src/vfdw_cluster.h:16`).

The reply is walked pairwise. `CLUSTER SHARDS` answers with a flat alternating array under RESP2 and with a map under RESP3, and libvalkey lays both out as alternating key and value, so one walk serves both (`vfdw_cluster_field`, `src/vfdw_cluster.c:66`). Fields are looked up by name and an absent one is not an error — the command has gained fields between versions and will gain more. Children are taken through `vfdw_reply_child` rather than indexed out of `element[]`, because the pair walk derives its indices from a count the server supplied and a frame declaring more elements than it carries would otherwise be read past its end.

Three parse decisions are worth knowing before editing:

| Decision | Where | Why |
|---|---|---|
| `endpoint` preferred, `ip` as fallback | `src/vfdw_cluster.c:150` | They differ behind NAT and under `announce-ip`; endpoint is what a client should connect to |
| A shard with no `master` role contributes no owner (`-1`) | `src/vfdw_cluster.c:126` | A primary that is down is a cluster state, not a parse failure — its slots stay unowned and the server will say MOVED |
| A range outside `0..16383` is ignored, never clamped | `src/vfdw_cluster.c:190` | A server saying something impossible should not have it quietly made plausible |

The map is built in a scratch context and only copied into its own on success (`vfdw_cluster_install`, `src/vfdw_cluster.c:211`). The cluster context has backend lifetime and is never reset, and `owner[]` alone is 32 KB, so a half-built map abandoned there once per statement would accumulate. `install` also reallocates the node array and copies the host strings: the `memcpy` of the struct brings the *pointer*, which still refers to the scratch context, and leaving it would hand every later route a pointer into memory freed when discovery returned (`src/vfdw_cluster.c:220`). The batch is drained before the install, so the last thing that can raise happens while a failure still costs nothing (`src/vfdw_cluster.c:297`).

The cache is a backend-lifetime hash keyed `(serverid, userid)` — the same identity the connection pool keys on (`vfdw_conn_serverid`/`vfdw_conn_userid`, `src/vfdw_conn.c:560`). A new entry has `map` cleared explicitly from dynahash's own `found` flag (`src/vfdw_cluster.c:352`): `hash_search(HASH_ENTER)` writes only the key, so testing `entry->map != NULL` to decide whether to load reads freelist residue, and a chunk whose residue is non-zero passes for a loaded map and is then dereferenced as `map->owner[slot]`.

`vfdw_cluster_invalidate` (`src/vfdw_cluster.c:374`) drops the entry rather than reloading it, because the caller is usually mid-command holding a reply it has not finished with — the worst moment to issue another command on that connection. **It does not free the retired map.** A scan captures the map for a whole pass, and freeing here would leave that capture dangling: a use-after-free reachable by nothing more exotic than a slot moving while a query runs. One map is abandoned per observed reshard, for the life of the backend.

### CRC-16 and the hash tag

`vfdw_val_slot` (`src/vfdw_slot.c:123`) is CRC-16 of the hash tag masked with `0x3FFF`. The mask rather than `% 16384`, to match the server byte for byte; for a `uint16` accumulator the two are equivalent, and they diverge only if the CRC is ever held in a signed type that goes negative.

`vfdw_val_crc16` (`src/vfdw_slot.c:41`) is CRC-16/XMODEM written as the bitwise loop rather than the 256-entry table Valkey ships. The table would be transcribed data whose only proof is that it matches what it was copied from; the loop's agreement with the server is a test result — `test/regress/sql/cluster.sql` compares it against `CLUSTER KEYSLOT` on the same bytes. Bytes are read through `unsigned char`: plain `char` is signed on x86-64 and ARM, every byte ≥ 0x80 would sign-extend, and that bug is invisible to any ASCII-only test.

`vfdw_val_hashtag` (`src/vfdw_slot.c:82`) reproduces `keyHashSlot` exactly: first `{`, then the first `}` searched from the byte *after* it; no match, or a `}` immediately following, means the whole key. No brace balancing, no nesting, no second candidate pair. Every plausible improvement puts a key on a different node than the server does. Searching for `}` from index 0 is the subtle one — on `a}b{c` it finds a closer to the left of the opener and computes a negative length. `memchr` with explicit lengths throughout: a NUL is an ordinary byte, legal inside a tag, and does not end the key.

Two facts a maintainer will otherwise learn the hard way. This helper must not be shared with the read path's `SCAN MATCH` handling: Valkey has a separate `patternHashSlot()` where a pattern containing `*`, `?`, `[` or `\` before the closing brace routes to all slots, so a glob and a literal key with the same bytes route differently (`src/vfdw_slot.c:109`). And slot 0 is not a usable "no slot" sentinel — the empty key hashes to 0, as does any key of only NUL bytes, because CRC-16 with init 0x0000 is a fixed point at 0x00 (`src/vfdw_val.h:251`).

### The fan-out: one node at a time

`SCAN` is per node, so a keyspace scan visits every primary. `vfdw_scan_cluster_setup` (`src/vfdw_scan_cluster.c:97`) decides whether to fan out, copies the node list, sets the redirect budget, and attaches node 0 explicitly — the seed connection is not necessarily a primary and not necessarily the first one listed, so moving onto node 0 means the traversal visits exactly the primaries the map names, in a known order, whoever answered discovery.

The node list is **copied** into the scan's own context (`vfdw_scan_cluster_copy_nodes`, `src/vfdw_scan_cluster.c:81`), and the map the pass routes by is captured in `state->pass_map` rather than read live (`src/vfdw_scan_internal.h:107`). Both are the same defence one level apart: another statement may reload the map mid-pass, and a scan halfway through node two would then skip a primary or visit one twice, or offer one key to both the node the stale map named and the node the fresh one names and return a duplicate row. These are wrong answers rather than errors. (Routing off the live map was actually run and changed nothing in the fixture, so the snapshot stands on being correct and cheap rather than on a demonstration.)

Connections are held one at a time. `vfdw_scan_cluster_attach` (`src/vfdw_scan_cluster.c:143`) ends the batch and releases the current connection before taking the next node's. Holding all of them would cost one lease per primary for the whole scan, and `VFDW_MAX_CONN_SLOTS` (32, `src/vfdw_conn_internal.h`) is per identity, so a query joining a few foreign tables on a large cluster would exhaust the pool for no benefit — rows are emitted as they are read and nothing needs two nodes open at once. The exhaustion message says so (`src/vfdw_conn_pool.c:120`). Pooling is per node: an entry already pointing at the wanted endpoint is reused, a free entry with nothing open is preferred over one open to a different node, and repointing closes the socket first because reusing it would silently talk to the wrong peer (`src/vfdw_conn_pool.c:163`). Attach also resets `batch_cxt` rather than allocating a new batch in the scan context — this runs on every node advance and every redirect, so it is the site that produces the most batches by far.

Attach clears `started` and the cursor. Leaving `started` set would resume node *n* at a cursor position that meant something on node *n−1*, which is not an error anywhere — just a different set of keys.

**An exhausted node is not an exhausted scan.** This is invariant I5 in its cluster form, and `vfdw_scan_cluster_advance` (`src/vfdw_scan_cluster.c:189`) is the only thing that may end a keyspace traversal: `vfdw_scan_next_page` reaching cursor 0 calls it and only ends the scan when it returns false (`src/vfdw_scan.c:284`). Treating `cursor == NULL` as the end returns whichever third of the keyspace happened to be visited first, with no error attached. The same trap arrives by a second route on a keyed lookup: `vfdw_scan_cluster_keys_for_node` partitions the plan's keys by owner, so a node owning none of them contributes nothing, and `vfdw_scan_keys_page` must advance rather than return false on an empty subset (`src/vfdw_scan_cluster.c:376`). Both are covered by mutations `P4-i5` and `P4-keys` in `scripts/mutate.py`.

Two smaller things that follow. `vfdw_scan_state_create` takes its connection through `vfdw_get_connection_cluster` (`src/vfdw_scan.c:645`); the plain `vfdw_get_connection` refuses a server with `cluster 'true'` (`src/vfdw_conn.c:506`) because an unrouted scan against a cluster does not fail, it silently returns whichever node it reached. Permission to reach a cluster is opt-in per call site. And a cluster connection accepts only database 0 (`src/vfdw_conn.c:331`).

### MOVED versus ASK, and the retry budget

Both arrive as ordinary error replies, and that is the danger: the scan treats an error on a value fetch as a key that vanished or held the wrong type and skips it, which is right for WRONGTYPE and catastrophic for a redirect. The row is not absent, it is elsewhere. `vfdw_scan_note_redirect` therefore runs *before* the skippable test (`src/vfdw_scan.c:536`), and mutation `P4-moved` disables exactly that line.

`vfdw_cluster_is_redirect` and `vfdw_cluster_redirect_is_ask` (`src/vfdw_cluster.c:449`, `:460`) match the `MOVED ` and `ASK ` prefixes on the reply's own length. The distinction is the whole point: MOVED means the slot has a new owner and the map is stale; ASK means the slot is mid-migration and this *one* key has already moved while ownership has not changed. So `vfdw_scan_redirected` (`src/vfdw_scan_cluster.c:259`) records the key in both cases but invalidates the map only when the reply is not an ASK (`:278`) — refreshing on an ASK would rewrite the entire routing table from a transient state.

Nothing sends `ASKING`, and the retry does not target the ASK redirect's endpoint: the key is simply re-fetched on a later pass, routed by the map. A migration window held open — the source keeping the slot and answering ASK for a key it no longer holds — is therefore not resolvable by this client, which is what the `cl_hold_open_ask` fixture in `test/regress/sql/cluster.sql` sets up.

The retry pass reuses the keyed page loop rather than unwinding a batch that still has replies in flight (`vfdw_scan_retry_pass`, `src/vfdw_scan_cluster.c:292`). It promotes the redirected list to `plan_keys`, rebuilds the node list from the refreshed map — a slot moving between existing primaries leaves the set unchanged, but a node joining does not, and chasing a key onto a node this scan has never heard of is the point of the pass — and reattaches at node 0.

The budget is three passes, set at `src/vfdw_scan_cluster.c:124`. It is bounded because two nodes mid-migration can point at each other, and an unbounded chase is a hang rather than an error. What matters more is what happens when it runs out:

> Owed a pass and out of budget is **not** the same answer as having nothing left to chase. These were once a single test, so a scan that gave up chasing keys ended exactly like a scan that had finished: the redirected keys' rows simply were not in the result and nothing was reported. That is a result silently short of the rows it owed, arriving through the slot map instead of through the cursor. It was measured as a keyed lookup returning zero rows on roughly one cluster formation in six, silently.

Exhausting the budget now raises `ERRCODE_T_R_SERIALIZATION_FAILURE` naming the number of keys (`src/vfdw_scan_cluster.c:318`). This is a normal outcome rather than a rare one: `CLUSTER SETSLOT` tells two nodes and the rest learn by gossip, and the retry pass reloads the map through whichever node the fan-out was last attached to — so a cluster that is merely converging spends the whole budget. The suite waits for every primary to agree before asserting, and says why (`test/regress/sql/cluster.sql:348`).

One consequence to keep in mind when editing `vfdw_scan_cluster_setup`: the budget is set after the early return for a non-cluster server, and `VfdwScanState` is zero-allocated (`src/vfdw_scan.c:633`). So off a cluster `redirect_passes` is 0, and a MOVED from a server *not* declared `cluster 'true'` surfaces as that same "gave up following redirects" error on the first retry rather than being skipped. That is the safe direction, but it is not what the message describes.

A second consequence, which follows from the code and is not pinned by any test: because an unplaced key is kept for every node, each non-owner answers MOVED and each MOVED is appended to `redirected` unconditionally, so a key whose owner did answer with a value can be fetched again on the retry pass and emitted twice. Nothing de-duplicates server-sourced rows — the overlay seen-set is only populated when this transaction has written the table (`src/vfdw_scan_overlay.c:85`). The suite asserts that a healthy cluster leaves no key unowned, so the path is not exercised there.

**The write path treats a redirect completely differently.** `vfdw_flush_connect` (`src/vfdw_flush.c:542`) computes the single slot the transaction writes *before* taking any connection — a cross-slot transaction is refused whole, and refused before a connection can be left mid-conversation. It then routes to that slot's owner via `vfdw_get_connection_node`. If a MOVED comes back anyway, it is in the set of prefixes that prove the program never ran (`src/vfdw_flush.c:236`), so the transaction fails cleanly with "the Valkey write was refused" and the phase marked proven-clean. There is no retry and **no map invalidation on the write path**: `vfdw_cluster_invalidate` is called only from `src/vfdw_scan_cluster.c`. A stale map that misroutes a write fails that transaction and stays stale until a scan corrects it.

The cross-slot check itself (`vfdw_flush_cluster_slot`, `src/vfdw_flush_cluster.c:105`) walks every op and checks `key`, `oldkey` *and* `keyset`. Checking only `key` would let a rename or a keyset-index write through as single-slot, and the server would then answer CROSSSLOT from *inside* the program, after the writes before it had applied — partial application arrived at by the exact route the check exists to close. Mutation `P4-xslot` removes it. The refusal names both keys through `vfdw_safe_text` (invariant I2: a bytea key column carries arbitrary bytes) and the hint names hash tags, because a refusal a user cannot act on is a wall rather than a design.

### Datum to bytes

`src/vfdw_val.c` is the exact inverse of `src/vfdw_row.c`, and has the same two branches on the same flag. `vfdw_val_render_scratch` (`src/vfdw_val.c:155`) detoasts when the type is varlena, then either takes `VARDATA_ANY`/`VARSIZE_ANY_EXHDR` for a binary column or calls the type's output function and `strlen`s the cstring it returns.

**There is deliberately no per-type table and no supported-type whitelist.** A whitelist would have to be maintained, would refuse user-defined types for no reason, and would be a second place that could disagree with the read path about a type's wire form. The out/in function pair is the type's own definition of its text representation; committing to it in one direction and to a table in the other is what breaks a round trip.

`is_binary` means "the base type is `bytea`" and is resolved once per column from the base type, so a domain over bytea is binary in both directions (`src/vfdw_map.c:541`). With `atttypid` alone it would be non-binary, and its bytes would come back in through `pg_verifymbstr` and `bytea_in`.

Details that are load-bearing rather than incidental:

- The four `strlen` calls in this file are not I3 violations. I3 forbids `strlen` on Valkey-originated bytes; these are on an output function's cstring and on catalog option strings. The consequence is real: a text-typed write can never carry an interior NUL, so only the bytea branch has to be binary-safe — a fact about PostgreSQL types, not a check performed anywhere.
- Only `VARDATA_ANY`/`VARSIZE_ANY_EXHDR` appear. A bytea of 126 bytes or fewer arrives with a 1-byte varlena header, so `VARSIZE - VARHDRSZ` is wrong by 3 on every small value. Detoasting is likewise mandatory — `VARDATA_ANY` on an undetoasted TOAST pointer yields the 18-byte pointer struct.
- No encoding validation happens outbound, for any type. Outbound the Datum came through the type's input function or a server-side computation, so `pg_verifymbstr` would verify the server against its own encoding — a tautology costing a full scan of every byte written — and it would have to be skipped for bytea anyway. What follows is documented rather than prevented: two databases with different server encodings over one keyspace will disagree about the same bytes.
- `VfdwValue` signals NULL by the `isnull` flag and by nothing else (`src/vfdw_val.h:58`). A non-NULL empty value has `len = 0` and `data` may be any pointer including NULL. Reusing the pointer as the NULL signal — which the read side can afford — would collapse `''` into NULL, and the read path returns three distinct states (absent key, absent field, empty value) that the write side must not conflate.
- Values are rendered into scratch and only retained into the write buffer once accepted (`vfdw_val_retain`, `src/vfdw_val.c:132`). The buffer's byte counter is monotonic, so bytes retained for a value that was then refused would consume `write_max_bytes` the user never spent. Retention is per column, so a row refused by a *later* column's rule leaves the earlier ones behind and counted — required, not tolerated, or a `LOOP ... EXCEPTION` over failing rows would allocate without bound while the cap read as untouched. The modify callbacks render in refusal-cost order, key first (`src/vfdw_modify.c:484`).
- The retained copy is deliberately not NUL-terminated. A terminator would make `strlen` appear to work on a value that may contain NUL.
- `VFDW_MAX_ARG_BYTES` is 512 MiB, `proto-max-bulk-len`'s default, imposed client-side and never probed (`src/vfdw_val.h:38`). `write_max_bytes` does not subsume it: a single 600 MB value slips under a 1 GiB transaction cap and reaches the wire, where an over-long bulk argument is a *protocol* error that closes the connection — at pre-commit, classifiable only as 08007 for the whole transaction. The check is the difference between a statement error naming a column and an unknown-outcome COMMIT.

`vfdw_val_from_slot` (`src/vfdw_val.c:296`) applies the NULL rule each column role implies, and is exhaustive over `VfdwColKind` so that the next kind added to the enum cannot acquire accidental semantics. Key, value, member and score reject NULL with `ERRCODE_NOT_NULL_VIOLATION` and `errtablecol` attached, so an application already branching on that SQLSTATE needs no new case. A NULL *field* is meaningful rather than an error: no HSET argument on INSERT, an HDEL on UPDATE — the exact inverse of the read direction, where an absent field leaves the column NULL.

### Key construction and validation

`vfdw_val_key_classify` (`src/vfdw_val.c:455`) returns a verdict and `vfdw_val_build_key` reports it. They are separate because three of the five refusals share SQLSTATE 23514, so a test comparing only the SQLSTATE passes while the builder rejects the key through the wrong branch; one classifier feeds both the raising path and the diagnostic, so rule and name cannot drift.

Order matters and is commented at each step: singleton first, because `map->keyattno` may legitimately be `InvalidAttrNumber` on a singleton table and that is the path that would index `cols[-1]`; then the missing-key-column case, checked even though `vfdw_map_build` cannot produce that shape, because a SQL-callable probe reaches here; then NULL, so a row missing its key gets the specific error rather than a check violation that sends the user to the wrong option; then the rendered-bytes tests.

**The key is validated and passed through — never concatenated, stripped or rewritten.** The key column holds the complete Valkey key, prefix included. The read path stores the raw SCAN key and refines with the prefix at plan time, so a write that prepended would produce `str:str:a` for a table whose scan records `str:a`.

`vfdw_val_has_prefix` (`src/vfdw_val.c:391`) uses `memcmp` behind a length guard, never `strncmp`: a write-path key may come from a bytea column where `VARDATA_ANY` is not terminated. The prefix is compared as a literal byte string and never as a glob, because the read path escapes `*`, `?`, `[`, `]` and `\` before the prefix becomes a `SCAN MATCH` pattern (`vfdw_escape_glob`, `src/vfdw_plan.c:491`) — a glob-matching check here would accept keys the scan can never return. `vfdw_val_is_keyset` (`src/vfdw_val.c:407`) tests length equality first: without it, `kset2` would be refused as colliding with keyset `kset`, and that key is an ordinary row. The keyset is one exact name, not a namespace.

On a singleton table a NULL key column means "unspecified" and is filled with the option value; a non-NULL one must match byte for byte (`src/vfdw_val.c:430`). Accepting anything else would write the row to the singleton key regardless, so the user's value would be discarded while INSERT reported success and a later `WHERE k = 'other'` returned nothing.

### Scores

`src/vfdw_score.c` answers a different question from the rest of `vfdw_val` — not "what are this Datum's bytes" but "is this byte string a number Valkey will store" — and the answer is deliberately not "whatever the server would take".

The grammar (`vfdw_val_score_scan`, `src/vfdw_score.c:46`) is a strict *subset* of what every supported server accepts: optional sign, digits with an optional fractional part or a leading point, optional complete exponent. The accepted set has already changed once between server minor versions, when libc `strtod` gave way to fast_float and hex float syntax silently disappeared, so a validator tracking it would be version-conditional and still wrong on the next change. A subset is correct against every version and fails only in the safe direction. It walks `(bytes, len)`, so a NUL fails the grammar instead of hiding the rest of the string.

Classification runs on the **rendered bytes, never the column's PostgreSQL type** (`vfdw_val_score_classify`, `src/vfdw_score.c:152`). A score column may be float8, float4, numeric or text; an `isnan()`/`isinf()` test on a float8 Datum covers one of those and leaves `'NaN'::numeric` and `'inf'::text` for the server to reject in phase 2 of the flush — after phase 1 passed and after earlier actions ran, which is partial application. `strtod` is only reached once the grammar has excluded every byte it could stop on, which is what makes terminating a copy and handing it to a C API safe there and only there. `ERANGE` alone is not a rejection: glibc reports it for subnormals too and `1e-320` is a good score, so the errno is paired with the value the way the server's `string2d` does (`src/vfdw_score.c:188`).

Infinity has its own verdict (`VFDW_SCORE_NOT_FINITE`) and its own detection (`src/vfdw_score.c:114`), matched case-insensitively over ASCII only — a locale-aware `tolower` would make the verdict depend on `LC_CTYPE`. It is worth the separate verdict because it is the one value both ends produce and only one end accepts: Valkey stores `+inf` and the read path hands it back as `Infinity` through float8 or `inf` through text, so `INSERT INTO z SELECT * FROM z` meets this refusal on a row the wrapper itself just returned. Calling that "not a number" sends the reader hunting a parse bug that is not there.

Refusing at DML time rather than letting ZADD answer is argued at `src/vfdw_score.c:277` and rests on two things. Atomicity: the score is an ARGV element consumed in phase 2, and no phase-1 opcode can express "this parses as a finite double" — the phase-1 verb set is EXISTS / TYPE / HEXISTS / SISMEMBER / ZSCORE / LPOS / SCARD / ZCARD / LLEN. Attribution: a DML-time refusal names the row, column and statement; a pre-commit one names none of them. The message must not claim Valkey refuses infinities — it does not; refusing them is this wrapper's policy, because the text does not survive the round trip and ZINCRBY on an infinite score produces NaN, which then cannot be stored. This is the only place where the read path accepts something the write path refuses, and it is recorded in README.md and pinned by the S-17/S-19 vectors. The offending text is echoed only when it is short and entirely printable ASCII (`src/vfdw_score.c:262`), since a score column can hold arbitrary bytes and this error fires before they have been proved to be a number.

### Principal functions

| Function | File | Responsible for |
|---|---|---|
| `vfdw_cluster_map` | `src/vfdw_cluster.c:358` | The slot map for a connection's `(serverid, userid)`, loading on first use or after invalidation |
| `vfdw_cluster_load` | `src/vfdw_cluster.c:255` | One `CLUSTER SHARDS` round trip, parsed in a scratch context |
| `vfdw_cluster_install` | `src/vfdw_cluster.c:211` | Copying a map that parsed whole into the backend-lifetime context, hosts and all |
| `vfdw_cluster_invalidate` | `src/vfdw_cluster.c:374` | Dropping the cached map on MOVED; never frees the retired one |
| `vfdw_cluster_route` | `src/vfdw_cluster.c:404` | The node owning a key's slot, or NULL when nothing claims it |
| `vfdw_cluster_is_redirect` / `vfdw_cluster_redirect_is_ask` | `src/vfdw_cluster.c:449`, `:460` | Telling a redirect from an ordinary error reply, and MOVED from ASK |
| `vfdw_val_slot` | `src/vfdw_slot.c:123` | CRC-16 of the hash tag, masked to 14 bits |
| `vfdw_val_hashtag` | `src/vfdw_slot.c:82` | The substring the slot is computed over, matching `keyHashSlot` exactly |
| `vfdw_val_crc16` | `src/vfdw_slot.c:41` | CRC-16/XMODEM, bitwise, unsigned-byte-safe |
| `vfdw_scan_cluster_setup` | `src/vfdw_scan_cluster.c:97` | Deciding fan-out, copying the node list, setting the redirect budget, attaching node 0 |
| `vfdw_scan_cluster_attach` | `src/vfdw_scan_cluster.c:143` | Pointing the scan at one node: release, reconnect, fresh batch, cursor reset |
| `vfdw_scan_cluster_advance` | `src/vfdw_scan_cluster.c:189` | The only thing that may end a keyspace scan on a cluster (I5) |
| `vfdw_scan_cluster_keys_for_node` | `src/vfdw_scan_cluster.c:209` | Partitioning plan-named keys to the current primary; keeps unplaced keys |
| `vfdw_scan_redirected` | `src/vfdw_scan_cluster.c:259` | Recording a moved key and invalidating the map on MOVED only |
| `vfdw_scan_retry_pass` | `src/vfdw_scan_cluster.c:292` | Starting a pass over redirected keys, or raising when the budget is spent |
| `vfdw_scan_keys_page` | `src/vfdw_scan_cluster.c:364` | One page for keys the plan named, node-aware |
| `vfdw_flush_cluster_slot` | `src/vfdw_flush_cluster.c:105` | The one slot a transaction writes, or the cross-slot refusal |
| `vfdw_flush_connect` | `src/vfdw_flush.c:542` | Choosing the primary the pre-commit program runs on |
| `vfdw_get_connection_node` | `src/vfdw_conn.c:534` | A pooled connection to a named cluster endpoint |
| `vfdw_val_render` / `vfdw_val_from_slot` | `src/vfdw_val.c:187`, `:296` | Datum to bytes; the NULL rule each column role implies |
| `vfdw_val_key_classify` / `vfdw_val_build_key` | `src/vfdw_val.c:455`, `:612` | The key rules as a verdict, and reporting that verdict |
| `vfdw_val_score_classify` / `vfdw_val_check_score` | `src/vfdw_score.c:152`, `:310` | The score grammar, and refusing at DML time with attribution |
| `valkey_fdw_test_cluster_map` / `_route` / `_nodes` | `src/vfdw_testcluster.c` | The map, a routing decision and per-node reachability as SQL rows — routing has no output of its own |

---

## How this project is tested

Every build and every test runs through `scripts/harness.sh`. There is no supported host build — the Makefile says so in its own header (`Makefile:1-4`), and `make` is only ever invoked from inside a container that the harness started. The point of that rule is stated at the top of the harness itself (`scripts/harness.sh:3-8`): "green on my machine" and "green in CI" have to be the same statement, and they can only be the same statement if they are the same command.

```bash
./scripts/harness.sh images                  # build toolchain + server images first
./scripts/harness.sh up standalone           # bring a topology up
./scripts/harness.sh test                    # build, install, run this topology's suites
./scripts/harness.sh test --suite scan       # one suite
./scripts/harness.sh test --topology cluster
./scripts/harness.sh lint                    # structural limits and banned constructs
./scripts/harness.sh isolation               # two-session specs (standalone only)
./scripts/harness.sh tap                     # cancellation proofs (standalone only)
./scripts/harness.sh mutate                  # re-run the defect behind every closed row
./scripts/harness.sh down
```

`.github/workflows/ci.yml` contains no `make` and no `docker build` of its own: all eight jobs shell out to these same subcommands. That is what the container rule buys. A CI failure is reproducible by typing the command from the job's log, and a local pass is not a claim about a machine nobody else has.

### What a run actually consists of

`run_in_build` (`scripts/harness.sh:489`) starts one container from the build image, bind-mounts the repository at `/work`, joins it to the `vfdw-net` Docker network so the Valkey containers are reachable by name, and passes the topology, strictness, coverage and sanitiser settings in as environment. `docker/build-entrypoint.sh` runs first and brings up a private PostgreSQL on a Unix socket in `/tmp` before exec'ing whatever it was asked to run — a fresh cluster per invocation, so a run cannot inherit a previous run's catalogue.

Build, install and test are one container invocation and cannot be split (`scripts/harness.sh:556-560`): `make install` writes into the image's PostgreSQL directories, which are discarded when the container exits, so a second container would find no extension installed. The build image hands those directories to the invoking UID at build time (`docker/Dockerfile.build:96-98`) precisely so that `make install` needs no `sudo`, and object files land in the bind mount owned by the host user rather than by root.

The entrypoint starts the server with `-c max_prepared_transactions=4` rather than appending to `postgresql.conf` (`docker/build-entrypoint.sh:31-41`), because it reuses an existing `$PGDATA` and a post-`initdb` append would never reach it. The `wbuf` suite needs a successful `PREPARE` of an *empty* transaction to catch an over-broad refusal in our own `PRE_PREPARE` handler; with the default of 0, core's refusal would mask ours either way.

### Topologies, and which suites mean something on which

`suites_for_topology` (`scripts/harness.sh:84`) is the mapping, and `cmd_test` consults it whenever `--suite` is not given.

| Topology | Server | Suites | Why not the others |
|---|---|---|---|
| `standalone` | plain TCP 6379 | `smoke probe options ddl mapping scan io pool leak val wbuf modify script dml overlay priv` | — |
| `tls` | TLS only, plain port disabled | `tls probe_tls` | The plain port is off entirely (`docker/valkey/entrypoint.sh:45-62`), so a suite that connects in cleartext must fail to connect rather than quietly succeed. `smoke` now really scans, so it needs plaintext and is not listed. |
| `acl` | `--aclfile`, `user default off` | `acl probe_acl` | With the default user disabled, nothing that connects without credentials runs here — `smoke` included. |
| `fault` | server behind a Python proxy | `smoke fault wfault resp` | The proxy answers to the alias `valkey`; the upstream server answers to `valkey-upstream`. |
| `cluster` | six nodes, three primaries | `smoke cluster` | Formed by the harness with `--cluster-replicas 1`; node 1 also answers to `valkey`. |
| `search` | `valkey-bundle` image | `smoke vsearch` | The bundle *ships* `/usr/lib/valkey/*.so` and loads nothing, so the entrypoint passes `--loadmodule` explicitly (`docker/valkey/entrypoint.sh:29-37`). |

Running a standalone suite against `tls`, `acl` or `cluster` fails for reasons that have nothing to do with the code under test, and the comment at `scripts/harness.sh:101-107` is explicit that pretending otherwise would make a green CI mean less. `priv` runs last on standalone because it drops and re-creates `valkey_fdw_test` to assert the split between the two extensions; anything after it would run against a catalogue the suite briefly emptied.

Three mechanisms keep one run from leaking into the next. `rm_all_servers` (`:259`) deletes every server container the harness owns before any topology starts, because topologies share network aliases: a leftover container competes for a name the new topology needs, Docker's DNS alternates between them, and roughly half the connections land somewhere they were never meant to. `topology_stamp` (`:294`) records not just the topology name but a SHA-1 of `harness.sh` itself, so editing how a topology is built invalidates the running one — otherwise a fix appears not to work because the old containers are still up. And `reset_valkey` (`:571`) issues `FLUSHALL` before each run, giving the keyspace the same fresh start `pg_regress` gives the database; it is best-effort and says so when it cannot reach the server.

Readiness is per-topology too. `wait_ready` builds its probe from `valkey_cli_args` (`:276`), because an unauthenticated `PING` against the ACL topology is answered `NOAUTH` and a naive probe would time out on a server that came up perfectly. The TLS topology also starts a second node serving a certificate that expired a month ago; its readiness check passes `--insecure`, since verifying it is exactly what the suite must fail at.

### The cluster topology is disposable

`topology_is_disposable` (`scripts/harness.sh:603`) returns true for `cluster` alone, and `cmd_test`, `cmd_record` and `cmd_spike` all re-form the cluster rather than reusing a matching stamp. The reason is that `FLUSHALL` empties a keyspace but does not move a slot back: `cluster.sql` performs a real four-step slot migration (`CLUSTER SETSLOT ... IMPORTING`/`MIGRATING`, `MIGRATE`, `SETSLOT ... NODE`) and migrates it back, so its fixture is the topology and not only the keys in it. A run that dies between those points leaves the slot where it was moved to. The comment records what that cost: a bisect across four commits produced a clean, plausible culprit, and every one of those runs was measuring the previous run's slot map. Re-forming showed the same assertion failing on master. One cluster formation per run buys the ability to believe the result.

### Stale images and stale objects

Two classes of failure look exactly like a code defect and are not.

**A container older than the files that built it.** Each image is stamped at build time with `sha256` over the files that went into it (`image_sources`/`src_sha`, `scripts/harness.sh:163-176`), and `check_image_fresh` (`:180`) compares that label against the working tree before anything runs, naming the inputs and the command to fix it. An image with no stamp counts as stale, on the principle that "I cannot tell" and "it matches" must not be the same answer. Both directions are covered: `cmd_images` deletes the topology stamp when it finishes (`:237`), so rebuilt images cannot be represented by containers that predate them. The header records the two incidents that motivated it — a `max_prepared_transactions` change baked into the build image that made `wbuf` fail with "prepared transactions are disabled", and a fault proxy whose container was older than its script, which answered `+OK` to `arm no_resp3`, incremented the fired counter, and forwarded the command untouched. Freshness is only checked for images that exist; a missing one is caught separately by the explicit `docker image inspect` in `cmd_up` and `run_in_build`.

**Objects from another PostgreSQL major.** The build is a bind mount, so `src/*.o` outlive the container, and nothing in a plain `make` knows which major produced them. Running `--pg 17` then `--pg 18` with no source change leaves make with objects newer than every source: it skips compiling and links PG 17 objects into a PG 18 library, which fails at `CREATE EXTENSION` with an undefined symbol — or, for a symbol present in both, does not fail at all and quietly tests the wrong thing. `ensure_major_objects` (`:468`) keeps a stamp in `.harness/built_pg` and deletes every `.o`, `.bc`, `.d`, `.so`, `.gcno` and `.gcda` when it changes. `maybe_clean` (`:519`) does the same for the wider build configuration (`pg:vendored:sanitize:coverage`) via `.harness/last-pg`. `harness.sh clean` is the manual form, and it re-checks and removes anything `make clean` missed.

Header dependencies are the third instance of the same shape. PGXS generates none, so `PG_CFLAGS += -MMD -MP` and `-include $(SRCS:.c=.d)` are added by hand (`Makefile:102-155`). The comment names the failure: adding two fields to `VfdwWriteOp` left `vfdw_testwbuf.o` stale, and the ledger's first run died with "invalid string enlargement request size: -1392226208" in code that was correct.

### Expected output is recorded, never written

`test/regress/expected/*.out` is generated from a real run by `cmd_record` (`scripts/harness.sh:802`), which brings the topology up, runs `make installcheck`, and copies `test/regress/results/*.out` over the expected files. The `results/` directory is gitignored, so the only artefact a reviewer sees is the diff against `expected/`, which is where the review has to happen — `cmd_record` prints a warning saying so and runs `installcheck` with `|| true`, since a failing run is exactly the case whose output you want to look at.

```bash
./scripts/harness.sh record --suite <name>   # then read the diff before committing
```

Suite files carry their reasoning in SQL comments, and those comments land in the expected output too, so a recorded `.out` diff shows changed prose as well as changed results.

### Structural lint, and the self-test that proves it can still fire

`harness.sh lint` runs `scripts/lint.sh --selftest` **first** and only then the gate itself (`scripts/harness.sh:868`). The order is the point: a banned-construct pattern that has stopped matching reports the tree clean, and a clean report from a gate that never searched is indistinguishable from a tree that really is clean.

The gate enforces 800 lines per file and 60 lines per function (`scripts/lint.sh:182-213`, relying on the PostgreSQL brace style: `{` and `}` in column 1), and refuses a table of constructs each of which maps to a specific defect (`:38-43`): `atoi`, `sprintf`, `strcat`, and any `errmsg`/`errdetail`/`errhint`/`errcontext` whose first argument is not a literal — invariant I2 is about server bytes reaching *any* of them, not only the message. `HASH_ENTER` with a NULL `foundPtr` is checked on joined statements rather than lines (`:64`), because that one is a relationship between two arguments of a call that routinely wraps, and a line-at-a-time pattern would pass its fixture and miss the code.

Three details are worth knowing before editing this file:

- Whitespace is spelled `[[:space:]]`, never `\s`, so the gate gives the same answer under every grep the project builds with.
- `grep` has three exit statuses and only two are answers. `scan_banned` (`:98`) reads the status into a variable and treats `>1` as a broken pattern, because `if grep ...` puts "found nothing" and "could not look" in the same branch — which is how three patterns spent their whole life rejected for an unterminated group while the gate passed every build.
- `selftest_banned` (`:124`) runs the same loop over `test/lint/fixtures/banned.c`, which holds exactly one instance of every banned construct plus all four spellings of the wrapped `HASH_ENTER` call, and fails if any pattern matches *nothing* there. The fixture is never compiled — `SRCS` is `$(wildcard src/*.c)` — and its header asks that it not be repaired into something that builds.

Two further gates live here. Shell-out fixtures are frozen: `COPY ... FROM PROGRAM` is refused in any new suite, with seven grandfathered files listed in `ALLOWED_PROGRAM_FIXTURES` (`:226`); comments are stripped before matching and the match is case- and whitespace-insensitive, because both spellings were once a way through in opposite directions. And invariant W1 is checked as a *count* rather than a presence (`:272-305`): each of the eleven write verbs must appear as a literal `server.pcall('VERB'` in `src/vfdw_script.c`, phase 2 must appear after phase 1, and phase 1 must contain no `server.pcall` at all. A script that took its verb from `ARGV` would work identically and make the gate vacuous.

### The mutation manifest

`scripts/mutate.py` holds fourteen entries, each naming a closed defect, the file, the exact text to replace, the replacement, and the topology and suite that must go red (`scripts/mutate.py:45`). `harness.sh mutate` applies each in turn and **reports a mutation that leaves the suite green as a failure of the check**, because that is the whole signal: the assertion behind that row no longer asserts it.

The rule behind it is that a defect is closed when a test names it, not when someone believes it is fixed — and the script exists because that rule holds at the moment a row is closed and then decays. The script's own header records three rows found stale in one session (G9, G12, U3), and four assertions in this tree that could not fail at all when checked; two of those four were written by someone who already knew about the trap. Reading a test is not how this gets found.

Three safeguards inside the runner matter more than they look:

- The search text must appear **exactly once** (`:171`). A pattern that stopped matching after a refactor would otherwise report the row as passing while never applying the mutation — the same silent failure, one level up.
- A build failure is not a caught mutation (`:150`). An earlier runner read "did not exit 0" as "the test caught it", which counted broken builds as proof and made every mutation look successful.
- After restoring the file, its mtime is bumped with `os.utime` (`:187`), or make skips the rebuild, the mutated library stays installed, and the next clean run reads as a real defect. The script also rebuilds from unmutated source before exiting.

### Documentation drift is a test

`test/unit/run.sh` runs at the end of every `harness.sh test`, on every topology (`scripts/harness.sh:790`). It builds its own database and asserts, in both directions, that the README option table and `valkey_fdw_options()` agree: no option missing from the README (`:39-51`), no README row naming an option the code does not accept (`:57-69`), no sensitive option carrying a default value in the SRF (`:76-82`), and the README's Superuser column matching `requires_superuser` both ways (`:98-109`). It also checks that `valkey_fdw_version()` agrees with `default_version` in `valkey_fdw.control`. Adding an option means editing the README table in the same change.

The same file carries the value-layer vector tables — CRC-16/XMODEM against the published check value, hash-tag extraction against every brace shape, 21 slot vectors, the score grammar's accept/refuse list, the key builder's prefix rules, and the toast/short-header cases that no test built from literals can reach, since a literal is never toasted. Two conventions here are load-bearing. `check()` (`:140`) runs with `ON_ERROR_STOP=1` and inspects the exit status, because a query that fails to parse returns no rows, which is indistinguishable from a query whose vectors all passed. And `skip()` (`:21`) prints a visible *skip*, never "ok": the cross-check against the server's own `CLUSTER KEYSLOT` cannot run on standalone (cluster support disabled) or on tls/acl (no credentials), and when it printed "ok" on that branch the one comparison against the server's arithmetic executed on no topology this project runs while reporting success on all of them. It is now a hard failure if it does not run on the `cluster` topology, where the server can answer.

### The diagnostics are a separate, superuser-only extension

One shared library, two sets of catalogue entries. `MODULE_big` is `valkey_fdw` and `EXTENSION` is both names (`Makefile:11-15`). `valkey_fdw--0.1.sql` creates five functions, none of which reaches a Valkey server by itself; every `valkey_fdw_test_*` entry point lives in `valkey_fdw_test--0.1.sql`, whose control file sets `superuser = true` explicitly and points `module_pathname` at `$libdir/valkey_fdw`.

The reason is stated in both files: PostgreSQL grants `EXECUTE` on a new function to `PUBLIC`, and every diagnostic reaches a server — most through the pool, but `valkey_fdw_test_ping`, `_binary` and `_block` take a host and a port from their caller, which is the postmaster's network reach aimed by any role that can log in. Compiling them out behind a build flag was considered and rejected, because the object a user installs would then not be the object the suites ran against. Only the catalogue exposure moves. Each `CREATE FUNCTION` is still followed by a `REVOKE ALL ... FROM PUBLIC`, since the control file governs who may *install* and the grants govern who may *call*.

If you are writing a new suite, that split imposes five things:

1. **`pg_regress` loads both**, via two separate `--load-extension` flags in dependency order (`Makefile:34`). One flag per extension, spelled out rather than interpolated from `$(EXTENSION)`: PGXS expands the variable unquoted and pg_regress would read the second word as a test name.
2. **A harness that builds its own database must name both itself** — `test/unit/run.sh:25-29`, `test/tap/t/001_cancel.pl:33-35`, and the bench scripts all do.
3. **Assert the keyspace through `valkey_fdw_test_probe`, not a shell-out.** The probe goes through the pool, so it inherits AUTH, TLS, the logical database and the timeouts, and works on tls, acl and cluster where a plaintext `valkey-cli` cannot connect or connects as nobody; its arguments are a `bytea` vector, so a key containing a space, quote, newline or NUL is ordinary; and a failure is a failure, where a Valkey error arrives on a shell's stdout with exit status 0. The lint gate enforces this for new files.
4. **A probe round trip on its own proves nothing about the transport.** The probe shares `vfdw_conn.c`, `vfdw_cmd.c`, `vfdw_io.c` and `vfdw_error.c` with the code under test, so a defect confined to those four is invisible *symmetrically* — a bug that truncates an outgoing argument truncates it identically for the probe's write and its read. `src/vfdw_testprobe.c` states the rule that follows: every keyspace assertion must include at least one number the server computed and the probe cannot echo (`STRLEN`, `HLEN`, `SCARD`, `ZCARD`, `LLEN`, `EXISTS`, `TYPE`, `LPOS`). The transport itself is tested by `io.sql` and `fault.sql` through `valkey_fdw_test_ping` and `_binary`.
5. **Know which error contract you are calling.** `valkey_fdw_test_probe` returns an error reply as data; its sibling `valkey_fdw_test_keys` raises on one. The divergence is deliberate — a key dump has no row shape for an error, and a row of NULLs would be indistinguishable from an empty keyspace — and it is asserted in `probe.sql` and `probe_acl.sql`.

`priv.sql` is what keeps the split honest. It queries `pg_depend` for any `valkey_fdw_test_%` function whose extension is not `valkey_fdw_test` (the check that fails the day an entry point is added to the wrong file), then drops the diagnostics extension, confirms the probes become undefined while a scan through the wrapper still works, and creates it again.

### The rest of the harness

`isolation` and `tap` are standalone-only and deliberately not part of `test` (`scripts/harness.sh:731`, `:688`). Their properties — two concurrent writers, and a query cancelled from another session — are about semantics and interrupt handling rather than transport, so running them per topology would be five repeats of one answer plus five failures. `ISOLATION` and `TAP_TESTS` are therefore set on the `make` command line and never in the Makefile (`Makefile:120-147`), or PGXS would run them on every `installcheck` across all six topologies. `cmd_tap` counts the `.pl` files before running, because `prove` exits 0 when it finds nothing.

`bench` prints numbers for a human to read and is kept out of `test` on the grounds that a benchmark whose result is "it did not crash" is a slow test, not a measurement; `test/bench/flush_latency.sh` explains that `write_max_ops` is a blast radius rather than a throughput knob, since a Lua script that has already written cannot be killed. `spike` compiles the small C programs under `test/spike/` against libvalkey with no PostgreSQL at all, so a failure points squarely at the client library. `coverage-report` runs `gcovr` over a `--coverage` build with a line-coverage floor.

`harness.sh ci` sweeps three PostgreSQL majors × two Valkey versions × six topologies locally; GitHub Actions splits the same work differently and adds jobs `cmd_ci` does not run — sanitisers, the mutation manifest, isolation and TAP, the `--cassert` server, the vendored-libvalkey link, and coverage.

The `--cassert` image (`docker/Dockerfile.cassert`) is worth its twenty-minute build for one reason, given in its header: `LD_PRELOAD`-ing ASan into the packaged postmaster both hangs and would not see the bugs that matter, because palloc carves chunks out of blocks ASan considers live — and the ledger points into the write buffer while the overlay points into both. `CLOBBER_FREED_MEMORY`, `MEMORY_CONTEXT_CHECKING` and `Assert()` are compile-time and arrive only with `--enable-cassert`. Its OS user is named `vfdw` to match the default image, because the name appears in `DROP CASCADE` messages and a different one would diff every suite that drops a server.

### Principal functions

| Function | File | Responsibility |
|---|---|---|
| `suites_for_topology` | `scripts/harness.sh:84` | The only place that says which regression suites are meaningful on which topology |
| `check_image_fresh` / `image_sources` | `scripts/harness.sh:180` / `:163` | Refuse to run when an image was built from different sources than the working tree |
| `ensure_major_objects` | `scripts/harness.sh:468` | Delete build products when the PostgreSQL major (or `--cassert`) changes, so objects never cross majors |
| `maybe_clean` | `scripts/harness.sh:519` | The same for the wider build configuration: vendored, sanitiser, coverage |
| `run_in_build` | `scripts/harness.sh:489` | Start the build container with the repo bind-mounted, the network joined and the run's settings in the environment |
| `rm_all_servers` | `scripts/harness.sh:259` | Delete every server container the harness owns before a topology starts, so aliases cannot collide |
| `topology_stamp` | `scripts/harness.sh:294` | Decide whether a running topology may be reused — name plus a hash of the harness itself |
| `topology_is_disposable` | `scripts/harness.sh:603` | Force a rebuild of the cluster, whose fixture is the topology rather than the keyspace |
| `reset_valkey` | `scripts/harness.sh:571` | `FLUSHALL` before a run, so a suite measures its own fixture and not history |
| `cmd_record` | `scripts/harness.sh:802` | Regenerate `test/regress/expected/*.out` from a real run, for review as a diff |
| `cmd_lint` | `scripts/harness.sh:868` | Run the lint self-test first and treat its failure as the command's failure |
| `scan_banned` / `scan_hash_enter` | `scripts/lint.sh:98` / `:64` | The banned-construct gate, and the joined-statement `HASH_ENTER` check that a line-based pattern cannot do |
| `selftest_banned` | `scripts/lint.sh:124` | Prove every pattern still matches its fixture before the gate's verdict on `src/` is worth reading |
| `run_suite` (mutate) | `scripts/mutate.py:141` | Run one suite under one mutation and classify RED / GREEN / BUILD-FAILED |
| `main` (mutate) | `scripts/mutate.py:155` | Apply each manifest entry exactly once, restore the file, bump its mtime, and fail on any mutation that stayed green |
| `check` / `skip` | `test/unit/run.sh:140` / `:21` | Assert through psql with `ON_ERROR_STOP`, and make a check that did not run visibly different from one that passed |

---

## Known documentation drift

Comments here are load-bearing, and a comment that gives the wrong reason is
treated as a defect rather than as untidiness — one such row recorded in
`scripts/mutate.py`'s header was exactly that, a justification describing a
different function. A full read of the source turned up around thirty such
places. They have been corrected, and this section is kept for the pattern
rather than the list, because the pattern is what will produce the next one.

**Splitting a file orphans its comments.** Most of the drift was a block left
above whichever function ended up beneath it after `vfdw_conn.c`,
`vfdw_render.c` and `vfdw_flush.c` were divided. The block still read
plausibly; it simply described something else, sometimes in another file. When
you move code, move the comment above it, and check what is left behind.

**A header outlives the state it describes.** `vfdw_flush.h` called the flush a
stub long after it applied a whole transaction, and `vfdw_refuse.h` said
`BeginForeignInsert` was unregistered after it had been registered. A header
that explains what a file does not do yet has to be revisited on the commit
that makes it do it.

**A mechanism can be named without existing.** Invariant I1 was stated in terms
of a `ResourceOwner` releasing connections on abort. The guarantee held, but by
`RegisterXactCallback` and a memory context reset callback; no ResourceOwner
API appears anywhere in this tree. The wording had propagated to five source
files, and each copy made the next look corroborated. When an invariant names a
mechanism, grep for the mechanism.

**Counts go stale silently.** A comment saying "five files" beside a list of
seven, an extension description naming thirty-eight functions where the script
creates thirty-nine, a suite count off by one. None of it breaks anything,
which is why none of it was noticed. Where a number can be derived, derive it:
`test/unit/run.sh` compares the README option table against
`valkey_fdw_options()` in both directions precisely so that pair cannot drift.

**A field can be written for a future that has not arrived.**
`VfdwSlotMap.generation`, `VfdwWriteUnit.hashslot`, `VfdwScanState.umid` and
`vfdw_cluster_covered` were each assigned, documented, and read by nothing.
They have been removed. `VfdwWriteOp.hashslot` was kept, because the test probe
does read it and `modify.sql` asserts it — which is the distinction worth
drawing before deleting anything that looks dead.

### What is still true and worth knowing

Some comments record **measurements rather than properties**: that removing a
guard changed no suite's output, that a duplicate row was once observed, that a
cluster formation fails roughly one run in six. They are consistent with the
code, but they are records of experiments and cannot be re-derived by reading.
Treat them as evidence with a date on it rather than as invariants.

## Where to look first

| If you are changing | Read first |
|---|---|
| Anything that talks to a server | `src/vfdw_io.h`, `src/vfdw_cmd.h`, then I1 and I6 above |
| An option | `src/vfdw_option.c`'s table, then the README table — `test/unit/run.sh` asserts they agree, in both directions |
| The read path | `src/vfdw_scan_internal.h`, then I5 |
| The write path | `src/vfdw_wbuf.h`, then `src/vfdw_ledger.h` and `src/vfdw_flush.h`, and W3 above — which is what none of them may break |
| The Lua program | `src/vfdw_script.h` for W1, and `test/regress/sql/script.sql` for the golden vector |
| Anything on a cluster | `src/vfdw_cluster.h` for I8, and note that the `cluster` topology re-forms per run |
| A test helper | `sql/valkey_fdw_test--0.1.sql` — it is a separate, superuser-only extension |

And two rules that are not in any header:

**A defect is closed when a test names it, not when someone believes it is
fixed.** When you close one, add its mutation to `scripts/mutate.py`: the edit
that reintroduces the defect, and the suite that must go red. A mutation that
leaves the suite green is reported as a failure of the check, and that is the
only thing that has ever caught an assertion which quietly stopped asserting.

**Expected output is recorded, never written.** `./scripts/harness.sh record
--suite <name>`, then read the diff before committing it. A hand-written
expected file is a test that asserts whatever its author assumed.
