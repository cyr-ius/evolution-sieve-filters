/* sieve-editor-dialog.c
 *
 * "Sieve Filters" dialog (Edit -> Sieve Filters…): editing Sieve scripts
 * server-side. It no LONGER handles connection settings -- those are
 * configured per-account in Evolution's account editor ("Sieve Filters"
 * page, see sieve-config-page). Here we simply pick an account, connect
 * to it with the profile already saved (sieve-config, one profile per
 * ESource UID), then edit / save.
 *
 * All network operations go through a GTask run on a worker thread (the
 * sieve-managesieve-client client is 100% synchronous) so as to never
 * block Evolution's main GTK loop.
 *
 * Account choice: a drop-down menu lists Evolution's mail accounts
 * (sieve-account.c). Choosing an account loads its connection profile
 * (host, port, TLS mode, username) saved by the account editor's "Sieve
 * Filters" page. Without a usable profile, the dialog sends the user
 * there.
 *
 * Authentication: the dialog never keeps a secret in the clear, and has
 * no password field at all. On connect, on the worker thread:
 *   - OAuth2 account: an access token is requested from
 *     evolution-data-server (sieve_account_dup_oauth2_token, acquisition
 *     + refresh handled by EDS) and passed as OAUTHBEARER / XOAUTH2.
 *   - Otherwise, the password is resolved in order of preference: the
 *     plugin's own keyring (sieve-secret.c, host/port/user triplet,
 *     populated only by a SPECIFIC password entered in the account
 *     editor page) -> failing that, the Evolution account's password as
 *     stored by EDS (sieve_account_dup_stored_password), used as-is.
 *     The latter is NEVER copied into the plugin's keyring: it is
 *     re-read on every connection. Removing a specific password
 *     ("Forget") is done from the account editor page.
 *
 * Last account: the chosen account is remembered (sieve-config.c); on
 * reopening it is reselected, and if its profile is set for
 * auto-connect (checkbox in the account editor) and a host + username
 * are known, the connection is started right away (password resolved as
 * above: plugin keyring then Evolution account).
 *
 * There is no longer a Connect / Disconnect button: only a status
 * indicator (● Connected / ● Not connected) is shown.
 *   - Choosing an account in the list directly opens the session
 *     (closing a previous account's session first); going back to
 *     "— Choose an account —" closes it.
 *   - If the session has dropped, the visual editor's "Reload rules"
 *     button retries a (re)connection before re-reading the script.
 */

#include <string.h>

#include <shell/e-shell.h>
#include <mail/e-mail-backend.h>       /* EMailBackend, e_mail_backend_get_session */
#include <camel/camel.h>              /* CamelStore, camel_store_get_folder_info_sync */

#include "sieve-editor-dialog.h"
#include "sieve-account.h"
#include "sieve-managesieve-client.h"
#include "sieve-model.h"
#include "sieve-rule-editor.h"
#include "sieve-secret.h"
#include "sieve-config.h"

/* Default ManageSieve port (RFC 5804 §1.8). Not derivable from the IMAP
 * account: it's a separate service. */
#define SIEVE_DEFAULT_PORT "4190"

typedef struct {
  GtkWidget *dialog;
  GtkComboBoxText *account_combo;
  GtkWidget *stack;              /* switches visual editor / raw text */
  GtkWidget *rule_editor;        /* SieveRuleEditor: "visual" page */
  GtkWidget *editor_toolbar;     /* editor's +/-/reload bar, placed in the
                                   * dialog's action bar; hidden in "raw
                                   * text" view */
  GtkTextView *script_view;      /* "text" page */
  GtkLabel *status_label;
  GtkWidget *conn_indicator;     /* ● Connected / ● Not connected indicator */
  GtkWidget *save_button;
  GtkWidget *cancel_button;      /* visible only during a network operation */

  gboolean syncing;              /* guard: visual<->text copy in progress */

  GCancellable *op_cancellable;  /* current network operation (connect / save) */
  gboolean op_in_flight;         /* a connect/save task is running on a thread */
  gboolean dialog_destroyed;     /* the dialog was closed during this operation */

  ESourceRegistry *registry;     /* borrowed from the shell, referenced for the dialog's lifetime */
  EMailSession *mail_session;    /* same: used to enumerate the account's folders */
  GList *accounts;               /* SieveAccountInfo*; combo index = position + 1 */
  GCancellable *mbox_cancellable; /* folder enumeration in progress, cancellable */
  guint mbox_generation;         /* invalidates a stale result (account changed) */

  SieveManageSieveClient *client; /* NULL until connected */
  gchar *active_script_name;      /* currently loaded script, for SETACTIVE */

  /* "Seeded" mode (sieve_editor_dialog_new_with_seed, called by the
   * "Create a Sieve Filter…" context menu). */
  gchar     *seed_account_uid;   /* message's account to preselect/join, or NULL */
  SieveRule *seed_rule;          /* pre-filled rule to inject; owned; NULL once applied */
  gboolean   seed_injected;      /* guard: injection attempted only once */
} SieveEditorState;

static void
set_status (SieveEditorState *state, const gchar *text)
{
  gtk_label_set_text (state->status_label, text);
}

/* Defined further below; called from network task callbacks. */
static void start_connect (SieveEditorState *state);
static void remember_last_account (SieveEditorState *state);
static void on_account_changed (GtkComboBox *combo, SieveEditorState *state);

/* Reflects the connection state on the indicator. Has no effect on
 * button sensitivity, which is handled by op_begin/op_end. */
static void
update_connect_state (SieveEditorState *state)
{
  gboolean connected = (state->client != NULL);

  gtk_label_set_markup (
    GTK_LABEL (state->conn_indicator),
    connected
      ? "<span foreground=\"#2e7d32\" weight=\"bold\">●</span> Connected"
      : "<span foreground=\"#c62828\" weight=\"bold\">●</span> Not connected");
}

/* TRUE if the currently selected account has a usable ManageSieve
 * connection profile (at least a host). Used to decide whether a
 * (re)connection is possible from the "Reload rules" button. */
static gboolean
selected_account_profile_usable (SieveEditorState *state)
{
  gint active = gtk_combo_box_get_active (GTK_COMBO_BOX (state->account_combo));
  SieveAccountInfo *info;
  SieveConfig *cfg;
  gboolean usable;

  if (active <= 0)
    return FALSE;
  info = g_list_nth_data (state->accounts, active - 1);
  if (info == NULL)
    return FALSE;

  cfg = sieve_config_load_for_account (info->source_uid);
  usable = (cfg->host != NULL && *cfg->host != '\0');
  sieve_config_free (cfg);
  return usable;
}

/* Since there is no longer a Connect button, the visual editor's
 * "Reload rules" button also serves as (re)connect: it stays clickable
 * as soon as a session is open OR an account with a profile is
 * selected, as long as no network operation is in progress. */
static void
update_reload_sensitive (SieveEditorState *state)
{
  gboolean can = !state->op_in_flight &&
                 (state->client != NULL || selected_account_profile_usable (state));

  sieve_rule_editor_set_refresh_sensitive (SIEVE_RULE_EDITOR (state->rule_editor), can);
}

/* ---- Enumerating the account's folders ("fileinto" action) --------- */

typedef struct {
  CamelStore *store;     /* ref. transferred from the main thread */
  guint       generation;
} MailboxTaskInput;

static void
mailbox_task_input_free (MailboxTaskInput *in)
{
  g_clear_object (&in->store);
  g_free (in);
}

typedef struct {
  GPtrArray *paths;      /* gchar*; a final NULL makes it usable as a GStrv */
  guint      generation;
} MailboxTaskResult;

static void
mailbox_task_result_free (MailboxTaskResult *r)
{
  if (r == NULL)
    return;
  g_ptr_array_unref (r->paths);
  g_free (r);
}

/* Recursive walk of the CamelFolderInfo tree (next / child chaining): we
 * keep the full path of folders a message can actually be filed into.
 * Virtual folders ("Search Folders") and non-selectable nodes are
 * skipped, but we still descend into their children. */
static void
collect_folder_paths (CamelFolderInfo *fi, GPtrArray *out)
{
  for (; fi != NULL; fi = fi->next) {
    if ((fi->flags & CAMEL_FOLDER_VIRTUAL) == 0 &&
        (fi->flags & CAMEL_FOLDER_NOSELECT) == 0 &&
        fi->full_name != NULL && *fi->full_name != '\0')
      g_ptr_array_add (out, g_strdup (fi->full_name));
    if (fi->child != NULL)
      collect_folder_paths (fi->child, out);
  }
}

static void
mailbox_task_run (GTask *task, gpointer source_object, gpointer task_data,
                  GCancellable *cancellable)
{
  MailboxTaskInput *in = task_data;
  MailboxTaskResult *result = g_new0 (MailboxTaskResult, 1);
  CamelFolderInfo *root;
  GError *error = NULL;

  (void) source_object;
  result->generation = in->generation;
  result->paths = g_ptr_array_new_with_free_func (g_free);

  /* _FAST: no network round trip, we just use what Camel already has
   * cached -- good enough to populate a drop-down list, and with no risk
   * of blocking for long even while offline. _RECURSIVE: the whole tree
   * at once. */
  root = camel_store_get_folder_info_sync (
           in->store, NULL,
           CAMEL_STORE_FOLDER_INFO_RECURSIVE | CAMEL_STORE_FOLDER_INFO_FAST,
           cancellable, &error);

  if (root != NULL) {
    collect_folder_paths (root, result->paths);
    camel_folder_info_free (root);
  } else if (error != NULL &&
             !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_debug ("sieve: failed to enumerate account folders: %s",
             error->message);
  }
  g_clear_error (&error);

  g_ptr_array_add (result->paths, NULL); /* GStrv-style termination */
  g_task_return_pointer (task, result, (GDestroyNotify) mailbox_task_result_free);
}

static void
mailbox_task_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  SieveEditorState *state = user_data;
  GCancellable *cancellable = g_task_get_cancellable (G_TASK (res));
  MailboxTaskResult *result = g_task_propagate_pointer (G_TASK (res), NULL);

  (void) source;
  if (result == NULL)
    return;

  /* The GTask keeps the GCancellable alive for the duration of this
   * callback: this read stays safe even if the dialog has already been
   * destroyed and `state` freed (on_dialog_destroy cancels before
   * freeing). */
  if (cancellable != NULL && g_cancellable_is_cancelled (cancellable)) {
    mailbox_task_result_free (result);
    return;
  }

  /* Account changed in the meantime without cancellation (safety net). */
  if (result->generation != state->mbox_generation) {
    mailbox_task_result_free (result);
    return;
  }

  /* paths->pdata: a NULL-terminated array of gchar*, usable as-is as a
   * (const gchar * const *). The editor makes its own copy. */
  sieve_rule_editor_set_mailboxes (
    SIEVE_RULE_EDITOR (state->rule_editor),
    (const gchar * const *) result->paths->pdata);

  mailbox_task_result_free (result);
}

/* Starts (or restarts) folder enumeration for ESource `source_uid`.
 * `source_uid == NULL` (manual entry) or no mail session available:
 * clears the list -- the "fileinto" action falls back to a free text
 * field. */
static void
start_mailbox_fetch (SieveEditorState *state, const gchar *source_uid)
{
  CamelService *service;
  MailboxTaskInput *in;
  GTask *task;

  if (state->mbox_cancellable != NULL) {
    g_cancellable_cancel (state->mbox_cancellable);
    g_clear_object (&state->mbox_cancellable);
  }
  state->mbox_generation++;

  if (state->mail_session == NULL || source_uid == NULL || *source_uid == '\0') {
    sieve_rule_editor_set_mailboxes (SIEVE_RULE_EDITOR (state->rule_editor), NULL);
    return;
  }

  /* The mail account's ESource UID is also that of its CamelService in
   * the mail session. */
  service = camel_session_ref_service (CAMEL_SESSION (state->mail_session),
                                       source_uid);
  if (service == NULL || !CAMEL_IS_STORE (service)) {
    g_clear_object (&service);
    sieve_rule_editor_set_mailboxes (SIEVE_RULE_EDITOR (state->rule_editor), NULL);
    return;
  }

  state->mbox_cancellable = g_cancellable_new ();

  in = g_new0 (MailboxTaskInput, 1);
  in->store = CAMEL_STORE (service);   /* transfers the ref from ref_service */
  in->generation = state->mbox_generation;

  task = g_task_new (NULL, state->mbox_cancellable, mailbox_task_done, state);
  g_task_set_task_data (task, in, (GDestroyNotify) mailbox_task_input_free);
  g_task_run_in_thread (task, mailbox_task_run);
  g_object_unref (task);
}

/* Message shown when the chosen account has no ManageSieve server
 * configured: the user must go through the account editor. */
#define SIEVE_NO_PROFILE_HINT                                              \
  "No ManageSieve server configured for this account. Set it up in "     \
  "Edit → Accounts → this account → \"Sieve Filters\" tab."

static void
on_account_changed (GtkComboBox *combo, SieveEditorState *state)
{
  gint active = gtk_combo_box_get_active (combo);
  SieveAccountInfo *info;
  SieveConfig *cfg;
  gboolean usable;

  /* Index 0 = "— Choose an account —" row: nothing selected. Close any
   * session currently open. */
  if (active <= 0) {
    if (state->client != NULL && !state->op_in_flight) {
      sieve_managesieve_client_disconnect (state->client);
      g_clear_object (&state->client);
      g_clear_pointer (&state->active_script_name, g_free);
      gtk_widget_set_sensitive (state->save_button, FALSE);
      update_connect_state (state);
      set_status (state, "Disconnected.");
    }
    update_reload_sensitive (state);
    start_mailbox_fetch (state, NULL); /* "fileinto" field back to free entry */
    remember_last_account (state);
    return;
  }

  info = g_list_nth_data (state->accounts, active - 1);
  if (info == NULL)
    return;

  /* Connection profile configured for this account via the account
   * editor's "Sieve Filters" page. The dialog only reads it. */
  cfg = sieve_config_load_for_account (info->source_uid);
  usable = (cfg->host != NULL && *cfg->host != '\0');
  sieve_config_free (cfg);

  update_reload_sensitive (state);

  /* Populates the folder list offered by the "fileinto" action. */
  start_mailbox_fetch (state, info->source_uid);

  /* The chosen account is remembered right away: it will be reselected
   * next time the dialog opens. */
  remember_last_account (state);

  /* Choosing an account connects directly. Without a usable profile, we
   * send the user to the account editor; start_connect() closes the
   * previous session (a different account) if needed before opening the
   * new one. */
  if (usable) {
    if (!state->op_in_flight)
      start_connect (state);
  } else {
    set_status (state, SIEVE_NO_PROFILE_HINT);
  }
}

static void
populate_account_combo (SieveEditorState *state)
{
  GList *link;

  gtk_combo_box_text_append_text (state->account_combo, "— Choose an account —");

  state->accounts = sieve_account_list (state->registry);
  for (link = state->accounts; link != NULL; link = link->next) {
    SieveAccountInfo *info = link->data;
    gchar *label = g_strdup_printf ("%s  (%s)",
                                    info->display_name != NULL
                                      ? info->display_name : "(unnamed)",
                                    info->host);
    gtk_combo_box_text_append_text (state->account_combo, label);
    g_free (label);
  }

  gtk_combo_box_set_active (GTK_COMBO_BOX (state->account_combo), 0);

  if (state->accounts == NULL) {
    gtk_widget_set_tooltip_text (
      GTK_WIDGET (state->account_combo),
      state->registry == NULL
        ? "Account registry unavailable."
        : "No mail account with a server.");
  }
}

/* ---- Bridge between the visual editor and the raw text tab --------------- */

static gchar *
current_script_text (SieveEditorState *state)
{
  GtkTextBuffer *buf = gtk_text_view_get_buffer (state->script_view);
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds (buf, &start, &end);
  return gtk_text_buffer_get_text (buf, &start, &end, FALSE);
}

static void
set_script_text (SieveEditorState *state, const gchar *text)
{
  GtkTextBuffer *buf = gtk_text_view_get_buffer (state->script_view);

  gtk_text_buffer_set_text (buf, text, -1);
}

/* Visual editor model -> text buffer. */
static void
sync_visual_to_text (SieveEditorState *state)
{
  SieveRuleSet *set =
    sieve_rule_editor_dup_rule_set (SIEVE_RULE_EDITOR (state->rule_editor));
  gchar *script = sieve_rule_set_to_script (set);

  set_script_text (state, script);
  g_free (script);
  sieve_rule_set_free (set);
}

/* Text buffer -> visual editor model.
 * FALSE (with *error) if the script is outside the visual editor's scope. */
static gboolean
sync_text_to_visual (SieveEditorState *state, GError **error)
{
  gchar *script = current_script_text (state);
  SieveRuleSet *set = sieve_rule_set_parse (script, error);

  g_free (script);
  if (set == NULL)
    return FALSE;

  sieve_rule_editor_set_rule_set (SIEVE_RULE_EDITOR (state->rule_editor), set);
  sieve_rule_set_free (set);
  return TRUE;
}

/* Loads `content` into the text tab, then attempts visual editing:
 * switches to "visual" if the script is representable, otherwise stays
 * on "text". Composes the status from `ok_prefix` ("Connected. Script
 * loaded", "Rules reloaded…") and, if given, appends `extra_note`.
 * Shared by the connect-time load and the manual reload. */
static void
apply_script_to_ui (SieveEditorState *state, const gchar *content,
                    const gchar *ok_prefix, const gchar *extra_note)
{
  GError *verror = NULL;
  gchar *base;

  set_script_text (state, content);

  state->syncing = TRUE;
  if (sync_text_to_visual (state, &verror)) {
    gtk_stack_set_visible_child_name (GTK_STACK (state->stack), "visuel");
    base = g_strdup_printf ("%s — visual editing available.", ok_prefix);
  } else {
    gtk_stack_set_visible_child_name (GTK_STACK (state->stack), "texte");
    base = g_strdup_printf ("%s as raw text "
                            "(visual editing unavailable: %s).",
                            ok_prefix, verror->message);
    g_clear_error (&verror);
  }
  state->syncing = FALSE;

  if (extra_note != NULL) {
    gchar *full = g_strconcat (base, " ", extra_note, NULL);
    set_status (state, full);
    g_free (full);
  } else {
    set_status (state, base);
  }
  g_free (base);
}

/* "Seeded" mode: adds state->seed_rule (a rule pre-filled from a
 * message) as a new rule at the end of the visual editor's current rule
 * set, switches to "visual" and prompts to complete the action.
 * Idempotent (seed_injected). Called after the script loads on connect,
 * or directly if no connection could be started.
 *
 * If the server's script is lexically broken (visual editor unavailable,
 * "text" tab forced), we don't inject: overwriting the text that needs
 * fixing would do more harm than good. */
static void
inject_seed_rule (SieveEditorState *state)
{
  SieveRuleSet *set;
  gint seed_index;

  if (state->seed_rule == NULL || state->seed_injected)
    return;

  if (g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (state->stack)),
                 "texte") == 0) {
    state->seed_injected = TRUE;
    g_clear_pointer (&state->seed_rule, sieve_rule_free);
    set_status (state,
                "The server's script has a syntax error: fix it in "
                "\"Raw text\", then add the rule by hand.");
    return;
  }

  /* Current state of the visual editor: after a successful connection it
   * holds the server's script (opaque rules included); without a
   * connection it is empty. We add the seeded rule to it. */
  set = sieve_rule_editor_dup_rule_set (SIEVE_RULE_EDITOR (state->rule_editor));
  g_ptr_array_add (set->rules, state->seed_rule);
  state->seed_rule = NULL;
  state->seed_injected = TRUE;
  seed_index = (gint) set->rules->len - 1;

  state->syncing = TRUE;
  sieve_rule_editor_set_rule_set (SIEVE_RULE_EDITOR (state->rule_editor), set);
  sync_visual_to_text (state);
  state->syncing = FALSE;
  sieve_rule_set_free (set);

  gtk_stack_set_visible_child_name (GTK_STACK (state->stack), "visuel");

  /* Highlights the rule just added: selection + keyboard focus
   * (sieve_rule_editor_set_rule_set would otherwise select the first
   * rule). */
  sieve_rule_editor_select_rule (SIEVE_RULE_EDITOR (state->rule_editor),
                                 seed_index);
  set_status (state,
              "Rule pre-filled from the message (selected on the right): "
              "complete the action — e.g. \"File into\" — then "
              "\"Save\".");
}

/* ---- Lifecycle of a network operation (cancellation / errors) ------- */

/* Start of a network operation: arms a fresh GCancellable, locks the
 * Save / Reload buttons, reveals "Cancel", shows `status`. */
static void
op_begin (SieveEditorState *state, const gchar *status)
{
  g_clear_object (&state->op_cancellable);
  state->op_cancellable = g_cancellable_new ();
  state->op_in_flight = TRUE;

  sieve_rule_editor_set_refresh_sensitive (SIEVE_RULE_EDITOR (state->rule_editor), FALSE);
  /* "Cancel operation" takes the place of "Save" (same corner of the
   * editing area) for the duration of the network operation. */
  gtk_widget_set_sensitive (state->save_button, FALSE);
  gtk_widget_hide (state->save_button);
  gtk_widget_set_sensitive (state->cancel_button, TRUE);
  gtk_widget_show (state->cancel_button);

  set_status (state, status);
}

/* End of a network operation. Returns TRUE if the dialog was closed in
 * the meantime: the caller must then free the state without touching
 * any widget (they are destroyed). */
static gboolean
op_end (SieveEditorState *state)
{
  state->op_in_flight = FALSE;
  g_clear_object (&state->op_cancellable);

  if (state->dialog_destroyed) {
    g_clear_object (&state->client);
    g_clear_pointer (&state->active_script_name, g_free);
    g_free (state);
    return TRUE;
  }

  gtk_widget_set_sensitive (state->cancel_button, FALSE);
  gtk_widget_hide (state->cancel_button);
  /* "Save" gets its place back. It only becomes active again if a
   * session is open; Reload stays clickable as long as a (re)connection
   * is possible. */
  gtk_widget_show (state->save_button);
  gtk_widget_set_sensitive (state->save_button, state->client != NULL);
  update_reload_sensitive (state);
  update_connect_state (state);
  return FALSE;
}

static void
on_cancel_clicked (GtkButton *button, SieveEditorState *state)
{
  (void) button;
  if (state->op_cancellable == NULL)
    return;
  g_cancellable_cancel (state->op_cancellable);
  gtk_widget_set_sensitive (state->cancel_button, FALSE);
  set_status (state, "Cancelling…");
}

/* TRUE if the error reflects a cancellation requested by the user. */
static gboolean
error_is_cancelled (const GError *error)
{
  return g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

/* TRUE if the error makes the session unusable (connection lost, timeout,
 * protocol desync, server BYE): a reconnection is needed before any new
 * operation. */
static gboolean
error_is_fatal_for_session (const GError *error)
{
  if (error == NULL)
    return FALSE;
  if (error->domain == SIEVE_MANAGESIEVE_ERROR)
    return error->code == SIEVE_MANAGESIEVE_ERROR_SERVER_BYE
        || error->code == SIEVE_MANAGESIEVE_ERROR_TIMEOUT
        || error->code == SIEVE_MANAGESIEVE_ERROR_PROTOCOL;
  /* GIO transport errors (connection dropped, TLS broken…), excluding
   * cancellation. */
  return error->domain == G_IO_ERROR && !error_is_cancelled (error);
}

/* ---- "Connect + list + load active script" task ------------------- */

typedef struct {
  gchar *host;
  guint16 port;
  gboolean implicit_tls;
  gchar *user;
  gchar *password;        /* entered by the user; empty -> read from keyring */

  /* Selected Evolution account (index > 0): `account_source_uid` +
   * `registry` let the worker thread either request an OAuth2 token
   * from EDS (`use_oauth2`), or -- password path -- fall back to
   * re-reading the account's (IMAP) password already stored by EDS in
   * the keyring. */
  gboolean use_oauth2;
  gchar *account_source_uid;    /* the account's ESource UID, or NULL (manual entry) */
  ESourceRegistry *registry;    /* ref. transferred from the main thread */
} ConnectTaskInput;

static void
connect_task_input_free (ConnectTaskInput *in)
{
  g_free (in->host);
  g_free (in->user);
  g_free (in->password);
  g_free (in->account_source_uid);
  g_clear_object (&in->registry);
  g_free (in);
}

typedef struct {
  SieveManageSieveClient *client;
  gchar *active_name;
  gchar *script_content;
  gchar *keyring_note;   /* message to append to the status (password
                          * origin), or NULL */
} ConnectTaskResult;

static void
connect_task_result_free (ConnectTaskResult *result)
{
  if (result == NULL)
    return;
  g_clear_object (&result->client);
  g_free (result->active_name);
  g_free (result->script_content);
  g_free (result->keyring_note);
  g_free (result);
}

/* On an already-open, authenticated session: lists the scripts and
 * fetches the active script's content. `*out_active` receives the active
 * script's name (or NULL if none), `*out_content` its content (never
 * NULL on success -- placeholder text if no script is active). FALSE
 * (with *error) otherwise. Shared by the "connect" task and the
 * "reload" task. */
static gboolean
fetch_active_script (SieveManageSieveClient *client, gchar **out_active,
                     gchar **out_content, GCancellable *cancellable, GError **error)
{
  GPtrArray *names;
  gchar *active = NULL;
  gchar *content = NULL;

  names = sieve_managesieve_client_list_scripts_sync (client, &active, cancellable, error);
  if (names == NULL)
    return FALSE;
  g_ptr_array_unref (names);

  if (active != NULL) {
    content = sieve_managesieve_client_get_script_sync (client, active, cancellable, error);
    if (content == NULL) {
      g_free (active);
      return FALSE;
    }
  } else {
    content = g_strdup ("# No active script yet.\n"
                         "# Type your Sieve script then \"Save\".\n");
  }

  *out_active = active;
  *out_content = content;
  return TRUE;
}

static void
connect_task_run (GTask *task, gpointer source_object, gpointer task_data,
                   GCancellable *cancellable)
{
  ConnectTaskInput *in = task_data;
  GError *error = NULL;
  SieveManageSieveClient *client;
  gchar *active = NULL;
  gchar *content = NULL;
  gchar *keyring_pw = NULL;      /* password read from the plugin's keyring (secret_password_free) */
  gchar *eds_pw = NULL;         /* Evolution account's password re-read via EDS (g_free, wiped) */
  gboolean from_eds_keyring = FALSE;
  const gchar *effective_pw = NULL; /* entered, otherwise the keyring's (password path) */
  gchar *keyring_note = NULL;

  client = sieve_managesieve_client_new (in->host, in->port, in->implicit_tls);
  /* Safety net in case the server never responds: without this,
   * "Cancel" would be the only recourse. The client's default (30 s) is
   * fine; made explicit here so a future per-account setting could
   * adjust it. */
  sieve_managesieve_client_set_timeout (client,
                                        SIEVE_MANAGESIEVE_DEFAULT_TIMEOUT_SECONDS);

  if (!sieve_managesieve_client_connect_sync (client, cancellable, &error))
    goto fail;

  if (in->use_oauth2) {
    /* OAuth2 account (Gmail, Microsoft 365…): no password. We ask
     * evolution-data-server for the access token -- it handles initial
     * acquisition (credentials prompt) and refreshing an expired token.
     * The bare token then goes into OAUTHBEARER / XOAUTH2 (automatic
     * negotiation: these mechanisms are tried first as soon as a token
     * is supplied). */
    gchar *token = sieve_account_dup_oauth2_token (in->registry,
                                                   in->account_source_uid,
                                                   cancellable, NULL, &error);
    if (token == NULL)
      goto fail;

    {
      SieveManageSieveAuth auth = { .authid = in->user, .oauth2_token = token };
      gboolean ok = sieve_managesieve_client_authenticate_sync (client, NULL, &auth,
                                                                cancellable, &error);
      /* The token is a secret: wipe it before releasing the memory. */
      if (*token != '\0')
        memset (token, 0, strlen (token));
      g_free (token);
      if (!ok)
        goto fail;
    }

    keyring_note = g_strdup ("OAuth2 authentication (token supplied by Evolution).");
  } else {
    /* Password resolution, in order of preference:
     *   1. the one entered in the field;
     *   2. a password SPECIFIC to the plugin, stored in its own keyring
     *      (sieve-secret schema), populated only if the user entered
     *      one in the account editor page;
     *   3. failing that, the Evolution (IMAP) account's password already
     *      managed by EDS -- the common case where ManageSieve and IMAP
     *      share the same password (Dovecot). It is used AS-IS, never
     *      copied into the plugin's keyring: it is re-read on every
     *      connection.
     * A read failure (keyring absent/locked) is never blocking: auth
     * will fail further down with a clear message. */
    if (in->password != NULL && *in->password != '\0') {
      effective_pw = in->password;
    } else {
      keyring_pw = sieve_secret_lookup_password_sync (in->host, in->port, in->user,
                                                      cancellable, NULL);
      if (keyring_pw != NULL && *keyring_pw != '\0') {
        effective_pw = keyring_pw;
      } else if (in->account_source_uid != NULL) {
        eds_pw = sieve_account_dup_stored_password (in->registry,
                                                    in->account_source_uid,
                                                    cancellable, NULL);
        effective_pw = eds_pw;
        if (eds_pw != NULL)
          from_eds_keyring = TRUE;
      }
    }

    {
      /* mechanism = NULL: automatic negotiation (SCRAM if the server
       * offers it, otherwise PLAIN…). */
      SieveManageSieveAuth auth = { .authid = in->user, .password = effective_pw };
      if (!sieve_managesieve_client_authenticate_sync (client, NULL, &auth,
                                                       cancellable, &error))
        goto fail;
    }

    /* Auth OK. The password taken from the Evolution account (EDS) is
     * NOT copied into the plugin's keyring: it will be re-read on the
     * next connection. Only an explicit entry in the account editor page
     * populates the plugin's own keyring (and the "Forget" button
     * removes it from there). */
    if (from_eds_keyring)
      keyring_note = g_strdup ("Password reused from the Evolution account.");
    effective_pw = NULL; /* may point at keyring_pw / eds_pw, freed right after */
    if (keyring_pw != NULL) {
      sieve_secret_password_free (keyring_pw);
      keyring_pw = NULL;
    }
    if (eds_pw != NULL) {
      if (*eds_pw != '\0')
        memset (eds_pw, 0, strlen (eds_pw));
      g_clear_pointer (&eds_pw, g_free);
    }
  }

  if (!fetch_active_script (client, &active, &content, cancellable, &error))
    goto fail;

  {
    ConnectTaskResult *result = g_new0 (ConnectTaskResult, 1);
    result->client = client;
    result->active_name = active; /* ownership transferred */
    result->script_content = content;
    result->keyring_note = keyring_note; /* ownership transferred */
    /* The destroy_notify covers the race where the operation completes
     * just as the user cancels: GTask then discards this result instead
     * of handing it to connect_task_done, and frees it cleanly (the
     * freshly opened client is closed again). */
    g_task_return_pointer (task, result,
                           (GDestroyNotify) connect_task_result_free);
  }
  return;

fail:
  g_clear_object (&client);
  g_clear_pointer (&active, g_free);
  g_clear_pointer (&content, g_free);
  if (keyring_pw != NULL)
    sieve_secret_password_free (keyring_pw);
  if (eds_pw != NULL) {
    if (*eds_pw != '\0')
      memset (eds_pw, 0, strlen (eds_pw));
    g_free (eds_pw);
  }
  g_free (keyring_note);
  g_task_return_error (task, error);
}

static void
connect_task_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  SieveEditorState *state = user_data;
  GError *error = NULL;
  ConnectTaskResult *result = g_task_propagate_pointer (G_TASK (res), &error);

  if (op_end (state)) {
    /* Dialog closed during the connection: touch nothing, just free. */
    connect_task_result_free (result);
    g_clear_error (&error);
    return;
  }

  if (result == NULL) {
    /* Failure (or cancellation): stay cleanly disconnected. */
    g_clear_object (&state->client);
    g_clear_pointer (&state->active_script_name, g_free);
    gtk_widget_set_sensitive (state->save_button, FALSE);

    if (error_is_cancelled (error)) {
      set_status (state, "Connection cancelled.");
    } else {
      gchar *msg = g_strdup_printf ("Connection failed: %s", error->message);
      set_status (state, msg);
      g_free (msg);
    }
    g_clear_error (&error);

    /* Context menu: the server couldn't be reached, but the user keeps
     * their pre-filled rule and can retry via "Reload rules". */
    if (state->seed_rule != NULL && !state->seed_injected) {
      gchar *hint;
      inject_seed_rule (state);
      hint = g_strconcat (gtk_label_get_text (state->status_label),
                          " (offline: use \"Reload rules\" to send it.)",
                          NULL);
      set_status (state, hint);
      g_free (hint);
    }
    return;
  }

  state->client = result->client;
  result->client = NULL;                 /* ownership transferred to the state */
  g_free (state->active_script_name);
  state->active_script_name = result->active_name; /* may be NULL */
  result->active_name = NULL;

  /* Session open: light up the indicator, and remember the account as
   * the last one used -- it's the one that will be reselected / rejoined
   * next time the dialog opens if its profile is set for auto-connect. */
  update_connect_state (state);
  remember_last_account (state);

  gtk_widget_set_sensitive (state->save_button, TRUE);
  update_reload_sensitive (state);

  /* Attempt visual editing; if the loaded script is beyond what the
   * editor can represent, stay on the raw text tab. */
  apply_script_to_ui (state, result->script_content,
                      "Connected. Script loaded", result->keyring_note);

  /* "Seeded" mode (context menu): the server's script is loaded, now add
   * the rule pre-filled from the message on top of it. */
  inject_seed_rule (state);

  connect_task_result_free (result);
}

/* Opens a session: reads the chosen account's connection profile (set in
 * the account editor), then runs the "connect + list + load active
 * script" task on a worker thread. Without a usable profile, sends the
 * user to the account editor. */
static void
start_connect (SieveEditorState *state)
{
  GTask *task;
  ConnectTaskInput *in;
  SieveAccountInfo *info;
  SieveConfig *cfg;
  const gchar *eff_user;
  gint active;

  if (state->op_in_flight)
    return;

  active = gtk_combo_box_get_active (GTK_COMBO_BOX (state->account_combo));
  if (active <= 0) {
    set_status (state, "Choose an account first.");
    return;
  }
  info = g_list_nth_data (state->accounts, active - 1);
  if (info == NULL)
    return;

  cfg = sieve_config_load_for_account (info->source_uid);
  if (cfg->host == NULL || *cfg->host == '\0') {
    set_status (state, SIEVE_NO_PROFILE_HINT);
    sieve_config_free (cfg);
    return;
  }

  eff_user = (cfg->user != NULL && *cfg->user != '\0')
               ? cfg->user
               : (info->user != NULL ? info->user : "");

  in = g_new0 (ConnectTaskInput, 1);
  in->host = g_strdup (cfg->host);
  in->port = cfg->port != 0
               ? cfg->port
               : (guint16) g_ascii_strtoull (SIEVE_DEFAULT_PORT, NULL, 10);
  in->implicit_tls = cfg->implicit_tls;
  in->user = g_strdup (eff_user);
  in->password = NULL;   /* no field here: a password specific to the
                          * plugin (its own keyring, populated from the
                          * account editor's "Sieve Filters" page) then,
                          * failing that, the account's password re-read
                          * via EDS and used as-is (never copied into the
                          * plugin's keyring). */

  /* We pass the registry + the ESource UID to the worker thread, to
   * request there either an OAuth2 token, or -- password path -- the
   * (IMAP) account's password already stored by EDS in the keyring
   * (failing a plugin-specific password). */
  in->use_oauth2 = info->uses_oauth2;
  in->account_source_uid = g_strdup (info->source_uid);
  in->registry = (state->registry != NULL) ? g_object_ref (state->registry) : NULL;

  sieve_config_free (cfg);

  /* Reconnect / account change: close the current session first (stream
   * shutdown, non-blocking) so it doesn't leak when connect_task_done
   * reassigns state->client. */
  if (state->client != NULL) {
    sieve_managesieve_client_disconnect (state->client);
    g_clear_object (&state->client);
    g_clear_pointer (&state->active_script_name, g_free);
    update_connect_state (state);
  }

  op_begin (state, in->use_oauth2
                     ? "Connecting via OAuth2… (token requested from Evolution)"
                     : "Connecting… (\"Cancel\" to interrupt)");

  task = g_task_new (NULL, state->op_cancellable, connect_task_done, state);
  g_task_set_task_data (task, in, (GDestroyNotify) connect_task_input_free);
  g_task_run_in_thread (task, connect_task_run);
  g_object_unref (task);
}

/* ---- "Reload rules from the server" task ----------------------- */

typedef struct {
  gchar *active_name;
  gchar *script_content;
} ReloadTaskResult;

static void
reload_task_result_free (ReloadTaskResult *r)
{
  if (r == NULL)
    return;
  g_free (r->active_name);
  g_free (r->script_content);
  g_free (r);
}

static void
reload_task_run (GTask *task, gpointer source_object, gpointer task_data,
                 GCancellable *cancellable)
{
  SieveManageSieveClient *client = task_data;   /* borrowed from the state */
  GError *error = NULL;
  gchar *active = NULL;
  gchar *content = NULL;

  (void) source_object;

  if (!fetch_active_script (client, &active, &content, cancellable, &error)) {
    g_task_return_error (task, error);
    return;
  }

  {
    ReloadTaskResult *result = g_new0 (ReloadTaskResult, 1);
    result->active_name = active;
    result->script_content = content;
    g_task_return_pointer (task, result, (GDestroyNotify) reload_task_result_free);
  }
}

static void
reload_task_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  SieveEditorState *state = user_data;
  GError *error = NULL;
  ReloadTaskResult *result = g_task_propagate_pointer (G_TASK (res), &error);

  (void) source;

  if (op_end (state)) {         /* dialog closed during the reload */
    reload_task_result_free (result);
    g_clear_error (&error);
    return;
  }

  if (result == NULL) {
    if (error_is_cancelled (error)) {
      set_status (state, "Reload cancelled.");
    } else if (error_is_fatal_for_session (error)) {
      /* Connection lost: the session is no longer reliable. "Reload
       * rules" will start a fresh connection. */
      gchar *msg = g_strdup_printf ("Connection lost during reload: %s. "
                                    "Use \"Reload rules\" to retry the connection.",
                                    error->message);
      set_status (state, msg);
      g_free (msg);
      g_clear_object (&state->client);
      g_clear_pointer (&state->active_script_name, g_free);
      gtk_widget_set_sensitive (state->save_button, FALSE);
      update_reload_sensitive (state);
      update_connect_state (state);
    } else {
      gchar *msg = g_strdup_printf ("Reload failed: %s", error->message);
      set_status (state, msg);
      g_free (msg);
    }
    g_clear_error (&error);
    return;
  }

  g_free (state->active_script_name);
  state->active_script_name = result->active_name; /* may be NULL */
  result->active_name = NULL;

  apply_script_to_ui (state, result->script_content,
                      "Rules reloaded from the server", NULL);

  reload_task_result_free (result);
}

/* Re-reads the active script on the already-open session and re-injects
 * it into the editor. Reuses `state->client` (like the "save" task):
 * borrowed client, no destroy_notify -- the GCancellable is cancelled by
 * op_end / on_dialog_destroy, and the task finishes before op_end frees
 * the client. */
static void
start_reload (SieveEditorState *state)
{
  GTask *task;

  if (state->op_in_flight || state->client == NULL)
    return;

  op_begin (state, "Reloading rules… (\"Cancel\" to interrupt)");

  task = g_task_new (NULL, state->op_cancellable, reload_task_done, state);
  g_task_set_task_data (task, state->client, NULL);
  g_task_run_in_thread (task, reload_task_run);
  g_object_unref (task);
}

/* The visual editor requests a reload (its toolbar's "Reload" button,
 * next to "+" / "-"). Two cases:
 *   - session closed: this button acts as (re)connect -- we call
 *     start_connect(), which will reload the active script right after;
 *   - session open: confirm first (reloading overwrites unsaved local
 *     changes), then re-read the script. */
static void
on_rule_editor_refresh (GtkWidget *editor, SieveEditorState *state)
{
  GtkWidget *confirm;
  gint resp;

  (void) editor;

  if (state->op_in_flight)
    return;

  if (state->client == NULL) {
    /* No session: (re)connect. start_connect() sets its own status
     * ("Connecting…" or redirect to the account editor if no usable
     * profile). */
    start_connect (state);
    return;
  }

  confirm = gtk_message_dialog_new (
    GTK_WINDOW (state->dialog),
    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
    GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
    "Reload rules from the server?");
  gtk_message_dialog_format_secondary_text (
    GTK_MESSAGE_DIALOG (confirm),
    "Unsaved changes will be lost.");
  gtk_dialog_add_buttons (GTK_DIALOG (confirm),
                          "_Cancel", GTK_RESPONSE_CANCEL,
                          "_Reload", GTK_RESPONSE_ACCEPT, NULL);
  gtk_dialog_set_default_response (GTK_DIALOG (confirm), GTK_RESPONSE_CANCEL);
  resp = gtk_dialog_run (GTK_DIALOG (confirm));
  gtk_widget_destroy (confirm);

  if (resp == GTK_RESPONSE_ACCEPT)
    start_reload (state);
}

/* ---- "Save + activate" task ------------------------------------ */

typedef struct {
  SieveManageSieveClient *client;  /* borrowed from the state (not owned) */
  gchar *name;
  gchar *content;
} SaveTaskInput;

static void
save_task_input_free (SaveTaskInput *in)
{
  g_free (in->name);
  g_free (in->content);
  g_free (in);
}

static void
save_task_run (GTask *task, gpointer source_object, gpointer task_data,
               GCancellable *cancellable)
{
  SaveTaskInput *in = task_data;
  GError *error = NULL;

  if (!sieve_managesieve_client_put_script_sync (in->client, in->name, in->content,
                                                  cancellable, &error)) {
    g_task_return_error (task, error);
    return;
  }
  if (!sieve_managesieve_client_set_active_sync (in->client, in->name, cancellable, &error)) {
    g_task_return_error (task, error);
    return;
  }
  g_task_return_boolean (task, TRUE);
}

static void
save_task_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  SieveEditorState *state = user_data;
  GError *error = NULL;
  gboolean ok = g_task_propagate_boolean (G_TASK (res), &error);

  if (op_end (state)) {         /* dialog closed during the save */
    g_clear_error (&error);
    return;
  }

  if (ok) {
    set_status (state, "Script saved and activated.");
    return;
  }

  if (error_is_cancelled (error)) {
    set_status (state, "Save cancelled — the script may not have been "
                       "fully transmitted; check on the server.");
  } else if (error_is_fatal_for_session (error)) {
    /* Connection lost / timed out while sending: the session is no
     * longer reliable. "Reload rules" will start a fresh connection. */
    gchar *msg = g_strdup_printf ("Connection lost during save: %s. "
                                  "Use \"Reload rules\" to retry the connection.",
                                  error->message);
    set_status (state, msg);
    g_free (msg);
    g_clear_object (&state->client);
    g_clear_pointer (&state->active_script_name, g_free);
    gtk_widget_set_sensitive (state->save_button, FALSE);
    update_reload_sensitive (state);
    update_connect_state (state);
  } else {
    gchar *msg = g_strdup_printf ("Save failed: %s", error->message);
    set_status (state, msg);
    g_free (msg);
  }
  g_clear_error (&error);
}

static void
on_save_clicked (GtkButton *button, SieveEditorState *state)
{
  GTask *task;
  SaveTaskInput *in;
  GtkTextBuffer *buf;
  GtkTextIter start, end;

  if (state->op_in_flight)
    return;

  if (state->client == NULL) {
    set_status (state, "No session: use \"Reload rules\" to "
                       "(re)connect first.");
    return;
  }

  /* If the user is on the visual editor, regenerate the text from the
   * model first: saving always starts from the buffer. */
  if (g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (state->stack)),
                 "visuel") == 0) {
    state->syncing = TRUE;
    sync_visual_to_text (state);
    state->syncing = FALSE;
  }

  in = g_new0 (SaveTaskInput, 1);
  in->client = state->client;
  /* TODO: offer a script name chosen by the user instead of always
   * reusing/overwriting the active script (or "evolution" by default if
   * no script existed). */
  in->name = g_strdup (state->active_script_name != NULL
                        ? state->active_script_name : "evolution");

  buf = gtk_text_view_get_buffer (state->script_view);
  gtk_text_buffer_get_bounds (buf, &start, &end);
  in->content = gtk_text_buffer_get_text (buf, &start, &end, FALSE);

  (void) button;
  op_begin (state, "Saving… (\"Cancel\" to interrupt)");

  task = g_task_new (NULL, state->op_cancellable, save_task_done, state);
  g_task_set_task_data (task, in, (GDestroyNotify) save_task_input_free);
  g_task_run_in_thread (task, save_task_run);
  g_object_unref (task);
}

/* ---- Visual / text switch ----------------------------------------- */

static void
on_stack_switch (GObject *object, GParamSpec *pspec, SieveEditorState *state)
{
  const gchar *name;

  (void) object;
  (void) pspec;
  if (state->syncing)
    return;

  name = gtk_stack_get_visible_child_name (GTK_STACK (state->stack));

  /* The +/-/reload buttons only concern the rule list: hide them while
   * editing the script as raw text. */
  gtk_widget_set_visible (state->editor_toolbar,
                          g_strcmp0 (name, "visuel") == 0);

  if (g_strcmp0 (name, "texte") == 0) {
    state->syncing = TRUE;
    sync_visual_to_text (state);
    state->syncing = FALSE;
  } else if (g_strcmp0 (name, "visuel") == 0) {
    GError *error = NULL;

    state->syncing = TRUE;
    if (!sync_text_to_visual (state, &error)) {
      gchar *msg = g_strdup_printf ("Visual editing unavailable: %s. "
                                    "The script remains editable as raw text.",
                                    error->message);
      set_status (state, msg);
      g_free (msg);
      g_clear_error (&error);
      gtk_stack_set_visible_child_name (GTK_STACK (state->stack), "texte");
    }
    state->syncing = FALSE;
  }
}

static void
on_rule_editor_changed (GtkWidget *editor, SieveEditorState *state)
{
  (void) editor;
  if (state->syncing)
    return;
  /* Keeps the text buffer up to date while editing visually. */
  if (g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (state->stack)),
                 "visuel") != 0)
    return;

  state->syncing = TRUE;
  sync_visual_to_text (state);
  state->syncing = FALSE;
}

/* ---- Persisting the last chosen account --------------------------- */

/* Remembers the currently chosen account as the "last account": it's the
 * one that will be reselected (and possibly rejoined) next time the
 * dialog opens. Connection settings themselves belong to the account's
 * profile (account editor's "Sieve Filters" page) -- the dialog no
 * longer writes them. Best-effort: a write failure is only logged. */
static void
remember_last_account (SieveEditorState *state)
{
  gint active = gtk_combo_box_get_active (GTK_COMBO_BOX (state->account_combo));
  const gchar *uid = NULL;
  GError *error = NULL;

  if (active > 0) {
    SieveAccountInfo *info = g_list_nth_data (state->accounts, active - 1);
    if (info != NULL)
      uid = info->source_uid;
  }

  if (!sieve_config_set_last_account (uid, &error)) {
    g_warning ("sieve: failed to save the last account: %s",
               error != NULL ? error->message : "unknown error");
    g_clear_error (&error);
  }
}

/* On open: reselects the remembered account, starts enumerating its
 * folders (for the "fileinto" action) and -- if its profile is set for
 * auto-connect (checkbox in the account editor) and we have a host + a
 * username -- starts the connection (password taken from the keyring). */
static void
apply_saved_config (SieveEditorState *state)
{
  g_autofree gchar *last = sieve_config_dup_last_account ();
  SieveConfig *cfg;
  SieveAccountInfo *info = NULL;
  const gchar *eff_user;
  gint idx = 0;

  if (last != NULL && *last != '\0') {
    GList *link;
    gint i = 1;

    for (link = state->accounts; link != NULL; link = link->next, i++) {
      SieveAccountInfo *ai = link->data;
      if (g_strcmp0 (ai->source_uid, last) == 0) {
        idx = i;
        info = ai;
        break;
      }
    }
  }

  if (idx == 0)
    return;   /* no remembered account (or it vanished from the registry) */

  /* Silent repositioning: neutralize on_account_changed while moving the
   * menu, then do ourselves what it would have done. */
  g_signal_handlers_block_by_func (state->account_combo,
                                   G_CALLBACK (on_account_changed), state);
  gtk_combo_box_set_active (GTK_COMBO_BOX (state->account_combo), idx);
  g_signal_handlers_unblock_by_func (state->account_combo,
                                     G_CALLBACK (on_account_changed), state);

  cfg = sieve_config_load_for_account (info->source_uid);

  /* Enumerate the restored account's folders (for the "fileinto" action). */
  start_mailbox_fetch (state, info->source_uid);

  eff_user = (cfg->user != NULL && *cfg->user != '\0')
               ? cfg->user
               : (info->user != NULL ? info->user : NULL);

  if (cfg->host != NULL && *cfg->host != '\0') {
    if (cfg->auto_connect && eff_user != NULL && *eff_user != '\0') {
      set_status (state, "Auto-reconnecting to the last account…");
      start_connect (state);
    } else {
      set_status (state,
                  "Account ready: use \"Reload rules\" to open the session.");
    }
  } else {
    set_status (state, SIEVE_NO_PROFILE_HINT);
  }

  update_reload_sensitive (state);
  sieve_config_free (cfg);
}

/* "Seeded" mode ("Create a Sieve Filter…" context menu): used instead of
 * apply_saved_config. Preselects the message's account
 * (state->seed_account_uid) and, if its ManageSieve profile is
 * configured, starts the connection right away (without waiting for the
 * "auto-connect" checkbox). The pre-filled rule is then injected by
 * connect_task_done; if no connection is possible, it is shown here
 * anyway. */
static void
apply_seed_config (SieveEditorState *state)
{
  SieveAccountInfo *info = NULL;
  gint idx = 0;

  if (state->seed_account_uid != NULL && *state->seed_account_uid != '\0') {
    GList *link;
    gint i = 1;

    for (link = state->accounts; link != NULL; link = link->next, i++) {
      SieveAccountInfo *ai = link->data;
      if (g_strcmp0 (ai->source_uid, state->seed_account_uid) == 0) {
        idx = i;
        info = ai;
        break;
      }
    }
  }

  if (idx > 0) {
    SieveConfig *cfg;

    /* Silent repositioning (see apply_saved_config). */
    g_signal_handlers_block_by_func (state->account_combo,
                                     G_CALLBACK (on_account_changed), state);
    gtk_combo_box_set_active (GTK_COMBO_BOX (state->account_combo), idx);
    g_signal_handlers_unblock_by_func (state->account_combo,
                                       G_CALLBACK (on_account_changed), state);

    cfg = sieve_config_load_for_account (info->source_uid);
    start_mailbox_fetch (state, info->source_uid);
    remember_last_account (state);

    if (cfg->host != NULL && *cfg->host != '\0') {
      const gchar *eff_user = (cfg->user != NULL && *cfg->user != '\0')
                                ? cfg->user
                                : (info->user != NULL ? info->user : NULL);

      if (eff_user != NULL && *eff_user != '\0') {
        set_status (state, "Connecting to the message's account…");
        start_connect (state);
        sieve_config_free (cfg);
        return;   /* inject_seed_rule will be called by connect_task_done */
      }
    }
    update_reload_sensitive (state);
    sieve_config_free (cfg);
  }

  /* No connection started: the message's account was not found in the
   * list, has no ManageSieve profile, or has no username. Show the
   * pre-filled rule anyway; inject_seed_rule sets its own status, we
   * append the appropriate hint to it. */
  inject_seed_rule (state);
  if (state->seed_injected) {
    const gchar *hint =
      (idx == 0)
        ? " Choose the relevant account, then \"Reload rules\"."
        : " Configure this account's ManageSieve server in "
          "Edit → Accounts → \"Sieve Filters\" tab, then "
          "\"Reload rules\".";
    gchar *full = g_strconcat (gtk_label_get_text (state->status_label), hint, NULL);
    set_status (state, full);
    g_free (full);
  }
}

/* ---- Building the dialog ------------------------------------------ */

static void
on_dialog_destroy (GtkWidget *widget, SieveEditorState *state)
{
  (void) widget;

  /* Cancelled before `state` is freed: mailbox_task_done checks the
   * GCancellable (kept alive by the GTask) and backs off without
   * touching `state`. */
  if (state->mbox_cancellable != NULL) {
    g_cancellable_cancel (state->mbox_cancellable);
    g_clear_object (&state->mbox_cancellable);
  }
  g_list_free_full (state->accounts, (GDestroyNotify) sieve_account_info_free);
  state->accounts = NULL;
  g_clear_object (&state->registry);
  g_clear_object (&state->mail_session);

  g_clear_pointer (&state->seed_account_uid, g_free);
  g_clear_pointer (&state->seed_rule, sieve_rule_free);

  if (state->op_in_flight) {
    /* A connect/save task is still running on a thread and holds
     * `state`. Cancel it and mark the dialog destroyed: its completion
     * callback (op_end) will free `state` once the thread has returned. */
    state->dialog_destroyed = TRUE;
    if (state->op_cancellable != NULL)
      g_cancellable_cancel (state->op_cancellable);
    return;
  }

  g_clear_object (&state->client);
  g_clear_pointer (&state->active_script_name, g_free);
  g_free (state);
}

GtkWidget *
sieve_editor_dialog_new (GtkWindow *parent,
                         EShell    *shell)
{
  return sieve_editor_dialog_new_with_seed (parent, shell, NULL, NULL);
}

GtkWidget *
sieve_editor_dialog_new_with_seed (GtkWindow   *parent,
                                   EShell      *shell,
                                   const gchar *prefer_account_uid,
                                   SieveRule   *seed_rule)
{
  SieveEditorState *state = g_new0 (SieveEditorState, 1);
  GtkWidget *dialog, *content, *grid, *scrolled, *switcher;
  GtkWidget *account_label;
  ESourceRegistry *registry = (shell != NULL) ? e_shell_get_registry (shell) : NULL;
  gint row = 0;

  state->registry = (registry != NULL) ? g_object_ref (registry) : NULL;
  state->seed_account_uid = g_strdup (prefer_account_uid);
  state->seed_rule = seed_rule;   /* ownership transferred (may be NULL) */

  /* Mail session (optional): used to enumerate the account's folder tree
   * for the "fileinto" action. Absent -> "fileinto" stays a free text
   * field. */
  if (shell != NULL) {
    EShellBackend *backend = e_shell_get_backend_by_name (shell, "mail");
    if (backend != NULL && E_IS_MAIL_BACKEND (backend)) {
      EMailSession *session =
        e_mail_backend_get_session (E_MAIL_BACKEND (backend));
      state->mail_session = (session != NULL) ? g_object_ref (session) : NULL;
    }
  }

  dialog = g_object_new (GTK_TYPE_DIALOG,
                         "title", "Sieve Filters",
                         "destroy-with-parent", TRUE,
                         NULL);
  if (parent != NULL)
    gtk_window_set_transient_for (GTK_WINDOW (dialog), parent);
  gtk_window_set_default_size (GTK_WINDOW (dialog), 960, 760);
  state->dialog = dialog;

  content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));

  grid = gtk_grid_new ();
  gtk_grid_set_column_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_container_set_border_width (GTK_CONTAINER (grid), 12);

  account_label = gtk_label_new ("Account:");
  state->account_combo = GTK_COMBO_BOX_TEXT (gtk_combo_box_text_new ());
  gtk_widget_set_hexpand (GTK_WIDGET (state->account_combo), TRUE);
  gtk_widget_set_tooltip_text (
    GTK_WIDGET (state->account_combo),
    "Connection settings (server, port, encryption, username, "
    "password, auto-connect) are configured per account in "
    "Edit → Accounts, \"Sieve Filters\" tab.");

  state->conn_indicator = gtk_label_new (NULL);
  gtk_widget_set_halign (state->conn_indicator, GTK_ALIGN_START);
  gtk_widget_set_margin_start (state->conn_indicator, 6);
  gtk_widget_set_tooltip_text (
    state->conn_indicator,
    "ManageSieve session status. There is no Connect button: the "
    "connection opens automatically on open (if the account is set up "
    "for it); otherwise use the visual editor's \"Reload rules\" to "
    "(re)connect.");

  /* Status indicator on the same row as the account, last column. */
  gtk_grid_attach (GTK_GRID (grid), account_label, 0, row, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), GTK_WIDGET (state->account_combo), 1, row, 2, 1);
  gtk_grid_attach (GTK_GRID (grid), state->conn_indicator, 3, row, 1, 1);

  gtk_box_pack_start (GTK_BOX (content), grid, FALSE, FALSE, 0);

  /* Two views of the same script: a criteria/actions visual editor and
   * the raw text editor. Content is copied from one to the other on
   * every switch (see on_stack_switch). */
  state->stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (state->stack),
                                 GTK_STACK_TRANSITION_TYPE_NONE);

  switcher = gtk_stack_switcher_new ();
  gtk_stack_switcher_set_stack (GTK_STACK_SWITCHER (switcher), GTK_STACK (state->stack));
  gtk_widget_set_halign (switcher, GTK_ALIGN_CENTER);
  gtk_box_pack_start (GTK_BOX (content), switcher, FALSE, FALSE, 0);

  state->rule_editor = sieve_rule_editor_new ();
  gtk_stack_add_titled (GTK_STACK (state->stack), state->rule_editor,
                        "visuel", "Visual editor");

  scrolled = gtk_scrolled_window_new (NULL, NULL);
  state->script_view = GTK_TEXT_VIEW (gtk_text_view_new ());
  gtk_text_view_set_monospace (state->script_view, TRUE);
  gtk_container_add (GTK_CONTAINER (scrolled), GTK_WIDGET (state->script_view));
  gtk_stack_add_titled (GTK_STACK (state->stack), scrolled, "texte", "Raw text");

  gtk_widget_set_vexpand (state->stack, TRUE);
  gtk_widget_set_margin_start (state->stack, 12);
  gtk_widget_set_margin_end (state->stack, 12);
  gtk_widget_set_margin_top (state->stack, 6);
  gtk_widget_set_margin_bottom (state->stack, 6);
  gtk_box_pack_start (GTK_BOX (content), state->stack, TRUE, TRUE, 0);

  /* Separator, then a full-width action bar right under the editing
   * area: the visual editor's +/-/reload bar on the left (fetched as-is),
   * "Save" on the right -- and, during a network operation, "Cancel
   * operation" in its place. Nothing overlaps the content; the rule list
   * and the detail panel end at the same level, and both bars line up on
   * the same row. */
  gtk_box_pack_start (GTK_BOX (content),
                      gtk_separator_new (GTK_ORIENTATION_HORIZONTAL),
                      FALSE, FALSE, 0);
  {
    GtkWidget *action_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    /* Same horizontal margins as the editing area (12 px): the
     * +/-/reload bar lands exactly under the rule column and "Save"
     * exactly under the detail panel's right edge. */
    gtk_widget_set_margin_start (action_bar, 12);
    gtk_widget_set_margin_end (action_bar, 12);
    gtk_widget_set_margin_top (action_bar, 6);
    gtk_widget_set_margin_bottom (action_bar, 8);

    state->editor_toolbar =
      sieve_rule_editor_get_toolbar (SIEVE_RULE_EDITOR (state->rule_editor));
    gtk_widget_set_valign (state->editor_toolbar, GTK_ALIGN_CENTER);
    gtk_box_pack_start (GTK_BOX (action_bar), state->editor_toolbar,
                        FALSE, FALSE, 0);

    state->save_button = gtk_button_new_with_label ("Save");
    gtk_widget_set_sensitive (state->save_button, FALSE);
    gtk_box_pack_end (GTK_BOX (action_bar), state->save_button, FALSE, FALSE, 0);

    /* Takes the place of "Save": op_begin hides one and reveals the
     * other. no_show_all: gtk_widget_show_all must not reveal it at
     * startup. */
    state->cancel_button = gtk_button_new_with_label ("Cancel operation");
    gtk_widget_set_sensitive (state->cancel_button, FALSE);
    gtk_widget_set_no_show_all (state->cancel_button, TRUE);
    gtk_box_pack_end (GTK_BOX (action_bar), state->cancel_button,
                      FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (content), action_bar, FALSE, FALSE, 0);
  }

  state->status_label = GTK_LABEL (gtk_label_new ("Not connected."));
  gtk_widget_set_halign (GTK_WIDGET (state->status_label), GTK_ALIGN_START);
  gtk_label_set_xalign (state->status_label, 0.0);
  gtk_label_set_line_wrap (state->status_label, TRUE);
  /* Dimmed text: the status line is secondary compared to the buttons. */
  gtk_style_context_add_class (
    gtk_widget_get_style_context (GTK_WIDGET (state->status_label)), "dim-label");
  gtk_widget_set_margin_start (GTK_WIDGET (state->status_label), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (state->status_label), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (state->status_label), 10);
  gtk_box_pack_start (GTK_BOX (content), GTK_WIDGET (state->status_label), FALSE, FALSE, 0);

  populate_account_combo (state);
  update_connect_state (state); /* "● Not connected" indicator up front */

  g_signal_connect (state->account_combo, "changed",
                    G_CALLBACK (on_account_changed), state);
  g_signal_connect (state->save_button, "clicked", G_CALLBACK (on_save_clicked), state);
  g_signal_connect (state->cancel_button, "clicked", G_CALLBACK (on_cancel_clicked), state);
  g_signal_connect (state->stack, "notify::visible-child",
                    G_CALLBACK (on_stack_switch), state);
  g_signal_connect (state->rule_editor, "changed",
                    G_CALLBACK (on_rule_editor_changed), state);
  g_signal_connect (state->rule_editor, "refresh-requested",
                    G_CALLBACK (on_rule_editor_refresh), state);
  g_signal_connect (dialog, "destroy", G_CALLBACK (on_dialog_destroy), state);
  g_signal_connect_swapped (dialog, "response", G_CALLBACK (gtk_widget_destroy), dialog);

  gtk_widget_show_all (dialog);

  /* After show_all: op_begin/op_end manipulate realized widgets. */
  if (state->seed_account_uid != NULL || state->seed_rule != NULL) {
    /* Opened from the "Create a Sieve Filter…" context menu: join the
     * message's account and graft the pre-filled rule onto it. */
    apply_seed_config (state);
  } else {
    /* Regular open (Edit -> Sieve Filters…): reselects the last chosen
     * account and triggers auto-connect if its profile arms it (password
     * taken from the keyring). */
    apply_saved_config (state);
  }

  return dialog;
}
