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

"$FIX/run.sh" --daemon
cleanup() { "$FIX/run.sh" --stop >/dev/null 2>&1 || true; }
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

echo
echo "smoke OK — implicit TLS, STARTTLS and SASL negotiation validated"
