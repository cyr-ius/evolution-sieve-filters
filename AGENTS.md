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
                                          # no network): negotiation + OAuth;
                                          # response parser unit tests
                                          # (tests/test-managesieve-parser,
                                          # canned server bytes, no network);
                                          # Sieve parser + rule model
                                          # (tests/test-sieve-ast,
                                          # tests/test-sieve-model)

tests/dovecot/smoke.sh                    # end-to-end: brings up Dovecot,
                                          # tests implicit TLS AND STARTTLS,
                                          # LIST/CHECK/PUT/GET/SETACTIVE/DELETE,
                                          # then each SASL mechanism + auto,
                                          # then SASL GSSAPI against a
                                          # disposable KDC (skipped cleanly
                                          # if krb5-kdc/krb5-user or the
                                          # dovecot-gssapi plugin are absent),
                                          # then the IMAP hierarchy separator
                                          # probe (issue #3 — skipped cleanly
                                          # if dovecot-imapd is absent)
# or: meson compile -C build dovecot-smoke

tests/dovecot/run.sh --daemon             # persistent server
tests/dovecot/run.sh --stop

tests/dovecot/kdc.sh --daemon             # disposable Kerberos KDC, for SASL
                                          # GSSAPI (see smoke.sh) — must be
                                          # started before run.sh --daemon
tests/dovecot/kdc.sh --stop
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
  XOAUTH2|GSSAPI` forces a mechanism; without `--mech`, auto negotiation
  (prefers GSSAPI when a Kerberos ticket is available, then SCRAM).
  `--oauth2-token` (or `$SIEVE_OAUTH2_TOKEN`) for OAUTHBEARER/XOAUTH2. The
  test Dovecot advertises `plain login cram-md5 scram-sha-1 scram-sha-256`,
  plus `gssapi` once `tests/dovecot/kdc.sh --daemon` has run and the
  `dovecot-gssapi` package is installed. GSSAPI needs no `--password`: the
  identity comes from `kinit`ing against the disposable KDC first (see
  `kdc.sh`'s header comment for the exact `KRB5_CONFIG`/`KRB5CCNAME` env
  vars and realm).

- Automatic negotiation falls back when GSSAPI fails (ticket present but
  service ticket / keytab / DNS issue): `sieve_managesieve_client_authenticate_sync()`
  retries once with the next mechanism, **only** for GSSAPI in auto mode
  (never for password mechanisms — no multiplied failed logins — and never
  for a forced `--mech`). Covered by the "GSSAPI failing" cases of `smoke.sh`
  (broken `KRB5_CONFIG` + TGT-only ccache) and by `/sasl/select/exclude-*`.
  **Exception, opt-out**: a *forced* GSSAPI also falls back the same way
  when `SieveManageSieveAuth.gssapi_fallback` is set (`--gssapi-fallback`
  in `test-managesieve`; `gssapi-fallback` in `sieve-config`, **TRUE by
  default**; the "Fall back to another type if GSSAPI fails" checkbox under
  "Canonicalize automatically (DNS)" in `sieve-config-page.c`, both
  shown only while the "Type" list is on GSSAPI). This exists because
  "Check Supported Types" selects the most secure working type — GSSAPI
  whenever a ticket was present at check time — and an expired ticket
  would otherwise break the connection. The type check itself never sets
  it (it must judge GSSAPI on its own). Covered by `smoke.sh`'s "forced
  and failing, with --gssapi-fallback" case.

- **GSSAPI + DNS alias/CNAME (issue #2)**: the target service principal
  ("sieve/\<hostname\>") is normally built from the connection host, but a
  server can be configured to expect a *different* hostname there (Dovecot's
  own `auth_gssapi_hostname`) — typically because clients connect via a
  CNAME while the server's Kerberos identity is its canonical name. Fixed
  by `--gssapi-hostname HOST` (`test-managesieve`) / `gssapi-hostname` in
  `sieve-config`'s state.ini (empty by default = use the connection host):
  it overrides `GSASL_HOSTNAME` for GSSAPI only
  (`SieveSaslCredentials.gssapi_hostname` /
  `SieveManageSieveAuth.gssapi_hostname`), never OAUTHBEARER's own host=
  field. **Deliberately not exposed as a GUI field** (`sieve-config-page.c`
  only reads it back and re-saves it untouched, in `conn_input_from_widgets()`
  / `sieve_config_page_commit_changes()`): an advanced escape hatch meant
  to be hand-edited in state.ini for the rare case that isn't already
  covered by the checkbox below, so most users never need to touch it.
  `kdc.sh` exports a second keytab principal (`sieve/<CANON_HOST>@SIEVE.TEST`,
  `CANON_HOST` a fictitious name — Kerberos principals are just strings, no
  DNS involved) and `smoke.sh` reproduces the mismatch by pointing
  Dovecot's `auth_gssapi_hostname` at it via a live `reload` (client still
  connects to plain `localhost`): forced GSSAPI fails without the
  override, succeeds with `--gssapi-hostname` set to it.

  **Automatic alternative (the one actually exposed in the GUI)**:
  `--gssapi-canonicalize-hostname` (`test-managesieve`) / the "Canonicalize
  automatically (DNS)" checkbox in the account editor's Authentication
  section (`sieve-config-page.c`, stored as `gssapi-canonicalize-hostname`
  in `sieve-config`, **TRUE by default** — see
  `sieve_config_load_for_account()`, alongside `auto_connect` /
  `remember_password` — ignored when `gssapi-hostname` is set in state.ini,
  which always wins) resolves the connection host's DNS canonical name
  itself (`sieve_sasl_canonicalize_hostname()` in `sieve-sasl.c`: plain
  `getaddrinfo(AI_CANONNAME)`, a forward/CNAME-chasing lookup only —
  blocking, not cancellable, done inside `sieve_sasl_new()` only when the
  mechanism actually being negotiated is GSSAPI) instead of requiring the
  canonical name typed by hand. This exists because Evolution/Camel's own
  GSSAPI SASL (`camel-sasl-gssapi.c`, used for IMAP) does exactly this
  **unconditionally, with no way to turn it off** — see the pitfalls
  table entry below for how that was confirmed by reading Camel's
  source; defaulting it to on here matches that out-of-the-box behavior
  so ManageSieve doesn't need extra configuration for the common case. A
  lookup failure fails the whole authentication attempt (same as Camel).

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
| `GETSCRIPT` returned the script with one extra trailing `\n` (and a "NO {N}" left a stray empty line for the next command) | the CRLF ending the line *after* a `{N}` literal is framing, not data — it was read as an extra empty data line. Fixed by `read_logical_line()` in `sieve-managesieve-client.c` (literal + rest of its line = one logical line); covered by `/parser/getscript/literal-exact` and `/parser/status/no-literal-then-next`. |
| TLS handshake `Connection reset by peer` on startup | Dovecot's `config` process crashed (invalid conf) → read `tests/dovecot/run/dovecot.log` |
| `test-sieve-secret`: `SKIP ... Failed to execute child process "dbus-launch"` | normal outside a D-Bus session: libsecret can't start a bus. The test skips. For the real round-trip: `tests/secret/smoke.sh`. |
| keyring smoke: `Cannot create an item in a locked collection` | the `default` Secret Service alias points to a locked collection (no graphical prompter available headless). `smoke.sh` pre-designates the `login` keyring (created unlocked via `--unlock`) via `keyrings/default`. If it persists: `pkill -9 gnome-keyring-daemon` (leftover daemon from a previous run) then rerun. |
| "Create Sieve Filter" context menu item: nothing happens, `WARNING sieve: view content (EMailShellContent) is not an EMailReader` | in Evolution 3.56, `EMailShellContent` **no longer implements** the `EMailReader` interface (it's the internal `EMailPanedView` widget, a descendant, that carries it). `module-sieve-filters.c` finds it with `sieve_find_mail_reader()` (recursive widget walk under the `EShellContent`) — no private `e-mail-shell-content.h` header needed. |
| Merging the `.eui` fragment into `mail-message-popup` (message list context menu) | `e_ui_manager_add_actions_with_eui_data` stays reserved for the `main-menu` fragment (it also registers the actions); the context menu fragment is merged separately via `e_ui_parser_merge_data(e_ui_manager_get_parser(ui_manager), …)` + `e_ui_manager_changed()`, so a `GError` can be logged. Target: the `mail-conversion-actions` placeholder in the `mail-create-menu` submenu; falls back automatically to `mail-message-popup-common-actions` if refused. |
| "Can we authenticate with SASL GSSAPI?" | **Yes**, since the KDC-backed fixture landed. `known_mechs[]` in `src/sieve-sasl.c` includes `GSSAPI`; libgsasl provides the mechanism itself (verified against a real Kerberos KDC — no extra wiring needed there). Automatic negotiation only picks it when `gssapi_ticket_available()` finds a usable ticket in the caller's default Kerberos credential cache (checked via libkrb5, optional dependency `mit-krb5` — without it, GSSAPI just never gets auto-picked); forcing `--mech GSSAPI` always works if the server offers it and a ticket exists, regardless of that dependency. Tested end-to-end by `tests/dovecot/kdc.sh` (disposable MIT krb5 KDC) + `smoke.sh`'s "SASL GSSAPI" battery. Only GS2-KRB5, SCRAM-*-PLUS and EXTERNAL remain unimplemented — see README.md "Security". |
| `gss_acquire_cred()` via `libgssglue` fails with "unsupported mechanism", even with the krb5 OID explicit | `libgssglue` 0.9 (Debian) dispatches to MIT krb5 through a private symbol, `mechglue_internal_krb5_init`, that no longer exists in current `libgssapi-krb5` (verified: absent from `libgssapi_krb5.so.2` 1.21.3) — so libgssglue's own generic GSS entry points can't load the krb5 mechanism at all on this distro, despite `/etc/gssapi_mech.conf` correctly listing it and the `.so` opening fine. **This does not affect authentication itself**: libgsasl's GSSAPI mechanism works fine end-to-end (confirmed against a real KDC) through whatever internal path it uses. It only broke a first attempt at probing "is there a usable ticket" via `gss_acquire_cred` — `gssapi_ticket_available()` in `src/sieve-sasl.c` uses `krb5_cc_get_principal()` (libkrb5) instead, which is both simpler and correct for this specific question. |
| Dovecot: `auth: Fatal: Unknown authentication mechanism 'GSSAPI'`, then the whole auth service throttle-loops (breaks PLAIN/LOGIN too) | Debian's `dovecot-core` does **not** actually build in GSSAPI — despite `gssapi` appearing as a string inside `/usr/lib/dovecot/auth` (config-name table, not an implementation) and `doveconf` silently accepting `auth_mechanisms = ... gssapi`. The real mechanism is the separate `dovecot-gssapi` package (`/usr/lib/dovecot/modules/auth/libmech_gssapi.so`). Listing an unknown mechanism name in `auth_mechanisms` is **fatal for the whole auth process**, not just for that mechanism — `tests/dovecot/run.sh` therefore only appends `gssapi` to `auth_mechanisms` (via the `@GSSAPI_MECH@` template token in `dovecot.conf.in`) when that `.so` is actually present, never unconditionally. |
| Dovecot GSSAPI login succeeds but then `passwd-file: unknown user` / `Authenticated user not found from userdb` | Dovecot's GSSAPI mechanism authenticates as `user@REALM` (the Kerberos principal), but the fixture's passwd-file only knows plain `testuser`. Fix: `auth_username_format = %{user|username|lower}` in `dovecot.conf.in` (the `username` filter strips the `@domain` part) — applied unconditionally, harmless for the other mechanisms since their usernames never carry an `@`. |
| Dovecot GSSAPI: `While acquiring service credentials: ... Permission denied` | the auth worker reading `auth_krb5_keytab` does not run as root even though the master process is started via `sudo` (see run.sh) — the keytab must be world-readable. `kdc.sh` `chmod 644`s the keytab it generates; it's a disposable test-only key for a throwaway realm, so this is fine here (would not be, for a real deployment's keytab). |
| Dovecot: `Fatal: service(imap) access(/usr/lib/dovecot/imap) failed: No such file or directory` | same story as the GSSAPI mechanism above, but for a whole protocol: `dovecot-imapd` (the actual `/usr/lib/dovecot/imap` executable, needed by `tests/test-imap-probe.c` / `sieve-imap-probe.c`, issue #3) is a **separate package** from `dovecot-core`. Listing `imap` in `protocols` when the binary is missing is **fatal at startup for the whole instance**, not just IMAP — `run.sh` therefore only adds `imap` to `protocols` (via `@IMAP_PROTOCOL_NAME@`) and keeps the `service imap-login { ... }` block (stripped between the `IMAP_BLOCK_BEGIN`/`IMAP_BLOCK_END` markers otherwise) when `/usr/lib/dovecot/imap` actually exists; `smoke.sh`'s IMAP probe battery is skipped the same way. Already in the Dockerfile (`dovecot-imapd`), but a devcontainer built before this fix needs `sudo apt-get install dovecot-imapd`. |
| Evolution's own account editor, IMAP account: sends `fileinto "INBOX/Sub"` but Dovecot rejects it (`Name must not have '/' characters`) if its own namespace separator isn't `/` (often `.` for Maildir++) | issue #3 (https://github.com/cyr-ius/evolution-sieve-filters/issues/3). Camel's `CamelFolderInfo->full_name` is **always** `/`-normalized internally (confirmed against evolution-data-server's IMAPX provider: `camel_imapx_mailbox_to_folder_path()`), regardless of the server's real separator — and that real separator is **not exposed by any public Camel/EDS API** (`camel-imapx-store.h` / `camel-imapx-settings.h` are deliberately not installed as public headers; verified against the upstream `CMakeLists.txt`). Fix: `sieve_rule_set_translate_folder_separator()` (`src/sieve-model.[ch]`) translates fileinto paths between `/` and the account's real separator at exactly two points in `sieve-editor-dialog.c` (`sync_visual_to_text` / `sync_text_to_visual` — the visual model always stays `/`, matching the Camel folder dropdown; the raw text tab always stays in the real separator, matching what's actually sent/received over ManageSieve). The real separator itself is either entered manually (`sieve-config-page.c`, "Folders" section, per-account) or detected with the "Detect Automatically" button, which opens its own minimal IMAP connection (`src/sieve-imap-probe.[ch]`, `LIST "" ""` — RFC 3501 §6.3.8) since there's no other way to learn it; not available for OAuth2 accounts (no password to hand a plain `LOGIN`). |
| "Evolution canonicalizes the GSSAPI hostname even though krb5.conf has `dns_canonicalize_hostname = false`" | Not a krb5 bug: **Evolution/Camel does its own canonicalization, in its own code, before krb5 ever sees the hostname.** Read straight from `camel-sasl-gssapi.c` upstream (github.com/GNOME/evolution-data-server), function `sasl_gssapi_challenge_sync()`: `hints.ai_flags = AI_CANONNAME; ai = camel_getaddrinfo (host, NULL, &hints, cancellable, error); str = g_strdup_printf ("%s@%s", service_name, ai->ai_canonname);` — a plain `getaddrinfo(AI_CANONNAME)` forward/CNAME-chasing lookup, generic (IMAP/SMTP/POP3/HTTP all share this function), with **no setting anywhere to disable it**. `dns_canonicalize_hostname` only controls what krb5 itself does internally (`krb5_sname_to_principal()`); it can't affect a hostname Camel already resolved and handed it as a literal string. (Also confirmed via official MIT krb5 docs: `rdns` — reverse/PTR lookup — explicitly "has no effect" when `dns_canonicalize_hostname = false`, so that's not the explanation either.) This plugin's own `sieve-sasl.c` does *not* do this (`GSASL_HOSTNAME` gets the literal host, hence issue #2's manual override) — see the entry right below for the opt-in equivalent now available here. |
| GSSAPI + DNS alias/CNAME, automatic alternative to typing the canonical hostname | `sieve_sasl_canonicalize_hostname()` (`src/sieve-sasl.c`) reproduces Camel's `AI_CANONNAME` lookup above, opt-in via `SieveSaslCredentials.gssapi_canonicalize_hostname` / the "Canonicalize automatically (DNS)" checkbox (`sieve-config-page.c`) — see the dedicated bullet under "GSSAPI + DNS alias/CNAME (issue #2)" further down. |
| "Pre-fill ManageSieve's encryption from the IMAP account's (IMAPS → implicit TLS)" | **Don't**: RFC 5804 only defines STARTTLS on 4190; implicit TLS on a dedicated port is a non-standard extra, while IMAPS on 993 is the norm — that mapping would break the common case (e.g. Dovecot defaults) to serve a rare one. `sieve_config_page_load_fields()` therefore only carries over host, user and — via `sieve_sasl_mechanism_for_account_method()` (`src/sieve-sasl.c`, tested in `/sasl/introspect/account-method`) — GSSAPI as the pre-selected "Type" (which also skips the automatic type check on opening). Password mechanisms are not carried over: the IMAP choice says nothing about what the Sieve server offers. The IMAP settings are read from the account ESource's Authentication/Security extensions (`AccountServerSettings` in `sieve-config-page.c`, the backing store of `CamelNetworkSettings`), so no `CamelSession` is needed. |

## Layout

```
src/sieve-managesieve-client.[ch]  reusable ManageSieve client (GLib/GIO)
src/sieve-sasl.[ch]                SASL layer (negotiation + mechanisms),
                                   libgsasl + homemade OAUTHBEARER/XOAUTH2
src/sieve-model.[ch]               Sieve rule model (criteria/actions,
                                   + "opaque" rules kept verbatim) +
                                   (de)serialization; pure GLib, testable alone
src/sieve-ast.[ch]                 generic RFC 5228 parser -> syntax tree
                                   (commands/tests/arguments, byte offsets
                                   on every node, per-command recovery
                                   from grammar errors, nesting bounded
                                   by SIEVE_AST_MAX_DEPTH); knows no
                                   command and no comment convention;
                                   built into sieve_model_lib, used by
                                   sieve_rule_set_parse() /
                                   sieve_rule_unlock()
src/sieve-secret.[ch]              password storage in the keyring
                                   (libsecret); pure GLib/GIO, testable alone
src/sieve-account.[ch]             reading Evolution accounts; OAuth2
                                   detection + access token via EDS
                                   (e_source_get_oauth2_access_token_sync);
                                   re-reading the account's password via EDS
                                   (e_source_credentials_provider_lookup_sync);
                                   DEPENDS on libedataserver (built with the module)
src/sieve-imap-probe.[ch]          minimal, read-only IMAP4rev1 probe
                                   (RFC 3501): connect (implicit TLS /
                                   STARTTLS) + LOGIN + `LIST "" ""` +
                                   LOGOUT, used ONLY to discover an
                                   account's real IMAP hierarchy
                                   separator (issue #3 — see the
                                   pitfalls table above); pure GLib/GIO,
                                   testable alone (tests/test-imap-probe)
src/sieve-config.[ch]              GKeyFile preferences (XDG_CONFIG_HOME):
                                   one connection profile PER ACCOUNT
                                   ([account <UID>]) + [manual] profile +
                                   [state] last-account; migration of the
                                   old [connection]; per-account
                                   `folder-separator` (issue #3, see
                                   sieve-imap-probe.[ch] above); pure
                                   GLib, testable
src/sieve-config-page.[ch]         "Sieve Filters" page of the account
                                   editor (EMailConfigPage + EExtension on
                                   E_TYPE_MAIL_CONFIG_NOTEBOOK): the
                                   account's connection settings
                                   (host/port/user/TLS) + auto-connect,
                                   written to sieve-config; Authentication
                                   section: "Type" list (forced SASL
                                   mechanism, stored as `auth-mechanism`;
                                   no "Automatic" entry — when nothing is
                                   saved yet the list is empty, the type
                                   check below runs by itself when the
                                   page opens, and connections keep
                                   negotiating automatically until a type
                                   is saved) + "Check Supported
                                   Types" button (probes each mechanism on
                                   its own connection, strikes through the
                                   failing ones; a failure only counts if
                                   another password/OAuth mechanism
                                   succeeded, and the run stops after 2
                                   ambiguous failures, to avoid tripping
                                   brute-force protection; at the end the
                                   selected type is kept unless it failed,
                                   otherwise the most secure working one
                                   is selected, first in
                                   sieve_sasl_known_mechanisms() order;
                                   result on a visible status line under
                                   "Type" — this button replaced the old
                                   "Connectivity" section's "Test"
                                   button) + "Fall back to another type if
                                   GSSAPI fails" checkbox (stored as
                                   `gssapi-fallback`, ON by default;
                                   like the "Canonicalize" checkbox
                                   below, only shown while the "Type"
                                   list is on GSSAPI) + "Folders"
                                   section: "Folder separator:" field
                                   (per-account `folder-separator`,
                                   default '/') + "Detect Automatically"
                                   button (issue #3: opens its own IMAP
                                   connection via sieve-imap-probe.[ch]
                                   using the account's REAL IMAP
                                   host/port/encryption — read from
                                   the account ESource's Authentication
                                   + Security extensions (the backing
                                   store of CamelNetworkSettings, see
                                   `AccountServerSettings`), NOT the
                                   ManageSieve settings above — and its
                                   EDS-stored password;
                                   insensitive for OAuth2 accounts) +
                                   "Canonicalize automatically (DNS)"
                                   checkbox (stored as
                                   `gssapi-canonicalize-hostname`, ON by
                                   default): resolves the DNS canonical
                                   name for GSSAPI's service principal
                                   itself (sieve-sasl.c) instead of
                                   requiring it typed by hand — mirrors
                                   Evolution/Camel's own unconditional
                                   behavior for IMAP; the exact-override
                                   escape hatch it's an alternative to,
                                   `gssapi-hostname` (issue #2, see the
                                   "GSSAPI + DNS alias/CNAME" entry
                                   above), is deliberately NOT a GUI
                                   field — advanced, state.ini-only,
                                   always wins when set there; password
                                   field HIDDEN by
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
tests/test-imap-probe.c            sieve-imap-probe test CLI (network,
                                   issue #3 — see tests/dovecot/ below)
tests/test-sasl.c                  sieve-sasl unit tests (no network)
tests/test-managesieve-parser.c    response parser unit tests: the client
                                   is attached to in-memory streams
                                   carrying canned (incl. malformed /
                                   hostile) server bytes, via
                                   src/sieve-managesieve-client-private.h
                                   (test-only hook, not part of the API)
tests/test-sieve-model.c           sieve-model unit tests (no network)
tests/test-sieve-ast.c             sieve-ast unit tests (no network): tokens,
                                   offsets, recovery, fatal errors, depth
                                   bound, every truncation of a script
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
- **English is the project's reference language.** Comments, msgids,
  shell scripts (`scripts/`, `tests/dovecot/`, `tests/secret/`),
  `debian/*` metadata, and project documentation (`README.md`,
  `AGENTS.md`, other `.md` files, `meson.build` comments) are all
  written in English. The only exceptions are the shipped French UI
  translation (`po/fr.po`) and its `LINGUAS` entry — see "Translations
  (gettext)" below; new contributions should not introduce French
  elsewhere. User-visible strings in the Evolution module go through
  gettext (`_()`/`N_()`) and are therefore translatable.
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
- Response parser (`sieve-managesieve-client.c`): everything the server
  sends is bounded — physical line (`SIEVE_MANAGESIEVE_MAX_LINE_SIZE`,
  64 KiB), `{N}` literal (`…_MAX_LITERAL_SIZE`, 16 MiB, checked before
  allocating), whole response (`…_MAX_RESPONSE_SIZE`, 32 MiB), SASL
  round trips (`…_MAX_SASL_ROUNDS`); NUL bytes, invalid UTF-8 and
  malformed base64 are protocol errors. `read_logical_line()` turns every
  literal into a quoted-string and consumes the rest of its line, so the
  callers only ever parse quoted-strings (`scan_quoted_string()`) — keep
  it that way rather than special-casing literals again. New parser
  cases go in `tests/test-managesieve-parser.c`.
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

## Visual editor (`sieve-model` / `sieve-rule-editor`)

- What the visual editor represents: a `# rule:[name]` marker followed
  by `if <test> { <actions> }`, where `<test>` is `true`, a single test
  or one `allof`/`anyof` list of them. Tests: `header` (From / To / Cc /
  Subject / any header, `:contains`/`:is`/`:matches`/`:regex`, one or
  several values), `size :over`/`:under`, `body :text`, `exists` (one
  header name), each optionally under a single `not`
  (`SieveCondition.negate`). Actions: `keep`, `discard`, `stop`,
  `fileinto`/`redirect` (optionally `:copy`, `SieveAction.copy`),
  `addflag`/`setflag`/`removeflag` (one flag), `reject`, `vacation` with
  `:days`/`:subject` (`SieveAction.days`/`.subject`, reply text in
  `.arg`). In the editor, each match is offered with its negated form in
  the same combo (`text_match_choices` / `size_match_choices` in
  `sieve-rule-editor.c`), and reject/vacation get a multi-line form
  under their row.
- Everything else (unmarked rules, `elsif`/`else`, nested
  `allof`/`anyof`, `not not`, multi-name `exists`, `address`/`envelope`,
  `:flags`, the other `vacation` tags, unknown commands, Nextcloud Mail /
  Roundcube blocks…) is **kept verbatim** as an "opaque" rule
  (`SieveRule.opaque` / `.raw`): the visual editor shows it locked
  (padlock, read-only), only the plain text tab can still edit it. The
  rule is never approximated: a construct that would re-serialize with a
  different meaning stays opaque (`/sieve-model/lossy-constructs-stay-opaque`
  lists them — extend it when mapping something new).
- Parsing is two-step: `sieve_ast_parse()` (`sieve-ast.c`, generic
  RFC 5228 tree, knows no command) then the mapping in `sieve-model.c`
  (`rule_from_if()`, `condition_from_test()`, `action_from_command()`).
  The `# rule:[name]` marker and `if false # <test>` conventions are
  recognized by the mapping layer from the AST's byte offsets (the
  comments before a command, the comment after `false`) — keep them out
  of `sieve-ast.c`. A marked `if` followed by `elsif`/`else` stays a
  single opaque unit (a structured `if` with a dangling `else` would
  break on reorder).
- Multi-line strings: a reject/vacation message with a newline is
  written back as a dot-stuffed `text:` literal (`append_string()`); one
  without a final newline gains it once, then the round trip is stable.
- The `require` line (`sieve_rule_set_to_script()`) is recomputed for
  the extensions the model knows (`managed_requires`: body, copy,
  encoded-character, fileinto, imap4flags, regex, reject, vacation,
  variables — the last two from `${...}` references, issue #4); any
  other extension of the original line is always kept. Opaque rules'
  text is parsed too (`collect_opaque_requires()`), but only to keep a
  managed extension it uses from being dropped, **never to add one**
  (re-emitted verbatim, it must keep its meaning: `${f}` without
  `variables` is a literal); if it doesn't parse cleanly, the whole
  original line is kept.
- Per-rule enable/disable (`SieveRule.enabled`): a disabled rule is
  serialized as `if false # <mode>(<conditions>)` — the same convention
  Roundcube's managesieve plugin uses — so the original test survives
  re-enabling; a trailing comment that doesn't parse as a recognizable
  test falls back to opaque rather than being silently dropped.
- Unlocking an opaque rule on demand: `sieve_rule_unlock()` retries
  the same `if allof/anyof(...) { actions }` grammar as
  `sieve_rule_set_parse()`, but **without** requiring a leading
  `# rule:[name]` marker — it's triggered per rule (the "Unlock" button
  under the padlock view in `sieve-rule-editor.c`), typically after the
  user has fixed up the text in the "Raw text" tab, so relaxing the
  marker there doesn't risk the tolerant whole-script parse silently
  reinterpreting a foreign block (Nextcloud Mail, Roundcube) elsewhere
  in the script. On failure the rule is left untouched (still opaque,
  exact text preserved) and the reason is shown inline.
- Serialization stays close to hand-written Sieve (issue #1): a rule
  with a single condition is written without the `allof (...)`
  wrapper (RFC 5228: `allof` of one test is that test); comments before
  the leading `require` are kept verbatim (`SieveRuleSet.preamble`)
  instead of pushing that `require` into an opaque rule; and a script
  whose `require` declares the old `imapflags` (draft, older Cyrus)
  rather than `imap4flags` keeps `imapflags` for the flag actions.
- Rule order can be changed (up/down buttons in the rule list's
  toolbar, next to +/-/reload) — order matters in Sieve (top-to-bottom
  evaluation, `stop` short-circuits the rest), including for opaque
  rules (only their position moves, not their content).

`sieve-model` / `sieve-rule-editor` are independent of Evolution — keep
it that way. The editor is therefore unaware of where the folders come
from: it's `sieve-editor-dialog.c` (Evolution side) that enumerates the
chosen account's `CamelStore` (`camel_store_get_folder_info_sync`,
cached, on a thread) and injects them via
`sieve_rule_editor_set_mailboxes()` so the `fileinto` action can offer an
editable dropdown. If you touch the parser or the (de)serializer, rerun
`meson test`: `tests/test-sieve-ast.c` covers the RFC 5228 grammar
itself (byte offsets, error recovery, nesting bound, every truncation of
a script), `tests/test-sieve-model.c` checks that the model ⇄ script
round-trip is stable to the character, including for opaque rules (text
copied verbatim from another tool), and that only a lexically broken
script (unclosed brace, string, `text:` literal or comment, invalid
UTF-8) still makes `sieve_rule_set_parse()` fail.

## To do (detailed in README.md "Known limitations")

- **ManageSieve port/security auto-discovery**: when no profile is
  saved, `sieve-config-page` pre-fills host/user from the IMAP account
  and pre-selects GSSAPI when the IMAP account uses it (see the pitfalls
  table: the IMAP port/security are deliberately *not* carried over).
  Port/encryption stay at 4190 + STARTTLS; candidates for doing better:
  a DNS SRV `_sieve._tcp` lookup (RFC 5804 §1.8 — `src/sieve-srv.[ch]`
  exists but the page doesn't call it yet), or retrying implicit TLS on
  4191 when STARTTLS on 4190 fails.
- **SCRAM-\*-PLUS (TLS channel binding) and GS2-KRB5** — `sieve-sasl.c`
  still sends a `n,` gs2-header (no channel binding).
- **More constructs in the visual editor** (the parser already reads
  the full RFC 5228 grammar, see "Visual editor" above; what's missing
  is in the model + editor): nested `anyof`/`allof` (a tree-shaped
  model and editor — the biggest item), `elsif`/`else` chains,
  `address`/`envelope` tests, `fileinto :flags`, the other `vacation`
  tags (`:from`, `:addresses`, `:mime`, `:handle`), and the
  `variables` actions (`set`…).
