#!/usr/bin/env bash
#
# End-to-end test: starts the test Dovecot, exercises every command of the
# ManageSieve client via the tests/test-managesieve tool — once over implicit
# TLS (port 4191), once over STARTTLS (port 4190) — then stops Dovecot.
# Non-zero exit code if any step fails.

set -euo pipefail

FIX="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$FIX/../.." && pwd)"
BIN="$ROOT/build/tests/test-managesieve"

if [ ! -x "$BIN" ]; then
  echo "$BIN not found — build it first:  meson compile -C build test-managesieve" >&2
  exit 1
fi

# SASL GSSAPI needs a disposable Kerberos KDC (kdc.sh) — only attempted if
# the krb5 tools are installed; the keytab it creates must exist before
# Dovecot starts (see kdc.sh's comments), so it comes first.
HAVE_KDC=0
if command -v kinit >/dev/null 2>&1 && command -v kdb5_util >/dev/null 2>&1; then
  "$FIX/kdc.sh" --daemon
  HAVE_KDC=1
fi

"$FIX/run.sh" --daemon
cleanup() {
  "$FIX/run.sh" --stop >/dev/null 2>&1 || true
  [ "$HAVE_KDC" = 1 ] && "$FIX/kdc.sh" --stop >/dev/null 2>&1 || true
}
trap cleanup EXIT

wait_port() {
  for _ in $(seq 1 50); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; then exec 3>&- ; return 0; fi
    sleep 0.2
  done
  echo "port $1 unreachable" >&2; return 1
}
wait_port 4190
wait_port 4191

SAMPLE="$FIX/run/sample.sieve"
cat > "$SAMPLE" <<'EOF'
require ["fileinto"];
if header :contains "subject" "[SPAM]" {
  fileinto "Junk";
  stop;
}
EOF

# $1 = label, remaining $@ = TLS/port options passed to the client
run_battery() {
  local label="$1"; shift
  local creds=(--host localhost --user testuser --password testpass "$@")

  echo
  echo "############################################################"
  echo "# Battery: $label"
  echo "############################################################"

  echo; echo "===> LISTSCRIPTS (empty at the start)"
  "$BIN" "${creds[@]}"
  echo; echo "===> CHECKSCRIPT (server-side validation, nothing installed)"
  "$BIN" "${creds[@]}" --check "$SAMPLE"
  echo; echo "===> PUTSCRIPT + SETACTIVE"
  "$BIN" "${creds[@]}" --put "$SAMPLE"
  echo; echo "===> LISTSCRIPTS (should show test-managesieve [ACTIVE])"
  "$BIN" "${creds[@]}"
  echo; echo "===> GETSCRIPT"
  "$BIN" "${creds[@]}" --get test-managesieve
  echo; echo "===> SETACTIVE \"\" (Dovecot refuses to delete the active script)"
  "$BIN" "${creds[@]}" --deactivate
  echo; echo "===> DELETESCRIPT"
  "$BIN" "${creds[@]}" --delete test-managesieve
  echo; echo "===> LISTSCRIPTS (should be empty)"
  "$BIN" "${creds[@]}"
}

run_battery "implicit TLS (port 4191)" --port 4191
run_battery "STARTTLS (port 4190)"     --starttls --port 4190

# SASL coverage: every mechanism advertised by Dovecot must allow
# authentication (force --mech then check the client reports back that same
# mechanism), plus a pass with automatic negotiation (must pick the
# strongest one, here SCRAM-SHA-256).
echo
echo "############################################################"
echo "# Battery: SASL mechanisms (port 4191, implicit TLS)"
echo "############################################################"
sasl_creds=(--host localhost --user testuser --password testpass --port 4191)

for m in PLAIN LOGIN CRAM-MD5 SCRAM-SHA-1 SCRAM-SHA-256; do
  echo; echo "===> SASL $m (forced)"
  out="$("$BIN" "${sasl_creds[@]}" --mech "$m" 2>&1)"
  echo "$out"
  echo "$out" | grep -q "mechanism: $m)" \
    || { echo "FAILED: $m not confirmed by the client" >&2; exit 1; }
done

echo; echo "===> SASL automatic negotiation (should pick SCRAM-SHA-256)"
out="$("$BIN" "${sasl_creds[@]}" 2>&1)"
echo "$out"
echo "$out" | grep -q "mechanism: SCRAM-SHA-256)" \
  || { echo "FAILED: automatic negotiation didn't pick SCRAM-SHA-256" >&2; exit 1; }

echo; echo "===> SASL mechanism not advertised (should fail cleanly)"
out="$("$BIN" "${sasl_creds[@]}" --mech OAUTHBEARER 2>&1 || true)"
echo "$out"
echo "$out" | grep -q "doesn't advertise" \
  && echo "OK: expected refusal for an unadvertised mechanism" \
  || { echo "FAILED: --mech OAUTHBEARER should have been refused" >&2; exit 1; }

# SASL GSSAPI: only if the disposable KDC came up (HAVE_KDC) AND Dovecot
# actually loaded the "dovecot-gssapi" plugin (run.sh only puts "gssapi"
# in auth_mechanisms when the plugin .so is present — see run.sh).
if [ "$HAVE_KDC" = 1 ] && grep -q "^auth_mechanisms.*gssapi" "$FIX/run/dovecot.conf"; then
  echo
  echo "############################################################"
  echo "# Battery: SASL GSSAPI"
  echo "############################################################"

  export KRB5_CONFIG="$FIX/run/krb5/krb5.conf"
  export KRB5CCNAME="FILE:$FIX/run/krb5/ccache"
  echo testpass | kinit testuser@SIEVE.TEST

  gssapi_creds=(--host localhost --user testuser --port 4191)

  echo; echo "===> SASL GSSAPI (forced)"
  out="$("$BIN" "${gssapi_creds[@]}" --mech GSSAPI 2>&1)"
  echo "$out"
  echo "$out" | grep -q "mechanism: GSSAPI)" \
    || { echo "FAILED: GSSAPI not confirmed by the client" >&2; exit 1; }

  echo; echo "===> SASL automatic negotiation with a ticket (should now pick GSSAPI)"
  out="$("$BIN" "${gssapi_creds[@]}" 2>&1)"
  echo "$out"
  echo "$out" | grep -q "mechanism: GSSAPI)" \
    || { echo "FAILED: automatic negotiation didn't pick GSSAPI although a ticket was available" >&2; exit 1; }

  # --gssapi-hostname (issue #2): reproduces the actual report. Dovecot
  # itself decides which service principal it accepts, via its own
  # auth_gssapi_hostname setting (normally == the address clients connect
  # to; dovecot.conf.in sets it to "localhost" for every other case in
  # this file). A DNS alias/CNAME means the client's connection hostname
  # and the server's configured principal diverge: here we simulate that
  # by pointing Dovecot's auth_gssapi_hostname at kdc.sh's CANON_HOST
  # (a principal that IS in the keytab, just under a different name),
  # while the client still connects to plain "localhost".
  canon_host="sieve-canonical.sieve.test"   # must match kdc.sh's CANON_HOST
  DOVECOT_CONF="$FIX/run/dovecot.conf"
  sed -i "s/^auth_gssapi_hostname.*/auth_gssapi_hostname = $canon_host/" \
    "$DOVECOT_CONF"
  sudo dovecot -c "$DOVECOT_CONF" reload

  echo; echo "===> SASL GSSAPI, server expects a different principal (CNAME case, no override)"
  out="$("$BIN" "${gssapi_creds[@]}" --mech GSSAPI 2>&1 || true)"
  echo "$out"
  echo "$out" | grep -q "mechanism: GSSAPI)" \
    && { echo "FAILED: GSSAPI can't have succeeded against a mismatched server principal" >&2; exit 1; }

  echo; echo "===> SASL GSSAPI with --gssapi-hostname matching the server's principal (the fix)"
  out="$("$BIN" "${gssapi_creds[@]}" --mech GSSAPI --gssapi-hostname "$canon_host" 2>&1)"
  echo "$out"
  echo "$out" | grep -q "mechanism: GSSAPI)" \
    || { echo "FAILED: GSSAPI with --gssapi-hostname didn't authenticate" >&2; exit 1; }

  # Restore the default before the rest of the battery (and in case
  # something else reuses this persistent fixture afterwards).
  sed -i "s/^auth_gssapi_hostname.*/auth_gssapi_hostname = localhost/" \
    "$DOVECOT_CONF"
  sudo dovecot -c "$DOVECOT_CONF" reload

  # GSSAPI that fails on the client side although a ticket exists (here: the
  # service ticket can't be obtained because the KDC is unreachable after the
  # TGT was acquired — the same client-side failure as a service principal
  # unknown to the KDC, e.g. sieve/<CNAME>). Automatic negotiation must fall
  # back to another mechanism; a forced --mech GSSAPI must NOT.
  BROKEN_KRB5="$FIX/run/krb5/krb5-broken.conf"
  sed -E 's/^([[:space:]]*kdc[[:space:]]*=).*/\1 127.0.0.1:1/' \
    "$KRB5_CONFIG" > "$BROKEN_KRB5"
  grep -q "127.0.0.1:1" "$BROKEN_KRB5" \
    || { echo "FAILED: couldn't derive the broken krb5.conf (no kdc = line?)" >&2; exit 1; }
  # Fresh ccache holding only a TGT: the service tickets cached by the runs
  # above would otherwise let GSSAPI succeed without asking the KDC.
  BROKEN_CC="FILE:$FIX/run/krb5/ccache-broken"
  echo testpass | KRB5CCNAME="$BROKEN_CC" kinit testuser@SIEVE.TEST
  broken_creds=(--host localhost --user testuser --password testpass --port 4191)

  echo; echo "===> SASL automatic negotiation, GSSAPI failing (should fall back)"
  out="$(KRB5CCNAME="$BROKEN_CC" KRB5_CONFIG="$BROKEN_KRB5" "$BIN" "${broken_creds[@]}" 2>&1)"
  echo "$out"
  echo "$out" | grep -q "mechanism: GSSAPI)" \
    && { echo "FAILED: GSSAPI can't have succeeded with a broken KDC" >&2; exit 1; }
  echo "$out" | grep -q "falling back" \
    || { echo "FAILED: no GSSAPI fallback was attempted" >&2; exit 1; }
  echo "$out" | grep -q "mechanism: SCRAM-SHA-256)" \
    || { echo "FAILED: fallback didn't authenticate with SCRAM-SHA-256" >&2; exit 1; }

  echo; echo "===> SASL GSSAPI forced and failing (must NOT fall back)"
  out="$(KRB5CCNAME="$BROKEN_CC" KRB5_CONFIG="$BROKEN_KRB5" "$BIN" "${broken_creds[@]}" --mech GSSAPI 2>&1 || true)"
  echo "$out"
  echo "$out" | grep -q "falling back" \
    && { echo "FAILED: a forced mechanism must never be replaced" >&2; exit 1; }
  echo "$out" | grep -q "SASL authentication failed (GSSAPI)" \
    || { echo "FAILED: forced GSSAPI should have failed with a GSSAPI error" >&2; exit 1; }

  KRB5CCNAME="$BROKEN_CC" kdestroy >/dev/null 2>&1 || true
  kdestroy >/dev/null 2>&1 || true
  unset KRB5_CONFIG KRB5CCNAME
else
  echo
  echo "SKIP: SASL GSSAPI (krb5-kdc/krb5-user or the dovecot-gssapi plugin not installed)"
fi

echo
echo "############################################################"
echo "# Battery: IMAP hierarchy separator probe (issue #3)"
echo "############################################################"
if [ -x "/usr/lib/dovecot/imap" ]; then
  IMAP_BIN="$ROOT/build/tests/test-imap-probe"
  if [ ! -x "$IMAP_BIN" ]; then
    echo "$IMAP_BIN not found — build it first:  meson compile -C build test-imap-probe" >&2
    exit 1
  fi
  wait_port 4143
  wait_port 4144
  imap_creds=(--host localhost --user testuser --password testpass)

  echo; echo "===> implicit TLS (port 4144) — mail_driver=maildir defaults to Maildir++, separator '.'"
  out="$("$IMAP_BIN" "${imap_creds[@]}" --port 4144 2>&1)"
  echo "$out"
  echo "$out" | grep -q "Hierarchy separator: '.'" \
    || { echo "FAILED: expected separator '.', see issue #3" >&2; exit 1; }

  echo; echo "===> STARTTLS (port 4143)"
  out="$("$IMAP_BIN" "${imap_creds[@]}" --port 4143 --starttls 2>&1)"
  echo "$out"
  echo "$out" | grep -q "Hierarchy separator: '.'" \
    || { echo "FAILED: expected separator '.', see issue #3" >&2; exit 1; }

  echo; echo "===> wrong password (must fail cleanly, not crash)"
  out="$("$IMAP_BIN" --host localhost --user testuser --password wrong --port 4144 2>&1 || true)"
  echo "$out"
  echo "$out" | grep -q "Probe failed" \
    || { echo "FAILED: a wrong password should have been rejected" >&2; exit 1; }
else
  echo
  echo "SKIP: IMAP hierarchy separator probe (dovecot-imapd not installed --"
  echo "      /usr/lib/dovecot/imap missing; run.sh already left IMAP out of"
  echo "      protocols= for this same reason)"
fi

echo
if [ -x "/usr/lib/dovecot/imap" ]; then
  echo "smoke OK — implicit TLS, STARTTLS, SASL negotiation and IMAP separator probe validated"
else
  echo "smoke OK — implicit TLS, STARTTLS and SASL negotiation validated (IMAP separator probe skipped)"
fi
