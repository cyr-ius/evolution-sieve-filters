/* sieve-config-page.c — see sieve-config-page.h */

#include "sieve-config-page.h"

#include <string.h>

#include <glib/gi18n-lib.h>

#include <gtk/gtk.h>
#include <libedataserver/libedataserver.h>
#include <libemail-engine/libemail-engine.h>   /* EMailSession */
#include <camel/camel.h>                       /* CamelNetworkSettings */

#include <e-util/e-util.h>                 /* EExtension */
#include <mail/e-mail-config-notebook.h>
#include <mail/e-mail-config-page.h>

#include "sieve-account.h"
#include "sieve-config.h"
#include "sieve-imap-probe.h"
#include "sieve-managesieve-client.h"
#include "sieve-sasl.h"
#include "sieve-secret.h"

#define SIEVE_CONFIG_PAGE_SORT_ORDER  660   /* right after "Security" (600) */
#define SIEVE_DEFAULT_PORT            4190

/* Description shown at the top of the page (the "Description:" line, in
 * the style of the "Receiving Email" page). Used to be an intro
 * paragraph. */
#define SIEVE_CONFIG_PAGE_DESCRIPTION \
  _("Connection settings for this account's Sieve filter server " \
    "(ManageSieve). If left blank, the receiving server's settings are " \
    "used as defaults. Rule editing is done in Edit -> Sieve Filters.")

/* ------------------------------------------------------------------ *
 *  The page: a GtkScrolledWindow implementing EMailConfigPage         *
 * ------------------------------------------------------------------ */

/* The EMailConfigPage interface requires "GtkBin" (like Evolution's
 * native pages, e.g. EMailConfigSecurityPage). So we derive from
 * GtkScrolledWindow and insert the content via
 * e_mail_config_page_set_content(). */

#define SIEVE_TYPE_CONFIG_PAGE (sieve_config_page_get_type ())
G_DECLARE_FINAL_TYPE (SieveConfigPage, sieve_config_page,
                      SIEVE, CONFIG_PAGE, GtkScrolledWindow)

/* Order of entries in the "Encryption method" list (mirroring
 * Evolution's "Receiving Email" page, minus "No encryption": the
 * ManageSieve client always requires TLS). */
enum {
  SIEVE_ENC_STARTTLS = 0,   /* StartTLS after connecting (implicit_tls = FALSE) */
  SIEVE_ENC_IMPLICIT = 1,   /* TLS on a dedicated port    (implicit_tls = TRUE)  */
};

/* Columns of the "Type" list model. */
enum {
  AUTH_COL_LABEL = 0,       /* G_TYPE_STRING: displayed text */
  AUTH_COL_MECH,            /* G_TYPE_STRING: SASL name, NULL = automatic */
  AUTH_COL_STRUCK,          /* G_TYPE_BOOLEAN: struck through = unsupported */
  AUTH_N_COLS
};

struct _SieveConfigPage {
  GtkScrolledWindow parent_instance;

  ESource         *account_source;  /* ref; its UID is the sieve-config key */
  ESourceRegistry *registry;        /* ref; for OAuth2 detection */
  EMailSession    *mail_session;    /* ref; the account's real IMAP host/port/
                                    * security (CamelNetworkSettings), for
                                    * "Detect automatically" (issue #3) */
  GtkWidget *host_entry;
  GtkWidget *port_spin;
  GtkWidget *user_entry;
  GtkWidget *encryption_combo;      /* StartTLS / TLS on a dedicated port */
  GtkWidget *password_label;        /* "Password:"; hidden along with the field */
  GtkWidget *password_entry;        /* hidden by default (password taken from
                                    * the keyring); revealed by "Forget" */
  GtkWidget *forget_button;         /* "Forget Password": visible by default
                                    * (outside OAuth2); clicking clears the
                                    * keyring entry, reveals the field + label,
                                    * and hides itself */
  GtkWidget *forget_status;         /* status line below the field / button */
  GtkWidget *auth_type_combo;       /* "Type:" — SASL mechanism (Automatic +
                                    * sieve_sasl_known_mechanisms()); model
                                    * columns: see AUTH_COL_* */
  GtkWidget *check_types_button;    /* "Check Supported Types" */
  GtkWidget *gssapi_hostname_entry; /* advanced: GSSAPI service principal
                                    * hostname override (empty = use
                                    * "Server:" above); see issue #2 */
  GtkWidget *auto_connect_check;    /* "Connect automatically" */
  GtkWidget *folder_separator_entry;   /* the account's real IMAP hierarchy
                                        * separator (empty = '/'); issue #3 */
  GtkWidget *detect_separator_button;  /* "Detect Automatically" */
  GtkWidget *detect_separator_status;  /* result line below the field */
  GtkWidget *test_button;           /* "Connectivity" section: "Test" button */
  GtkWidget *test_status;           /* result line below the button */
  GCancellable *test_cancellable;   /* cancels the running test / type check
                                    * (dispose) */
  gboolean   test_in_flight;        /* a test or type check is running on a
                                    * worker thread (the two buttons are
                                    * disabled together) */
  gboolean   account_is_oauth2;     /* token managed by Evolution: no password */
};

static void sieve_config_page_iface_init (EMailConfigPageInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (
  SieveConfigPage,
  sieve_config_page,
  GTK_TYPE_SCROLLED_WINDOW,
  0,
  G_IMPLEMENT_INTERFACE_DYNAMIC (E_TYPE_MAIL_CONFIG_PAGE,
                                 sieve_config_page_iface_init))

static void
sieve_config_page_dispose (GObject *object)
{
  SieveConfigPage *self = SIEVE_CONFIG_PAGE (object);

  if (self->test_cancellable != NULL)
    g_cancellable_cancel (self->test_cancellable);
  g_clear_object (&self->test_cancellable);

  g_clear_object (&self->account_source);
  g_clear_object (&self->registry);
  g_clear_object (&self->mail_session);

  G_OBJECT_CLASS (sieve_config_page_parent_class)->dispose (object);
}

static void
sieve_config_page_class_init (SieveConfigPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = sieve_config_page_dispose;
}

static void
sieve_config_page_class_finalize (SieveConfigPageClass *klass)
{
  (void) klass;
}

static void
sieve_config_page_init (SieveConfigPage *self)
{
  (void) self;
}

/* --- "Type" list (SASL mechanism) ------------------------------------ */

/* Selects `mech` (NULL / unknown name => "Automatic", row 0). */
static void
auth_type_select (SieveConfigPage *self, const gchar *mech)
{
  GtkTreeModel *model = gtk_combo_box_get_model (GTK_COMBO_BOX (self->auth_type_combo));
  GtkTreeIter iter;
  gboolean valid;

  for (valid = gtk_tree_model_get_iter_first (model, &iter); valid;
       valid = gtk_tree_model_iter_next (model, &iter)) {
    g_autofree gchar *row_mech = NULL;

    gtk_tree_model_get (model, &iter, AUTH_COL_MECH, &row_mech, -1);
    if (g_strcmp0 (row_mech, mech) == 0) {
      gtk_combo_box_set_active_iter (GTK_COMBO_BOX (self->auth_type_combo), &iter);
      return;
    }
  }
  gtk_combo_box_set_active (GTK_COMBO_BOX (self->auth_type_combo), 0);
}

/* Selected mechanism (free with g_free), NULL for "Automatic". */
static gchar *
auth_type_dup_selected (SieveConfigPage *self)
{
  GtkTreeIter iter;
  gchar *mech = NULL;

  if (gtk_combo_box_get_active_iter (GTK_COMBO_BOX (self->auth_type_combo), &iter))
    gtk_tree_model_get (gtk_combo_box_get_model (GTK_COMBO_BOX (self->auth_type_combo)),
                        &iter, AUTH_COL_MECH, &mech, -1);
  return mech;
}

/* Sets the "struck through" mark of one mechanism's row (NULL = all rows). */
static void
auth_type_set_struck (SieveConfigPage *self, const gchar *mech, gboolean struck)
{
  GtkTreeModel *model = gtk_combo_box_get_model (GTK_COMBO_BOX (self->auth_type_combo));
  GtkTreeIter iter;
  gboolean valid;

  for (valid = gtk_tree_model_get_iter_first (model, &iter); valid;
       valid = gtk_tree_model_iter_next (model, &iter)) {
    g_autofree gchar *row_mech = NULL;

    gtk_tree_model_get (model, &iter, AUTH_COL_MECH, &row_mech, -1);
    if (row_mech != NULL && (mech == NULL || g_ascii_strcasecmp (row_mech, mech) == 0))
      gtk_list_store_set (GTK_LIST_STORE (model), &iter, AUTH_COL_STRUCK, struck, -1);
  }
  /* The combo's own button caches the rendered active row. */
  gtk_widget_queue_resize (self->auth_type_combo);
}

static void check_types_set_status (SieveConfigPage *self, const gchar *text);

/* A connection setting changed: the previous "Check Supported Types"
 * verdicts no longer describe this configuration (e.g. GSSAPI struck
 * before the Kerberos hostname was filled in), so drop them. */
static void
sieve_config_page_connection_changed (SieveConfigPage *self)
{
  auth_type_set_struck (self, NULL, FALSE);
  check_types_set_status (self, NULL);
  e_mail_config_page_changed (E_MAIL_CONFIG_PAGE (self));
}

static GtkWidget *
auth_type_combo_new (void)
{
  GtkListStore *store = gtk_list_store_new (AUTH_N_COLS, G_TYPE_STRING,
                                            G_TYPE_STRING, G_TYPE_BOOLEAN);
  GtkWidget *combo = gtk_combo_box_new_with_model (GTK_TREE_MODEL (store));
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new ();
  const gchar * const *mechs = sieve_sasl_known_mechanisms ();
  GtkTreeIter iter;

  gtk_list_store_insert_with_values (store, &iter, -1,
                                     AUTH_COL_LABEL, _("Automatic"),
                                     AUTH_COL_MECH, NULL,
                                     AUTH_COL_STRUCK, FALSE, -1);
  for (gsize i = 0; mechs[i] != NULL; i++)
    gtk_list_store_insert_with_values (store, &iter, -1,
                                       AUTH_COL_LABEL, mechs[i],
                                       AUTH_COL_MECH, mechs[i],
                                       AUTH_COL_STRUCK, FALSE, -1);
  g_object_unref (store);

  gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo), renderer, TRUE);
  gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (combo), renderer,
                                 "text", AUTH_COL_LABEL);
  gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (combo), renderer,
                                 "strikethrough", AUTH_COL_STRUCK);
  gtk_widget_set_halign (combo, GTK_ALIGN_START);
  return combo;
}

/* --- EMailConfigPage: defaults / validation / commit ----------------- */

/* Seeds the fields: the account's sieve-config profile if it exists,
 * otherwise the IMAP receiving server's details (ManageSieve most
 * often shares host + login), port 4190, StartTLS.
 *
 * Called both at widget construction (the account editor does NOT
 * invoke setup_defaults — only the "new account" wizard does; without
 * this call, the page would always reopen empty even though
 * commit_changes did persist the values). */
static void
sieve_config_page_load_fields (SieveConfigPage *self)
{
  const gchar *uid = e_source_get_uid (self->account_source);
  SieveConfig *cfg = sieve_config_load_for_account (uid);
  g_autofree gchar *host = g_strdup (cfg->host);
  g_autofree gchar *user = g_strdup (cfg->user);
  guint16 port = cfg->port;

  if (host == NULL
      && e_source_has_extension (self->account_source,
                                 E_SOURCE_EXTENSION_AUTHENTICATION)) {
    ESourceAuthentication *auth =
      e_source_get_extension (self->account_source,
                              E_SOURCE_EXTENSION_AUTHENTICATION);
    host = e_source_authentication_dup_host (auth);
    if (user == NULL)
      user = e_source_authentication_dup_user (auth);
  }

  gtk_entry_set_text (GTK_ENTRY (self->host_entry), host != NULL ? host : "");
  gtk_entry_set_text (GTK_ENTRY (self->user_entry), user != NULL ? user : "");
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->port_spin),
                             port != 0 ? port : SIEVE_DEFAULT_PORT);
  gtk_combo_box_set_active (GTK_COMBO_BOX (self->encryption_combo),
                            cfg->implicit_tls ? SIEVE_ENC_IMPLICIT
                                              : SIEVE_ENC_STARTTLS);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->auto_connect_check),
                                cfg->auto_connect);
  auth_type_select (self, cfg->auth_mechanism);
  gtk_entry_set_text (GTK_ENTRY (self->gssapi_hostname_entry),
                      cfg->gssapi_hostname != NULL ? cfg->gssapi_hostname : "");
  {
    gchar sep_text[2] = { cfg->folder_separator, '\0' };
    gtk_entry_set_text (GTK_ENTRY (self->folder_separator_entry),
                        cfg->folder_separator != '\0' ? sep_text : "");
  }
  gtk_label_set_text (GTK_LABEL (self->detect_separator_status), "");

  /* Default state: password field (and its label) hidden, "Forget"
   * button visible. Connecting takes the password from the keyring;
   * only a click on "Forget" clears the entry and reveals the field. */
  if (!self->account_is_oauth2) {
    gtk_entry_set_text (GTK_ENTRY (self->password_entry), "");
    gtk_widget_hide (self->password_entry);
    gtk_widget_hide (self->password_label);
    gtk_widget_show (self->forget_button);
    gtk_label_set_text (GTK_LABEL (self->forget_status), "");
  }

  sieve_config_free (cfg);
}

static void
sieve_config_page_setup_defaults (EMailConfigPage *page)
{
  sieve_config_page_load_fields (SIEVE_CONFIG_PAGE (page));
}

/* Keyring write moved off the GTK loop (convention: never call
 * sieve_secret_* on the main thread — a locked Secret Service may open
 * a modal prompt). Best-effort, detached thread: the payload carries
 * only string copies, no widgets. */
typedef struct {
  gchar   *host;
  gchar   *user;
  gchar   *password;   /* NULL if "forget"; wiped then freed here */
  guint16  port;
  gboolean remember;
} KeyringWrite;

static gpointer
keyring_write_thread (gpointer data)
{
  KeyringWrite *w = data;
  GError *error = NULL;

  if (w->remember && w->password != NULL && *w->password != '\0') {
    if (!sieve_secret_store_password_sync (w->host, w->port, w->user,
                                           w->password, NULL, &error))
      g_warning ("sieve: failed to store password in the keyring: %s",
                 error != NULL ? error->message : "unknown error");
  } else if (!w->remember && w->host != NULL && w->user != NULL) {
    /* "Forget" request ("Forget Password" button): clear the plugin's
     * keyring entry for this triplet. */
    if (!sieve_secret_clear_password_sync (w->host, w->port, w->user,
                                           NULL, &error)
        && error != NULL)
      g_warning ("sieve: failed to remove password from the keyring: %s",
                 error->message);
  }
  g_clear_error (&error);

  if (w->password != NULL) {
    memset (w->password, 0, strlen (w->password));
    g_free (w->password);
  }
  g_free (w->host);
  g_free (w->user);
  g_free (w);
  return NULL;
}

/* Click on "Forget Password": clears the keyring entry for the
 * displayed (host, port, user) triplet — on a detached thread (never
 * sieve_secret_* on the GTK loop), with no effect if there is none —
 * then REVEALS the password field so a new one can be entered.
 * Canceling from the account editor does not restore the cleared
 * entry. */
static void
sieve_config_page_forget_password (GtkButton *button, gpointer user_data)
{
  SieveConfigPage *self = SIEVE_CONFIG_PAGE (user_data);
  const gchar *host = gtk_entry_get_text (GTK_ENTRY (self->host_entry));
  const gchar *user = gtk_entry_get_text (GTK_ENTRY (self->user_entry));

  (void) button;

  /* No host/user: nothing to clear in the keyring, but show the field
   * anyway (the user wants to enter a password). */
  if (host != NULL && *host != '\0' && user != NULL && *user != '\0') {
    KeyringWrite *w = g_new0 (KeyringWrite, 1);
    GThread *thread;

    w->host = g_strdup (host);
    w->user = g_strdup (user);
    w->port = (guint16) gtk_spin_button_get_value_as_int (
                          GTK_SPIN_BUTTON (self->port_spin));
    if (w->port == 0)
      w->port = SIEVE_DEFAULT_PORT;
    w->remember = FALSE;   /* => keyring_write_thread clears the entry */
    w->password = NULL;

    thread = g_thread_new ("sieve-keyring", keyring_write_thread, w);
    g_thread_unref (thread);
  }

  /* Reveals the field + its label, hides the "Forget" button. */
  gtk_entry_set_text (GTK_ENTRY (self->password_entry), "");
  gtk_widget_show (self->password_label);
  gtk_widget_show (self->password_entry);
  gtk_widget_hide (self->forget_button);
  gtk_widget_grab_focus (self->password_entry);
  gtk_label_set_text (GTK_LABEL (self->forget_status),
                      _("Keyring entry cleared — enter a password "
                        "(it will be stored on \"Apply\")."));
}

/* --- "Connectivity" section + "Type" check: shared worker plumbing ----- *
 *
 * "Test" attempts a ManageSieve connection + authentication with the
 * currently entered values (nothing is saved) and shows the result;
 * "Check Supported Types" probes each SASL mechanism the same way.
 * All networking goes through a GTask on a worker thread — never the
 * GTK loop, nor sieve_secret_* / sieve_account_* on the main thread. */
typedef struct {
  gchar   *host;
  guint16  port;
  gboolean implicit_tls;
  gchar   *user;
  gchar   *password;            /* entered; empty -> keyring then IMAP password (EDS) */
  gchar   *auth_mechanism;      /* forced mechanism, NULL = automatic ("Test" only) */
  gchar   *gssapi_hostname;     /* GSSAPI service principal hostname override,
                                * NULL = use `host` */
  gboolean use_oauth2;
  gchar   *account_uid;         /* for EDS: OAuth2 token or account password */
  ESourceRegistry *registry;    /* ref transferred from the main thread */
} TestConnInput;

static void
test_conn_input_free (TestConnInput *in)
{
  if (in == NULL)
    return;
  g_free (in->host);
  g_free (in->user);
  g_free (in->auth_mechanism);
  g_free (in->gssapi_hostname);
  if (in->password != NULL) {
    if (*in->password != '\0')
      memset (in->password, 0, strlen (in->password));
    g_free (in->password);
  }
  g_free (in->account_uid);
  g_clear_object (&in->registry);
  g_free (in);
}

/* Secrets resolved on the worker thread for one run. */
typedef struct {
  gchar       *token;        /* OAuth2 access token (g_free, wiped) */
  gchar       *keyring_pw;   /* sieve_secret_password_free */
  gchar       *eds_pw;       /* g_free, wiped */
  const gchar *password;     /* points into `in`, keyring_pw or eds_pw */
} ResolvedCreds;

static void
wipe_free (gchar **secret)
{
  if (*secret != NULL && **secret != '\0')
    memset (*secret, 0, strlen (*secret));
  g_clear_pointer (secret, g_free);
}

static void
resolved_creds_clear (ResolvedCreds *rc)
{
  rc->password = NULL;
  wipe_free (&rc->token);
  wipe_free (&rc->eds_pw);
  if (rc->keyring_pw != NULL) {
    sieve_secret_password_free (rc->keyring_pw);
    rc->keyring_pw = NULL;
  }
}

/* OAuth2 account: access token requested from evolution-data-server
 * (acquisition + refresh delegated to EDS). Otherwise password
 * resolution: entered -> plugin's keyring -> Evolution account's (IMAP)
 * password read back via EDS — same order as the editing dialog; a
 * failed lookup is not blocking (rc->password stays NULL). */
static gboolean
resolve_creds (const TestConnInput *in, ResolvedCreds *rc,
               GCancellable *cancellable, GError **error)
{
  if (in->use_oauth2) {
    rc->token = sieve_account_dup_oauth2_token (in->registry, in->account_uid,
                                                cancellable, NULL, error);
    return rc->token != NULL;
  }

  if (in->password != NULL && *in->password != '\0') {
    rc->password = in->password;
    return TRUE;
  }

  rc->keyring_pw = sieve_secret_lookup_password_sync (in->host, in->port,
                                                      in->user, cancellable, NULL);
  if (rc->keyring_pw != NULL && *rc->keyring_pw != '\0')
    rc->password = rc->keyring_pw;
  else if (in->account_uid != NULL) {
    rc->eds_pw = sieve_account_dup_stored_password (in->registry, in->account_uid,
                                                    cancellable, NULL);
    rc->password = rc->eds_pw;
  }
  return TRUE;
}

/* New client connected to the input's server (TLS + capabilities). */
static SieveManageSieveClient *
conn_client_connect (const TestConnInput *in, GCancellable *cancellable,
                     GError **error)
{
  SieveManageSieveClient *client =
    sieve_managesieve_client_new (in->host, in->port, in->implicit_tls);

  sieve_managesieve_client_set_timeout (client,
                                        SIEVE_MANAGESIEVE_DEFAULT_TIMEOUT_SECONDS);
  if (!sieve_managesieve_client_connect_sync (client, cancellable, error)) {
    g_object_unref (client);
    return NULL;
  }
  return client;
}

static void
test_conn_task_run (GTask *task, gpointer source_object, gpointer task_data,
                    GCancellable *cancellable)
{
  TestConnInput *in = task_data;
  GError *error = NULL;
  SieveManageSieveClient *client;
  ResolvedCreds rc = { 0 };
  gchar *mech = NULL;          /* negotiated SASL mechanism, returned to the caller */

  (void) source_object;

  client = conn_client_connect (in, cancellable, &error);
  if (client == NULL) {
    g_task_return_error (task, error);
    return;
  }

  if (resolve_creds (in, &rc, cancellable, &error)) {
    SieveManageSieveAuth auth = { .authid = in->user,
                                  .password = rc.password,
                                  .oauth2_token = rc.token,
                                  .gssapi_hostname = in->gssapi_hostname };

    if (sieve_managesieve_client_authenticate_sync (client, in->auth_mechanism,
                                                    &auth, cancellable, &error))
      mech = g_strdup (sieve_managesieve_client_get_auth_mechanism (client));
  }

  resolved_creds_clear (&rc);
  sieve_managesieve_client_disconnect (client);
  g_object_unref (client);

  if (error != NULL)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, mech, g_free);
}

/* All network buttons are disabled while one of them runs. */
static void
set_network_busy (SieveConfigPage *self, gboolean busy)
{
  self->test_in_flight = busy;
  gtk_widget_set_sensitive (self->test_button, !busy);
  gtk_widget_set_sensitive (self->check_types_button, !busy);
  gtk_widget_set_sensitive (self->detect_separator_button,
                            !busy && !self->account_is_oauth2
                              && self->mail_session != NULL);
}

/* The type check has no result line of its own: its outcome goes into
 * the button's tooltip (the struck-through entries are the visible
 * result). An empty text restores the default tooltip. */
static void
check_types_set_status (SieveConfigPage *self, const gchar *text)
{
  gtk_widget_set_tooltip_text (
    self->check_types_button,
    (text != NULL && *text != '\0')
      ? text
      : _("Tries each authentication type with the server and strikes "
          "through those that do not work. Uses the settings above, "
          "without saving them."));
}

/* Snapshot of the widgets for a worker thread; NULL (+ message on the
 * requesting button's status) if no server address was entered. */
static TestConnInput *
conn_input_from_widgets (SieveConfigPage *self, gboolean for_check)
{
  const gchar *host = gtk_entry_get_text (GTK_ENTRY (self->host_entry));
  const gchar *user = gtk_entry_get_text (GTK_ENTRY (self->user_entry));
  const gchar *password =
    gtk_entry_get_text (GTK_ENTRY (self->password_entry));
  TestConnInput *in;

  if (host == NULL || *host == '\0') {
    if (for_check)
      check_types_set_status (self, _("Please enter the server address first."));
    else
      gtk_label_set_text (GTK_LABEL (self->test_status),
                          _("Please enter the server address first."));
    return NULL;
  }

  in = g_new0 (TestConnInput, 1);
  in->host = g_strdup (host);
  in->port = (guint16) gtk_spin_button_get_value_as_int (
                          GTK_SPIN_BUTTON (self->port_spin));
  if (in->port == 0)
    in->port = SIEVE_DEFAULT_PORT;
  in->implicit_tls =
    gtk_combo_box_get_active (GTK_COMBO_BOX (self->encryption_combo))
      == SIEVE_ENC_IMPLICIT;
  in->user = g_strdup (user != NULL ? user : "");
  in->auth_mechanism = auth_type_dup_selected (self);
  {
    const gchar *gssapi_hostname =
      gtk_entry_get_text (GTK_ENTRY (self->gssapi_hostname_entry));
    in->gssapi_hostname = (gssapi_hostname != NULL && *gssapi_hostname != '\0')
                            ? g_strdup (gssapi_hostname) : NULL;
  }
  /* The password field is visible only after "Forget"; otherwise
   * resolution is left to fall back to the keyring then the account's
   * IMAP password. */
  if (gtk_widget_get_visible (self->password_entry)
      && password != NULL && *password != '\0')
    in->password = g_strdup (password);
  in->use_oauth2 = self->account_is_oauth2;
  in->account_uid = g_strdup (e_source_get_uid (self->account_source));
  in->registry =
    (self->registry != NULL) ? g_object_ref (self->registry) : NULL;
  return in;
}

/* Starts `run` on a worker thread; `done` gets a reference on the page.
 * `in` is freed with `in_free` once the task completes. */
static void
network_task_start (SieveConfigPage *self, gpointer in, GDestroyNotify in_free,
                    GAsyncReadyCallback done, GTaskThreadFunc run)
{
  GTask *task;

  set_network_busy (self, TRUE);
  self->test_cancellable = g_cancellable_new ();
  task = g_task_new (NULL, self->test_cancellable, done, g_object_ref (self));
  g_task_set_task_data (task, in, in_free);
  g_task_run_in_thread (task, run);
  g_object_unref (task);
}

static void
test_conn_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  SieveConfigPage *self = SIEVE_CONFIG_PAGE (user_data);
  GError *error = NULL;
  gchar *mech;

  (void) source;

  mech = g_task_propagate_pointer (G_TASK (res), &error);

  /* Page being destroyed (account editor closed during the test): the
   * GTask's GCancellable was canceled in dispose. Don't touch any
   * widget — they may already be destroyed. */
  if (g_cancellable_is_cancelled (g_task_get_cancellable (G_TASK (res)))) {
    g_clear_error (&error);
    g_free (mech);
    g_object_unref (self);
    return;
  }

  set_network_busy (self, FALSE);
  g_clear_object (&self->test_cancellable);

  if (error != NULL) {
    g_autofree gchar *msg = g_strdup_printf (_("Failed: %s"), error->message);
    gtk_label_set_text (GTK_LABEL (self->test_status), msg);
    g_error_free (error);
  } else if (mech != NULL && *mech != '\0') {
    g_autofree gchar *msg =
      g_strdup_printf (_("Connection and authentication succeeded "
                         "(SASL mechanism: %s)."), mech);
    gtk_label_set_text (GTK_LABEL (self->test_status), msg);
    /* Proven to work: must not stay struck from an earlier type check. */
    auth_type_set_struck (self, mech, FALSE);
  } else {
    gtk_label_set_text (GTK_LABEL (self->test_status),
                        _("Connection and authentication succeeded."));
  }

  g_free (mech);
  g_object_unref (self);   /* ref taken when launching the test */
}

static void
sieve_config_page_test_clicked (GtkButton *button, gpointer user_data)
{
  SieveConfigPage *self = SIEVE_CONFIG_PAGE (user_data);
  TestConnInput *in;

  (void) button;

  if (self->test_in_flight)
    return;

  in = conn_input_from_widgets (self, FALSE);
  if (in == NULL)
    return;

  gtk_label_set_text (GTK_LABEL (self->test_status), _("Testing..."));
  network_task_start (self, in, (GDestroyNotify) test_conn_input_free,
                     test_conn_done, test_conn_task_run);
}

/* --- "Check Supported Types" button ----------------------------------- *
 *
 * Connects, reads the server's SASL capability, then tries to
 * authenticate with each mechanism this module knows (one fresh
 * connection per attempt) and strikes through in the "Type" list those
 * that don't work.
 *
 *   - Not advertised by the server, or unusable with this account
 *     (OAuth mechanisms without an OAuth2 account, password mechanisms
 *     with one): unsupported, no login attempted.
 *   - GSSAPI: tried as such (no password involved, a failure is
 *     unambiguous).
 *   - Password / OAuth mechanisms: a failed attempt could just as well be
 *     a wrong password as a broken mechanism. So a failure only counts
 *     against the mechanism if another one of that kind succeeded; when
 *     none did, the failures are reported as "untested" (left un-struck)
 *     and the run stops after MAX_AMBIGUOUS_FAILURES of them, so a wrong
 *     password can't trip the server's brute-force protection. */

#define MAX_AMBIGUOUS_FAILURES 2
#define MAX_MECHS 16

typedef enum {
  PROBE_UNTESTED = 0,   /* no verdict: left as is */
  PROBE_OK,
  PROBE_UNSUPPORTED,
  PROBE_AMBIGUOUS,      /* worker-internal: failed, cause unknown */
} ProbeState;

typedef struct {
  ProbeState states[MAX_MECHS];  /* indexed like sieve_sasl_known_mechanisms() */
  gboolean   stopped_early;      /* credentials look wrong: run cut short */
  gboolean   no_credentials;     /* password/token mechanisms could not be tried */
  gchar     *gssapi_error;       /* why GSSAPI failed (unambiguous), or NULL */
} CheckResult;

static void
check_result_free (CheckResult *r)
{
  if (r == NULL)
    return;
  g_free (r->gssapi_error);
  g_free (r);
}

static gboolean
mech_is_oauth_name (const gchar *mech)
{
  return g_str_equal (mech, "OAUTHBEARER") || g_str_equal (mech, "XOAUTH2");
}

static gboolean
mech_offered (gchar **offered, const gchar *mech)
{
  for (gsize i = 0; offered != NULL && offered[i] != NULL; i++)
    if (g_ascii_strcasecmp (offered[i], mech) == 0)
      return TRUE;
  return FALSE;
}

static void
check_types_task_run (GTask *task, gpointer source_object, gpointer task_data,
                      GCancellable *cancellable)
{
  TestConnInput *in = task_data;
  const gchar * const *mechs = sieve_sasl_known_mechanisms ();
  CheckResult *result = g_new0 (CheckResult, 1);
  SieveManageSieveClient *client;
  ResolvedCreds rc = { 0 };
  g_auto (GStrv) offered = NULL;
  GError *error = NULL;
  gboolean any_ok = FALSE;
  guint ambiguous = 0;
  gsize n;

  (void) source_object;

  client = conn_client_connect (in, cancellable, &error);
  if (client == NULL) {
    g_free (result);
    g_task_return_error (task, error);
    return;
  }
  {
    const gchar *cap = sieve_managesieve_client_get_sasl_capability (client);

    offered = g_strsplit_set (cap != NULL ? cap : "", " \t", -1);
  }
  sieve_managesieve_client_disconnect (client);
  g_object_unref (client);

  /* A token that can't be obtained is an error worth reporting; a missing
   * password is not (those mechanisms are simply left untested). */
  if (!resolve_creds (in, &rc, cancellable, &error)) {
    g_free (result);
    g_task_return_error (task, error);
    return;
  }

  for (n = 0; mechs[n] != NULL && n < MAX_MECHS; n++) {
    const gchar *mech = mechs[n];
    gboolean is_oauth = mech_is_oauth_name (mech);
    gboolean is_gssapi = g_str_equal (mech, "GSSAPI");
    gboolean have_secret;
    SieveManageSieveAuth auth;
    GError *probe_err = NULL;
    gboolean ok;

    if (g_cancellable_is_cancelled (cancellable))
      break;

    if (!mech_offered (offered, mech) || (is_oauth && !in->use_oauth2)
        || (!is_oauth && !is_gssapi && in->use_oauth2)) {
      result->states[n] = PROBE_UNSUPPORTED;
      continue;
    }

    have_secret = is_gssapi
                  || (is_oauth ? rc.token != NULL
                               : (rc.password != NULL && *rc.password != '\0'));
    if (!have_secret) {
      result->states[n] = PROBE_UNTESTED;
      result->no_credentials = TRUE;
      continue;
    }

    if (!is_gssapi && !any_ok && ambiguous >= MAX_AMBIGUOUS_FAILURES) {
      result->stopped_early = TRUE;
      continue;   /* remains PROBE_UNTESTED */
    }

    client = conn_client_connect (in, cancellable, &probe_err);
    if (client == NULL) {
      /* Connection lost midway: keep what we learnt so far. */
      g_clear_error (&probe_err);
      break;
    }

    auth = (SieveManageSieveAuth) { .authid = in->user,
                                    .password = is_oauth ? NULL : rc.password,
                                    .oauth2_token = is_oauth ? rc.token : NULL,
                                    .gssapi_hostname = in->gssapi_hostname };
    ok = sieve_managesieve_client_authenticate_sync (client, mech, &auth,
                                                     cancellable, &probe_err);
    sieve_managesieve_client_disconnect (client);
    g_object_unref (client);
    if (!ok && is_gssapi && probe_err != NULL)
      result->gssapi_error = g_strdup (probe_err->message);
    g_clear_error (&probe_err);

    if (g_cancellable_is_cancelled (cancellable))
      break;

    if (ok) {
      result->states[n] = PROBE_OK;
      if (!is_gssapi)
        any_ok = TRUE;
    } else if (is_gssapi) {
      result->states[n] = PROBE_UNSUPPORTED;
    } else {
      result->states[n] = PROBE_AMBIGUOUS;
      ambiguous++;
    }
  }

  /* Resolve the ambiguous failures now that the whole run is known. */
  for (n = 0; n < MAX_MECHS; n++)
    if (result->states[n] == PROBE_AMBIGUOUS)
      result->states[n] = any_ok ? PROBE_UNSUPPORTED : PROBE_UNTESTED;

  resolved_creds_clear (&rc);
  g_task_return_pointer (task, result, (GDestroyNotify) check_result_free);
}

/* Builds "A, B, C" from the mechanisms whose state is `wanted`. */
static gchar *
join_mechs_in_state (const CheckResult *r, ProbeState wanted)
{
  const gchar * const *mechs = sieve_sasl_known_mechanisms ();
  g_autoptr (GString) out = g_string_new (NULL);

  for (gsize i = 0; mechs[i] != NULL && i < MAX_MECHS; i++) {
    if (r->states[i] != wanted)
      continue;
    if (out->len > 0)
      g_string_append (out, ", ");
    g_string_append (out, mechs[i]);
  }
  return g_string_free (g_steal_pointer (&out), FALSE);
}

static void
check_types_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  SieveConfigPage *self = SIEVE_CONFIG_PAGE (user_data);
  GError *error = NULL;
  CheckResult *r;
  GtkTreeModel *model;
  GtkTreeIter iter;
  g_autoptr (GString) msg = NULL;
  g_autofree gchar *selected = NULL;
  gsize idx = 0;
  gboolean valid;

  (void) source;

  r = g_task_propagate_pointer (G_TASK (res), &error);

  if (g_cancellable_is_cancelled (g_task_get_cancellable (G_TASK (res)))) {
    g_clear_error (&error);
    check_result_free (r);
    g_object_unref (self);
    return;
  }

  set_network_busy (self, FALSE);
  g_clear_object (&self->test_cancellable);

  if (r == NULL) {
    g_autofree gchar *text = g_strdup_printf (_("Failed: %s"), error->message);

    check_types_set_status (self, text);
    g_error_free (error);
    g_object_unref (self);
    return;
  }

  /* Row 0 is "Automatic"; row i + 1 is known mechanism i. Only mechanisms
   * with a verdict are touched: "untested" keeps the previous state. */
  model = gtk_combo_box_get_model (GTK_COMBO_BOX (self->auth_type_combo));
  selected = auth_type_dup_selected (self);
  valid = gtk_tree_model_get_iter_first (model, &iter);
  if (valid)
    valid = gtk_tree_model_iter_next (model, &iter);   /* skip "Automatic" */
  for (; valid && idx < MAX_MECHS; valid = gtk_tree_model_iter_next (model, &iter), idx++)
    if (r->states[idx] == PROBE_OK || r->states[idx] == PROBE_UNSUPPORTED)
      gtk_list_store_set (GTK_LIST_STORE (model), &iter, AUTH_COL_STRUCK,
                          r->states[idx] == PROBE_UNSUPPORTED, -1);

  msg = g_string_new (NULL);
  {
    g_autofree gchar *ok = join_mechs_in_state (r, PROBE_OK);
    g_autofree gchar *bad = join_mechs_in_state (r, PROBE_UNSUPPORTED);

    if (*ok != '\0')
      g_string_append_printf (msg, _("Working: %s."), ok);
    else
      g_string_append (msg, _("No type could be verified."));
    if (*bad != '\0')
      g_string_append_printf (msg, _(" Not working (struck through): %s."), bad);
  }
  if (r->stopped_early)
    g_string_append_printf (msg, " %s",
      _("The server rejected the credentials, so the remaining password "
        "types were not tried (to avoid locking the account)."));
  else if (r->no_credentials)
    g_string_append_printf (msg, " %s",
      _("Some types could not be tried: no password or token available."));
  if (selected != NULL) {
    const gchar * const *mechs = sieve_sasl_known_mechanisms ();

    for (gsize i = 0; mechs[i] != NULL && i < MAX_MECHS; i++)
      if (g_str_equal (mechs[i], selected) && r->states[i] == PROBE_UNSUPPORTED)
        g_string_append_printf (msg, " %s",
          _("The selected type does not work with this server."));
  }
  if (r->gssapi_error != NULL) {
    g_string_append_c (msg, ' ');
    /* Translators: %s is the (English) error detail. */
    g_string_append_printf (msg, _("GSSAPI failed: %s"), r->gssapi_error);
  }
  check_types_set_status (self, msg->str);

  check_result_free (r);
  g_object_unref (self);
}

static void
sieve_config_page_check_types_clicked (GtkButton *button, gpointer user_data)
{
  SieveConfigPage *self = SIEVE_CONFIG_PAGE (user_data);
  TestConnInput *in;

  (void) button;

  if (self->test_in_flight)
    return;

  in = conn_input_from_widgets (self, TRUE);
  if (in == NULL)
    return;

  check_types_set_status (self, _("Checking supported types..."));
  network_task_start (self, in, (GDestroyNotify) test_conn_input_free,
                     check_types_done, check_types_task_run);
}

/* --- "Detect Automatically" (folder separator, issue #3) -------------- *
 *
 * There is no public Camel/EDS API exposing the account's real IMAP
 * hierarchy separator (see sieve-imap-probe.h), so this opens a brief
 * IMAP connection of its own: same host/port/encryption as the
 * account's actual receiving server (CamelNetworkSettings, read from
 * the mail session -- NOT the ManageSieve settings above, which are a
 * distinct service on their own host/port), same IMAP password as
 * already resolved by EDS. Not offered for OAuth2 accounts (no
 * password to hand a plain LOGIN) -- the button is kept insensitive
 * for those (see set_network_busy()). */
typedef struct {
  gchar   *host;
  guint16  port;
  gboolean implicit_tls;
  gchar   *user;
  gchar   *account_uid;
  ESourceRegistry *registry;
} DetectSeparatorInput;

static void
detect_separator_input_free (DetectSeparatorInput *in)
{
  if (in == NULL)
    return;
  g_free (in->host);
  g_free (in->user);
  g_free (in->account_uid);
  g_clear_object (&in->registry);
  g_free (in);
}

static void
detect_separator_task_run (GTask *task, gpointer source_object, gpointer task_data,
                          GCancellable *cancellable)
{
  DetectSeparatorInput *in = task_data;
  gchar *password;
  gchar separator = '\0';
  GError *error = NULL;
  gchar *result;

  (void) source_object;

  password = sieve_account_dup_stored_password (in->registry, in->account_uid,
                                                cancellable, &error);
  if (password == NULL) {
    if (error == NULL)
      g_set_error_literal (&error, SIEVE_IMAP_PROBE_ERROR, SIEVE_IMAP_PROBE_ERROR_LOGIN,
                           "No password on file for this account (EDS keyring "
                           "empty or locked)");
    g_task_return_error (task, error);
    return;
  }

  if (!sieve_imap_probe_hierarchy_separator_sync (
        in->host, in->port, in->implicit_tls, in->user, password,
        SIEVE_IMAP_PROBE_DEFAULT_TIMEOUT_SECONDS, cancellable, &separator, &error)) {
    wipe_free (&password);
    g_task_return_error (task, error);
    return;
  }

  wipe_free (&password);
  result = g_new (gchar, 1);
  *result = separator;
  g_task_return_pointer (task, result, g_free);
}

static void
detect_separator_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  SieveConfigPage *self = SIEVE_CONFIG_PAGE (user_data);
  GError *error = NULL;
  gchar *result;

  (void) source;

  result = g_task_propagate_pointer (G_TASK (res), &error);

  if (g_cancellable_is_cancelled (g_task_get_cancellable (G_TASK (res)))) {
    g_clear_error (&error);
    g_free (result);
    g_object_unref (self);
    return;
  }

  set_network_busy (self, FALSE);
  g_clear_object (&self->test_cancellable);

  if (error != NULL) {
    g_autofree gchar *msg = g_strdup_printf (_("Detection failed: %s"), error->message);
    gtk_label_set_text (GTK_LABEL (self->detect_separator_status), msg);
    g_error_free (error);
  } else {
    gchar sep_text[2] = { *result, '\0' };
    g_autofree gchar *msg =
      (*result == '/')
        ? g_strdup (_("Detected: '/' (same as the model's default -- "
                      "nothing to translate)."))
        : g_strdup_printf (_("Detected: '%s'."), sep_text);

    gtk_entry_set_text (GTK_ENTRY (self->folder_separator_entry), sep_text);
    gtk_label_set_text (GTK_LABEL (self->detect_separator_status), msg);
  }

  g_free (result);
  g_object_unref (self);   /* ref taken when launching the task */
}

static void
sieve_config_page_detect_separator_clicked (GtkButton *button, gpointer user_data)
{
  SieveConfigPage *self = SIEVE_CONFIG_PAGE (user_data);
  CamelService *service;
  CamelSettings *settings;
  DetectSeparatorInput *in;

  (void) button;

  if (self->test_in_flight)
    return;

  if (self->account_is_oauth2 || self->mail_session == NULL) {
    gtk_label_set_text (GTK_LABEL (self->detect_separator_status),
                        _("Not available for this account."));
    return;
  }

  service = camel_session_ref_service (CAMEL_SESSION (self->mail_session),
                                       e_source_get_uid (self->account_source));
  if (service == NULL) {
    gtk_label_set_text (GTK_LABEL (self->detect_separator_status),
                        _("Could not access this account's IMAP settings."));
    return;
  }

  settings = camel_service_ref_settings (CAMEL_SERVICE (service));
  g_object_unref (service);

  if (!CAMEL_IS_NETWORK_SETTINGS (settings)) {
    g_clear_object (&settings);
    gtk_label_set_text (GTK_LABEL (self->detect_separator_status),
                        _("This account's receiving server has no network "
                          "settings to read."));
    return;
  }

  {
    CamelNetworkSettings *ns = CAMEL_NETWORK_SETTINGS (settings);
    const gchar *host = camel_network_settings_get_host (ns);
    const gchar *user = camel_network_settings_get_user (ns);
    guint16 port = camel_network_settings_get_port (ns);
    CamelNetworkSecurityMethod sec = camel_network_settings_get_security_method (ns);

    if (host == NULL || *host == '\0') {
      g_object_unref (settings);
      gtk_label_set_text (GTK_LABEL (self->detect_separator_status),
                          _("This account has no IMAP server configured."));
      return;
    }

    in = g_new0 (DetectSeparatorInput, 1);
    in->host = g_strdup (host);
    in->user = g_strdup (user != NULL ? user : "");
    in->implicit_tls = (sec == CAMEL_NETWORK_SECURITY_METHOD_SSL_ON_ALTERNATE_PORT);
    in->port = (port != 0) ? port : (in->implicit_tls ? 993 : 143);
    in->account_uid = g_strdup (e_source_get_uid (self->account_source));
    in->registry = (self->registry != NULL) ? g_object_ref (self->registry) : NULL;
  }
  g_object_unref (settings);

  gtk_label_set_text (GTK_LABEL (self->detect_separator_status), _("Detecting..."));
  network_task_start (self, in, (GDestroyNotify) detect_separator_input_free,
                      detect_separator_done, detect_separator_task_run);
}

/* Purely local settings: nothing to write to the ESource
 * (source_queue ignored), we update the account's sieve-config
 * profile. An entered password (optional) goes to the plugin's
 * keyring; left blank, the existing entry is kept (it's the "Forget"
 * button that clears it). */
static void
sieve_config_page_commit_changes (EMailConfigPage *page,
                                  GQueue          *source_queue)
{
  SieveConfigPage *self = SIEVE_CONFIG_PAGE (page);
  const gchar *uid = e_source_get_uid (self->account_source);
  SieveConfig *cfg = sieve_config_load_for_account (uid);
  const gchar *host = gtk_entry_get_text (GTK_ENTRY (self->host_entry));
  const gchar *user = gtk_entry_get_text (GTK_ENTRY (self->user_entry));
  const gchar *password =
    gtk_entry_get_text (GTK_ENTRY (self->password_entry));
  GError *error = NULL;

  (void) source_queue;

  g_free (cfg->host);
  cfg->host = (host != NULL && *host != '\0') ? g_strdup (host) : NULL;
  g_free (cfg->user);
  cfg->user = (user != NULL && *user != '\0') ? g_strdup (user) : NULL;
  cfg->port = (guint16) gtk_spin_button_get_value_as_int (
                          GTK_SPIN_BUTTON (self->port_spin));
  cfg->implicit_tls =
    gtk_combo_box_get_active (GTK_COMBO_BOX (self->encryption_combo))
      == SIEVE_ENC_IMPLICIT;
  cfg->auto_connect =
    gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->auto_connect_check));
  g_free (cfg->auth_mechanism);
  cfg->auth_mechanism = auth_type_dup_selected (self);
  {
    const gchar *gssapi_hostname =
      gtk_entry_get_text (GTK_ENTRY (self->gssapi_hostname_entry));
    g_free (cfg->gssapi_hostname);
    cfg->gssapi_hostname = (gssapi_hostname != NULL && *gssapi_hostname != '\0')
                             ? g_strdup (gssapi_hostname) : NULL;
  }
  {
    const gchar *sep =
      gtk_entry_get_text (GTK_ENTRY (self->folder_separator_entry));
    cfg->folder_separator = (sep != NULL && *sep != '\0') ? sep[0] : '\0';
  }
  /* The keyring is now the default mode (no more "Remember" checkbox:
   * any entered password is stored, the "Forget" button is what
   * clears it). */
  cfg->remember_password = TRUE;

  if (!sieve_config_save_for_account (cfg, &error)) {
    g_warning ("sieve: failed to save account settings: %s",
               error != NULL ? error->message : "unknown error");
    g_clear_error (&error);
  }

  /* Keyring: not applicable for an OAuth2 account (token managed by
   * Evolution). Otherwise, an entered password is stored; an empty
   * field means nothing is touched (the existing entry stays in
   * place, only the "Forget" button clears it). */
  if (!self->account_is_oauth2 && password != NULL && *password != '\0') {
    KeyringWrite *w = g_new0 (KeyringWrite, 1);
    GThread *thread;

    w->host = g_strdup (cfg->host);
    w->user = g_strdup (cfg->user);
    w->port = cfg->port != 0 ? cfg->port : SIEVE_DEFAULT_PORT;
    w->remember = TRUE;
    w->password = g_strdup (password);

    thread = g_thread_new ("sieve-keyring", keyring_write_thread, w);
    g_thread_unref (thread);

    /* Don't keep the secret in cleartext in the widget; return to the
     * default state (field + label hidden, "Forget" button visible). */
    gtk_entry_set_text (GTK_ENTRY (self->password_entry), "");
    gtk_widget_hide (self->password_entry);
    gtk_widget_hide (self->password_label);
    gtk_widget_show (self->forget_button);
    gtk_label_set_text (GTK_LABEL (self->forget_status), "");
  }

  sieve_config_free (cfg);
}

static void
sieve_config_page_iface_init (EMailConfigPageInterface *iface)
{
  iface->title = _("Sieve Filters");
  iface->sort_order = SIEVE_CONFIG_PAGE_SORT_ORDER;
  iface->page_type = GTK_ASSISTANT_PAGE_CONTENT;
  iface->setup_defaults = sieve_config_page_setup_defaults;
  iface->commit_changes = sieve_config_page_commit_changes;
  /* No check_complete: the spin button already bounds the port,
   * everything else is optional (falls back to the receiving
   * server's settings). */
}

/* --- Widget construction --------------------------------------------- *
 *
 * Layout modeled on Evolution's "Receiving Email" page: bold section
 * headers, right-aligned labels, a "Configuration" section (server +
 * port on the same line, login) followed by a "Security" section
 * (encryption method as a dropdown list).
 */

/* Left-aligned "<b>...</b>" section header. */
static void
add_section_header (GtkBox *box, const gchar *text, gint margin_top)
{
  GtkWidget *label = gtk_label_new (NULL);
  g_autofree gchar *markup = g_markup_printf_escaped ("<b>%s</b>", text);

  gtk_label_set_markup (GTK_LABEL (label), markup);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_set_margin_top (label, margin_top);
  gtk_box_pack_start (box, label, FALSE, FALSE, 0);
}

/* Indented grid for a section. */
static GtkWidget *
add_section_grid (GtkBox *box)
{
  GtkWidget *grid = gtk_grid_new ();

  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
  gtk_widget_set_margin_start (grid, 12);
  gtk_widget_set_margin_top (grid, 3);
  gtk_box_pack_start (box, grid, FALSE, FALSE, 0);
  return grid;
}

/* Right-aligned label in column 0 + widget starting at column 1. */
static void
add_field (GtkGrid *grid, gint row, const gchar *label_text,
           GtkWidget *widget, gint width)
{
  GtkWidget *label = gtk_label_new (label_text);

  gtk_label_set_xalign (GTK_LABEL (label), 1.0);
  gtk_grid_attach (grid, label, 0, row, 1, 1);
  gtk_grid_attach (grid, widget, 1, row, width, 1);
}

static GtkWidget *
sieve_config_page_new (ESource *account_source, ESourceRegistry *registry,
                       EMailSession *session)
{
  SieveConfigPage *self;
  GtkWidget *content;
  GtkWidget *header_grid;
  GtkWidget *type_label, *type_value, *desc_label, *desc_value;
  GtkWidget *grid;
  GtkWidget *port_label;

  self = g_object_new (SIEVE_TYPE_CONFIG_PAGE, NULL);
  self->account_source = g_object_ref (account_source);
  self->mail_session = (session != NULL) ? g_object_ref (session) : NULL;
  self->registry = (registry != NULL) ? g_object_ref (registry) : NULL;
  self->account_is_oauth2 =
    sieve_account_source_uses_oauth2 (registry, account_source);

  content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_container_set_border_width (GTK_CONTAINER (content), 0);
  e_mail_config_page_set_content (E_MAIL_CONFIG_PAGE (self), content);

  /* Header in the style of the "Receiving Email" page: "Server Type:"
   * + "Description:", right-aligned labels, left-aligned values. */
  header_grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (header_grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (header_grid), 12);
  gtk_widget_set_margin_bottom (header_grid, 6);

  type_label = gtk_label_new (_("Server Type:"));
  gtk_label_set_xalign (GTK_LABEL (type_label), 1.0);
  type_value = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (type_value), "<b>SIEVE</b>");
  gtk_label_set_xalign (GTK_LABEL (type_value), 0.0);

  desc_label = gtk_label_new (_("Description:"));
  gtk_label_set_xalign (GTK_LABEL (desc_label), 1.0);
  gtk_widget_set_valign (desc_label, GTK_ALIGN_START);
  desc_value = gtk_label_new (SIEVE_CONFIG_PAGE_DESCRIPTION);
  gtk_label_set_xalign (GTK_LABEL (desc_value), 0.0);
  gtk_label_set_line_wrap (GTK_LABEL (desc_value), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (desc_value), 60);
  gtk_widget_set_hexpand (desc_value, TRUE);

  gtk_grid_attach (GTK_GRID (header_grid), type_label, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (header_grid), type_value, 1, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (header_grid), desc_label, 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (header_grid), desc_value, 1, 1, 1, 1);
  gtk_box_pack_start (GTK_BOX (content), header_grid, FALSE, FALSE, 0);

  self->host_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (self->host_entry),
                                  "sieve.example.tld");
  gtk_widget_set_hexpand (self->host_entry, TRUE);

  self->port_spin = gtk_spin_button_new_with_range (1, 65535, 1);
  gtk_spin_button_set_numeric (GTK_SPIN_BUTTON (self->port_spin), TRUE);
  gtk_widget_set_halign (self->port_spin, GTK_ALIGN_START);

  self->user_entry = gtk_entry_new ();
  gtk_widget_set_hexpand (self->user_entry, TRUE);

  self->encryption_combo = gtk_combo_box_text_new ();
  gtk_combo_box_text_insert_text (GTK_COMBO_BOX_TEXT (self->encryption_combo),
                                  SIEVE_ENC_STARTTLS, _("STARTTLS after connecting"));
  gtk_combo_box_text_insert_text (GTK_COMBO_BOX_TEXT (self->encryption_combo),
                                  SIEVE_ENC_IMPLICIT, _("TLS on a dedicated port"));
  gtk_widget_set_halign (self->encryption_combo, GTK_ALIGN_START);

  self->auth_type_combo = auth_type_combo_new ();
  self->check_types_button =
    gtk_button_new_with_label (_("Check Supported Types"));
  check_types_set_status (self, NULL);

  self->gssapi_hostname_entry = gtk_entry_new ();
  gtk_widget_set_hexpand (self->gssapi_hostname_entry, TRUE);
  gtk_entry_set_placeholder_text (GTK_ENTRY (self->gssapi_hostname_entry),
                                  _("same as Server, above"));
  gtk_widget_set_tooltip_text (
    self->gssapi_hostname_entry,
    _("Only needed with GSSAPI, and only if the server address above is "
      "a DNS alias (CNAME): the hostname used to build the GSSAPI "
      "service principal (\"sieve/<hostname>\"). Leave empty to use the "
      "server address."));

  self->password_label = gtk_label_new (_("Password:"));
  gtk_label_set_xalign (GTK_LABEL (self->password_label), 1.0);
  self->password_entry = gtk_entry_new ();
  gtk_entry_set_visibility (GTK_ENTRY (self->password_entry), FALSE);
  gtk_widget_set_hexpand (self->password_entry, TRUE);
  gtk_entry_set_placeholder_text (GTK_ENTRY (self->password_entry),
                                  _("new password"));
  /* Field + label hidden by default: gtk_widget_show_all must not
   * reveal them; only a click on "Forget Password" shows them
   * (load_fields hides them again on every (re)opening). */
  gtk_widget_set_no_show_all (self->password_label, TRUE);
  gtk_widget_set_no_show_all (self->password_entry, TRUE);
  self->forget_button =
    gtk_button_new_with_label (_("Forget Password"));
  gtk_widget_set_halign (self->forget_button, GTK_ALIGN_START);
  gtk_widget_set_tooltip_text (
    self->forget_button,
    _("Clears the password stored in the keyring for this account "
      "(no effect if there is none) and shows the field to enter a "
      "new one."));
  self->forget_status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->forget_status), 0.0);
  gtk_label_set_line_wrap (GTK_LABEL (self->forget_status), TRUE);
  self->auto_connect_check =
    gtk_check_button_new_with_label (_("Connect automatically on open"));

  self->folder_separator_entry = gtk_entry_new ();
  gtk_entry_set_max_length (GTK_ENTRY (self->folder_separator_entry), 1);
  gtk_entry_set_width_chars (GTK_ENTRY (self->folder_separator_entry), 2);
  gtk_entry_set_placeholder_text (GTK_ENTRY (self->folder_separator_entry), "/");
  gtk_widget_set_halign (self->folder_separator_entry, GTK_ALIGN_START);
  gtk_widget_set_tooltip_text (
    self->folder_separator_entry,
    _("The character this account's real IMAP server uses between "
      "folder path segments (Dovecot with Maildir++ often uses '.'). "
      "Left empty, '/' is assumed. A fileinto action for a nested "
      "folder is translated to this separator before being sent to "
      "the server, and back to '/' when read -- without this, the "
      "server may reject the filter (\"Name must not have '/' "
      "characters\")."));
  self->detect_separator_button =
    gtk_button_new_with_label (_("Detect Automatically"));
  gtk_widget_set_tooltip_text (
    self->detect_separator_button,
    _("Opens a brief IMAP connection to this account's mail server "
      "(same host/port/encryption/password as the receiving server) "
      "to ask it directly. Not available for OAuth2 accounts -- enter "
      "the separator manually for those."));
  self->detect_separator_status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->detect_separator_status), 0.0);
  gtk_label_set_line_wrap (GTK_LABEL (self->detect_separator_status), TRUE);
  if (self->account_is_oauth2 || self->mail_session == NULL)
    gtk_widget_set_sensitive (self->detect_separator_button, FALSE);

  if (self->account_is_oauth2) {
    /* Token managed by Evolution: no password to enter or store. Hide
     * the "Forget" button and explain it in the status line. */
    gtk_widget_set_no_show_all (self->forget_button, TRUE);
    gtk_label_set_text (GTK_LABEL (self->forget_status),
                        _("OAuth2: access token managed by Evolution, "
                          "no password to enter."));
  }

  /* "Configuration" section: server + port, then "Connect
   * automatically" right below the server field. */
  add_section_header (GTK_BOX (content), _("Configuration"), 0);
  grid = add_section_grid (GTK_BOX (content));

  add_field (GTK_GRID (grid), 0, _("Server:"), self->host_entry, 1);
  port_label = gtk_label_new (_("Port:"));
  gtk_label_set_xalign (GTK_LABEL (port_label), 1.0);
  gtk_grid_attach (GTK_GRID (grid), port_label, 2, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), self->port_spin, 3, 0, 1, 1);

  gtk_grid_attach (GTK_GRID (grid), self->auto_connect_check, 1, 1, 3, 1);

  /* "Security" section. */
  add_section_header (GTK_BOX (content), _("Security"), 0);
  grid = add_section_grid (GTK_BOX (content));
  add_field (GTK_GRID (grid), 0, _("Encryption method:"),
             self->encryption_combo, 1);

  /* "Folders" section: the account's real IMAP hierarchy separator
   * (issue #3), with an automatic detection button. */
  add_section_header (GTK_BOX (content), _("Folders"), 0);
  grid = add_section_grid (GTK_BOX (content));
  {
    GtkWidget *sep_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

    gtk_box_pack_start (GTK_BOX (sep_box), self->folder_separator_entry,
                        FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (sep_box), self->detect_separator_button,
                        FALSE, FALSE, 0);
    add_field (GTK_GRID (grid), 0, _("Folder separator:"), sep_box, 3);
  }
  gtk_grid_attach (GTK_GRID (grid), self->detect_separator_status, 1, 1, 3, 1);

  /* "Authentication" section: login, then — on the same line — the
   * "Forget Password" button (by default) OR the password field
   * preceded by its "Password:" label (after clicking "Forget").
   * Everything aligned in column 1 like the login; status line
   * below. */
  add_section_header (GTK_BOX (content), _("Authentication"), 0);
  grid = add_section_grid (GTK_BOX (content));
  {
    GtkWidget *type_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

    gtk_box_pack_start (GTK_BOX (type_box), self->check_types_button,
                        FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (type_box), self->auth_type_combo,
                        FALSE, FALSE, 0);
    add_field (GTK_GRID (grid), 0, _("Type:"), type_box, 3);
  }
  add_field (GTK_GRID (grid), 1, _("Username:"), self->user_entry, 3);
  gtk_grid_attach (GTK_GRID (grid), self->password_label, 0, 2, 1, 1);
  {
    GtkWidget *auth_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start (GTK_BOX (auth_box), self->password_entry,
                        TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (auth_box), self->forget_button,
                        FALSE, FALSE, 0);
    gtk_grid_attach (GTK_GRID (grid), auth_box, 1, 2, 3, 1);
  }
  gtk_grid_attach (GTK_GRID (grid), self->forget_status, 1, 3, 3, 1);
  add_field (GTK_GRID (grid), 4, _("Kerberos hostname:"),
             self->gssapi_hostname_entry, 3);

  /* "Connectivity" section: "Test" button that attempts a ManageSieve
   * connection + authentication with the entered values (without
   * saving anything) and shows the result on the status line.
   *
   * Reduced top margin (6 instead of 12): the preceding
   * "Authentication" section ends on the "forget_status" status line
   * (a wrapping label, empty by default) which already takes up one
   * line's height; without this compensation the spacing before
   * "Connectivity" looks larger than that of the "Security" /
   * "Authentication" sections. */
  add_section_header (GTK_BOX (content), _("Connectivity"), 0);
  grid = add_section_grid (GTK_BOX (content));
  self->test_button = gtk_button_new_with_label (_("Test"));
  gtk_widget_set_halign (self->test_button, GTK_ALIGN_START);
  gtk_widget_set_tooltip_text (
    self->test_button,
    _("Attempts a connection and authentication to the ManageSieve "
      "server with the settings above, without saving them."));
  gtk_grid_attach (GTK_GRID (grid), self->test_button, 0, 0, 2, 1);
  self->test_status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->test_status), 0.0);
  gtk_label_set_line_wrap (GTK_LABEL (self->test_status), TRUE);
  gtk_grid_attach (GTK_GRID (grid), self->test_status, 0, 1, 2, 1);

  /* Populate the fields from the sieve-config profile BEFORE
   * connecting the "changed" signals (the account editor does not
   * call setup_defaults). */
  sieve_config_page_load_fields (self);

  g_signal_connect_swapped (self->host_entry, "changed",
                            G_CALLBACK (sieve_config_page_connection_changed), self);
  g_signal_connect_swapped (self->port_spin, "value-changed",
                            G_CALLBACK (sieve_config_page_connection_changed), self);
  g_signal_connect_swapped (self->user_entry, "changed",
                            G_CALLBACK (sieve_config_page_connection_changed), self);
  g_signal_connect_swapped (self->encryption_combo, "changed",
                            G_CALLBACK (sieve_config_page_connection_changed), self);
  g_signal_connect_swapped (self->auth_type_combo, "changed",
                            G_CALLBACK (e_mail_config_page_changed), self);
  g_signal_connect_swapped (self->password_entry, "changed",
                            G_CALLBACK (sieve_config_page_connection_changed), self);
  g_signal_connect_swapped (self->auto_connect_check, "toggled",
                            G_CALLBACK (e_mail_config_page_changed), self);
  g_signal_connect_swapped (self->gssapi_hostname_entry, "changed",
                            G_CALLBACK (sieve_config_page_connection_changed), self);
  g_signal_connect_swapped (self->folder_separator_entry, "changed",
                            G_CALLBACK (e_mail_config_page_changed), self);
  g_signal_connect (self->forget_button, "clicked",
                    G_CALLBACK (sieve_config_page_forget_password), self);
  g_signal_connect (self->check_types_button, "clicked",
                    G_CALLBACK (sieve_config_page_check_types_clicked), self);
  g_signal_connect (self->test_button, "clicked",
                    G_CALLBACK (sieve_config_page_test_clicked), self);
  g_signal_connect (self->detect_separator_button, "clicked",
                    G_CALLBACK (sieve_config_page_detect_separator_clicked), self);

  gtk_widget_show_all (GTK_WIDGET (self));
  return GTK_WIDGET (self);
}

/* ------------------------------------------------------------------ *
 *  The extension: hooks the page into the account config notebook    *
 * ------------------------------------------------------------------ */

#define SIEVE_TYPE_CONFIG_NOTEBOOK_EXTENSION \
  (sieve_config_notebook_extension_get_type ())
G_DECLARE_FINAL_TYPE (SieveConfigNotebookExtension,
                      sieve_config_notebook_extension,
                      SIEVE, CONFIG_NOTEBOOK_EXTENSION, EExtension)

struct _SieveConfigNotebookExtension {
  EExtension parent_instance;
};

G_DEFINE_DYNAMIC_TYPE (SieveConfigNotebookExtension,
                       sieve_config_notebook_extension,
                       E_TYPE_EXTENSION)

/* ManageSieve is IMAP's server-side companion: the page is added only
 * for accounts whose receiving backend is "imapx" (Evolution >= 3.12)
 * or "imap". POP / local / EWS / etc.: nothing to manage on the Sieve
 * side. */
static gboolean
account_backend_is_imap (ESource *account_source)
{
  ESourceBackend *backend_ext;
  g_autofree gchar *name = NULL;

  if (account_source == NULL
      || !e_source_has_extension (account_source,
                                  E_SOURCE_EXTENSION_MAIL_ACCOUNT))
    return FALSE;

  backend_ext = e_source_get_extension (account_source,
                                        E_SOURCE_EXTENSION_MAIL_ACCOUNT);
  name = e_source_backend_dup_backend_name (backend_ext);

  return name != NULL && g_ascii_strncasecmp (name, "imap", 4) == 0;
}

static void
sieve_config_notebook_extension_constructed (GObject *object)
{
  EExtension *extension = E_EXTENSION (object);
  EMailConfigNotebook *notebook;
  EMailSession *session;
  ESourceRegistry *registry;
  ESource *account_source;
  GtkWidget *page;

  G_OBJECT_CLASS (sieve_config_notebook_extension_parent_class)->constructed (object);

  notebook = E_MAIL_CONFIG_NOTEBOOK (e_extension_get_extensible (extension));
  account_source = e_mail_config_notebook_get_account_source (notebook);

  if (!account_backend_is_imap (account_source))
    return;

  session = e_mail_config_notebook_get_session (notebook);
  registry = (session != NULL) ? e_mail_session_get_registry (session) : NULL;

  page = sieve_config_page_new (account_source, registry, session);
  e_mail_config_notebook_add_page (notebook, E_MAIL_CONFIG_PAGE (page));
}

static void
sieve_config_notebook_extension_class_init (SieveConfigNotebookExtensionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  EExtensionClass *extension_class = E_EXTENSION_CLASS (klass);

  object_class->constructed = sieve_config_notebook_extension_constructed;
  extension_class->extensible_type = E_TYPE_MAIL_CONFIG_NOTEBOOK;
}

static void
sieve_config_notebook_extension_class_finalize (SieveConfigNotebookExtensionClass *klass)
{
  (void) klass;
}

static void
sieve_config_notebook_extension_init (SieveConfigNotebookExtension *self)
{
  (void) self;
}

/* ------------------------------------------------------------------ */

void
sieve_config_page_types_register (GTypeModule *type_module)
{
  sieve_config_page_register_type (type_module);
  sieve_config_notebook_extension_register_type (type_module);
}
