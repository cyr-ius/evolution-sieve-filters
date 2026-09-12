#!/usr/bin/env bash
#
# Dovecot + Pigeonhole (ManageSieve) jetable pour tester le client Sieve
# sans dépendre d'un serveur externe.
#
#   tests/dovecot/run.sh              démarre en avant-plan (Ctrl-C pour arrêter)
#   tests/dovecot/run.sh --daemon     démarre en arrière-plan
#   tests/dovecot/run.sh --stop       arrête l'instance d'arrière-plan
#   tests/dovecot/run.sh --reload     recharge la configuration
#
# Écoute sur localhost :
#   - port 4190 : STARTTLS  (comportement d'un vrai serveur ManageSieve)
#   - port 4191 : TLS implicite  (mode par défaut de sieve-managesieve-client)
# Identifiants : testuser / testpass
#
# La première exécution génère un certificat auto-signé (CN/SAN = localhost) et
# l'ajoute au magasin de CA du conteneur, pour que la validation TLS du client
# passe sans avoir besoin d'un mode « insecure » (le client n'en a pas).

set -euo pipefail

FIX="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN="$FIX/run"
CONF="$RUN/dovecot.conf"

DOVECOT="$(command -v dovecot || true)"
[ -x "/usr/sbin/dovecot" ] && DOVECOT="/usr/sbin/dovecot"
if [ -z "$DOVECOT" ]; then
  echo "dovecot introuvable — installez :" >&2
  echo "  sudo apt-get install -y dovecot-core dovecot-managesieved dovecot-sieve" >&2
  exit 1
fi

mkdir -p "$RUN/state" "$FIX/mail"
# Pré-créer le journal en tant qu'utilisateur courant pour qu'il reste lisible
# (Dovecot tourne en root via sudo et ne ferait sinon qu'un fichier root).
touch "$RUN/dovecot.log"

# 1. Certificat auto-signé + confiance au niveau du conteneur.
if [ ! -s "$RUN/cert.pem" ] || [ ! -s "$RUN/key.pem" ]; then
  echo "Génération d'un certificat de test (localhost)…" >&2
  openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
    -keyout "$RUN/key.pem" -out "$RUN/cert.pem" \
    -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1
  sudo cp "$RUN/cert.pem" /usr/local/share/ca-certificates/dovecot-sieve-test.crt
  sudo update-ca-certificates >/dev/null
fi

# 2. Base d'utilisateurs (uid/gid = utilisateur courant : les maildirs restent
#    accessibles même si le maître Dovecot tourne en root via sudo).
printf 'testuser:{PLAIN}testpass:%s:%s::%s/mail/testuser\n' \
  "$(id -u)" "$(id -g)" "$FIX" > "$RUN/users"

# 3. Configuration, dérivée du gabarit versionné.
sed -e "s#@FIXTURE_DIR@#$FIX#g" \
    -e "s#@UID@#$(id -u)#g" \
    -e "s#@GID@#$(id -g)#g" \
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
    echo "Dovecot ManageSieve en arrière-plan — testuser / testpass"
    echo "  localhost:4190  STARTTLS        localhost:4191  TLS implicite"
    echo "  logs   : $RUN/dovecot.log"
    echo "  arrêt  : $0 --stop"
    ;;
  "")
    echo "Dovecot ManageSieve — testuser / testpass"
    echo "  localhost:4190  STARTTLS        localhost:4191  TLS implicite"
    echo "  logs : $RUN/dovecot.log   —   Ctrl-C pour arrêter."
    exec sudo "$DOVECOT" -F -c "$CONF"
    ;;
  *)
    echo "argument inconnu : $1" >&2
    exit 2
    ;;
esac
