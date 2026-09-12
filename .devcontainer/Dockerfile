# Image de développement pour evolution-sieve-filters.
# Basée sur l'image devcontainers Microsoft (Debian 13 « trixie ») : fournit
# déjà l'utilisateur non-root « vscode », git, sudo, etc.
#
# Pourquoi trixie et pas bookworm : trixie livre « evolution-dev » 3.56.2, la
# version qui tourne sur la machine cible (Hygor). Bookworm est bloqué en
# 3.46.4, et le module (EUIManager / EShellView, Evolution 3.54+) ne se
# compile/valide pas contre 3.46. Toutes les autres dépendances existent aussi
# en trixie (evolution-data-server-dev 3.56.2, libglib2.0-dev 2.84, etc.).
# Contrepartie : Dovecot passe en 2.4 (cf. tests/dovecot/).
FROM mcr.microsoft.com/devcontainers/base:trixie

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      # --- chaîne de construction ---
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
      # --- paquetage Debian (scripts/build-deb.sh, debian/) ---
      # dpkg-dev est déjà tiré par build-essential (dpkg-buildpackage) ;
      # debhelper (compat 13) et devscripts+equivs (mk-build-deps) ne le sont
      # pas.
      debhelper \
      devscripts \
      equivs \
      # --- GLib / GIO / GTK ---
      libglib2.0-dev \
      libgtk-3-dev \
      # --- SASL fort pour le client ManageSieve (SCRAM, CRAM-MD5, LOGIN…) ---
      libgsasl-dev \
      # --- trousseau : rangement du mot de passe ManageSieve (src/sieve-secret.c).
      # libsecret-1-dev est déjà tiré par evolution-dev, mais src/sieve-secret.c
      # en dépend directement — on l'installe explicitement pour que la lib
      # cliente se construise même si la dépendance transitive disparaît. ---
      libsecret-1-dev \
      # gnome-keyring : Secret Service jetable pour tests/secret/smoke.sh
      # (dbus-run-session est fourni par 'dbus', déjà présent via l'image de base).
      gnome-keyring \
      # --- en-têtes Evolution (evolution-shell-3.0.pc, evolution-mail-3.0.pc) ---
      # trixie : 3.56.2 — même version que la machine cible (API EUIManager).
      evolution-dev \
      # evolution-dev fournit e-util/e-contact-store.h qui inclut
      # <libebook/libebook.h> sans en dépendre : il faut l'ajouter à la main.
      libebook1.2-dev \
      # --- en-têtes Evolution Data Server (camel-1.2.pc) ---
      evolution-data-server-dev \
      # --- backend TLS de GIO (nécessaire à l'exécution pour le port 4190) ---
      glib-networking \
      # --- serveur ManageSieve de test (tests/dovecot/) ---
      dovecot-core \
      dovecot-managesieved \
      dovecot-sieve \
      openssl \
 && apt-get clean \
 && rm -rf /var/lib/apt/lists/*
