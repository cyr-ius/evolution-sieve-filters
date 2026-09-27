/* test-imap-probe.c
 *
 * Small command-line tool to validate sieve-imap-probe.c (hierarchy
 * separator discovery, issue #3) against a real IMAP server without
 * going through Evolution at all.
 *
 * Usage:
 *   test-imap-probe --host imap.example.tld --user alice [options]
 *
 * Options:
 *   --host HOST        (required)
 *   --port PORT         default: 993
 *   --user USER         (required)
 *   --password PASS     if omitted: prompted interactively (hidden)
 *   --starttls          use StartTLS instead of implicit TLS
 *   --timeout SECONDS    network timeout; 0 = unlimited; default: 15
 *
 * Prints the detected separator and exits 0, or an error message and
 * exits 1.
 */

#include <glib.h>
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

#ifdef G_OS_UNIX
#include <termios.h>
#include <unistd.h>
#endif

#include "sieve-imap-probe.h"

static gchar *
prompt_password (void)
{
  gchar buf[256];
  gchar *result;

#ifdef G_OS_UNIX
  struct termios old_term, new_term;

  fprintf (stderr, "Password: ");
  fflush (stderr);

  tcgetattr (STDIN_FILENO, &old_term);
  new_term = old_term;
  new_term.c_lflag &= ~ECHO;
  tcsetattr (STDIN_FILENO, TCSANOW, &new_term);
#else
  fprintf (stderr, "Password (visible, no hidden TTY available): ");
  fflush (stderr);
#endif

  if (fgets (buf, sizeof (buf), stdin) == NULL)
    buf[0] = '\0';

#ifdef G_OS_UNIX
  tcsetattr (STDIN_FILENO, TCSANOW, &old_term);
  fprintf (stderr, "\n");
#endif

  result = g_strchomp (g_strdup (buf));
  memset (buf, 0, sizeof (buf));
  return result;
}

int
main (int argc, char **argv)
{
  gchar *host = NULL;
  gint port = 993;
  gchar *user = NULL;
  gchar *password = NULL;
  gboolean starttls_flag = FALSE;
  gint timeout = SIEVE_IMAP_PROBE_DEFAULT_TIMEOUT_SECONDS;

  GOptionEntry entries[] = {
    { "host", 0, 0, G_OPTION_ARG_STRING, &host, "IMAP server", "HOST" },
    { "port", 0, 0, G_OPTION_ARG_INT, &port, "Port (default 993)", "PORT" },
    { "user", 0, 0, G_OPTION_ARG_STRING, &user, "Username", "USER" },
    { "password", 0, 0, G_OPTION_ARG_STRING, &password, "Password (otherwise hidden prompt)", "PASS" },
    { "starttls", 0, 0, G_OPTION_ARG_NONE, &starttls_flag, "Use StartTLS instead of implicit TLS", NULL },
    { "timeout", 0, 0, G_OPTION_ARG_INT, &timeout, "Network timeout in seconds (0 = unlimited, default 15)", "SECONDS" },
    { NULL }
  };

  GOptionContext *context;
  GError *error = NULL;
  gboolean implicit_tls;
  gchar separator = '\0';

  context = g_option_context_new ("— IMAP hierarchy separator probe test");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    fprintf (stderr, "Option error: %s\n", error->message);
    return 2;
  }
  g_option_context_free (context);

  if (host == NULL || user == NULL) {
    fprintf (stderr, "Usage: %s --host HOST --user USER [--port PORT] ...\n"
                      "See the source file header for all options.\n",
             argv[0]);
    return 2;
  }

  if (password == NULL)
    password = prompt_password ();

  implicit_tls = !starttls_flag;

  fprintf (stderr, "Connecting to %s:%d (%s)…\n", host, port,
           implicit_tls ? "implicit TLS" : "StartTLS");

  if (!sieve_imap_probe_hierarchy_separator_sync (
        host, (guint16) port, implicit_tls, user, password,
        timeout < 0 ? 0 : (guint) timeout, NULL, &separator, &error)) {
    fprintf (stderr, "Probe failed: %s\n", error->message);
    g_error_free (error);
    return 1;
  }

  printf ("Hierarchy separator: '%c'\n", separator);
  return 0;
}
