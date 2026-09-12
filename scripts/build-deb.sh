#!/usr/bin/env bash
# Builds the Debian package (source and/or binary) for evolution-sieve-filters
# from the debian/ directory at the repo root.
#
# Usage: scripts/build-deb.sh [--install-deps] [--binary|--source|--both]
#
#   --install-deps   Installs the Build-Depends from debian/control via
#                     mk-build-deps (devscripts + equivs package). Requires
#                     sudo. Not needed in the project's devcontainer, which
#                     already has them all (see .devcontainer/Dockerfile).
#   --binary         Builds only the binary .deb (default).
#   --source         Builds only the source package (.dsc/.tar.xz).
#   --both           Builds both source and binary.
#
# Artifacts are dropped in the repo's parent directory, per the
# dpkg-buildpackage convention:
#   ../evolution-sieve-filters_<version>_<arch>.deb
#   ../evolution-sieve-filters_<version>.dsc (+ .tar.xz, .changes, .buildinfo)

set -euo pipefail
cd "$(dirname "$0")/.."

usage() {
  cat <<'EOF'
Usage: scripts/build-deb.sh [--install-deps] [--binary|--source|--both]

  --install-deps   Installs the Build-Depends from debian/control via
                    mk-build-deps (devscripts + equivs). Requires sudo.
  --binary         Builds only the binary .deb (default).
  --source         Builds only the source package (.dsc/.tar.xz).
  --both           Builds both source and binary.
EOF
}

install_deps=0
dpkg_args=(-b)

while [ $# -gt 0 ]; do
  case "$1" in
    --install-deps) install_deps=1 ;;
    --binary) dpkg_args=(-b) ;;
    --source) dpkg_args=(-S) ;;
    --both) dpkg_args=() ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
  shift
done

if [ "$install_deps" -eq 1 ]; then
  command -v mk-build-deps >/dev/null 2>&1 || {
    echo "mk-build-deps not found: install 'devscripts' and 'equivs'." >&2
    exit 1
  }
  sudo mk-build-deps -i -r -t "apt-get -y --no-install-recommends" ./debian/control
fi

command -v dpkg-buildpackage >/dev/null 2>&1 || {
  echo "dpkg-buildpackage not found: install 'devscripts' (provides dpkg-dev)." >&2
  exit 1
}

# -us -uc: don't sign (GPG) the source package or the .changes file. Remove
# these two options to sign with your key before publishing.
exec dpkg-buildpackage "${dpkg_args[@]}" -us -uc
