/* sieve-config-page.c — see sieve-config-page.h */

#include "sieve-config-page.h"

#include <string.h>

#include <gtk/gtk.h>
#include <libedataserver/libedataserver.h>
#include <libemail-engine/libemail-engine.h>   /* EMailSession */

#include <e-util/e-util.h>                 /* EExtension */
#include <mail/e-mail-config-notebook.h>
#include <mail/e-mail-config-page.h>

#include "sieve-account.h"
#include "sieve-config.h"
#include "sieve-managesieve-client.h"
#include "sieve-secret.h"

#define SIEVE_CONFIG_PAGE_SORT_ORDER  660   /* right after "Security" (600) */
#define SIEVE_DEFAULT_PORT            4190

/* Description shown at the top of the page (the "Description:" line, in
 * the style of the "Receiving Email" page). Used to be an intro
 * paragraph. */
#define SIEVE_CONFIG_PAGE_DESCRIPTION \
  "Connection settings for this account's Sieve filter server " \
  "(ManageSieve). If left blank, the receiving server's settings are " \
  "used as defaults. Rule editing is done in Edit -> Sieve Filters."

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

struct _SieveConfigPage {
  GtkScrolledWindow parent_instance;

  ESource         *account_source;  /* ref; its UID is the sieve-config key */
  ESourceRegistry *registry;        /* ref; for OAuth2 detection */
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
  GtkWidget *auto_connect_check;    /* "Connect automatically" */
  GtkWidget *test_button;           /* "Connectivity" section: "Test" button */
  GtkWidget *test_status;           /* result line below the button */
  GCancellable *test_cancellable;   /* cancels the running test (dispose) */
  gboolean   test_in_flight;        /* a test is running on a worker thread */
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
                      "Keyring entry cleared — enter a password "
                      "(it will be stored on \"Apply\").");
}

/* --- "Connectivity" section: "Test" button --------------------------- *
 *
 * Attempts a ManageSieve connection + authentication with the
 * currently entered values (nothing is saved) and shows the result.
 * All networking goes through a GTask on a worker thread — never the
 * GTK loop, nor sieve_secret_* / sieve_account_* on the main thread. */
typedef struct {
  gchar   *host;
  guint16  port;
  gboolean implicit_tls;
  gchar   *user;
  gchar   *password;            /* entered; empty -> keyring then IMAP password (EDS) */
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
  if (in->password != NULL) {
    if (*in->password != '\0')
      memset (in->password, 0, strlen (in->password));
    g_free (in->password);
  }
  g_free (in->account_uid);
  g_clear_object (&in->registry);
  g_free (in);
}

static void
test_conn_task_run (GTask *task, gpointer source_object, gpointer task_data,
                    GCancellable *cancellable)
{
  TestConnInput *in = task_data;
  GError *error = NULL;
  SieveManageSieveClient *client;
  gchar *keyring_pw = NULL;    /* sieve_secret_password_free */
  gchar *eds_pw = NULL;        /* g_free, wiped beforehand */
  const gchar *effective_pw = NULL;
  gchar *mech = NULL;          /* negotiated SASL mechanism, returned to the caller */

  (void) source_object;

  client = sieve_managesieve_client_new (in->host, in->port, in->implicit_tls);
  sieve_managesieve_client_set_timeout (client,
                                        SIEVE_MANAGESIEVE_DEFAULT_TIMEOUT_SECONDS);

  if (!sieve_managesieve_client_connect_sync (client, cancellable, &error))
    goto done;

  if (in->use_oauth2) {
    /* Access token requested from evolution-data-server (acquisition +
     * refresh delegated to EDS), passed to OAUTHBEARER / XOAUTH2. */
    gchar *token = sieve_account_dup_oauth2_token (in->registry, in->account_uid,
                                                   cancellable, NULL, &error);
    if (token == NULL)
      goto done;
    {
      SieveManageSieveAuth auth = { .authid = in->user, .oauth2_token = token };
      gboolean ok = sieve_managesieve_client_authenticate_sync (client, NULL,
                                                                &auth, cancellable,
                                                                &error);
      if (*token != '\0')
        memset (token, 0, strlen (token));
      g_free (token);
      if (!ok)
        goto done;
    }
  } else {
    /* Password resolution: entered -> plugin's keyring -> Evolution
     * account's (IMAP) password read back via EDS. Same order as the
     * editing dialog. A failed lookup is not blocking. */
    if (in->password != NULL && *in->password != '\0') {
      effective_pw = in->password;
    } else {
      keyring_pw = sieve_secret_lookup_password_sync (in->host, in->port,
                                                      in->user, cancellable, NULL);
      if (keyring_pw != NULL && *keyring_pw != '\0') {
        effective_pw = keyring_pw;
      } else if (in->account_uid != NULL) {
        eds_pw = sieve_account_dup_stored_password (in->registry, in->account_uid,
                                                    cancellable, NULL);
        effective_pw = eds_pw;
      }
    }
    {
      SieveManageSieveAuth auth = { .authid = in->user, .password = effective_pw };
      if (!sieve_managesieve_client_authenticate_sync (client, NULL, &auth,
                                                       cancellable, &error))
        goto done;
    }
  }

  mech = g_strdup (sieve_managesieve_client_get_auth_mechanism (client));

done:
  effective_pw = NULL;
  if (keyring_pw != NULL)
    sieve_secret_password_free (keyring_pw);
  if (eds_pw != NULL) {
    if (*eds_pw != '\0')
      memset (eds_pw, 0, strlen (eds_pw));
    g_free (eds_pw);
  }
  sieve_managesieve_client_disconnect (client);
  g_object_unref (client);

  if (error != NULL)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, mech, g_free);
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

  self->test_in_flight = FALSE;
  gtk_widget_set_sensitive (self->test_button, TRUE);
  g_clear_object (&self->test_cancellable);

  if (error != NULL) {
    g_autofree gchar *msg = g_strdup_printf ("Failed: %s", error->message);
    gtk_label_set_text (GTK_LABEL (self->test_status), msg);
    g_error_free (error);
  } else if (mech != NULL && *mech != '\0') {
    g_autofree gchar *msg =
      g_strdup_printf ("Connection and authentication succeeded "
                       "(SASL mechanism: %s).", mech);
    gtk_label_set_text (GTK_LABEL (self->test_status), msg);
  } else {
    gtk_label_set_text (GTK_LABEL (self->test_status),
                        "Connection and authentication succeeded.");
  }

  g_free (mech);
  g_object_unref (self);   /* ref taken when launching the test */
}

static void
sieve_config_page_test_clicked (GtkButton *button, gpointer user_data)
{
  SieveConfigPage *self = SIEVE_CONFIG_PAGE (user_data);
  const gchar *host = gtk_entry_get_text (GTK_ENTRY (self->host_entry));
  const gchar *user = gtk_entry_get_text (GTK_ENTRY (self->user_entry));
  const gchar *password =
    gtk_entry_get_text (GTK_ENTRY (self->password_entry));
  TestConnInput *in;
  GTask *task;

  (void) button;

  if (self->test_in_flight)
    return;

  if (host == NULL || *host == '\0') {
    gtk_label_set_text (GTK_LABEL (self->test_status),
                        "Please enter the server address first.");
    return;
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

  self->test_in_flight = TRUE;
  gtk_widget_set_sensitive (self->test_button, FALSE);
  gtk_label_set_text (GTK_LABEL (self->test_status), "Testing...");

  self->test_cancellable = g_cancellable_new ();
  task = g_task_new (NULL, self->test_cancellable, test_conn_done,
                     g_object_ref (self));
  g_task_set_task_data (task, in, (GDestroyNotify) test_conn_input_free);
  g_task_run_in_thread (task, test_conn_task_run);
  g_object_unref (task);
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
  iface->title = "Sieve Filters";
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
sieve_config_page_new (ESource *account_source, ESourceRegistry *registry)
{
  SieveConfigPage *self;
  GtkWidget *content;
  GtkWidget *header_grid;
  GtkWidget *type_label, *type_value, *desc_label, *desc_value;
  GtkWidget *grid;
  GtkWidget *port_label;

  self = g_object_new (SIEVE_TYPE_CONFIG_PAGE, NULL);
  self->account_source = g_object_ref (account_source);
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

  type_label = gtk_label_new ("Server Type:");
  gtk_label_set_xalign (GTK_LABEL (type_label), 1.0);
  type_value = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (type_value), "<b>SIEVE</b>");
  gtk_label_set_xalign (GTK_LABEL (type_value), 0.0);

  desc_label = gtk_label_new ("Description:");
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
                                  SIEVE_ENC_STARTTLS, "STARTTLS after connecting");
  gtk_combo_box_text_insert_text (GTK_COMBO_BOX_TEXT (self->encryption_combo),
                                  SIEVE_ENC_IMPLICIT, "TLS on a dedicated port");
  gtk_widget_set_halign (self->encryption_combo, GTK_ALIGN_START);

  self->password_label = gtk_label_new ("Password:");
  gtk_label_set_xalign (GTK_LABEL (self->password_label), 1.0);
  self->password_entry = gtk_entry_new ();
  gtk_entry_set_visibility (GTK_ENTRY (self->password_entry), FALSE);
  gtk_widget_set_hexpand (self->password_entry, TRUE);
  gtk_entry_set_placeholder_text (GTK_ENTRY (self->password_entry),
                                  "new password");
  /* Field + label hidden by default: gtk_widget_show_all must not
   * reveal them; only a click on "Forget Password" shows them
   * (load_fields hides them again on every (re)opening). */
  gtk_widget_set_no_show_all (self->password_label, TRUE);
  gtk_widget_set_no_show_all (self->password_entry, TRUE);
  self->forget_button =
    gtk_button_new_with_label ("Forget Password");
  gtk_widget_set_halign (self->forget_button, GTK_ALIGN_START);
  gtk_widget_set_tooltip_text (
    self->forget_button,
    "Clears the password stored in the keyring for this account "
    "(no effect if there is none) and shows the field to enter a "
    "new one.");
  self->forget_status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->forget_status), 0.0);
  gtk_label_set_line_wrap (GTK_LABEL (self->forget_status), TRUE);
  self->auto_connect_check =
    gtk_check_button_new_with_label ("Connect automatically on open");

  if (self->account_is_oauth2) {
    /* Token managed by Evolution: no password to enter or store. Hide
     * the "Forget" button and explain it in the status line. */
    gtk_widget_set_no_show_all (self->forget_button, TRUE);
    gtk_label_set_text (GTK_LABEL (self->forget_status),
                        "OAuth2: access token managed by Evolution, "
                        "no password to enter.");
  }

  /* "Configuration" section: server + port, then "Connect
   * automatically" right below the server field. */
  add_section_header (GTK_BOX (content), "Configuration", 0);
  grid = add_section_grid (GTK_BOX (content));

  add_field (GTK_GRID (grid), 0, "Server:", self->host_entry, 1);
  port_label = gtk_label_new ("Port:");
  gtk_label_set_xalign (GTK_LABEL (port_label), 1.0);
  gtk_grid_attach (GTK_GRID (grid), port_label, 2, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), self->port_spin, 3, 0, 1, 1);

  gtk_grid_attach (GTK_GRID (grid), self->auto_connect_check, 1, 1, 3, 1);

  /* "Security" section. */
  add_section_header (GTK_BOX (content), "Security", 0);
  grid = add_section_grid (GTK_BOX (content));
  add_field (GTK_GRID (grid), 0, "Encryption method:",
             self->encryption_combo, 1);

  /* "Authentication" section: login, then — on the same line — the
   * "Forget Password" button (by default) OR the password field
   * preceded by its "Password:" label (after clicking "Forget").
   * Everything aligned in column 1 like the login; status line
   * below. */
  add_section_header (GTK_BOX (content), "Authentication", 0);
  grid = add_section_grid (GTK_BOX (content));
  add_field (GTK_GRID (grid), 0, "Username:", self->user_entry, 3);
  gtk_grid_attach (GTK_GRID (grid), self->password_label, 0, 1, 1, 1);
  {
    GtkWidget *auth_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start (GTK_BOX (auth_box), self->password_entry,
                        TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (auth_box), self->forget_button,
                        FALSE, FALSE, 0);
    gtk_grid_attach (GTK_GRID (grid), auth_box, 1, 1, 3, 1);
  }
  gtk_grid_attach (GTK_GRID (grid), self->forget_status, 1, 2, 3, 1);

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
  add_section_header (GTK_BOX (content), "Connectivity", 0);
  grid = add_section_grid (GTK_BOX (content));
  self->test_button = gtk_button_new_with_label ("Test");
  gtk_widget_set_halign (self->test_button, GTK_ALIGN_START);
  gtk_widget_set_tooltip_text (
    self->test_button,
    "Attempts a connection and authentication to the ManageSieve "
    "server with the settings above, without saving them.");
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
                            G_CALLBACK (e_mail_config_page_changed), self);
  g_signal_connect_swapped (self->port_spin, "value-changed",
                            G_CALLBACK (e_mail_config_page_changed), self);
  g_signal_connect_swapped (self->user_entry, "changed",
                            G_CALLBACK (e_mail_config_page_changed), self);
  g_signal_connect_swapped (self->encryption_combo, "changed",
                            G_CALLBACK (e_mail_config_page_changed), self);
  g_signal_connect_swapped (self->password_entry, "changed",
                            G_CALLBACK (e_mail_config_page_changed), self);
  g_signal_connect_swapped (self->auto_connect_check, "toggled",
                            G_CALLBACK (e_mail_config_page_changed), self);
  g_signal_connect (self->forget_button, "clicked",
                    G_CALLBACK (sieve_config_page_forget_password), self);
  g_signal_connect (self->test_button, "clicked",
                    G_CALLBACK (sieve_config_page_test_clicked), self);

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

  page = sieve_config_page_new (account_source, registry);
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
