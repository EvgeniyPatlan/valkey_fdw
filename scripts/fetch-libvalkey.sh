#!/usr/bin/env bash
#
# Build the pinned libvalkey into vendor/libvalkey, for VALKEY_VENDORED=1.
#
# WHY THIS EXISTS. The Makefile has always offered VALKEY_VENDORED=1 pointing
# at vendor/libvalkey, and its comment claimed "CI exercises both so neither
# path rots" - but nothing in this tree ever created that directory. There is
# no submodule and no fetch, so `harness.sh test --vendored` and the CI job of
# the same name compiled against an include path that did not exist and failed
# at exit 2. A build mode nothing can run is worse than one that is missing:
# it reads, in the Makefile and in CI, as a covered configuration.
#
# NOT A SUBMODULE, deliberately. The packaging builds already clone this tag
# inside their own containers, because a distro build cannot depend on how the
# source tree was checked out. Making the vendored path do the same thing
# means one mechanism and one pinned version rather than two that can drift -
# and it costs a clone that only the vendored build pays for.
#
# Idempotent: an existing build with the static library present is left alone,
# so repeat runs cost nothing.

set -euo pipefail

REF=${LIBVALKEY_REF:-0.5.0}
DIR=${LIBVALKEY_DIR:-vendor/libvalkey}
SRC="${DIR}/src"

if [ -f "${DIR}/lib/libvalkey.a" ]; then
    echo "libvalkey ${REF} already built in ${DIR}"
    exit 0
fi

mkdir -p "$(dirname "$DIR")"
rm -rf "$SRC"
git clone --depth 1 --branch "$REF" \
    https://github.com/valkey-io/libvalkey.git "$SRC"

# Static and position-independent: it is linked into a shared object, and a
# non-PIC archive fails to link into one on every architecture that matters.
cmake -S "$SRC" -B "$SRC/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_TLS=ON \
    -DDISABLE_TESTS=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INSTALL_PREFIX="$(cd "$(dirname "$DIR")" && pwd)/$(basename "$DIR")" \
    >/dev/null
cmake --build "$SRC/build" -j"$(nproc)" >/dev/null
cmake --install "$SRC/build" >/dev/null

# Some builds install to lib64; the Makefile names lib.
if [ ! -f "${DIR}/lib/libvalkey.a" ] && [ -f "${DIR}/lib64/libvalkey.a" ]; then
    mkdir -p "${DIR}/lib"
    cp "${DIR}/lib64/"libvalkey*.a "${DIR}/lib/"
fi

test -f "${DIR}/lib/libvalkey.a" \
    || { echo "libvalkey built but libvalkey.a is not where the Makefile looks" >&2; exit 1; }
echo "libvalkey ${REF} built in ${DIR}"
