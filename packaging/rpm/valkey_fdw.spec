# valkey_fdw, packaged per PostgreSQL major.
#
# One binary package per major, named the way PGDG names EL extension
# packages, because a user installing this already has that convention:
#
#   valkey_fdw_17  ->  /usr/pgsql-17/lib/valkey_fdw.so
#
# LIBVALKEY IS LINKED STATICALLY. It is packaged by no distribution in this
# matrix, so a dynamic link would mean shipping a second package and pinning
# its soname across eight targets. The static object is the pinned 0.5.0 the
# suite tests against, which also removes "which libvalkey is on the box" as
# a variable in bug reports. OpenSSL stays dynamic: it is present everywhere,
# it is security-critical, and statically linking it would mean shipping our
# own CVE surface.

# No debuginfo subpackage. PGXS controls its own compile flags, and rpm's
# debugsource extraction finds nothing to attribute, failing the build with
# "Empty %files debugsourcefiles.list". The Debian side ships a -dbgsym
# because dh produces one for free; matching that here would mean fighting
# PGXS for flag control, which is not worth it for a single .so.
%global debug_package %{nil}

%global pgmajor  %{?pgmajor}%{!?pgmajor:17}
%global pgpath   /usr/pgsql-%{pgmajor}
%global libvalkey_ref %{?libvalkey_ref}%{!?libvalkey_ref:0.5.0}

Name:           valkey_fdw_%{pgmajor}
Version:        %{?vfdw_version}%{!?vfdw_version:0.1}
Release:        1%{?dist}
Summary:        PostgreSQL foreign data wrapper for Valkey

# Matches the extension itself; see LICENSE. The statically linked libvalkey
# is BSD-3-Clause and keeps its own notice, shipped from %%files below - the
# tag here describes this extension's terms, not the binary's whole content.
License:        PostgreSQL
# No URL tag. It means "where this package comes from", and the answer is not
# a published address yet; any plausible-looking one would place the package
# inside an organisation that does not host it. Absent says nothing false.
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc make cmake git openssl-devel
BuildRequires:  postgresql%{pgmajor}-devel
Requires:       postgresql%{pgmajor}-server

%description
valkey_fdw presents Valkey keys as PostgreSQL foreign tables: strings, hashes,
lists, sets and sorted sets, with TLS, ACL authentication and cluster support.

Writes are buffered and applied atomically by a single script at pre-commit,
so a transaction that commits in PostgreSQL has been accepted by Valkey in
full or not at all.

%prep
%autosetup -n %{name}-%{version}

%build
# libvalkey is CLONED here rather than taken from vendor/, because the tree
# carries no vendored copy - VALKEY_VENDORED expects one and nothing populates
# it. Cloning at the pinned tag makes the package self-contained and makes the
# version in the binary the version this suite tests against.
git clone --depth 1 --branch %{libvalkey_ref} \
    https://github.com/valkey-io/libvalkey.git _libvalkey
# CMAKE_INSTALL_LIBDIR is set rather than left to GNUInstallDirs, which
# answers lib64 on a 64-bit RPM distribution and lib on a Debian one. The
# Makefile names lib, so the default is right on one half of the target matrix
# and wrong on the other - and wrong in a way that compiles: the headers are
# found under include, every object builds, and only the link fails, naming an
# archive that was installed one directory away.
cmake -S _libvalkey -B _libvalkey/build \
      -DCMAKE_BUILD_TYPE=Release -DENABLE_TLS=ON -DDISABLE_TESTS=ON \
      -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_INSTALL_PREFIX=%{_builddir}/libvalkey-prefix \
      -DCMAKE_INSTALL_LIBDIR=lib \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build _libvalkey/build -j%{?_smp_build_ncpus}%{!?_smp_build_ncpus:4}
cmake --install _libvalkey/build

make %{?_smp_mflags} USE_PGXS=1 PG_CONFIG=%{pgpath}/bin/pg_config \
     VALKEY_VENDORED=1 LIBVALKEY_DIR=%{_builddir}/libvalkey-prefix

%install
make install USE_PGXS=1 PG_CONFIG=%{pgpath}/bin/pg_config \
     VALKEY_VENDORED=1 LIBVALKEY_DIR=%{_builddir}/libvalkey-prefix \
     DESTDIR=%{buildroot}

# The diagnostic extension does not ship.
#
# valkey_fdw_test carries entry points that reach a Valkey server without
# going through a foreign table, three of which dial a host and port given as
# arguments. They exist for the suites and are guarded - the control file is
# superuser-only and every function is revoked from PUBLIC - but a machine
# that will never run the suites has no use for the surface, and the smallest
# surface is the one that is absent. `make install` emits both extensions
# because the build is one build; the choice of what to ship is made here.
rm -f %{buildroot}%{pgpath}/share/extension/valkey_fdw_test.control \
      %{buildroot}%{pgpath}/share/extension/valkey_fdw_test--*.sql

%files
%license LICENSE
# libvalkey is inside the .so, not beside it, and BSD-3-Clause clause 2 binds
# whoever ships that binary: the notice has to travel with it. A second
# %%license installs it into the same licence directory, so it survives
# --excludedocs and is found where a user looks for it.
%license LICENSE-libvalkey
%doc README.md
%{pgpath}/lib/valkey_fdw.so
# LLVM bitcode for JIT inlining. PGXS emits it whenever the server was built
# with --with-llvm, which every PGDG build is, and rpm fails on anything left
# unpackaged - so it is listed rather than deleted. The Debian side picks it
# up automatically.
%{pgpath}/lib/bitcode/valkey_fdw*
# One extension, named exactly. The glob in valkey_fdw--*.sql ranges over
# VERSIONS and not over extension names, so this pair matches the wrapper and
# every future upgrade script of its own without also matching valkey_fdw_test
# - which %%install has already removed from the buildroot, and which rpm would
# otherwise fail the build over as an unpackaged file. Doubled because rpm
# expands macros inside comments too, and %%install is one: written singly it
# is replaced by its own definition and the spec acquires a second install
# section, which fails the build a hundred lines from the comment that caused
# it. %%files and %%build are not macros and pass either way, which is why the
# one at the top of this file is left as it is.
%{pgpath}/share/extension/valkey_fdw.control
%{pgpath}/share/extension/valkey_fdw--*.sql

%changelog
* Mon Aug 10 2026 Evgeniy Patlan <evgeniy.patlan@percona.com> - 0.1-1
- Initial package.
