# tests/secret/ — disposable keyring

`smoke.sh` exercises `src/sieve-secret.[ch]` (storing the ManageSieve
password in the Secret Service via **libsecret**) end-to-end,
**without touching the user's own keyring**:

- a `dbus-run-session` isolates a fresh session bus;
- a disposable `gnome-keyring-daemon` is brought up in it, with
  `XDG_DATA_HOME` / `XDG_RUNTIME_DIR` under `tests/secret/run/`, unlocked
  with a throwaway password;
- `build/tests/test-sieve-secret` is rerun in that environment with
  `SIEVE_SECRET_REQUIRE_SERVICE=1`: with no service reachable the test
  **fails** (instead of skipping like under `meson test`).

```sh
meson compile -C build test-sieve-secret
tests/secret/smoke.sh                 # or: meson compile -C build secret-smoke
```

Dependencies (already in the devcontainer image): `dbus`,
`gnome-keyring`; `libsecret-tools` is not required.

## Without the smoke test: `meson test`

`meson test -C build sieve-secret` runs the same binary but **without**
a Secret Service: it detects the missing service and skips cleanly.
This is intentional — CI without a D-Bus session stays green, the smoke
test covers the real round-trip.

## Reminder: what the plugin stores in the keyring

Schema `net.ipocus.evolution.SieveFilters`, one entry per ManageSieve
account, indexed by the attributes `protocol=managesieve`, `host`,
`port`, `user`. This is a schema private to the plugin: the IMAP account
entry managed by evolution-data-server is not shared.
