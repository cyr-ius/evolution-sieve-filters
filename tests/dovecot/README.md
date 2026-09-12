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
| `smoke.sh`         | LIST→CHECK→PUT→GET→SETACTIVE ""→DELETE, in implicit TLS AND STARTTLS |
| `run/`             | Runtime state (cert, generated conf, logs, sockets) — ignored |
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
- `dovecot-core`, `dovecot-managesieved`, `dovecot-sieve` and `openssl`
  are installed by the devcontainer's `Dockerfile`.
