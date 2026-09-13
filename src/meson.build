# Protocol library, independent of Evolution: testable on its own.
sieve_client_deps = [glib_dep, gio_dep, gsasl_dep]
sieve_client_c_args = []
if krb5_dep.found()
  sieve_client_deps += krb5_dep
  sieve_client_c_args += '-DHAVE_KRB5=1'
endif

sieve_client_lib = static_library('sieve-managesieve-client',
  'sieve-managesieve-client.c',
  'sieve-sasl.c',
  c_args: sieve_client_c_args,
  dependencies: sieve_client_deps)

sieve_client_dep = declare_dependency(
  link_with: sieve_client_lib,
  dependencies: sieve_client_deps,
  include_directories: include_directories('.'))

# Sieve rule model (criteria / actions) + (de)serialization to a Sieve
# script. Pure GLib, no GTK nor Evolution dependency: testable on its
# own (see tests/test-sieve-model).
sieve_model_lib = static_library('sieve-model',
  'sieve-model.c',
  dependencies: [glib_dep])

sieve_model_dep = declare_dependency(
  link_with: sieve_model_lib,
  dependencies: [glib_dep],
  include_directories: include_directories('.'))

# ManageSieve server auto-discovery via the `_sieve._tcp` DNS SRV record
# (RFC 5804 §3). GLib/GIO only, no GTK nor Evolution dependency:
# testable on its own (see tests/test-sieve-srv).
sieve_srv_lib = static_library('sieve-srv',
  'sieve-srv.c',
  dependencies: [glib_dep, gio_dep])

sieve_srv_dep = declare_dependency(
  link_with: sieve_srv_lib,
  dependencies: [glib_dep, gio_dep],
  include_directories: include_directories('.'))

# Storing the ManageSieve password in the keyring (libsecret).
# GLib/GIO + libsecret, no GTK nor Evolution dependency: testable on its
# own (see tests/test-sieve-secret + tests/secret/smoke.sh).
sieve_secret_lib = static_library('sieve-secret',
  'sieve-secret.c',
  dependencies: [glib_dep, gio_dep, secret_dep])

sieve_secret_dep = declare_dependency(
  link_with: sieve_secret_lib,
  dependencies: [glib_dep, gio_dep, secret_dep],
  include_directories: include_directories('.'))

# Plugin preferences (last account chosen + one connection profile per
# account) stored in a GKeyFile under $XDG_CONFIG_HOME. Pure GLib, no
# GTK nor Evolution dependency: testable on its own (tests/test-sieve-config).
sieve_config_lib = static_library('sieve-config',
  'sieve-config.c',
  dependencies: [glib_dep])

sieve_config_dep = declare_dependency(
  link_with: sieve_config_lib,
  dependencies: [glib_dep],
  include_directories: include_directories('.'))

# The actual Evolution module only builds if Evolution's headers are
# present.
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
    name_prefix: '',
    install: true,
    install_dir: evo_shell_dep.get_variable(pkgconfig: 'moduledir'))
endif
