# Development image for evolution-sieve-filters.
# Based on the Microsoft devcontainers image (Debian 13 "trixie"): already
# provides the non-root "vscode" user, git, sudo, etc.
#
# Why trixie and not bookworm: trixie ships "evolution-dev" 3.56.2, the
# version that runs on the target machine (Hygor). Bookworm is stuck at
# 3.46.4, and the module (EUIManager / EShellView, Evolution 3.54+) doesn't
# build/validate against 3.46. All the other dependencies also exist in
# trixie (evolution-data-server-dev 3.56.2, libglib2.0-dev 2.84, etc.).
# Trade-off: Dovecot moves to 2.4 (see tests/dovecot/).
FROM mcr.microsoft.com/devcontainers/base:trixie

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      # --- build toolchain ---
      build-essential \
      gcc \
      clang \
      clangd \
      gdb \
      valgrind \
      meson \
      ninja-build \
      pkg-config \
      ca-certificates \
      # --- Debian packaging (scripts/build-deb.sh, debian/) ---
      # dpkg-dev is already pulled in by build-essential (dpkg-buildpackage);
      # debhelper (compat 13) and devscripts+equivs (mk-build-deps) are not.
      debhelper \
      devscripts \
      equivs \
      # --- GLib / GIO / GTK ---
      libglib2.0-dev \
      libgtk-3-dev \
      # --- strong SASL for the ManageSieve client (SCRAM, CRAM-MD5, LOGIN…) ---
      libgsasl-dev \
      # SASL GSSAPI: libgsasl already provides the mechanism itself at
      # runtime (transitively pulled in already), but src/sieve-sasl.c
      # also links libkrb5 directly to probe for a usable Kerberos ticket
      # (krb5_cc_get_principal) before letting automatic negotiation pick
      # GSSAPI — install it explicitly rather than relying on the
      # transitive pull. (Not libgssglue-dev: its GSSAPI/krb5 dispatch is
      # broken against this modern MIT krb5, see sieve-sasl.c.) ---
      libkrb5-dev \
      # --- keyring: storing the ManageSieve password (src/sieve-secret.c).
      # libsecret-1-dev is already pulled in by evolution-dev, but
      # src/sieve-secret.c depends on it directly — install it explicitly so
      # the client lib still builds if the transitive dependency disappears. ---
      libsecret-1-dev \
      # gnome-keyring: disposable Secret Service for tests/secret/smoke.sh
      # (dbus-run-session is provided by 'dbus', already present in the base image).
      gnome-keyring \
      # --- Evolution headers (evolution-shell-3.0.pc, evolution-mail-3.0.pc) ---
      # trixie: 3.56.2 — same version as the target machine (EUIManager API).
      evolution-dev \
      # evolution-dev ships e-util/e-contact-store.h which includes
      # <libebook/libebook.h> without depending on it: must be added by hand.
      libebook1.2-dev \
      # --- Evolution Data Server headers (camel-1.2.pc) ---
      evolution-data-server-dev \
      # --- GIO TLS backend (needed at runtime for port 4190) ---
      glib-networking \
      # --- test ManageSieve server (tests/dovecot/) ---
      dovecot-core \
      dovecot-managesieved \
      dovecot-sieve \
      openssl \
      # dovecot-gssapi: the SASL GSSAPI mechanism itself is a SEPARATE
      # plugin package, NOT part of dovecot-core (which only carries the
      # mechanism's name in a config-name table, enough to make
      # `doveconf` accept it silently but not to actually authenticate —
      # see AGENTS.md's pitfalls table for the "Fatal: Unknown
      # authentication mechanism" trap this causes without this package). ---
      dovecot-gssapi \
      # --- disposable Kerberos KDC for the SASL GSSAPI smoke test
      # (tests/dovecot/kdc.sh): krb5-kdc ships krb5kdc/kdb5_util,
      # krb5-admin-server ships kadmin.local (offline principal/keytab
      # administration, no admin server process needed), krb5-user ships
      # kinit/klist. ---
      krb5-kdc \
      krb5-admin-server \
      krb5-user \
 && apt-get clean \
 && rm -rf /var/lib/apt/lists/*
