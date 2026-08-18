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
# NOT A SUBMODULE, deliberately. A distro build cannot depend on how the source
# tree was checked out, so one mechanism and one pinned version serve every
# build here rather than two that can drift.
#
# A TARBALL FIRST, A CLONE ONLY AS A LAST RESORT. Cloning during a build needs
# the network, and the build systems distributions actually use - Koji, OBS,
# Debian's buildd - run offline from declared sources. So this looks for
# vendor/libvalkey-<ref>.tar.gz and builds from it without reaching anywhere;
# `--archive` is what produces that file, once, on a machine that does have
# the network. The rpm spec and the debian rules take the same tarball, so
# "can this be built offline" has one answer for all three.
#
# Idempotent: an existing build with the static library present is left alone,
# so repeat runs cost nothing.

set -euo pipefail

REF=${LIBVALKEY_REF:-0.5.0}
DIR=${LIBVALKEY_DIR:-vendor/libvalkey}
SRC="${DIR}/src"
TARBALL=${LIBVALKEY_TARBALL:-vendor/libvalkey-${REF}.tar.gz}

# --archive: produce the tarball and stop. The one step that needs the network.
if [ "${1:-}" = "--archive" ]; then
    mkdir -p "$(dirname "$TARBALL")"
    if [ -f "$TARBALL" ]; then
        echo "libvalkey ${REF} source already at ${TARBALL}"
        exit 0
    fi
    tmp="$(mktemp -d)"
    git clone --depth 1 --branch "$REF" \
        https://github.com/valkey-io/libvalkey.git "${tmp}/libvalkey-${REF}"
    rm -rf "${tmp}/libvalkey-${REF}/.git"
    tar -czf "$TARBALL" -C "$tmp" "libvalkey-${REF}"
    rm -rf "$tmp"
    echo "libvalkey ${REF} source written to ${TARBALL}"
    exit 0
fi

if [ -f "${DIR}/lib/libvalkey.a" ]; then
    echo "libvalkey ${REF} already built in ${DIR}"
    exit 0
fi

mkdir -p "$(dirname "$DIR")"
rm -rf "$SRC"

if [ -f "$TARBALL" ]; then
    # The offline path, and the one every packaging build takes.
    mkdir -p "$SRC"
    tar -xzf "$TARBALL" -C "$SRC" --strip-components=1
    echo "libvalkey ${REF} unpacked from ${TARBALL}"
else
    echo "no ${TARBALL}; cloning (this needs the network - see --archive)" >&2
    git clone --depth 1 --branch "$REF" \
        https://github.com/valkey-io/libvalkey.git "$SRC"
fi

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
