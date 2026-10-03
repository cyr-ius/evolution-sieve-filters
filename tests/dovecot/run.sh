#!/usr/bin/env bash
#
# Disposable Dovecot + Pigeonhole (ManageSieve) instance for testing the
# Sieve client without depending on an external server.
#
#   tests/dovecot/run.sh              starts in the foreground (Ctrl-C to stop)
#   tests/dovecot/run.sh --daemon     starts in the background
#   tests/dovecot/run.sh --stop       stops the background instance
#   tests/dovecot/run.sh --reload     reloads the configuration
#
# Listens on localhost:
#   - port 4190: STARTTLS  (behavior of a real ManageSieve server)
#   - port 4191: implicit TLS  (sieve-managesieve-client's default mode)
# Credentials: testuser / testpass
#
# The first run generates a self-signed certificate (CN/SAN = localhost); every
# run makes sure it is in the container's CA store (re-added after a
# devcontainer rebuild), so the client's TLS validation passes without
# needing an "insecure" mode (the client has none).

set -euo pipefail

FIX="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN="$FIX/run"
CONF="$RUN/dovecot.conf"

DOVECOT="$(command -v dovecot || true)"
[ -x "/usr/sbin/dovecot" ] && DOVECOT="/usr/sbin/dovecot"
if [ -z "$DOVECOT" ]; then
  echo "dovecot not found — install it:" >&2
  echo "  sudo apt-get install -y dovecot-core dovecot-managesieved dovecot-sieve" >&2
  exit 1
fi

mkdir -p "$RUN/state" "$FIX/mail"
# Pre-create the log file as the current user so it stays readable
# (Dovecot runs as root via sudo and would otherwise create a root-owned file).
touch "$RUN/dovecot.log"

# 1. Self-signed certificate + trust at the container level.
if [ ! -s "$RUN/cert.pem" ] || [ ! -s "$RUN/key.pem" ]; then
  echo "Generating a test certificate (localhost)…" >&2
  openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
    -keyout "$RUN/key.pem" -out "$RUN/cert.pem" \
    -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1
fi
# Trust is checked on EVERY run, not only when the cert is generated: run/
# lives in the workspace and survives a devcontainer rebuild, but the CA
# store does not — a reused cert would otherwise no longer be trusted.
CA_COPY=/usr/local/share/ca-certificates/dovecot-sieve-test.crt
if ! cmp -s "$RUN/cert.pem" "$CA_COPY"; then
  echo "Trusting the test certificate (container CA store)…" >&2
  sudo cp "$RUN/cert.pem" "$CA_COPY"
  sudo update-ca-certificates >/dev/null
fi

# 2. User database (uid/gid = current user: the maildirs stay accessible
#    even though the Dovecot master runs as root via sudo).
printf 'testuser:{PLAIN}testpass:%s:%s::%s/mail/testuser\n' \
  "$(id -u)" "$(id -g)" "$FIX" > "$RUN/users"

# 3. Configuration, derived from the versioned template.
# GSSAPI mechanism: only listed in auth_mechanisms if the "dovecot-gssapi"
# plugin is actually installed — an unknown mechanism name there is FATAL
# for the whole auth process (see dovecot.conf.in), so this must never be
# unconditional.
GSSAPI_MECH=""
[ -e "/usr/lib/dovecot/modules/auth/libmech_gssapi.so" ] && GSSAPI_MECH=" gssapi"

# IMAP: same story as GSSAPI above, but for the whole protocol rather
# than one mechanism -- "dovecot-imapd" (the /usr/lib/dovecot/imap
# executable) is a separate package from dovecot-core, not installed by
# default. Listing "imap" in `protocols` when it's missing is FATAL at
# startup ("service(imap) access(...) failed: No such file or
# directory"), which would break every test in this fixture, not just
# tests/test-imap-probe.c's battery in smoke.sh -- so both the protocol
# name and the "service imap-login { ... }" block are only kept when the
# binary actually exists.
IMAP_PROTOCOL_NAME=""
[ -x "/usr/lib/dovecot/imap" ] && IMAP_PROTOCOL_NAME=" imap"

sed -e "s#@FIXTURE_DIR@#$FIX#g" \
    -e "s#@UID@#$(id -u)#g" \
    -e "s#@GID@#$(id -g)#g" \
    -e "s#@GSSAPI_MECH@#$GSSAPI_MECH#g" \
    -e "s#@IMAP_PROTOCOL_NAME@#$IMAP_PROTOCOL_NAME#g" \
    "$FIX/dovecot.conf.in" > "$CONF"

if [ -z "$IMAP_PROTOCOL_NAME" ]; then
  sed -i '/# IMAP_BLOCK_BEGIN/,/# IMAP_BLOCK_END/d' "$CONF"
fi

case "${1:-}" in
  --stop)
    exec sudo "$DOVECOT" -c "$CONF" stop
    ;;
  --reload)
    exec sudo "$DOVECOT" -c "$CONF" reload
    ;;
  --daemon)
    sudo "$DOVECOT" -c "$CONF"
    echo "Dovecot ManageSieve running in the background — testuser / testpass"
    echo "  localhost:4190  STARTTLS        localhost:4191  implicit TLS"
    echo "  logs   : $RUN/dovecot.log"
    echo "  stop   : $0 --stop"
    ;;
  "")
    echo "Dovecot ManageSieve — testuser / testpass"
    echo "  localhost:4190  STARTTLS        localhost:4191  implicit TLS"
    echo "  logs : $RUN/dovecot.log   —   Ctrl-C to stop."
    exec sudo "$DOVECOT" -F -c "$CONF"
    ;;
  *)
    echo "unknown argument: $1" >&2
    exit 2
    ;;
esac
