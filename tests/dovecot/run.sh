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
# The first run generates a self-signed certificate (CN/SAN = localhost) and
# adds it to the container's CA store, so the client's TLS validation passes
# without needing an "insecure" mode (the client has none).

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
  sudo cp "$RUN/cert.pem" /usr/local/share/ca-certificates/dovecot-sieve-test.crt
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

sed -e "s#@FIXTURE_DIR@#$FIX#g" \
    -e "s#@UID@#$(id -u)#g" \
    -e "s#@GID@#$(id -g)#g" \
    -e "s#@GSSAPI_MECH@#$GSSAPI_MECH#g" \
    "$FIX/dovecot.conf.in" > "$CONF"

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
