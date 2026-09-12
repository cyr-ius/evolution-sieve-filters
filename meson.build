project('evolution-sieve-filters', 'c',
  version: '0.1.0',
  default_options: ['c_std=gnu11', 'warning_level=2'])

# Dépendances : GLib/GIO fournissent tout ce qu'il faut pour parler
# ManageSieve en TCP+TLS sans bibliothèque tierce. evolution-shell et
# evolution-mail donnent accès aux points d'extension (EModule/EExtension).
glib_dep    = dependency('glib-2.0', version: '>=2.66')
gio_dep     = dependency('gio-2.0')
gtk_dep     = dependency('gtk+-3.0', required: false)

# GNU SASL : mécanismes forts du client ManageSieve (SCRAM-SHA-1/256, CRAM-MD5,
# LOGIN, PLAIN). OAUTHBEARER/XOAUTH2 sont construits à la main dans
# src/sieve-sasl.c (libgsasl ne les fournit pas). Requis : la bibliothèque
# cliente en dépend, indépendamment d'Evolution.
gsasl_dep   = dependency('libgsasl', version: '>=1.10')

# libsecret : rangement du mot de passe ManageSieve dans le trousseau
# (Secret Service) au lieu d'un champ volatil — cf. README.md « Ce qui
# manque », point 2. src/sieve-secret.c n'en dépend que de GLib/GIO :
# la petite lib « sieve-secret » est bâtie et testée indépendamment
# d'Evolution, comme le client et le modèle.
secret_dep  = dependency('libsecret-1', version: '>=0.20')

# ATTENTION : ces .pc n'existent que si evolution-dev / evolution-data-server-dev
# (noms Debian/Ubuntu) sont installés. Sur Fedora : evolution-devel.
evo_shell_dep = dependency('evolution-shell-3.0', required: false)
evo_mail_dep  = dependency('evolution-mail-3.0', required: false)
camel_dep     = dependency('camel-1.2', required: false)
# libedataserver : lecture des réglages de compte (ESourceRegistry /
# ESourceAuthentication) pour pré-remplir le dialogue. Comme camel_dep,
# n'est requis que pour le module Evolution.
eds_dep       = dependency('libedataserver-1.2', required: false)

if not evo_shell_dep.found()
  warning('evolution-shell-3.0.pc introuvable : le module ne pourra pas être ' +
          'compilé tant que les en-têtes de développement d\'Evolution ne ' +
          'sont pas installés. La bibliothèque sieve-managesieve-client peut ' +
          'néanmoins être testée seule (voir tests/).')
endif

subdir('src')
subdir('tests')
