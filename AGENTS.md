# AGENTS.md — repères pour un assistant IA

Destiné à tout assistant de code (Claude Code, Copilot, Cursor, Aider…).
Complète le `README.md` (vision, « Ce qui manque »), ne le remplace pas.

## En deux phrases

Greffon Evolution pour gérer des filtres **Sieve** côté serveur via
**ManageSieve** (RFC 5804). Le cœur réutilisable est
`src/sieve-managesieve-client.[ch]` : un client ManageSieve autonome en
GLib/GIO pur, **sans dépendance à Evolution**, donc testable seul.

## Construire

Toutes les dépendances sont déjà dans l'image du devcontainer
(`.devcontainer/Dockerfile`).

```sh
meson setup build          # (déjà fait par postCreateCommand)
meson compile -C build     # bibliothèque + module + tests/test-managesieve
```

- Le **module Evolution** (`src/module-sieve-filters.c`) ne se compile que si
  `evolution-shell-3.0.pc` / `evolution-mail-3.0.pc` sont présents
  (`evolution-dev`). Sinon Meson ne bâtit que la lib cliente + les tests.
- **`libgsasl-dev` est requis** (pas seulement pour le module) : la lib cliente
  `sieve-managesieve-client` en dépend via `src/sieve-sasl.c`. Déjà dans le
  Dockerfile. Absent → `meson setup` échoue sur `dependency('libgsasl')`.
- **`libsecret-1-dev` est requis** de la même façon : `src/sieve-secret.c`
  (rangement du mot de passe au trousseau) en dépend. Déjà dans le Dockerfile
  (aussi tiré en transitif par `evolution-dev`, mais on l'installe explicitement).
  Absent → `meson setup` échoue sur `dependency('libsecret-1')`.
- **Piège de paquet** : `evolution-dev` livre
  `/usr/include/evolution/e-util/e-contact-store.h` qui fait
  `#include <libebook/libebook.h>` **sans en dépendre**. Il faut
  `libebook1.2-dev` (ajouté au Dockerfile). Symptôme si absent :
  `fatal error: libebook/libebook.h: No such file or directory`.

## Tester le client ManageSieve (sans Evolution)

`tests/test-managesieve` est un CLI qui appelle directement l'API du client.
`tests/dovecot/` fournit un **Dovecot + Pigeonhole jetable** (aucun compte
système, tout sous `tests/dovecot/`).

```sh
meson test -C build                       # tests unitaires SASL (tests/test-sasl,
                                          # sans réseau) : négociation + OAuth

tests/dovecot/smoke.sh                    # bout-en-bout : monte Dovecot,
                                          # teste TLS implicite ET STARTTLS,
                                          # LIST/CHECK/PUT/GET/SETACTIVE/DELETE,
                                          # puis chaque mécanisme SASL + auto
# ou : meson compile -C build dovecot-smoke

tests/dovecot/run.sh --daemon             # serveur persistant
tests/dovecot/run.sh --stop
```

### Trousseau (`sieve-secret`)

`meson test` lance `tests/test-sieve-secret` : sans Secret Service joignable
il se met en **skip** (pas d'échec — le CI sans session D-Bus reste vert).
Le vrai aller-retour `store`/`lookup`/`clear` est couvert par
`tests/secret/smoke.sh` (`meson compile -C build secret-smoke`) : il monte un
**gnome-keyring jetable** sous `dbus-run-session` (tout sous `tests/secret/run/`,
trousseau de session de l'utilisateur jamais touché) et relance le binaire avec
`SIEVE_SECRET_REQUIRE_SERVICE=1` (dans ce mode, l'absence de service = échec).
Voir `tests/secret/README.md`.

- SASL : `--mech PLAIN|LOGIN|CRAM-MD5|SCRAM-SHA-1|SCRAM-SHA-256|OAUTHBEARER|
  XOAUTH2` force un mécanisme ; sans `--mech`, négociation auto (préfère SCRAM).
  `--oauth2-token` (ou `$SIEVE_OAUTH2_TOKEN`) pour OAUTHBEARER/XOAUTH2.
  Le Dovecot de test annonce `plain login cram-md5 scram-sha-1 scram-sha-256`.

- **`localhost:4190` = STARTTLS**, **`localhost:4191` = TLS implicite**.
- Identifiants **`testuser` / `testpass`**.
- `ssl = required` : pas d'auth en clair.
- Le cert auto-signé (SAN `localhost`) est déposé dans
  `/usr/local/share/ca-certificates/` par `run.sh` : la validation TLS du
  client passe **sans** mode « insecure » (le client n'en a pas).

Contre un vrai serveur : `test-managesieve --host … --user … [--starttls] [--port N]`.

## Pièges rencontrés (ne pas re-déboguer)

| Symptôme | Cause / correctif |
|---|---|
| `libebook/libebook.h: No such file` | installer `libebook1.2-dev` (cf. plus haut) |
| Dovecot : `Too many levels of symbolic links` sur `dovecot.conf` | `base_dir` ne doit pas être le dossier qui contient la conf générée → il est mis dans `tests/dovecot/run/state/` |
| `dovecot.log` : `t_readlink(...dovecot.conf) failed: Invalid argument` | artefact Dovecot 2.3 sur overlayfs (workspace devcontainer). **Inoffensif.** |
| `NO ... "Cannot delete the active Sieve script."` | Dovecot refuse `DELETESCRIPT` sur le script actif → `SETACTIVE ""` d'abord (`test-managesieve --deactivate`) |
| `NO ... PUTSCRIPT: Invalid arguments` observé une fois | c'était l'état cassé par la boucle de symlink ci-dessus, pas un bug du client. Le cadrage `{N+}` du client est correct. |
| Handshake TLS `Connection reset by peer` au démarrage | le process `config` de Dovecot a planté (conf invalide) → lire `tests/dovecot/run/dovecot.log` |
| `test-sieve-secret` : `SKIP ... Failed to execute child process "dbus-launch"` | normal hors session D-Bus : libsecret ne peut pas démarrer de bus. Le test se met en skip. Pour le vrai aller-retour : `tests/secret/smoke.sh`. |
| smoke trousseau : `Cannot create an item in a locked collection` | l'alias Secret Service `default` pointe sur une collection verrouillée (prompteur graphique indispo en headless). `smoke.sh` pré-désigne le trousseau `login` (créé déverrouillé par `--unlock`) via `keyrings/default`. Si ça persiste : `pkill -9 gnome-keyring-daemon` (démon résiduel d'un run précédent) puis relancer. |
| Menu contextuel « Créer un filtre Sieve » : rien, `WARNING sieve: le contenu de la vue (EMailShellContent) n'est pas un EMailReader` | dans Evolution 3.56, `EMailShellContent` **n'implémente plus** l'interface `EMailReader` (c'est la vue interne `EMailPanedView` qui la porte, widget descendant). `module-sieve-filters.c` la retrouve avec `sieve_find_mail_reader()` (parcours récursif des widgets sous le `EShellContent`) — pas d'en-tête privé `e-mail-shell-content.h` requis. |
| Fusion `.eui` dans `mail-message-popup` (menu contextuel liste des messages) | `e_ui_manager_add_actions_with_eui_data` reste réservé au fragment `main-menu` (enregistre aussi les actions) ; le fragment du menu contextuel est fusionné à part via `e_ui_parser_merge_data(e_ui_manager_get_parser(ui_manager), …)` + `e_ui_manager_changed()`, pour pouvoir journaliser un `GError`. Cible : placeholder `mail-conversion-actions` du sous-menu `mail-create-menu` ; repli automatique sur `mail-message-popup-common-actions` si refusée. |

## Disposition

```
src/sieve-managesieve-client.[ch]  client ManageSieve réutilisable (GLib/GIO)
src/sieve-sasl.[ch]                couche SASL (négociation + mécanismes),
                                   libgsasl + OAUTHBEARER/XOAUTH2 maison
src/sieve-model.[ch]               modèle règles Sieve (critères/actions,
                                   + règles « opaques » conservées verbatim) +
                                   (dé)sérialisation ; GLib pur, testable seul
src/sieve-secret.[ch]              rangement du mot de passe au trousseau
                                   (libsecret) ; GLib/GIO pur, testable seul
src/sieve-account.[ch]             lecture des comptes Evolution ; repérage
                                   OAuth2 + jeton d'accès via EDS
                                   (e_source_get_oauth2_access_token_sync) ;
                                   relecture du mot de passe du compte via EDS
                                   (e_source_credentials_provider_lookup_sync) ;
                                   DÉPEND de libedataserver (bâti avec le module)
src/sieve-config.[ch]              préférences GKeyFile (XDG_CONFIG_HOME) :
                                   un profil de connexion PAR COMPTE
                                   ([account <UID>]) + profil [manual] +
                                   [state] last-account ; migration de
                                   l'ancien [connection] ; GLib pur, testable
src/sieve-config-page.[ch]         page « Filtres Sieve » de l'éditeur de
                                   comptes (EMailConfigPage + EExtension sur
                                   E_TYPE_MAIL_CONFIG_NOTEBOOK) : paramètres
                                   de connexion du compte (hôte/port/user/TLS)
                                   + auto-connect, écrits dans sieve-config ;
                                   section Authentification : champ mot de
                                   passe MASQUÉ par défaut (mot de passe pris
                                   au trousseau) ; le bouton « Oublier le mot
                                   de passe » efface l'entrée trousseau
                                   (thread détaché, sans effet si rien) ET
                                   révèle le champ ; au commit, mot de passe
                                   saisi rangé au trousseau (sieve-secret,
                                   thread détaché) puis champ remasqué ;
                                   OAuth2 : champ + bouton masqués ; DÉPEND
                                   d'evolution-mail + libedataserver (bâti
                                   avec le module)
src/sieve-rule-editor.[ch]         widget GTK « éditeur visuel » (GtkBox)
src/sieve-editor-dialog.[ch]       dialogue GTK autonome (GtkStack visuel/texte)
src/module-sieve-filters.c         point d'entrée EModule : entrée de menu
                                   Édition → Filtres Sieve… + entrée de menu
                                   contextuel Message → Créer → Créer un
                                   filtre Sieve… (règle pré-remplie sur
                                   l'expéditeur, via
                                   sieve_editor_dialog_new_with_seed) + page
                                   éditeur de comptes (sieve-config-page)
tests/test-managesieve.c           CLI de test du client (réseau)
tests/test-sasl.c                  tests unitaires de sieve-sasl (sans réseau)
tests/test-sieve-model.c           tests unitaires de sieve-model (sans réseau)
tests/test-sieve-secret.c          aller-retour trousseau ; skip sans Secret Service
tests/test-sieve-config.c          profils par compte + [manual] +
                                   last-account + migration [connection]
                                   (XDG_CONFIG_HOME temporaire, sans réseau)
tests/test-timeout.c               délai réseau + GCancellable du client
                                   (GSocketService local muet, sans réseau)
tests/secret/                      trousseau jetable (gnome-keyring + dbus-run-session)
tests/dovecot/                     fixture serveur (voir son README.md)
```

## Conventions

- C `gnu11`, `warning_level=2`, style GLib/GObject (`g_autoptr` bienvenu,
  `GError **` partout, `GCancellable` propagé même si pas encore exploité).
- **Commentaires et messages utilisateur en anglais** (cohérence avec
  l'existant ; ce dépôt de documentation reste rédigé en français, mais
  le code source — commentaires et chaînes visibles par l'utilisateur —
  est en anglais).
- Pas de nouvelle dépendance tierce sans raison forte : GLib/GIO couvrent
  TCP+TLS. `libgsasl` (SASL fort) et `libsecret` (trousseau) sont désormais
  en place ; ne rien ajouter d'autre à la légère.
- `sieve-secret.c` : couche mince au-dessus de l'API « simple password » de
  libsecret, **synchrone** (comme le client), aucune dépendance GTK ni
  Evolution — garder ainsi. Schéma **propre au greffon** : on ne réutilise pas
  l'entrée du compte IMAP d'evolution-data-server (mot de passe possiblement
  distinct, et eds ne l'expose pas hors `ESource`). Le dialogue appelle
  `sieve_secret_*` **depuis le thread de travail**, jamais sur la boucle GTK.
- Le protocole ET les mécanismes SASL ont été validés **contre un vrai
  Dovecot** : si tu touches au parseur de réponses, au cadrage des literals
  ou à `sieve-sasl.c`, relance `meson test` puis `smoke.sh`.
- SASL : la négociation/`sieve-sasl.c` est autonome (aucune dépendance
  Evolution) — garder ainsi. OAUTHBEARER/XOAUTH2 sont construits à la main
  (libgsasl ne les fournit pas) ; le client **consomme** un jeton, il ne
  l'acquiert pas.

## À faire (détaillé dans README.md « Ce qui manque »)

Intégration menu Evolution · lecture des réglages de compte
(`CamelSettings`/`ESource` — les paramètres de connexion ManageSieve ont
déjà une page dédiée dans l'éditeur de comptes, `sieve-config-page` ;
reste la lecture fine de `CamelSettings`, p. ex. sécurité IMAP →
pré-cocher « TLS implicite ») · ~~mots de passe via `libsecret`~~ (fait :
`sieve-secret`) · ~~acquisition du jeton OAuth2~~ (fait :
`sieve_account_dup_oauth2_token()` →
`e_source_get_oauth2_access_token_sync()`, branché sur
`SieveManageSieveAuth.oauth2_token` par `sieve-editor-dialog.c` ;
acquisition + rafraîchissement délégués à EDS) · SCRAM-*-PLUS (channel
binding TLS) et GSSAPI · durcissement du parseur de
réponses · éditeur visuel conditions/actions : **base en place**
(`sieve-model` + `sieve-rule-editor`). Les constructions non représentables
(scripts écrits à la main, blocs Nextcloud Mail / Roundcube, `vacation`…)
sont désormais **conservées verbatim** en règles « opaques »
(`SieveRule.opaque` / `.raw`) : l'éditeur visuel les affiche verrouillées
(cadenas, lecture seule), l'onglet texte brut reste seul à pouvoir les
modifier. Reste à faire : vrai parseur RFC 5228 pour les rendre éditables,
`variables`, réordonnancement et activation des règles · annulation réseau
côté UI.

`sieve-model` / `sieve-rule-editor` sont indépendants d'Evolution — garder
ainsi. L'éditeur ignore donc d'où viennent les dossiers : c'est
`sieve-editor-dialog.c` (côté Evolution) qui énumère le `CamelStore` du
compte choisi (`camel_store_get_folder_info_sync`, en cache, sur un thread)
et les injecte via `sieve_rule_editor_set_mailboxes()` pour que l'action
`fileinto` propose une liste déroulante éditable. Si tu touches au
(dé)sérialiseur, relance `meson test` :
`tests/test-sieve-model.c` vérifie que l'aller-retour modèle ⇄ script est
stable au caractère près, y compris pour les règles opaques (texte d'un
autre outil recopié tel quel), et que seul un script lexicalement cassé
(accolade / chaîne non fermée) fait encore échouer `sieve_rule_set_parse()`.
