# Dovecot de test (ManageSieve)

Instance **Dovecot + Pigeonhole** jetable pour exercer
`sieve-managesieve-client` / `test-managesieve` contre un vrai serveur, sans
compte système ni configuration globale : tout vit sous ce répertoire.

- **`localhost:4190` → STARTTLS** (comportement d'un vrai serveur ManageSieve).
- **`localhost:4191` → TLS implicite** (mode par défaut de
  `sieve_managesieve_client_new(..., implicit_tls = TRUE)`).
- `ssl = required` : l'authentification en clair est refusée avant chiffrement.
- Identifiants : **`testuser` / `testpass`** (base `passwd-file` statique).
- Le certificat auto-signé généré à la première exécution (CN/SAN `localhost`)
  est ajouté au magasin de CA du conteneur
  (`/usr/local/share/ca-certificates/`) pour que la validation TLS du client
  passe sans code « insecure » (le client n'en a pas).

## Utilisation

```sh
# depuis n'importe où dans le dépôt
tests/dovecot/run.sh              # avant-plan, Ctrl-C pour arrêter
tests/dovecot/run.sh --daemon     # arrière-plan
tests/dovecot/run.sh --stop       # arrêter l'instance d'arrière-plan
tests/dovecot/run.sh --reload     # recharger la conf

# test de bout en bout automatique (démarre/arrête Dovecot, teste les 2 modes TLS)
tests/dovecot/smoke.sh
```

Via Meson :

```sh
meson compile -C build dovecot        # = run.sh (avant-plan)
meson compile -C build dovecot-smoke  # = smoke.sh
```

Essais manuels une fois le serveur lancé :

```sh
# TLS implicite
build/tests/test-managesieve --host localhost --port 4191 \
    --user testuser --password testpass
# STARTTLS
build/tests/test-managesieve --host localhost --port 4190 --starttls \
    --user testuser --password testpass --check monscript.sieve
```

## Fichiers

| Fichier            | Rôle                                                          |
|--------------------|-------------------------------------------------------------|
| `dovecot.conf.in`  | Gabarit de configuration (jetons `@FIXTURE_DIR@`, `@UID@`…) |
| `run.sh`           | Génère cert + conf + base utilisateurs, lance Dovecot       |
| `smoke.sh`         | LIST→CHECK→PUT→GET→SETACTIVE ""→DELETE, en TLS implicite ET STARTTLS |
| `run/`             | État runtime (cert, conf générée, logs, sockets) — ignoré   |
| `mail/`            | Maildirs + scripts Sieve des utilisateurs — ignoré          |

## À faire — portage Dovecot 2.4

Le devcontainer est passé à Debian *trixie*, qui livre **Dovecot 2.4** (au lieu
de 2.3). Le format de configuration a changé en profondeur : `dovecot.conf.in`
est encore en syntaxe 2.3 et doit être porté puis vérifié contre le vrai
Dovecot 2.4 (blocs `passdb`/`userdb` nommés, `mail_driver` + `mail_path`,
`ssl_server_cert_file`/`ssl_server_key_file`/`ssl_server_dh_file`, suppression
du bloc `plugin {}` remplacé par `sieve { }` côté Pigeonhole,
`auth_allow_cleartext` à la place de `disable_plaintext_auth`). Les tests sans
réseau (`meson test`) ne sont pas concernés.

## Notes

- `run.sh` lance le maître Dovecot via `sudo` (le devcontainer l'autorise
  sans mot de passe). Les uid/gid des maildirs sont ceux de l'utilisateur
  courant, donc les fichiers restent accessibles sans `sudo`.
- `base_dir` (`run/state/`) est volontairement distinct de l'emplacement de
  la conf générée (`run/dovecot.conf`) : Dovecot recopie sa conf dans
  `base_dir/dovecot.conf`, ce qui créerait une boucle de lien symbolique si
  les deux coïncidaient.
- Le journal (`run/dovecot.log`) contient des lignes
  `t_readlink(...dovecot.conf) failed: ... Invalid argument` : artefact connu
  de Dovecot 2.3 quand la conf est sur un montage overlay (le cas du
  workspace du devcontainer). Sans effet sur le fonctionnement.
- Dovecot refuse `DELETESCRIPT` sur le script **actif** → `smoke.sh` fait un
  `SETACTIVE ""` (`test-managesieve --deactivate`) juste avant.
- `dovecot-core`, `dovecot-managesieved`, `dovecot-sieve` et `openssl` sont
  installés par le `Dockerfile` du devcontainer.
