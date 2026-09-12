#!/usr/bin/env bash
# Construit le paquet Debian (source et/ou binaire) d'evolution-sieve-filters
# à partir du répertoire debian/ à la racine du dépôt.
#
# Usage : scripts/build-deb.sh [--install-deps] [--binary|--source|--both]
#
#   --install-deps   Installe les Build-Depends de debian/control via
#                     mk-build-deps (paquet devscripts + equivs). Nécessite
#                     sudo. Inutile dans le devcontainer du projet, qui les
#                     a déjà toutes (voir .devcontainer/Dockerfile).
#   --binary         Ne construit que le .deb binaire (défaut).
#   --source         Ne construit que le paquet source (.dsc/.tar.xz).
#   --both           Construit source + binaire.
#
# Artefacts déposés dans le répertoire parent du dépôt, convention
# dpkg-buildpackage :
#   ../evolution-sieve-filters_<version>_<arch>.deb
#   ../evolution-sieve-filters_<version>.dsc (+ .tar.xz, .changes, .buildinfo)

set -euo pipefail
cd "$(dirname "$0")/.."

usage() {
  cat <<'EOF'
Usage : scripts/build-deb.sh [--install-deps] [--binary|--source|--both]

  --install-deps   Installe les Build-Depends de debian/control via
                    mk-build-deps (devscripts + equivs). Nécessite sudo.
  --binary         Ne construit que le .deb binaire (défaut).
  --source         Ne construit que le paquet source (.dsc/.tar.xz).
  --both           Construit source + binaire.
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
    *) echo "Option inconnue : $1" >&2; usage; exit 1 ;;
  esac
  shift
done

if [ "$install_deps" -eq 1 ]; then
  command -v mk-build-deps >/dev/null 2>&1 || {
    echo "mk-build-deps introuvable : installez 'devscripts' et 'equivs'." >&2
    exit 1
  }
  sudo mk-build-deps -i -r -t "apt-get -y --no-install-recommends" ./debian/control
fi

command -v dpkg-buildpackage >/dev/null 2>&1 || {
  echo "dpkg-buildpackage introuvable : installez 'devscripts' (fournit dpkg-dev)." >&2
  exit 1
}

# -us -uc : ne signe pas (GPG) le paquet source ni le .changes. Retirez ces
# deux options pour signer avec votre clé avant de publier.
exec dpkg-buildpackage "${dpkg_args[@]}" -us -uc
