#!/usr/bin/env bash
#
# Test de bout en bout : démarre le Dovecot de test, exerce toutes les commandes
# du client ManageSieve via l'outil tests/test-managesieve — une fois en TLS
# implicite (port 4191), une fois en STARTTLS (port 4190) — puis arrête Dovecot.
# Code de sortie non nul si une étape échoue.

set -euo pipefail

FIX="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$FIX/../.." && pwd)"
BIN="$ROOT/build/tests/test-managesieve"

if [ ! -x "$BIN" ]; then
  echo "$BIN absent — compilez d'abord :  meson compile -C build test-managesieve" >&2
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
  echo "port $1 injoignable" >&2; return 1
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

# $1 = libellé, $@ restants = options TLS/port passées au client
run_battery() {
  local label="$1"; shift
  local creds=(--host localhost --user testuser --password testpass "$@")

  echo
  echo "############################################################"
  echo "# Batterie : $label"
  echo "############################################################"

  echo; echo "===> LISTSCRIPTS (vide au départ)"
  "$BIN" "${creds[@]}"
  echo; echo "===> CHECKSCRIPT (validation côté serveur, rien d'installé)"
  "$BIN" "${creds[@]}" --check "$SAMPLE"
  echo; echo "===> PUTSCRIPT + SETACTIVE"
  "$BIN" "${creds[@]}" --put "$SAMPLE"
  echo; echo "===> LISTSCRIPTS (doit montrer test-managesieve [ACTIF])"
  "$BIN" "${creds[@]}"
  echo; echo "===> GETSCRIPT"
  "$BIN" "${creds[@]}" --get test-managesieve
  echo; echo "===> SETACTIVE \"\" (Dovecot refuse de supprimer le script actif)"
  "$BIN" "${creds[@]}" --deactivate
  echo; echo "===> DELETESCRIPT"
  "$BIN" "${creds[@]}" --delete test-managesieve
  echo; echo "===> LISTSCRIPTS (doit être vide)"
  "$BIN" "${creds[@]}"
}

run_battery "TLS implicite (port 4191)" --port 4191
run_battery "STARTTLS (port 4190)"      --starttls --port 4190

# Couverture SASL : chaque mécanisme annoncé par Dovecot doit permettre de
# s'authentifier (on force --mech puis on vérifie que le client rapporte bien
# ce mécanisme), plus un passage en négociation automatique (doit choisir le
# plus fort, ici SCRAM-SHA-256).
echo
echo "############################################################"
echo "# Batterie : mécanismes SASL (port 4191, TLS implicite)"
echo "############################################################"
sasl_creds=(--host localhost --user testuser --password testpass --port 4191)

for m in PLAIN LOGIN CRAM-MD5 SCRAM-SHA-1 SCRAM-SHA-256; do
  echo; echo "===> SASL $m (forcé)"
  out="$("$BIN" "${sasl_creds[@]}" --mech "$m" 2>&1)"
  echo "$out"
  echo "$out" | grep -q "mécanisme : $m)" \
    || { echo "ÉCHEC : $m non confirmé par le client" >&2; exit 1; }
done

echo; echo "===> SASL négociation automatique (doit choisir SCRAM-SHA-256)"
out="$("$BIN" "${sasl_creds[@]}" 2>&1)"
echo "$out"
echo "$out" | grep -q "mécanisme : SCRAM-SHA-256)" \
  || { echo "ÉCHEC : la négociation auto n'a pas choisi SCRAM-SHA-256" >&2; exit 1; }

echo; echo "===> SASL mécanisme non annoncé (doit échouer proprement)"
out="$("$BIN" "${sasl_creds[@]}" --mech OAUTHBEARER 2>&1 || true)"
echo "$out"
echo "$out" | grep -q "n'annonce pas" \
  && echo "OK : refus attendu pour un mécanisme non proposé" \
  || { echo "ÉCHEC : --mech OAUTHBEARER aurait dû être refusé" >&2; exit 1; }

echo
echo "smoke OK — TLS implicite, STARTTLS et négociation SASL validés"
