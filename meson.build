project('evolution-sieve-filters', 'c',
  version: '0.1.0',
  default_options: ['c_std=gnu11', 'warning_level=2'])

i18n = import('i18n')
gettext_package = meson.project_name()

# Translations (gettext): only the Evolution module's user-visible
# strings (menus, dialog, visual editor, account page) --
# module-sieve-filters.c, sieve-editor-dialog.c, sieve-config-page.c,
# sieve-rule-editor.c (see po/POTFILES.in). The protocol library
# (sieve-managesieve-client, sieve-sasl, sieve-account, sieve-secret,
# sieve-config, sieve-srv, sieve-model) deliberately stays outside
# gettext: it's tested on its own via CLIs that aren't meant to be
# translated, and its error messages (log / GError) stay in English,
# like the comments (see AGENTS.md).
add_project_arguments(
  '-DGETTEXT_PACKAGE="' + gettext_package + '"',
  '-DLOCALEDIR="' + get_option('prefix') / get_option('localedir') + '"',
  language: 'c')

# Dependencies: GLib/GIO provide everything needed to speak ManageSieve
# over TCP+TLS without a third-party library. evolution-shell and
# evolution-mail give access to the extension points (EModule/EExtension).
glib_dep    = dependency('glib-2.0', version: '>=2.66')
gio_dep     = dependency('gio-2.0')
gtk_dep     = dependency('gtk+-3.0', required: false)

# GNU SASL: the ManageSieve client's strong mechanisms (SCRAM-SHA-1/256,
# CRAM-MD5, LOGIN, PLAIN). OAUTHBEARER/XOAUTH2 are built by hand in
# src/sieve-sasl.c (libgsasl doesn't provide them). Required: the client
# library depends on it, independently of Evolution.
gsasl_dep   = dependency('libgsasl', version: '>=1.10')

# libsecret: storing the ManageSieve password in the keyring (Secret
# Service) instead of a volatile field. src/sieve-secret.c depends only
# on GLib/GIO: the small "sieve-secret" lib is built and tested
# independently of Evolution, like the client and the model.
secret_dep  = dependency('libsecret-1', version: '>=0.20')

# libkrb5: only used to probe, at runtime, whether the process's Kerberos
# credential cache holds a usable ticket (krb5_cc_get_principal), so
# automatic SASL negotiation only picks GSSAPI when it stands a real
# chance of working. Optional: the GSSAPI mechanism itself is already
# provided by libgsasl on its own regardless of this dependency — without
# it, GSSAPI just stays out of automatic negotiation but remains usable by
# forcing it explicitly (--mech GSSAPI). (Not libgssglue: its GSSAPI/krb5
# dispatch turned out to be broken against a modern MIT krb5 — see the
# comment above gssapi_ticket_available() in sieve-sasl.c.)
krb5_dep = dependency('mit-krb5', required: false)
if not krb5_dep.found()
  warning('mit-krb5.pc not found: SASL GSSAPI will still work if forced ' +
          'explicitly (--mech GSSAPI), but will never be picked by ' +
          'automatic negotiation (no way to probe for a Kerberos ticket).')
endif

# WARNING: these .pc files only exist if evolution-dev /
# evolution-data-server-dev (Debian/Ubuntu names) are installed. On
# Fedora: evolution-devel.
evo_shell_dep = dependency('evolution-shell-3.0', required: false)
evo_mail_dep  = dependency('evolution-mail-3.0', required: false)
camel_dep     = dependency('camel-1.2', required: false)
# libedataserver: reading account settings (ESourceRegistry /
# ESourceAuthentication) to pre-fill the dialog. Like camel_dep, only
# required for the Evolution module.
eds_dep       = dependency('libedataserver-1.2', required: false)

if not evo_shell_dep.found()
  warning('evolution-shell-3.0.pc not found: the module cannot be ' +
          'built until Evolution\'s development headers are ' +
          'installed. The sieve-managesieve-client library can ' +
          'still be tested on its own (see tests/).')
endif

subdir('src')
subdir('po')
subdir('tests')
