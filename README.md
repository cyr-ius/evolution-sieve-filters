# evolution-sieve-filters

Plugin (module) for **Evolution** to manage server-side **Sieve** filters
via the **ManageSieve** protocol
([RFC 5804](https://www.rfc-editor.org/rfc/rfc5804)). It adds a rule
editor (visual and plain text) directly inside Evolution, without going
through an external tool (Roundcube, Nextcloud Mail…) to manage server
filters.

> [!WARNING]
> Project under active development, not yet audited for large-scale
> real-world use. The ManageSieve client and the rule model are tested
> end-to-end (see [Tests](#tests-and-development)); the GUI part (dialog,
> account page) has only been checked against a real Evolution 3.56 in a
> limited way — see [Known limitations](#known-limitations). Back up your
> existing Sieve scripts before modifying them with this plugin.

## Features

- **Visual editor** for rules, in the style of Evolution's "Message
  Filters": a list of rules, criteria (sender, recipient, subject,
  header, size, body…) and actions (file into, forward, mark, delete,
  stop processing…) built without writing a script by hand.
- **Plain text editor** as a fallback, for anything the visual editor
  doesn't represent yet (`vacation`, hand-written rules, scripts from
  other tools…) — these rules are kept **verbatim** and shown locked in
  the visual editor rather than being lost or mangled.
- **Evolution integration**: an *Edit → Sieve Filters…* menu entry, a
  context menu entry *(right-click a message) → Create → Create Sieve
  Filter…* (pre-fills a rule based on the sender), and a **"Sieve
  Filters"** page in the account editor to configure the connection per
  account.
- **Password kept in the keyring** (libsecret / Secret Service), never
  stored in plain text; falling back to the IMAP password already known
  to Evolution when unset.
- **Full authentication support**: PLAIN, LOGIN, CRAM-MD5, SCRAM-SHA-1,
  SCRAM-SHA-256, as well as OAUTHBEARER / XOAUTH2 for OAuth2 accounts
  (Gmail, Office 365…), with the token obtained and refreshed via
  evolution-data-server.
- **Implicit TLS or StartTLS**, server capabilities re-read after the TLS
  handshake so a plaintext response is never trusted.
- Protocol core (`sieve-managesieve-client`) **independent of Evolution**
  (pure GLib/GIO): testable on its own, reusable in another project.

## Requirements

- Evolution ≥ 3.54 with its development headers (`evolution-dev` /
  `evolution-devel`, `evolution-data-server-dev`).
- An IMAP server exposing **ManageSieve** (Dovecot + Pigeonhole is the
  only one tested in practice; the client follows the RFC and should
  therefore work against Cyrus timsieved and other implementations, but
  this hasn't been verified).
- GLib/GTK+3, `libgsasl` (≥ 1.10) and `libsecret` (≥ 0.20).

## Installation

### From source

```sh
meson setup build
ninja -C build
sudo meson install -C build
```

`meson install` places the module in Evolution's module directory
(derived from `evolution-shell-3.0.pc`). Restart Evolution afterwards
(`evolution --force-shutdown` then relaunch it).

If `evolution-shell-3.0.pc` / `evolution-mail-3.0.pc` are missing, Meson
only builds the ManageSieve client library and the tests — useful for
developing/testing the protocol without Evolution installed.

### Debian/Ubuntu package

A packaging skeleton is provided in [`debian/`](debian/):

```sh
sudo apt build-dep .
dpkg-buildpackage -us -uc -b
sudo apt install ../evolution-sieve-filters_*.deb
```

## Usage

1. **Edit → Accounts**, pick an IMAP account, **Sieve Filters** tab: fill
   in the ManageSieve server/port (StartTLS or implicit TLS) and the
   username; optionally check "Connect automatically on startup". The
   password is taken from the keyring or, failing that, from the IMAP
   account's own password.
2. **Edit → Sieve Filters…** opens the dialog: pick the account from the
   dropdown, connecting and loading the active script happen
   automatically.
3. Build your rules in the visual tab, or switch to plain text for full
   control. **Save and activate** sends the script to the server
   (`CHECKSCRIPT` then `PUTSCRIPT`/`SETACTIVE`).
4. From the message list, right-click a message → **Create → Create
   Sieve Filter…** pre-fills a rule based on its sender.

## Security

- No password is ever written to disk in plain text. It is kept in the
  system keyring (Secret Service via `libsecret`), under a schema
  private to the plugin (`net.ipocus.evolution.SieveFilters`).
- Server capabilities announced before `STARTTLS` are **never** reused
  after the TLS handshake (protection against plaintext command
  injection).
- **Supported SASL mechanisms**: PLAIN, LOGIN, CRAM-MD5, SCRAM-SHA-1,
  SCRAM-SHA-256, OAUTHBEARER, XOAUTH2, GSSAPI — strong SASL by default
  (automatic negotiation of the best common mechanism; GSSAPI preferred
  when the caller has a usable Kerberos ticket, then SCRAM). GSSAPI lets
  an Active Directory-joined workstation authenticate via its existing
  Kerberos ticket, with no password at all. SCRAM-*-PLUS (TLS channel
  binding), GS2-KRB5 and EXTERNAL are **not** supported: `sieve-sasl.c`
  never offers them, regardless of what the underlying `libgsasl` build
  is capable of (see AGENTS.md).

## Architecture

The ManageSieve protocol, the SASL layer and the Sieve rule model are
self-contained GLib/GIO libraries, with no dependency on Evolution, and
tested independently. A file-by-file breakdown (role of each module,
coding conventions, known pitfalls) is documented in
[`AGENTS.md`](AGENTS.md), aimed at both human contributors and AI coding
assistants.

## Tests and development

```sh
meson setup build
meson test -C build          # unit tests (SASL, rule model, config,
                              # keyring — no network)
tests/dovecot/smoke.sh        # end-to-end against a disposable Dovecot
tests/secret/smoke.sh         # real keyring round-trip (disposable gnome-keyring)
```

Full details (fixtures, `test-managesieve` options, procedure against a
real server…): see [`AGENTS.md`](AGENTS.md).

## Known limitations

- Visual editor: only scripts produced by the editor itself (or an
  equivalent simple structure) are fully editable. Everything else
  (hand-written scripts, `vacation`, blocks generated by other tools,
  unsupported extensions) is kept **verbatim** as an "opaque" rule,
  editable only in plain text. No full RFC 5228 parser yet.
- No drag-and-drop rule reordering, nor individual enable/disable of
  rules.
- The GUI part (dialog, account page) has only been partially tested
  against a real Evolution; the rest is only checked at
  compile/link time in this development environment, which has no
  runnable Evolution.
- Only one ManageSieve server (Dovecot + Pigeonhole) has been tested in
  practice.

## Contributing

Contributions are welcome (issues, pull requests). Before proposing a
change touching the ManageSieve protocol, SASL, or the rule model, run
`meson test` then `tests/dovecot/smoke.sh` — see
[`AGENTS.md`](AGENTS.md) for the full conventions and pitfalls already
encountered.

## License

[GPL-3.0-or-later](LICENSE)
