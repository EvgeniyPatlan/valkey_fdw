# valkey_fdw - foreign data wrapper for Valkey
#
# All supported builds run inside the containers described in docker/.
# Building on a host toolchain is unsupported; use scripts/harness.sh.

# Two extensions, one shared library. valkey_fdw_test carries the diagnostic
# functions and nothing else, so that an install which will never run the
# suites can leave them out; see valkey_fdw_test.control for why that matters.
# Both .control files live at the top of the tree because that is the only
# place PGXS looks for them.
MODULE_big = valkey_fdw
EXTENSION  = valkey_fdw valkey_fdw_test
PGFILEDESC = "valkey_fdw - foreign data wrapper for Valkey"

DATA = sql/valkey_fdw--0.1.sql sql/valkey_fdw_test--0.1.sql

SRCS = $(sort $(wildcard src/*.c))
OBJS = $(SRCS:.c=.o)

# Regression suites. Each is test/regress/sql/<name>.sql with expected output
# in test/regress/expected/<name>.out.
REGRESS = smoke probe options ddl mapping scan io pool leak val wbuf modify script dml overlay tls probe_tls acl probe_acl wfault resp cluster vsearch priv
#
# Both extension names are written out rather than interpolated from
# $(EXTENSION). PGXS expands this variable into the pg_regress command line
# unquoted, so --load-extension=$(EXTENSION) collapses to
# "--load-extension=valkey_fdw valkey_fdw_test" and pg_regress reads the second
# word as a TEST NAME - it goes looking for test/regress/sql/valkey_fdw_test.sql
# and the whole run dies before the first suite. One flag per extension.
#
# The order is load-bearing: pg_regress issues one CREATE EXTENSION IF NOT
# EXISTS per flag, in the order given and without CASCADE, and valkey_fdw_test
# requires valkey_fdw.
REGRESS_OPTS = --inputdir=test/regress --outputdir=test/regress \
               --load-extension=valkey_fdw --load-extension=valkey_fdw_test

# ---------------------------------------------------------------------------
# libvalkey linkage.
#
# VALKEY_VENDORED=0 (default) links the system library; =1 links a pinned
# build from vendor/libvalkey. CI exercises both so neither path rots.
# ---------------------------------------------------------------------------
VALKEY_VENDORED ?= 0

ifeq ($(VALKEY_VENDORED),1)
LIBVALKEY_DIR ?= vendor/libvalkey
PG_CPPFLAGS += -I$(abspath $(LIBVALKEY_DIR)/include)
# BOTH archives. TLS is a separate library in libvalkey - the comment in the
# system branch below says so, and this branch linked only the core one, so a
# vendored build resolved everything except valkeyInitiateTLS and failed at
# CREATE EXTENSION rather than at link time.
SHLIB_LINK  += $(abspath $(LIBVALKEY_DIR)/lib/libvalkey_tls.a) \
               $(abspath $(LIBVALKEY_DIR)/lib/libvalkey.a) -lssl -lcrypto
else
# The pkg-config modules are named valkey and valkey_tls - not libvalkey -
# and TLS lives in its own shared object. Getting the module name wrong is
# quiet: pkg-config fails, the fallback still links, and the missing symbol
# only surfaces when the server dlopens the module.
LIBVALKEY_CFLAGS := $(shell pkg-config --cflags valkey valkey_tls 2>/dev/null)
LIBVALKEY_LIBS   := $(shell pkg-config --libs valkey valkey_tls 2>/dev/null)
ifeq ($(strip $(LIBVALKEY_LIBS)),)
LIBVALKEY_LIBS   := -lvalkey -lvalkey_tls
endif
PG_CPPFLAGS += $(LIBVALKEY_CFLAGS)
SHLIB_LINK  += $(LIBVALKEY_LIBS) -lssl -lcrypto
endif

# ---------------------------------------------------------------------------
# Warnings.
#
# Signed/unsigned comparison stays on deliberately: mixing a signed row
# counter with a size_t element count is exactly the class of latent bug this
# project exists not to repeat. STRICT=1 promotes warnings to errors and is
# set on every CI job.
# ---------------------------------------------------------------------------
# -Wshadow is deliberately absent: PostgreSQL's own PG_TRY macros shadow their
# saved-state locals when nested, so plain -Wshadow cannot be satisfied
# against server headers. PGXS already supplies -Wshadow=compatible-local,
# which is what PostgreSQL builds itself with.
#
# -Wclobbered arrives with -Wextra and is kept: a local modified inside a
# PG_TRY and read afterwards is genuinely indeterminate after a longjmp unless
# it is volatile.
PG_CFLAGS += -Wall -Wextra -Wformat -Wformat-security \
             -Wmissing-prototypes -Wno-unused-parameter

ifeq ($(STRICT),1)
PG_CFLAGS += -Werror
endif

ifeq ($(COVERAGE),1)
PG_CFLAGS  += --coverage -O0
SHLIB_LINK += --coverage
endif

# SANITIZE=address,undefined
ifneq ($(SANITIZE),)
PG_CFLAGS  += -fsanitize=$(SANITIZE) -fno-omit-frame-pointer -g
SHLIB_LINK += -fsanitize=$(SANITIZE)
endif

# ---------------------------------------------------------------------------
# Header dependency tracking.
#
# PGXS generates none, and its .o rule depends on the .c alone. Edit a struct
# in a header and only the .c files that changed get recompiled: the rest keep
# objects built against the OLD layout, and they link without complaint. The
# result reads one field at another field's offset - a garbage length, a
# pointer where an int should be, a segfault a long way from the edit.
#
# That is not hypothetical. Adding two fields to VfdwWriteOp left
# vfdw_testwbuf.o stale, and the ledger's first suite run died with
# "invalid string enlargement request size: -1392226208" in code that was
# correct. Several rounds went into looking for a bug that was not there.
#
# -MMD writes a .d beside each .o; -MP adds phony targets so a deleted header
# does not wedge the build. Both are gcc and clang spellings of the same
# thing, and this project builds with each.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Isolation specs.
#
# ISOLATION is set from the command line by scripts/harness.sh, never here.
# Defining it unconditionally makes PGXS run the specs on every `make
# installcheck`, and harness.sh ci sweeps installcheck across six topologies -
# five of which have no plaintext server the specs can reach, so they would
# fail for reasons that have nothing to do with the code.
#
# It also must NOT appear in REGRESS: that list is interpolated into
# `make installcheck REGRESS='...'` and pg_regress would go looking for
# test/regress/sql/isolation.sql.
# ---------------------------------------------------------------------------
#
# One --load-extension per name, in dependency order, for the reason spelled
# out over REGRESS_OPTS: this variable reaches isolationtester's command line
# unquoted too, and a two-word expansion would be read as a spec name.
ISOLATION_OPTS = --inputdir=test/isolation --outputdir=test/isolation \
                 --load-extension=valkey_fdw --load-extension=valkey_fdw_test

# TAP tests, on the same terms as ISOLATION: TAP_TESTS is set from the command
# line by scripts/harness.sh and never here. PROVE_TESTS points at our tree,
# because PGXS's prove_installcheck defaults to ./t/*.pl and ours live under
# test/tap/. Going through PGXS rather than invoking prove directly is not
# ceremony - PostgreSQL::Test::Utils needs PG_REGRESS, TESTDIR and TESTLOGDIR
# in the environment, and prove_installcheck is what sets them. A hand-rolled
# prove run dies before it can even open a log to say why.
PROVE_TESTS ?= test/tap/t/*.pl

PG_CFLAGS += -MMD -MP

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

-include $(SRCS:.c=.d)

EXTRA_CLEAN = test/regress/results test/regress/regression.diffs \
              test/regress/regression.out $(SRCS:.c=.gcno) $(SRCS:.c=.gcda) \
              $(SRCS:.c=.d) test/isolation/output_iso \
              test/isolation/results test/isolation/regression.diffs \
              test/isolation/regression.out

.PHONY: lint
lint:
	@bash scripts/lint.sh
