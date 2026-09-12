# Bibliothèque protocole, indépendante d'Evolution : testable seule.
sieve_client_lib = static_library('sieve-managesieve-client',
  'sieve-managesieve-client.c',
  'sieve-sasl.c',
  dependencies: [glib_dep, gio_dep, gsasl_dep])

sieve_client_dep = declare_dependency(
  link_with: sieve_client_lib,
  dependencies: [glib_dep, gio_dep, gsasl_dep],
  include_directories: include_directories('.'))

# Modèle de règles Sieve (critères / actions) + (dé)sérialisation vers un
# script Sieve. GLib pur, aucune dépendance GTK ni Evolution : testable
# seul (voir tests/test-sieve-model).
sieve_model_lib = static_library('sieve-model',
  'sieve-model.c',
  dependencies: [glib_dep])

sieve_model_dep = declare_dependency(
  link_with: sieve_model_lib,
  dependencies: [glib_dep],
  include_directories: include_directories('.'))

# Auto-découverte du serveur ManageSieve par enregistrement DNS SRV
# `_sieve._tcp` (RFC 5804 §3). GLib/GIO seul, aucune dépendance GTK ni
# Evolution : testable seul (voir tests/test-sieve-srv).
sieve_srv_lib = static_library('sieve-srv',
  'sieve-srv.c',
  dependencies: [glib_dep, gio_dep])

sieve_srv_dep = declare_dependency(
  link_with: sieve_srv_lib,
  dependencies: [glib_dep, gio_dep],
  include_directories: include_directories('.'))

# Rangement du mot de passe ManageSieve dans le trousseau (libsecret).
# GLib/GIO + libsecret, aucune dépendance GTK ni Evolution : testable seul
# (voir tests/test-sieve-secret + tests/secret/smoke.sh).
sieve_secret_lib = static_library('sieve-secret',
  'sieve-secret.c',
  dependencies: [glib_dep, gio_dep, secret_dep])

sieve_secret_dep = declare_dependency(
  link_with: sieve_secret_lib,
  dependencies: [glib_dep, gio_dep, secret_dep],
  include_directories: include_directories('.'))

# Préférences du greffon (dernier compte choisi + un profil de connexion
# par compte) rangées dans un GKeyFile sous $XDG_CONFIG_HOME. GLib pur,
# aucune dépendance GTK ni Evolution : testable seul (tests/test-sieve-config).
sieve_config_lib = static_library('sieve-config',
  'sieve-config.c',
  dependencies: [glib_dep])

sieve_config_dep = declare_dependency(
  link_with: sieve_config_lib,
  dependencies: [glib_dep],
  include_directories: include_directories('.'))

# Le module Evolution proprement dit ne se construit que si les headers
# d'Evolution sont présents.
if evo_shell_dep.found() and evo_mail_dep.found()
  shared_module('module-sieve-filters',
    'module-sieve-filters.c',
    'sieve-editor-dialog.c',
    'sieve-config-page.c',
    'sieve-rule-editor.c',
    'sieve-account.c',
    dependencies: [glib_dep, gio_dep, gtk_dep, evo_shell_dep, evo_mail_dep,
                   camel_dep, eds_dep, sieve_client_dep, sieve_model_dep,
                   sieve_secret_dep, sieve_config_dep],
    install: true,
    install_dir: evo_shell_dep.get_variable(pkgconfig: 'moduledir'))
endif
