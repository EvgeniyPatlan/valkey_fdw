#!/usr/bin/env bash
#
# Build distribution packages, in Docker, for every supported target.
#
#   ./scripts/package.sh                          every target, host arch
#   ./scripts/package.sh --target debian-13        one target
#   ./scripts/package.sh --arch arm64              cross-build via QEMU
#   ./scripts/package.sh --pg 16 --target el-9
#
# Output lands in dist/<target>/<arch>/.
#
# WHY EVERY BUILD IS IN A CONTAINER, like everything else here: a package
# built on a developer's machine links against that machine's libraries and
# nobody finds out until an install fails on a box nobody has. The image is
# the target, not an approximation of it.
#
# LIBVALKEY IS STATIC IN EVERY PACKAGE. No distribution in this matrix
# packages it, so a dynamic link would mean shipping a second package and
# pinning its soname eight times over. The static object is the pinned 0.5.0
# the suite tests against, which also removes "which libvalkey is on the box"
# as a variable in any bug report.

set -euo pipefail

PG_MAJOR=${PG_MAJOR:-17}
ARCH=${ARCH:-$(uname -m)}
VERSION=${VERSION:-0.2}
TARGETS_ALL="el-8 el-9 el-10 amazon-2023 debian-12 debian-13 ubuntu-24.04 ubuntu-26.04"
TARGETS=""

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target)  TARGETS="$2"; shift 2 ;;
        --arch)    ARCH="$2"; shift 2 ;;
        --pg)      PG_MAJOR="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        *)         die "unknown flag: $1" ;;
    esac
done
[[ -n "$TARGETS" ]] || TARGETS="$TARGETS_ALL"

# Normalise so --arch accepts what both Docker and uname call things.
case "$ARCH" in
    x86_64|amd64)   DOCKER_ARCH=linux/amd64; RPM_ARCH=x86_64; DEB_ARCH=amd64 ;;
    aarch64|arm64)  DOCKER_ARCH=linux/arm64; RPM_ARCH=aarch64; DEB_ARCH=arm64 ;;
    *)              die "unsupported arch: $ARCH (x86_64 or aarch64)" ;;
esac

# Cross-arch needs binfmt registered. Checked rather than assumed: without it
# the build fails deep inside the container with "exec format error", which
# reads like a broken image.
if [[ "$DOCKER_ARCH" != "linux/$(uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/')" ]]; then
    docker run --rm --privileged tonistiigi/binfmt --install all >/dev/null 2>&1 \
        || die "cross-building $ARCH needs binfmt; run: docker run --rm --privileged tonistiigi/binfmt --install all"
fi

base_for() {
    case "$1" in
        el-8)         echo oraclelinux:8 ;;
        el-9)         echo oraclelinux:9 ;;
        el-10)        echo oraclelinux:10 ;;
        amazon-2023)  echo amazonlinux:2023 ;;
        debian-12)    echo debian:12 ;;
        debian-13)    echo debian:13 ;;
        ubuntu-24.04) echo ubuntu:24.04 ;;
        ubuntu-26.04) echo ubuntu:26.04 ;;
        *)            die "unknown target: $1" ;;
    esac
}

# What a target calls PostgreSQL, for the targets that do not use PGDG's names.
#
# Only names chosen by a package manager live here. WHERE the files go is asked
# of pg_config by the spec itself, so this stays short and cannot drift from
# the layout it describes: it says what to install, not where it lands.
#
# Empty for every PGDG target, and the spec's own defaults cover those.
spec_overrides_for() {
    SPEC_PGCONFIG=""
    SPEC_PGDEVEL=""
    case "$1" in
        amazon-*)
            SPEC_PGCONFIG=/usr/bin/pg_config
            SPEC_PGDEVEL="postgresql${PG_MAJOR}-server-devel"
            ;;
    esac
}

family_for() {
    case "$1" in
        el-*|amazon-*) echo rpm ;;
        *)             echo deb ;;
    esac
}

el_for() {
    case "$1" in
        el-8) echo 8 ;; el-9) echo 9 ;; el-10) echo 10 ;; *) echo 9 ;;
    esac
}

# The libvalkey source, fetched once on the host so the package build itself
# never reaches the network.
#
# That is the whole point of the tarball: rpmbuild and dpkg-buildpackage below
# run the way a distribution's build system runs them, and those build offline
# from declared sources. Fetching here rather than there keeps the one step
# that needs the network outside the step that must not.
ensure_libvalkey_source() {
    local ref="${LIBVALKEY_REF:-0.5.0}"

    [[ -f "vendor/libvalkey-${ref}.tar.gz" ]] && return 0
    say "fetching libvalkey ${ref} source"
    LIBVALKEY_REF="$ref" bash scripts/fetch-libvalkey.sh --archive
}

# What a distribution never shipped, this cannot build.
#
# Named rather than left to fail, for the same reason the binfmt check above
# is: without it the build dies inside the container on a package that does
# not exist, hundreds of lines from anything that mentions a version, and
# reads like a broken image. The workflow excludes the same pair.
unsupported_reason() {
    case "$1:$PG_MAJOR" in
        amazon-2023:14)
            echo "Amazon Linux 2023 packages PostgreSQL itself rather than taking PGDG, and carries 15 through 18" ;;
        *) echo "" ;;
    esac
}

build_one() {
    local target="$1" family base image out why
    why="$(unsupported_reason "$target")"
    if [[ -n "$why" ]]; then
        die "pg${PG_MAJOR} is not available on ${target}: ${why}"
    fi
    family="$(family_for "$target")"
    base="$(base_for "$target")"
    image="valkey_fdw/pkg-${target}:pg${PG_MAJOR}"
    out="dist/${target}/${ARCH}"

    spec_overrides_for "$target"

    ensure_libvalkey_source

    say "building ${target} (${base}, ${DOCKER_ARCH}, pg${PG_MAJOR})"
    docker build --platform "$DOCKER_ARCH" \
        -f "packaging/Dockerfile.${family}" \
        --build-arg "BASE=${base}" \
        --build-arg "PG_MAJOR=${PG_MAJOR}" \
        --build-arg "EL=$(el_for "$target")" \
        -t "$image" . >/dev/null

    mkdir -p "$out"

    if [[ "$family" == rpm ]]; then
        docker run --rm --platform "$DOCKER_ARCH" \
            -v "$PWD:/src:ro" -v "$PWD/$out:/out" \
            -e "PG_MAJOR=${PG_MAJOR}" -e "VERSION=${VERSION}" \
            -e "SPEC_PGCONFIG=${SPEC_PGCONFIG}" -e "SPEC_PGDEVEL=${SPEC_PGDEVEL}" \
            "$image" bash -lc '
                set -eux
                name="valkey_fdw_${PG_MAJOR}-${VERSION}"
                mkdir -p /root/rpmbuild/{SOURCES,SPECS} /build/$name
                cp -a /src/. /build/$name/
                rm -rf /build/$name/dist /build/$name/.git
                # The source copy is a WORKING TREE, not a clean checkout.
                #
                # The harness builds through a bind mount, so src/*.o and
                # valkey_fdw.so outlive its container and sit in the directory
                # this copies. make then finds every object newer than its
                # source, compiles nothing, and the package ships the library
                # the HARNESS built - a Debian binary linked against a shared
                # libvalkey, inside an EL package that then requires two
                # sonames the target does not have. It installs nowhere.
                #
                # Invisible in CI, which checks out clean, and reproducible on
                # any machine where the suites have been run. Whatever the
                # package contains has to be built here.
                for pat in '*.o' '*.bc' '*.d' '*.so' '*.gcno' '*.gcda'; do
                    find /build -type f -name "$pat" -delete
                done
                rm -rf /build/*/.harness
                # The built libvalkey, not its source. Each packaging build
                # makes one from vendor/*.tar.gz into its own prefix, so
                # carrying a tree of objects from another build along only
                # invites one of them to be linked by accident.
                #
                # No apostrophes in here: this whole block is the body of a
                # bash -lc '...' and one would end the string.
                rm -rf /build/*/vendor/libvalkey
                tar -C /build -czf /root/rpmbuild/SOURCES/$name.tar.gz $name
                cp /src/packaging/rpm/valkey_fdw.spec /root/rpmbuild/SPECS/
                # Added only when non-empty: an empty --define would set the
                # macro to nothing, which is not the same as leaving the
                # spec default in place - it would blank the path.
                extra=()
                [ -n "${SPEC_PGCONFIG:-}" ] && extra+=(--define "pgconfig ${SPEC_PGCONFIG}")
                [ -n "${SPEC_PGDEVEL:-}" ] && extra+=(--define "pgdevel ${SPEC_PGDEVEL}")
                rpmbuild -bb /root/rpmbuild/SPECS/valkey_fdw.spec \
                    --define "pgmajor ${PG_MAJOR}" \
                    --define "vfdw_version ${VERSION}" \
                    "${extra[@]}"
                find /root/rpmbuild/RPMS -name "*.rpm" -exec cp {} /out/ \;
            '
    else
        docker run --rm --platform "$DOCKER_ARCH" \
            -v "$PWD:/src:ro" -v "$PWD/$out:/out" \
            -e "PG_MAJOR=${PG_MAJOR}" -e "VERSION=${VERSION}" \
            "$image" bash -lc '
                set -eux
                mkdir -p /build/src
                cp -a /src/. /build/src/
                rm -rf /build/src/dist /build/src/.git
                # The source copy is a WORKING TREE, not a clean checkout.
                #
                # The harness builds through a bind mount, so src/*.o and
                # valkey_fdw.so outlive its container and sit in the directory
                # this copies. make then finds every object newer than its
                # source, compiles nothing, and the package ships the library
                # the HARNESS built - a Debian binary linked against a shared
                # libvalkey, inside an EL package that then requires two
                # sonames the target does not have. It installs nowhere.
                #
                # Invisible in CI, which checks out clean, and reproducible on
                # any machine where the suites have been run. Whatever the
                # package contains has to be built here.
                for pat in '*.o' '*.bc' '*.d' '*.so' '*.gcno' '*.gcda'; do
                    find /build -type f -name "$pat" -delete
                done
                rm -rf /build/*/.harness
                # The built libvalkey, not its source. Each packaging build
                # makes one from vendor/*.tar.gz into its own prefix, so
                # carrying a tree of objects from another build along only
                # invites one of them to be linked by accident.
                #
                # No apostrophes in here: this whole block is the body of a
                # bash -lc '...' and one would end the string.
                rm -rf /build/*/vendor/libvalkey
                cd /build/src
                cp -a packaging/deb/debian debian
                # The binary package name carries the major, so the control
                # file is templated rather than duplicated eight times.
                sed -i "s/PGMAJOR/${PG_MAJOR}/g" debian/control
                sed -i "1s/^valkey-fdw (.*)/valkey-fdw (${VERSION}-1)/" debian/changelog
                PG_MAJOR=${PG_MAJOR} dpkg-buildpackage -b -us -uc
                find /build -maxdepth 1 -name "*.deb" -exec cp {} /out/ \;
            '
    fi

    ls "$out" | sed "s/^/    /"
}

for t in $TARGETS; do build_one "$t"; done
say "packages in dist/"
