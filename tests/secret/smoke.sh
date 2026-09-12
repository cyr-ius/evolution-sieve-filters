#!/usr/bin/env bash
#
# Aller-retour trousseau de bout en bout, sans toucher au trousseau de
# session de l'utilisateur ni à aucune config système.
#
# Monte un gnome-keyring jetable — tout sous tests/secret/run/ :
# XDG_DATA_HOME, XDG_RUNTIME_DIR, XDG_CONFIG_HOME — à l'intérieur d'un bus
# de session dédié (dbus-run-session). Le trousseau « login » est créé et
# déverrouillé avec un mot de passe bidon, et pré-désigné trousseau par
# défaut (keyrings/default) pour que SECRET_COLLECTION_DEFAULT tombe sur
# une collection déverrouillée sans passer par un prompteur graphique.
#
# build/tests/test-sieve-secret est ensuite lancé avec
# SIEVE_SECRET_REQUIRE_SERVICE=1 : sans Secret Service joignable il
# *échoue* (au lieu de se mettre en « skip » comme sous `meson test`).
#
# Code de sortie non nul si une étape échoue.

set -euo pipefail

FIX="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$FIX/../.." && pwd)"
BIN="$ROOT/build/tests/test-sieve-secret"

if [ ! -x "$BIN" ]; then
  echo "$BIN absent — compilez d'abord :  meson compile -C build test-sieve-secret" >&2
  exit 1
fi

for tool in dbus-run-session gnome-keyring-daemon; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "$tool introuvable — installez 'dbus' et 'gnome-keyring'." >&2
    exit 1
  fi
done

RUN="$FIX/run"
rm -rf "$RUN"
mkdir -p "$RUN/xdg-data/keyrings" "$RUN/xdg-runtime" "$RUN/xdg-config"
chmod 700 "$RUN/xdg-runtime"
# Désigne « login » comme trousseau par défaut : il sera créé déverrouillé
# par --unlock, donc l'alias `default` pointe sur une collection ouverte.
printf 'login\n' > "$RUN/xdg-data/keyrings/default"

cleanup() {
  pkill -f "gnome-keyring-daemon.*${RUN//\//\\/}" 2>/dev/null || true
  rm -rf "$RUN"
}
trap cleanup EXIT

# Le corps tourne dans un bus de session neuf. --daemonize : le démon fork
# et le parent imprime les variables d'environnement puis rend la main
# (sans --daemonize, le pipe de $(...) reste ouvert et eval bloque).
dbus-run-session -- bash -euo pipefail -c '
  RUN="$1"; BIN="$2"
  export XDG_DATA_HOME="$RUN/xdg-data"
  export XDG_CONFIG_HOME="$RUN/xdg-config"
  export XDG_RUNTIME_DIR="$RUN/xdg-runtime"

  eval "$(printf "%s\n" "trousseau-de-test" \
            | gnome-keyring-daemon --daemonize --unlock --components=secrets)"
  export GNOME_KEYRING_CONTROL SSH_AUTH_SOCK

  # Attend que org.freedesktop.secrets soit enregistré sur le bus.
  ok=""
  for _ in $(seq 1 50); do
    if gdbus call --session --dest org.freedesktop.DBus \
         --object-path /org/freedesktop/DBus \
         --method org.freedesktop.DBus.NameHasOwner org.freedesktop.secrets \
         2>/dev/null | grep -q true; then
      ok=1; break
    fi
    sleep 0.1
  done
  if [ -z "$ok" ]; then
    echo "org.freedesktop.secrets ne s'\''est pas enregistré sur le bus" >&2
    exit 1
  fi

  export SIEVE_SECRET_REQUIRE_SERVICE=1
  exec "$BIN"
' _ "$RUN" "$BIN"
