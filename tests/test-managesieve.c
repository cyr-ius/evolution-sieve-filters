/* test-managesieve.c
 *
 * Small command-line tool to validate the ManageSieve client against a
 * real server without going through Evolution at all.
 *
 * Usage:
 *   test-managesieve --host sieve.example.tld --user alice [options]
 *
 * Options:
 *   --host HOST        (required)
 *   --port PORT         default: 4190
 *   --user USER         (required)
 *   --password PASS     if omitted: prompted interactively (hidden)
 *   --mech MECH          force the SASL mechanism (PLAIN, LOGIN, CRAM-MD5,
 *                         SCRAM-SHA-1, SCRAM-SHA-256, OAUTHBEARER, XOAUTH2);
 *                         default: automatic negotiation
 *   --authzid ID         SASL authorization identity (rare)
 *   --oauth2-token TOK    OAuth2 access token (for OAUTHBEARER / XOAUTH2);
 *                         otherwise read from $SIEVE_OAUTH2_TOKEN
 *   --starttls           force StartTLS instead of implicit TLS
 *                         (useful for older servers on port 2000)
 *   --timeout SECONDS     network timeout (connect + each read/write);
 *                         0 = unlimited; default: 30
 *   --get NAME            print the contents of script NAME and stop
 *   --put FILE             upload the contents of FILE as a new script
 *                          named "test-managesieve" and activate it
 *   --check FILE          validate FILE's syntax server-side (CHECKSCRIPT)
 *                          without installing anything, then stop
 *   --delete NAME          delete script NAME and stop
 *   --deactivate           deactivate any active script (SETACTIVE "") and stop
 *
 * Without --get/--put/--check/--delete: just lists the scripts and prints
 * the active script's contents (default behavior, most useful for a first
 * end-to-end test).
 */

#include <glib.h>
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

#ifdef G_OS_UNIX
#include <termios.h>
#include <unistd.h>
#endif

#include "sieve-managesieve-client.h"

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

static gchar *
read_whole_file (const gchar *path, GError **error)
{
  gchar *content = NULL;
  gsize len = 0;

  if (!g_file_get_contents (path, &content, &len, error))
    return NULL;
  return content;
}

int
main (int argc, char **argv)
{
  gchar *host = NULL;
  gint port = 4190;
  gchar *user = NULL;
  gchar *password = NULL;
  gchar *mech = NULL;
  gchar *authzid = NULL;
  gchar *oauth2_token = NULL;
  gboolean starttls_flag = FALSE;
  gint timeout = SIEVE_MANAGESIEVE_DEFAULT_TIMEOUT_SECONDS;
  gchar *get_name = NULL;
  gchar *put_file = NULL;
  gchar *check_file = NULL;
  gchar *delete_name = NULL;
  gboolean deactivate_flag = FALSE;

  GOptionEntry entries[] = {
    { "host", 0, 0, G_OPTION_ARG_STRING, &host, "ManageSieve server", "HOST" },
    { "port", 0, 0, G_OPTION_ARG_INT, &port, "Port (default 4190)", "PORT" },
    { "user", 0, 0, G_OPTION_ARG_STRING, &user, "Username", "USER" },
    { "password", 0, 0, G_OPTION_ARG_STRING, &password, "Password (otherwise hidden prompt)", "PASS" },
    { "mech", 0, 0, G_OPTION_ARG_STRING, &mech, "Force the SASL mechanism (default: auto)", "MECH" },
    { "authzid", 0, 0, G_OPTION_ARG_STRING, &authzid, "SASL authorization identity", "ID" },
    { "oauth2-token", 0, 0, G_OPTION_ARG_STRING, &oauth2_token, "OAuth2 token (OAUTHBEARER/XOAUTH2)", "TOK" },
    { "starttls", 0, 0, G_OPTION_ARG_NONE, &starttls_flag, "Use StartTLS instead of implicit TLS", NULL },
    { "timeout", 0, 0, G_OPTION_ARG_INT, &timeout, "Network timeout in seconds (0 = unlimited, default 30)", "SECONDS" },
    { "get", 0, 0, G_OPTION_ARG_STRING, &get_name, "Print this script then exit", "NAME" },
    { "put", 0, 0, G_OPTION_ARG_STRING, &put_file, "Upload and activate this file then exit", "FILE" },
    { "check", 0, 0, G_OPTION_ARG_STRING, &check_file, "Validate this file server-side then exit", "FILE" },
    { "delete", 0, 0, G_OPTION_ARG_STRING, &delete_name, "Delete this script then exit", "NAME" },
    { "deactivate", 0, 0, G_OPTION_ARG_NONE, &deactivate_flag, "Deactivate any active script (SETACTIVE \"\") then exit", NULL },
    { NULL }
  };

  GOptionContext *context;
  GError *error = NULL;
  SieveManageSieveClient *client;
  gboolean implicit_tls;
  gint exit_code = 0;

  context = g_option_context_new ("— ManageSieve client test");
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

  if (oauth2_token == NULL && g_getenv ("SIEVE_OAUTH2_TOKEN") != NULL)
    oauth2_token = g_strdup (g_getenv ("SIEVE_OAUTH2_TOKEN"));

  /* No password prompt needed when authenticating with an OAuth2 token. */
  if (password == NULL && oauth2_token == NULL)
    password = prompt_password ();

  implicit_tls = !starttls_flag;

  fprintf (stderr, "Connecting to %s:%d (%s)…\n", host, port,
           implicit_tls ? "implicit TLS" : "StartTLS");

  client = sieve_managesieve_client_new (host, (guint16) port, implicit_tls);
  sieve_managesieve_client_set_timeout (client, timeout < 0 ? 0 : (guint) timeout);

  if (!sieve_managesieve_client_connect_sync (client, NULL, &error)) {
    fprintf (stderr, "Connection failed: %s\n", error->message);
    exit_code = 1;
    goto out;
  }
  fprintf (stderr, "Connected. Authenticating…\n");

  {
    SieveManageSieveAuth auth = {
      .authid = user,
      .authzid = authzid,
      .password = password,
      .oauth2_token = oauth2_token,
    };
    const gchar *sasl_cap = sieve_managesieve_client_get_sasl_capability (client);

    fprintf (stderr, "Advertised SASL mechanisms: %s\n",
             sasl_cap != NULL ? sasl_cap : "(none)");

    if (!sieve_managesieve_client_authenticate_sync (client, mech, &auth, NULL, &error)) {
      fprintf (stderr, "Authentication failed: %s\n", error->message);
      exit_code = 1;
      goto out;
    }
  }
  fprintf (stderr, "Authenticated (mechanism: %s).\n\n",
           sieve_managesieve_client_get_auth_mechanism (client));

  if (check_file != NULL) {
    gchar *content = read_whole_file (check_file, &error);
    if (content == NULL) {
      fprintf (stderr, "Could not read %s: %s\n", check_file, error->message);
      exit_code = 1;
      goto out;
    }
    if (!sieve_managesieve_client_check_script_sync (client, content, NULL, &error)) {
      fprintf (stderr, "Server rejects the script: %s\n", error->message);
      exit_code = 1;
    } else {
      fprintf (stderr, "Server validates the syntax of %s.\n", check_file);
    }
    g_free (content);
    goto out;
  }

  if (put_file != NULL) {
    gchar *content = read_whole_file (put_file, &error);
    if (content == NULL) {
      fprintf (stderr, "Could not read %s: %s\n", put_file, error->message);
      exit_code = 1;
      goto out;
    }
    if (!sieve_managesieve_client_put_script_sync (client, "test-managesieve", content, NULL, &error)) {
      fprintf (stderr, "Upload failed: %s\n", error->message);
      g_free (content);
      exit_code = 1;
      goto out;
    }
    g_free (content);
    if (!sieve_managesieve_client_set_active_sync (client, "test-managesieve", NULL, &error)) {
      fprintf (stderr, "Uploaded but activation failed: %s\n", error->message);
      exit_code = 1;
      goto out;
    }
    fprintf (stderr, "Script \"test-managesieve\" uploaded and activated.\n");
    goto out;
  }

  if (deactivate_flag) {
    if (!sieve_managesieve_client_set_active_sync (client, NULL, NULL, &error)) {
      fprintf (stderr, "Deactivation failed: %s\n", error->message);
      exit_code = 1;
      goto out;
    }
    fprintf (stderr, "No script is active anymore.\n");
    goto out;
  }

  if (delete_name != NULL) {
    if (!sieve_managesieve_client_delete_script_sync (client, delete_name, NULL, &error)) {
      fprintf (stderr, "Deletion failed: %s\n", error->message);
      exit_code = 1;
      goto out;
    }
    fprintf (stderr, "Script \"%s\" deleted.\n", delete_name);
    goto out;
  }

  if (get_name != NULL) {
    gchar *content = sieve_managesieve_client_get_script_sync (client, get_name, NULL, &error);
    if (content == NULL) {
      fprintf (stderr, "Failed: %s\n", error->message);
      exit_code = 1;
      goto out;
    }
    printf ("%s", content);
    g_free (content);
    goto out;
  }

  /* Default behavior: list, then print the active script. */
  {
    gchar *active = NULL;
    GPtrArray *names = sieve_managesieve_client_list_scripts_sync (client, &active, NULL, &error);

    if (names == NULL) {
      fprintf (stderr, "LISTSCRIPTS failed: %s\n", error->message);
      exit_code = 1;
      goto out;
    }

    fprintf (stderr, "Scripts on the server:\n");
    for (guint i = 0; i < names->len; i++) {
      const gchar *name = g_ptr_array_index (names, i);
      gboolean is_active = (active != NULL && g_strcmp0 (name, active) == 0);
      fprintf (stderr, "  - %s%s\n", name, is_active ? "  [ACTIVE]" : "");
    }
    g_ptr_array_unref (names);

    if (active != NULL) {
      gchar *content = sieve_managesieve_client_get_script_sync (client, active, NULL, &error);
      if (content == NULL) {
        fprintf (stderr, "\nFailed to read the active script: %s\n", error->message);
        exit_code = 1;
      } else {
        fprintf (stderr, "\n--- Contents of \"%s\" ---\n", active);
        printf ("%s", content);
        g_free (content);
      }
      g_free (active);
    } else {
      fprintf (stderr, "\nNo active script.\n");
    }
  }

out:
  if (error != NULL)
    g_clear_error (&error);
  sieve_managesieve_client_disconnect (client);
  g_object_unref (client);
  g_free (host);
  g_free (user);
  if (password != NULL) {
    memset (password, 0, strlen (password));
    g_free (password);
  }
  if (oauth2_token != NULL) {
    memset (oauth2_token, 0, strlen (oauth2_token));
    g_free (oauth2_token);
  }
  g_free (mech);
  g_free (authzid);
  g_free (get_name);
  g_free (put_file);
  g_free (check_file);
  g_free (delete_name);

  return exit_code;
}
