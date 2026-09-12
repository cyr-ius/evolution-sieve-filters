test_managesieve = executable('test-managesieve',
  'test-managesieve.c',
  dependencies: [glib_dep, gio_dep, sieve_client_dep])

# Unit tests for the SASL layer (no network): run by `meson test`.
test_sasl = executable('test-sasl',
  'test-sasl.c',
  dependencies: [glib_dep, sieve_client_dep])
test('sasl', test_sasl)

# Unit tests for the Sieve rule model (model ⇄ script round-trip,
# rejection of scripts outside the visual editor's scope). No network.
test_sieve_model = executable('test-sieve-model',
  'test-sieve-model.c',
  dependencies: [glib_dep, sieve_model_dep])
test('sieve-model', test_sieve_model)

# Unit tests for SRV auto-discovery (domain extraction). No network.
test_sieve_srv = executable('test-sieve-srv',
  'test-sieve-srv.c',
  dependencies: [glib_dep, sieve_srv_dep])
test('sieve-srv', test_sieve_srv)

# Keyring round-trip (store/lookup/clear via libsecret). Skips if there's
# no reachable Secret Service; tests/secret/smoke.sh brings up a
# disposable one and reruns this binary with SIEVE_SECRET_REQUIRE_SERVICE.
test_sieve_secret = executable('test-sieve-secret',
  'test-sieve-secret.c',
  dependencies: [glib_dep, sieve_secret_dep])
test('sieve-secret', test_sieve_secret)

# Dialog preferences (last account + connection settings): save/load
# round-trip in a temporary $XDG_CONFIG_HOME. No network.
test_sieve_config = executable('test-sieve-config',
  'test-sieve-config.c',
  dependencies: [glib_dep, sieve_config_dep])
test('sieve-config', test_sieve_config)

# Network timeout + cancellation via GCancellable. Brings up a local mute
# GSocketService: no real ManageSieve server required, no external
# network.
test_timeout = executable('test-timeout',
  'test-timeout.c',
  dependencies: [glib_dep, gio_dep, sieve_client_dep])
test('timeout', test_timeout)

# Disposable Dovecot + Pigeonhole (implicit TLS, localhost:4190) to
# exercise the client against a real ManageSieve server. See
# tests/dovecot/README.md.
#   meson compile -C build dovecot        # foreground server
#   meson compile -C build dovecot-smoke  # end-to-end test
run_target('dovecot',
  command: [files('dovecot/run.sh')])

run_target('dovecot-smoke',
  command: [files('dovecot/smoke.sh')],
  depends: test_managesieve)

# Disposable keyring (gnome-keyring under dbus-run-session) + real
# store/lookup/clear round-trip. See tests/secret/README.md.
#   meson compile -C build secret-smoke
run_target('secret-smoke',
  command: [files('secret/smoke.sh')],
  depends: test_sieve_secret)
