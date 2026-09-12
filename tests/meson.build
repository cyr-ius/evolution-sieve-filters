test_managesieve = executable('test-managesieve',
  'test-managesieve.c',
  dependencies: [glib_dep, gio_dep, sieve_client_dep])

# Tests unitaires de la couche SASL (sans réseau) : lancés par `meson test`.
test_sasl = executable('test-sasl',
  'test-sasl.c',
  dependencies: [glib_dep, sieve_client_dep])
test('sasl', test_sasl)

# Tests unitaires du modèle de règles Sieve (aller-retour modèle ⇄ script,
# rejet des scripts hors du champ de l'éditeur visuel). Sans réseau.
test_sieve_model = executable('test-sieve-model',
  'test-sieve-model.c',
  dependencies: [glib_dep, sieve_model_dep])
test('sieve-model', test_sieve_model)

# Tests unitaires de l'auto-découverte SRV (extraction du domaine). Sans réseau.
test_sieve_srv = executable('test-sieve-srv',
  'test-sieve-srv.c',
  dependencies: [glib_dep, sieve_srv_dep])
test('sieve-srv', test_sieve_srv)

# Aller-retour trousseau (store/lookup/clear via libsecret). Se met en
# « skip » s'il n'y a pas de Secret Service joignable ; tests/secret/smoke.sh
# en monte un jetable et relance ce binaire avec SIEVE_SECRET_REQUIRE_SERVICE.
test_sieve_secret = executable('test-sieve-secret',
  'test-sieve-secret.c',
  dependencies: [glib_dep, sieve_secret_dep])
test('sieve-secret', test_sieve_secret)

# Préférences du dialogue (dernier compte + paramètres de connexion) :
# aller-retour save/load dans un $XDG_CONFIG_HOME temporaire. Sans réseau.
test_sieve_config = executable('test-sieve-config',
  'test-sieve-config.c',
  dependencies: [glib_dep, sieve_config_dep])
test('sieve-config', test_sieve_config)

# Délai d'attente réseau + annulation via GCancellable (point 6). Monte un
# GSocketService local muet : aucun vrai serveur ManageSieve requis, sans
# réseau externe.
test_timeout = executable('test-timeout',
  'test-timeout.c',
  dependencies: [glib_dep, gio_dep, sieve_client_dep])
test('timeout', test_timeout)

# Dovecot + Pigeonhole jetable (TLS implicite, localhost:4190) pour exercer le
# client contre un vrai serveur ManageSieve. Voir tests/dovecot/README.md.
#   meson compile -C build dovecot        # serveur en avant-plan
#   meson compile -C build dovecot-smoke  # test de bout en bout
run_target('dovecot',
  command: [files('dovecot/run.sh')])

run_target('dovecot-smoke',
  command: [files('dovecot/smoke.sh')],
  depends: test_managesieve)

# Trousseau jetable (gnome-keyring sous dbus-run-session) + aller-retour réel
# store/lookup/clear. Voir tests/secret/README.md.
#   meson compile -C build secret-smoke
run_target('secret-smoke',
  command: [files('secret/smoke.sh')],
  depends: test_sieve_secret)
