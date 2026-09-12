#!/usr/bin/env bash
#
# End-to-end keyring round trip, without touching the user's session
# keyring or any system config.
#
# Sets up a disposable gnome-keyring — everything under tests/secret/run/ :
# XDG_DATA_HOME, XDG_RUNTIME_DIR, XDG_CONFIG_HOME — inside a dedicated
# session bus (dbus-run-session). The "login" keyring is created and
# unlocked with a dummy password, and pre-designated as the default keyring
# (keyrings/default) so that SECRET_COLLECTION_DEFAULT resolves to an
# unlocked collection without going through a graphical prompter.
#
# build/tests/test-sieve-secret is then run with
# SIEVE_SECRET_REQUIRE_SERVICE=1: without a reachable Secret Service it
# *fails* (instead of being skipped as under `meson test`).
#
# Non-zero exit code if any step fails.

set -euo pipefail

FIX="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$FIX/../.." && pwd)"
BIN="$ROOT/build/tests/test-sieve-secret"

if [ ! -x "$BIN" ]; then
  echo "$BIN not found — build it first:  meson compile -C build test-sieve-secret" >&2
  exit 1
fi

for tool in dbus-run-session gnome-keyring-daemon; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "$tool not found — install 'dbus' and 'gnome-keyring'." >&2
    exit 1
  fi
done

RUN="$FIX/run"
rm -rf "$RUN"
mkdir -p "$RUN/xdg-data/keyrings" "$RUN/xdg-runtime" "$RUN/xdg-config"
chmod 700 "$RUN/xdg-runtime"
# Designate "login" as the default keyring: it will be created unlocked by
# --unlock, so the `default` alias points to an open collection.
printf 'login\n' > "$RUN/xdg-data/keyrings/default"

cleanup() {
  pkill -f "gnome-keyring-daemon.*${RUN//\//\\/}" 2>/dev/null || true
  rm -rf "$RUN"
}
trap cleanup EXIT

# The body runs inside a fresh session bus. --daemonize: the daemon forks
# and the parent prints the environment variables then returns control
# (without --daemonize, the $(...) pipe stays open and eval hangs).
dbus-run-session -- bash -euo pipefail -c '
  RUN="$1"; BIN="$2"
  export XDG_DATA_HOME="$RUN/xdg-data"
  export XDG_CONFIG_HOME="$RUN/xdg-config"
  export XDG_RUNTIME_DIR="$RUN/xdg-runtime"

  eval "$(printf "%s\n" "test-keyring-password" \
            | gnome-keyring-daemon --daemonize --unlock --components=secrets)"
  export GNOME_KEYRING_CONTROL SSH_AUTH_SOCK

  # Wait for org.freedesktop.secrets to be registered on the bus.
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
    echo "org.freedesktop.secrets did not register on the bus" >&2
    exit 1
  fi

  export SIEVE_SECRET_REQUIRE_SERVICE=1
  exec "$BIN"
' _ "$RUN" "$BIN"
