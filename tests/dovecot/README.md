# Test Dovecot (ManageSieve)

Disposable **Dovecot + Pigeonhole** instance for exercising
`sieve-managesieve-client` / `test-managesieve` against a real server,
with no system account nor global configuration: everything lives under
this directory.

- **`localhost:4190` → STARTTLS** (behavior of a real ManageSieve server).
- **`localhost:4191` → implicit TLS** (default mode of
  `sieve_managesieve_client_new(..., implicit_tls = TRUE)`).
- `ssl = required`: plaintext auth is refused before encryption.
- Credentials: **`testuser` / `testpass`** (static `passwd-file` database).
- The self-signed certificate generated on first run (CN/SAN `localhost`)
  is added to the container's CA store
  (`/usr/local/share/ca-certificates/`) so the client's TLS validation
  passes without an "insecure" flag (the client doesn't have one).
- SASL GSSAPI is also exercised, against a disposable Kerberos KDC
  (`kdc.sh`) — see "GSSAPI" below.

## Usage

```sh
# from anywhere in the repo
tests/dovecot/run.sh              # foreground, Ctrl-C to stop
tests/dovecot/run.sh --daemon     # background
tests/dovecot/run.sh --stop       # stop the background instance
tests/dovecot/run.sh --reload     # reload the conf

# automatic end-to-end test (starts/stops Dovecot, tests both TLS modes)
tests/dovecot/smoke.sh
```

Via Meson:

```sh
meson compile -C build dovecot        # = run.sh (foreground)
meson compile -C build dovecot-smoke  # = smoke.sh
```

Manual tries once the server is running:

```sh
# implicit TLS
build/tests/test-managesieve --host localhost --port 4191 \
    --user testuser --password testpass
# STARTTLS
build/tests/test-managesieve --host localhost --port 4190 --starttls \
    --user testuser --password testpass --check myscript.sieve
```

## Files

| File               | Role                                                          |
|--------------------|-------------------------------------------------------------|
| `dovecot.conf.in`  | Configuration template (`@FIXTURE_DIR@`, `@UID@`… tokens)   |
| `run.sh`           | Generates cert + conf + user database, starts Dovecot       |
| `kdc.sh`           | Disposable Kerberos KDC, for SASL GSSAPI (see below)         |
| `smoke.sh`         | LIST→CHECK→PUT→GET→SETACTIVE ""→DELETE, in implicit TLS AND STARTTLS, then every SASL mechanism incl. GSSAPI |
| `run/`             | Runtime state (cert, generated conf, logs, sockets, `krb5/` KDC state) — ignored |
| `mail/`            | Users' maildirs + Sieve scripts — ignored          |

## Dovecot 2.4

The devcontainer moved to Debian *trixie*, which ships **Dovecot 2.4**
(instead of 2.3): `dovecot.conf.in` is in 2.4 syntax and has been
verified against the real installed Dovecot 2.4.1 (`smoke.sh` passes:
implicit TLS, STARTTLS, all SASL mechanisms, LIST/CHECK/PUT/GET/
SETACTIVE/DELETE). Notable differences from the old 2.3 template: a
mandatory `dovecot_config_version = 2.4.0` (+ `dovecot_storage_version`)
as the first settings, named `passdb passwd-file { }` / `userdb
passwd-file { }` blocks (`passwd_file_path` instead of `args`),
`mail_driver` + `mail_path` instead of `mail_location`,
`ssl_server_cert_file`/`ssl_server_key_file` instead of `ssl_cert`/
`ssl_key` (no `ssl_dh`: DH params are deprecated/optional in 2.4), and
flat `sieve_script_path`/`sieve_script_active_path` settings instead of
the `plugin { sieve = path;active=path }` one-liner (the bare `plugin {}`
section no longer exists in 2.4). Network-free tests (`meson test`) are
unaffected either way.

## GSSAPI

`smoke.sh` also brings up a disposable MIT Kerberos KDC (`kdc.sh`,
realm `SIEVE.TEST`, entirely under `run/krb5/` — no system `/etc/krb5.conf`
touched) and tests SASL GSSAPI against it, both forced (`--mech GSSAPI`)
and via automatic negotiation with a ticket present. This needs:

- `krb5-kdc`, `krb5-admin-server`, `krb5-user` (the devcontainer installs
  them) — `smoke.sh` skips the GSSAPI battery cleanly if `kinit`/
  `kdb5_util` aren't found, everything else still runs.
- The **`dovecot-gssapi`** package — GSSAPI is a separate plugin, **not**
  part of `dovecot-core` despite `doveconf` silently accepting `gssapi` in
  `auth_mechanisms` either way (see AGENTS.md's pitfalls table: without
  the plugin, that silent acceptance turns into a fatal auth-process
  crash at startup, taking every mechanism down with it). `run.sh`
  therefore only lists `gssapi` in the generated conf when
  `/usr/lib/dovecot/modules/auth/libmech_gssapi.so` actually exists.

To try it by hand once `tests/dovecot/kdc.sh --daemon` and `run.sh
--daemon` are both up:

```sh
export KRB5_CONFIG="$PWD/tests/dovecot/run/krb5/krb5.conf"
export KRB5CCNAME="FILE:$PWD/tests/dovecot/run/krb5/ccache"
echo testpass | kinit testuser@SIEVE.TEST
build/tests/test-managesieve --host localhost --port 4191 \
    --user testuser --mech GSSAPI     # no --password needed
```

## Notes

- `run.sh` starts the Dovecot master via `sudo` (the devcontainer allows
  it without a password). The maildirs' uid/gid are the current user's,
  so the files stay accessible without `sudo`.
- `base_dir` (`run/state/`) is deliberately separate from where the
  generated conf lives (`run/dovecot.conf`): Dovecot copies its conf into
  `base_dir/dovecot.conf`, which would create a symlink loop if the two
  coincided.
- The log (`run/dovecot.log`) contains lines like
  `t_readlink(...dovecot.conf) failed: ... Invalid argument`: a known
  artifact of Dovecot 2.3 when the conf sits on an overlay mount (the
  devcontainer workspace's case). Harmless.
- Dovecot refuses `DELETESCRIPT` on the **active** script → `smoke.sh`
  does a `SETACTIVE ""` (`test-managesieve --deactivate`) right before.
- `dovecot-core`, `dovecot-managesieved`, `dovecot-sieve`, `openssl`,
  `dovecot-gssapi`, `krb5-kdc`, `krb5-admin-server` and `krb5-user` are
  installed by the devcontainer's `Dockerfile`.
