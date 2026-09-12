# tests/secret/ — trousseau jetable

`smoke.sh` exerce `src/sieve-secret.[ch]` (rangement du mot de passe
ManageSieve dans le Secret Service via **libsecret**) de bout en bout,
**sans toucher au trousseau de l'utilisateur** :

- un `dbus-run-session` isole un bus de session neuf ;
- un `gnome-keyring-daemon` jetable y est monté, avec
  `XDG_DATA_HOME` / `XDG_RUNTIME_DIR` sous `tests/secret/run/`, déverrouillé
  par un mot de passe bidon ;
- `build/tests/test-sieve-secret` est relancé dans cet environnement avec
  `SIEVE_SECRET_REQUIRE_SERVICE=1` : sans service joignable le test
  **échoue** (au lieu de se mettre en « skip » comme sous `meson test`).

```sh
meson compile -C build test-sieve-secret
tests/secret/smoke.sh                 # ou : meson compile -C build secret-smoke
```

Dépendances (déjà dans l'image du devcontainer) : `dbus`,
`gnome-keyring`, `libsecret-tools` n'est pas requis.

## Sans le smoke : `meson test`

`meson test -C build sieve-secret` lance le même binaire mais **sans**
Secret Service : il détecte l'absence de service et se met en « skip »
proprement. C'est voulu — le CI sans session D-Bus reste vert, le smoke
couvre le vrai aller-retour.

## Rappel : ce que le greffon range dans le trousseau

Schéma `net.ipocus.evolution.SieveFilters`, une entrée par compte
ManageSieve, indexée par les attributs `protocol=managesieve`, `host`,
`port`, `user`. C'est un schéma propre au greffon : on ne partage pas
l'entrée du compte IMAP gérée par evolution-data-server.
