# AGENTS.md — pointers for an AI assistant

Intended for any code assistant (Claude Code, Copilot, Cursor, Aider…).
Complements `README.md` (vision, "What's missing"), does not replace it.

## In two sentences

Evolution plugin for managing server-side **Sieve** filters via
**ManageSieve** (RFC 5804). The reusable core is
`src/sieve-managesieve-client.[ch]`: a standalone ManageSieve client in
pure GLib/GIO, **with no dependency on Evolution**, and therefore
testable on its own.

## Building

All dependencies are already in the devcontainer image
(`.devcontainer/Dockerfile`).

```sh
meson setup build          # (already done by postCreateCommand)
meson compile -C build     # library + module + tests/test-managesieve
```

- The **Evolution module** (`src/module-sieve-filters.c`) only builds if
  `evolution-shell-3.0.pc` / `evolution-mail-3.0.pc` are present
  (`evolution-dev`). Otherwise Meson only builds the client lib + tests.
- **`libgsasl-dev` is required** (not just for the module): the client
  lib `sieve-managesieve-client` depends on it via `src/sieve-sasl.c`.
  Already in the Dockerfile. Missing → `meson setup` fails on
  `dependency('libgsasl')`.
- **`libsecret-1-dev` is required** the same way: `src/sieve-secret.c`
  (storing the password in the keyring) depends on it. Already in the
  Dockerfile (also pulled in transitively by `evolution-dev`, but we
  install it explicitly).
  Missing → `meson setup` fails on `dependency('libsecret-1')`.
- **Package gotcha**: `evolution-dev` ships
  `/usr/include/evolution/e-util/e-contact-store.h` which does
  `#include <libebook/libebook.h>` **without depending on it**. You need
  `libebook1.2-dev` (added to the Dockerfile). Symptom if missing:
  `fatal error: libebook/libebook.h: No such file or directory`.

## Testing the ManageSieve client (without Evolution)

`tests/test-managesieve` is a CLI that calls the client's API directly.
`tests/dovecot/` provides a **disposable Dovecot + Pigeonhole** (no
system account, everything under `tests/dovecot/`).

```sh
meson test -C build                       # SASL unit tests (tests/test-sasl,
                                          # no network): negotiation + OAuth

tests/dovecot/smoke.sh                    # end-to-end: brings up Dovecot,
                                          # tests implicit TLS AND STARTTLS,
                                          # LIST/CHECK/PUT/GET/SETACTIVE/DELETE,
                                          # then each SASL mechanism + auto
# or: meson compile -C build dovecot-smoke

tests/dovecot/run.sh --daemon             # persistent server
tests/dovecot/run.sh --stop
```

### Keyring (`sieve-secret`)

`meson test` runs `tests/test-sieve-secret`: without a reachable Secret
Service it **skips** (not a failure — CI without a D-Bus session stays
green). The real `store`/`lookup`/`clear` round-trip is covered by
`tests/secret/smoke.sh` (`meson compile -C build secret-smoke`): it
brings up a **disposable gnome-keyring** under `dbus-run-session`
(everything under `tests/secret/run/`, the user's own session keyring is
never touched) and reruns the binary with
`SIEVE_SECRET_REQUIRE_SERVICE=1` (in that mode, no service present =
failure). See `tests/secret/README.md`.

- SASL: `--mech PLAIN|LOGIN|CRAM-MD5|SCRAM-SHA-1|SCRAM-SHA-256|OAUTHBEARER|
  XOAUTH2` forces a mechanism; without `--mech`, auto negotiation
  (prefers SCRAM). `--oauth2-token` (or `$SIEVE_OAUTH2_TOKEN`) for
  OAUTHBEARER/XOAUTH2. The test Dovecot advertises
  `plain login cram-md5 scram-sha-1 scram-sha-256`.

- **`localhost:4190` = STARTTLS**, **`localhost:4191` = implicit TLS**.
- Credentials: **`testuser` / `testpass`**.
- `ssl = required`: no plaintext auth.
- The self-signed cert (SAN `localhost`) is dropped into
  `/usr/local/share/ca-certificates/` by `run.sh`: the client's TLS
  validation passes **without** an "insecure" mode (the client doesn't
  have one).

Against a real server: `test-managesieve --host … --user … [--starttls] [--port N]`.

## Pitfalls encountered (don't re-debug these)

| Symptom | Cause / fix |
|---|---|
| `libebook/libebook.h: No such file` | install `libebook1.2-dev` (see above) |
| Dovecot: `Too many levels of symbolic links` on `dovecot.conf` | `base_dir` must not be the directory holding the generated conf → it's put in `tests/dovecot/run/state/` |
| `dovecot.log`: `t_readlink(...dovecot.conf) failed: Invalid argument` | Dovecot 2.3 artifact on overlayfs (devcontainer workspace). **Harmless.** |
| `NO ... "Cannot delete the active Sieve script."` | Dovecot refuses `DELETESCRIPT` on the active script → `SETACTIVE ""` first (`test-managesieve --deactivate`) |
| `NO ... PUTSCRIPT: Invalid arguments` seen once | it was state broken by the symlink loop above, not a client bug. The client's `{N+}` literal framing is correct. |
| TLS handshake `Connection reset by peer` on startup | Dovecot's `config` process crashed (invalid conf) → read `tests/dovecot/run/dovecot.log` |
| `test-sieve-secret`: `SKIP ... Failed to execute child process "dbus-launch"` | normal outside a D-Bus session: libsecret can't start a bus. The test skips. For the real round-trip: `tests/secret/smoke.sh`. |
| keyring smoke: `Cannot create an item in a locked collection` | the `default` Secret Service alias points to a locked collection (no graphical prompter available headless). `smoke.sh` pre-designates the `login` keyring (created unlocked via `--unlock`) via `keyrings/default`. If it persists: `pkill -9 gnome-keyring-daemon` (leftover daemon from a previous run) then rerun. |
| "Create Sieve Filter" context menu item: nothing happens, `WARNING sieve: view content (EMailShellContent) is not an EMailReader` | in Evolution 3.56, `EMailShellContent` **no longer implements** the `EMailReader` interface (it's the internal `EMailPanedView` widget, a descendant, that carries it). `module-sieve-filters.c` finds it with `sieve_find_mail_reader()` (recursive widget walk under the `EShellContent`) — no private `e-mail-shell-content.h` header needed. |
| Merging the `.eui` fragment into `mail-message-popup` (message list context menu) | `e_ui_manager_add_actions_with_eui_data` stays reserved for the `main-menu` fragment (it also registers the actions); the context menu fragment is merged separately via `e_ui_parser_merge_data(e_ui_manager_get_parser(ui_manager), …)` + `e_ui_manager_changed()`, so a `GError` can be logged. Target: the `mail-conversion-actions` placeholder in the `mail-create-menu` submenu; falls back automatically to `mail-message-popup-common-actions` if refused. |

## Layout

```
src/sieve-managesieve-client.[ch]  reusable ManageSieve client (GLib/GIO)
src/sieve-sasl.[ch]                SASL layer (negotiation + mechanisms),
                                   libgsasl + homemade OAUTHBEARER/XOAUTH2
src/sieve-model.[ch]               Sieve rule model (criteria/actions,
                                   + "opaque" rules kept verbatim) +
                                   (de)serialization; pure GLib, testable alone
src/sieve-secret.[ch]              password storage in the keyring
                                   (libsecret); pure GLib/GIO, testable alone
src/sieve-account.[ch]             reading Evolution accounts; OAuth2
                                   detection + access token via EDS
                                   (e_source_get_oauth2_access_token_sync);
                                   re-reading the account's password via EDS
                                   (e_source_credentials_provider_lookup_sync);
                                   DEPENDS on libedataserver (built with the module)
src/sieve-config.[ch]              GKeyFile preferences (XDG_CONFIG_HOME):
                                   one connection profile PER ACCOUNT
                                   ([account <UID>]) + [manual] profile +
                                   [state] last-account; migration of the
                                   old [connection]; pure GLib, testable
src/sieve-config-page.[ch]         "Sieve Filters" page of the account
                                   editor (EMailConfigPage + EExtension on
                                   E_TYPE_MAIL_CONFIG_NOTEBOOK): the
                                   account's connection settings
                                   (host/port/user/TLS) + auto-connect,
                                   written to sieve-config; Authentication
                                   section: password field HIDDEN by
                                   default (password taken from the
                                   keyring); the "Forget password" button
                                   clears the keyring entry (detached
                                   thread, no-op if nothing there) AND
                                   reveals the field; on commit, the
                                   entered password is stored in the
                                   keyring (sieve-secret, detached thread)
                                   then the field is re-hidden; OAuth2:
                                   field + button hidden; DEPENDS on
                                   evolution-mail + libedataserver (built
                                   with the module)
src/sieve-rule-editor.[ch]         "visual editor" GTK widget (GtkBox)
src/sieve-editor-dialog.[ch]       standalone GTK dialog (GtkStack visual/text)
src/module-sieve-filters.c         EModule entry point: Edit → Sieve
                                   Filters… menu entry + Message → Create
                                   → Create Sieve Filter… context menu
                                   entry (rule pre-filled on the sender,
                                   via sieve_editor_dialog_new_with_seed)
                                   + account editor page (sieve-config-page)
tests/test-managesieve.c           client test CLI (network)
tests/test-sasl.c                  sieve-sasl unit tests (no network)
tests/test-sieve-model.c           sieve-model unit tests (no network)
tests/test-sieve-secret.c          keyring round-trip; skips without Secret Service
tests/test-sieve-config.c          per-account profiles + [manual] +
                                   last-account + [connection] migration
                                   (temporary XDG_CONFIG_HOME, no network)
tests/test-timeout.c               network timeout + client GCancellable
                                   (local mute GSocketService, no network)
tests/secret/                      disposable keyring (gnome-keyring + dbus-run-session)
tests/dovecot/                     server fixture (see its README.md)
po/                                gettext translations for the Evolution
                                   module (POTFILES.in, LINGUAS, fr.po); see
                                   "Translations (gettext)" below
```

## Conventions

- C `gnu11`, `warning_level=2`, GLib/GObject style (`g_autoptr` welcome,
  `GError **` everywhere, `GCancellable` propagated even if not yet
  exploited).
- **Comments in English, msgids in English**, and project documentation
  (`README.md`, `AGENTS.md`, other `.md` files, `meson.build` comments)
  in English too. User-visible strings in the Evolution module go
  through gettext (`_()`/`N_()`) and are therefore translatable; a
  French translation is provided (see "Translations (gettext)" below).
- No new third-party dependency without a strong reason: GLib/GIO cover
  TCP+TLS. `libgsasl` (strong SASL) and `libsecret` (keyring) are now in
  place; don't add anything else lightly.
- `sieve-secret.c`: a thin layer over libsecret's "simple password" API,
  **synchronous** (like the client), no GTK nor Evolution dependency —
  keep it that way. Schema **private to the plugin**: we don't reuse the
  IMAP account entry from evolution-data-server (the password may
  differ, and eds doesn't expose it outside `ESource`). The dialog calls
  `sieve_secret_*` **from the worker thread**, never on the GTK main loop.
- The protocol AND the SASL mechanisms have been validated **against a
  real Dovecot**: if you touch the response parser, literal framing, or
  `sieve-sasl.c`, rerun `meson test` then `smoke.sh`.
- SASL: negotiation/`sieve-sasl.c` is self-contained (no Evolution
  dependency) — keep it that way. OAUTHBEARER/XOAUTH2 are built by hand
  (libgsasl doesn't provide them); the client **consumes** a token, it
  doesn't acquire one.

## Translations (gettext)

The Evolution module is translatable via gettext (GLib, no extra
dependency): `_()`/`N_()` on the user-visible strings of
`module-sieve-filters.c`, `sieve-editor-dialog.c`, `sieve-config-page.c`
and `sieve-rule-editor.c` (exact list: `po/POTFILES.in`). A complete
French translation is provided (`po/fr.po`; languages declared in
`po/LINGUAS`).

**Deliberately limited scope**: the protocol library
(`sieve-managesieve-client`, `sieve-sasl`, `sieve-account`,
`sieve-secret`, `sieve-config`, `sieve-srv`, `sieve-model`) stays
**outside gettext** — it's tested on its own via CLIs that aren't meant
to be translated, and its messages (`g_warning`/`g_debug` logs,
`GError->message`) stay in English. The user-facing wrappers around
these errors ("Connection failed: %s", "Save failed: %s"…) are
translated on the `sieve-editor-dialog.c` / `sieve-config-page.c` side;
the underlying error detail (`error->message`) stays in English.

**C gotcha**: a `static const` array (e.g. the label lists in
`sieve-rule-editor.c`, or the `EUIActionEntry` table in
`module-sieve-filters.c`) can't be initialized with `_()` — a gettext
call isn't a constant expression. These tables are therefore built
locally (function scope, no `static`) where they're used.

To add/change a translatable string: wrap it with `_()` (or `N_()` if
it's only used elsewhere), then regenerate `po/fr.po`:

```sh
meson compile -C build evolution-sieve-filters-pot   # regenerates po/evolution-sieve-filters.pot
msgmerge --update po/fr.po po/evolution-sieve-filters.pot
# then translate the new entries (msgstr "") in po/fr.po
```

`meson compile -C build` then regenerates the installed `.mo`
(`po/fr/LC_MESSAGES/evolution-sieve-filters.mo`). Syntax check:
`msgfmt --check --statistics po/fr.po`.

## To do (detailed in README.md "Known limitations")

Reading account settings (`CamelSettings`/`ESource` — the ManageSieve
connection settings already have a dedicated page in the account editor,
`sieve-config-page`; still missing: fine-grained `CamelSettings` reading,
e.g. IMAP security → pre-check "implicit TLS") · SCRAM-*-PLUS (TLS
channel binding) and GSSAPI · hardening the response parser · visual
editor for conditions/actions: **basics in place**
(`sieve-model` + `sieve-rule-editor`). Constructs that can't be
represented (hand-written scripts, Nextcloud Mail / Roundcube blocks,
`vacation`…) are now **kept verbatim** as "opaque" rules
(`SieveRule.opaque` / `.raw`): the visual editor shows them locked
(padlock, read-only), only the plain text tab can still edit them.
Remaining: a real RFC 5228 parser to make them editable, `variables`,
rule reordering and enabling/disabling · network cancellation on the UI
side.

`sieve-model` / `sieve-rule-editor` are independent of Evolution — keep
it that way. The editor is therefore unaware of where the folders come
from: it's `sieve-editor-dialog.c` (Evolution side) that enumerates the
chosen account's `CamelStore` (`camel_store_get_folder_info_sync`,
cached, on a thread) and injects them via
`sieve_rule_editor_set_mailboxes()` so the `fileinto` action can offer an
editable dropdown. If you touch the (de)serializer, rerun `meson test`:
`tests/test-sieve-model.c` checks that the model ⇄ script round-trip is
stable to the character, including for opaque rules (text copied
verbatim from another tool), and that only a lexically broken script
(unclosed brace / string) still makes `sieve_rule_set_parse()` fail.
