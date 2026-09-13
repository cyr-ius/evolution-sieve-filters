#!/usr/bin/env bash
#
# Disposable MIT Kerberos KDC, for testing SASL GSSAPI authentication
# against the ManageSieve fixture (see run.sh). Entirely self-contained
# under tests/dovecot/run/krb5/: no system krb5.conf is touched, no system
# principal database, and the KDC listens on a dedicated, non-standard
# port so it can never collide with a real Kerberos installation.
#
#   tests/dovecot/kdc.sh --daemon   creates the realm (if needed) and starts krb5kdc
#   tests/dovecot/kdc.sh --stop     stops it
#
# Realm: SIEVE.TEST — principals: testuser@SIEVE.TEST (password: testpass,
# same as the ManageSieve fixture's passwd-file) and the service principal
# sieve/localhost@SIEVE.TEST, exported to run/krb5/dovecot.keytab for
# Dovecot's auth_krb5_keytab (see dovecot.conf.in).
#
# To obtain a ticket against this KDC (e.g. before running
# test-managesieve --mech GSSAPI by hand):
#   export KRB5_CONFIG="$PWD/run/krb5/krb5.conf"
#   export KRB5CCNAME="FILE:$PWD/run/krb5/ccache"
#   echo testpass | kinit testuser@SIEVE.TEST

set -euo pipefail

FIX="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KRB="$FIX/run/krb5"
REALM="SIEVE.TEST"
KDC_PORT=60088

KDB5_UTIL="$(command -v kdb5_util || true)"
[ -x "/usr/sbin/kdb5_util" ] && KDB5_UTIL="/usr/sbin/kdb5_util"
KADMIN_LOCAL="$(command -v kadmin.local || true)"
[ -x "/usr/sbin/kadmin.local" ] && KADMIN_LOCAL="/usr/sbin/kadmin.local"
KRB5KDC="$(command -v krb5kdc || true)"
[ -x "/usr/sbin/krb5kdc" ] && KRB5KDC="/usr/sbin/krb5kdc"

if [ -z "$KDB5_UTIL" ] || [ -z "$KADMIN_LOCAL" ] || [ -z "$KRB5KDC" ]; then
  echo "MIT Kerberos KDC tools not found — install them:" >&2
  echo "  sudo apt-get install -y krb5-kdc krb5-admin-server krb5-user" >&2
  exit 1
fi

mkdir -p "$KRB/db"

# Fixture-local krb5.conf / kdc.conf, regenerated on every run (cheap, and
# keeps the absolute paths correct if the checkout moves). Never touches
# /etc/krb5.conf.
cat > "$KRB/krb5.conf" <<EOF
[libdefaults]
  default_realm = $REALM
  dns_lookup_kdc = false
  dns_lookup_realm = false
  rdns = false

[realms]
  $REALM = {
    kdc = localhost:$KDC_PORT
  }
EOF

cat > "$KRB/kdc.conf" <<EOF
[kdcdefaults]
  kdc_ports = $KDC_PORT
  kdc_tcp_ports = $KDC_PORT

[realms]
  $REALM = {
    database_name = $KRB/db/principal
    key_stash_file = $KRB/db/.k5.stash
    acl_file = $KRB/db/kadm5.acl
    admin_keytab = $KRB/db/kadm5.keytab
    max_life = 1h
    max_renewable_life = 1h
  }
EOF
touch "$KRB/db/kadm5.acl"

export KRB5_CONFIG="$KRB/krb5.conf"
export KRB5_KDC_PROFILE="$KRB/kdc.conf"

case "${1:-}" in
  --stop)
    if [ -f "$KRB/kdc.pid" ]; then
      kill "$(cat "$KRB/kdc.pid")" 2>/dev/null || true
      rm -f "$KRB/kdc.pid"
    fi
    ;;
  --daemon)
    # Create the realm database + principals once (offline: kdb5_util and
    # kadmin.local don't need a running KDC). Idempotent: skipped if a
    # previous run already created the database.
    if [ ! -e "$KRB/db/principal" ]; then
      echo "Creating the disposable Kerberos realm $REALM…" >&2
      "$KDB5_UTIL" -r "$REALM" -P "sieve-test-master-pw" create -s >/dev/null
      "$KADMIN_LOCAL" -q "addprinc -pw testpass testuser@$REALM" >/dev/null
      "$KADMIN_LOCAL" -q "addprinc -randkey sieve/localhost@$REALM" >/dev/null
      rm -f "$KRB/dovecot.keytab"
      "$KADMIN_LOCAL" -q "ktadd -k $KRB/dovecot.keytab sieve/localhost@$REALM" >/dev/null
      # World-readable: Dovecot's auth worker doesn't run as root even
      # though the master process is started via sudo (see run.sh), and
      # this keytab only protects a disposable, throwaway test realm.
      chmod 644 "$KRB/dovecot.keytab"
    fi
    "$KRB5KDC" -P "$KRB/kdc.pid"
    echo "Disposable Kerberos KDC running — realm $REALM, testuser/testpass"
    echo "  kdc    : localhost:$KDC_PORT"
    echo "  keytab : $KRB/dovecot.keytab"
    echo "  stop   : $0 --stop"
    ;;
  *)
    echo "usage: $0 --daemon|--stop" >&2
    exit 2
    ;;
esac
