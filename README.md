# evolution-sieve-filters

Greffon (module) pour **Evolution** permettant de gérer des filtres
**Sieve** exécutés côté serveur, via le protocole **ManageSieve**
([RFC 5804](https://www.rfc-editor.org/rfc/rfc5804)). Il ajoute un éditeur
de règles (visuel et texte brut) directement dans Evolution, sans passer
par un outil externe (Roundcube, Nextcloud Mail…) pour gérer ses filtres
serveur.

> [!WARNING]
> Projet en développement actif, pas encore audité en usage réel à
> grande échelle. Le client ManageSieve et le modèle de règles sont
> testés de bout en bout (voir [Tests](#tests-et-développement)) ; la
> partie interface graphique (dialogue, page de compte) n'a été vérifiée
> que contre un Evolution 3.56 réel de façon limitée — voir
> [Limites connues](#limites-connues). Faites une sauvegarde de vos
> scripts Sieve existants avant de les modifier avec ce greffon.

## Fonctionnalités

- **Éditeur visuel** de règles façon « Filtres de messages » d'Evolution :
  liste de règles, critères (expéditeur, destinataire, sujet, en-tête,
  taille, corps…) et actions (classer, rediriger, marquer, supprimer,
  arrêter le traitement…) construits sans écrire de script à la main.
- **Éditeur texte brut** en repli, pour tout ce que l'éditeur visuel ne
  représente pas encore (`vacation`, règles écrites à la main, scripts
  d'autres outils…) — ces règles sont conservées **verbatim** et affichées
  verrouillées côté éditeur visuel plutôt que d'être perdues ou déformées.
- **Intégration Evolution** : entrée de menu *Édition → Filtres Sieve…*,
  entrée de menu contextuel *(clic droit sur un message) → Créer → Créer un
  filtre Sieve…* (pré-remplit une règle sur l'expéditeur), et une page
  **« Filtres Sieve »** dans l'éditeur de comptes pour régler la connexion
  par compte.
- **Mot de passe au trousseau** (libsecret / Secret Service), jamais stocké
  en clair ; à défaut, reprise du mot de passe IMAP déjà connu d'Evolution.
- **Authentification complète** : PLAIN, LOGIN, CRAM-MD5, SCRAM-SHA-1,
  SCRAM-SHA-256, ainsi qu'OAUTHBEARER / XOAUTH2 pour les comptes OAuth2
  (Gmail, Office 365…), jeton obtenu et rafraîchi via
  evolution-data-server.
- **TLS implicite ou StartTLS**, capacités serveur relues après négociation
  TLS pour ne jamais faire confiance à une réponse en clair.
- Cœur du protocole (`sieve-managesieve-client`) **indépendant
  d'Evolution** (GLib/GIO pur) : testable seul, réutilisable dans un autre
  projet.

## Prérequis

- Evolution ≥ 3.54 avec ses en-têtes de développement (`evolution-dev` /
  `evolution-devel`, `evolution-data-server-dev`).
- Un serveur IMAP exposant **ManageSieve** (Dovecot + Pigeonhole est le
  seul testé en pratique ; le client suit la RFC et devrait donc
  fonctionner contre Cyrus timsieved et les autres implémentations, mais
  cela n'a pas été vérifié).
- GLib/GTK+3, `libgsasl` (≥ 1.10) et `libsecret` (≥ 0.20).

## Installation

### Depuis les sources

```sh
meson setup build
ninja -C build
sudo meson install -C build
```

`meson install` place le module dans le répertoire de modules
d'Evolution (déduit de `evolution-shell-3.0.pc`). Redémarrez Evolution
ensuite (`evolution --force-shutdown` puis relancez-le).

Si `evolution-shell-3.0.pc` / `evolution-mail-3.0.pc` sont absents, Meson
ne construit que la bibliothèque cliente ManageSieve et les tests — utile
pour développer/tester le protocole sans Evolution installé.

### Paquet Debian/Ubuntu

Un squelette de paquetage est fourni dans [`debian/`](debian/) :

```sh
sudo apt build-dep .
dpkg-buildpackage -us -uc -b
sudo apt install ../evolution-sieve-filters_*.deb
```

## Utilisation

1. **Édition → Comptes**, choisissez un compte IMAP, onglet **Filtres
   Sieve** : renseignez serveur/port ManageSieve (StartTLS ou TLS
   implicite) et le nom d'utilisateur ; cochez éventuellement
   « Se connecter automatiquement à l'ouverture ». Le mot de passe est pris
   au trousseau ou, à défaut, sur celui du compte IMAP.
2. **Édition → Filtres Sieve…** ouvre le dialogue : choisissez le compte
   dans le menu déroulant, la connexion et le chargement du script actif
   se font automatiquement.
3. Construisez vos règles dans l'onglet visuel, ou basculez en texte brut
   pour un contrôle total. **Enregistrer et activer** envoie le script au
   serveur (`CHECKSCRIPT` puis `PUTSCRIPT`/`SETACTIVE`).
4. Depuis la liste des messages, clic droit sur un message → **Créer →
   Créer un filtre Sieve…** pré-remplit une règle sur son expéditeur.

## Sécurité

- Aucun mot de passe n'est jamais écrit en clair sur disque. Il est rangé
  dans le trousseau système (Secret Service via `libsecret`), sous un
  schéma propre au greffon (`net.ipocus.evolution.SieveFilters`).
- Les capacités serveur annoncées avant `STARTTLS` ne sont **jamais**
  réutilisées après la négociation TLS (protection contre l'injection de
  commandes en clair).
- SASL fort par défaut (négociation automatique du meilleur mécanisme
  commun ; préférence pour SCRAM). SCRAM-*-PLUS (channel binding) et
  GSSAPI ne sont pas encore supportés.

## Architecture

Le protocole ManageSieve, la couche SASL et le modèle de règles Sieve sont
des bibliothèques GLib/GIO autonomes, sans dépendance à Evolution, et
testées indépendamment. Le détail fichier par fichier (rôle de chaque
module, conventions de code, pièges connus) est documenté dans
[`AGENTS.md`](AGENTS.md), destiné aussi bien à un⋅e contributeur⋅rice
humain⋅e qu'à un assistant de code.

## Tests et développement

```sh
meson setup build
meson test -C build          # tests unitaires (SASL, modèle de règles,
                              # config, trousseau — sans réseau)
tests/dovecot/smoke.sh        # bout-en-bout contre un Dovecot jetable
tests/secret/smoke.sh         # aller-retour trousseau réel (gnome-keyring jetable)
```

Détails complets (fixtures, options de `test-managesieve`, procédure pour
un vrai serveur…) : voir [`AGENTS.md`](AGENTS.md).

## Limites connues

- Éditeur visuel : seuls les scripts produits par l'éditeur lui-même (ou
  d'une structure équivalente simple) sont pleinement éditables. Tout le
  reste (scripts écrits à la main, `vacation`, blocs générés par d'autres
  outils, extensions non couvertes) est conservé **verbatim** en règle «
  opaque », modifiable seulement en texte brut. Pas encore de vrai parseur
  RFC 5228 complet.
- Pas de réordonnancement des règles par glisser-déposer, ni d'activation
  / désactivation individuelle.
- La partie interface graphique (dialogue, page de compte) n'a été testée
  que partiellement contre un Evolution réel ; le reste est vérifié à la
  compilation/liaison dans cet environnement de développement, dépourvu
  d'Evolution exécutable.
- Un seul serveur ManageSieve (Dovecot + Pigeonhole) a été testé en
  pratique.

## Contribuer

Les contributions sont bienvenues (issues, pull requests). Avant de
proposer un changement touchant au protocole ManageSieve, à SASL ou au
modèle de règles, lancez `meson test` puis `tests/dovecot/smoke.sh` —
voir [`AGENTS.md`](AGENTS.md) pour le détail des conventions et des
pièges déjà rencontrés.

## Licence

[GPL-3.0-or-later](LICENSE)
