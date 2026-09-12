/* module-sieve-filters.c
 *
 * EModule entry point. Adds:
 *   - a "Sieve Filters…" entry in Evolution's main window Edit menu,
 *     which opens the sieve-editor-dialog dialog;
 *   - a "Create a Sieve Filter…" entry in the message context menu's
 *     "Create" submenu (right-click in the message list). It reads the
 *     selected message's sender (fetched on a worker thread, never on
 *     the GTK loop) and opens the dialog, preselecting the message's
 *     account and grafting onto it a pre-filled "From :contains
 *     <sender>" rule (to be completed with an action).
 *
 * Menu merging API -- Evolution 3.54+ (hence 3.56 on Debian "trixie",
 * package evolution 3.56.2):
 *
 *   GtkUIManager / GtkActionGroup / GtkActionEntry are gone in favor of
 *   EUIManager / EUIActionGroup / EUIAction + .eui files. There is no
 *   longer an e_shell_window_get_ui_manager(): the UI manager is now
 *   carried by the EShellView (e_shell_view_get_ui_manager()).
 *
 *   We therefore extend E_TYPE_SHELL_VIEW (rather than
 *   E_TYPE_SHELL_WINDOW), and only hook into the "mail" view: Sieve
 *   filters are the server-side equivalent of "Message Filters…", whose
 *   entry likewise only lives in the mail view.
 *
 * References (evolution-dev 3.56 headers):
 *   src/shell/e-shell-view.h    EUIManager *e_shell_view_get_ui_manager (EShellView *);
 *                               EShellWindow *e_shell_view_get_shell_window (EShellView *);
 *                               const gchar *e_shell_view_get_name (EShellView *);
 *   src/e-util/e-ui-manager.h   e_ui_manager_add_actions_with_eui_data ();
 *   src/e-util/e-ui-action.h    EUIActionEntry { name, icon_name, label, accel,
 *                                 tooltip, EUIActionFunc activate,
 *                                 parameter_type, state, EUIActionFunc change_state };
 *                               typedef void (*EUIActionFunc)(EUIAction *, GVariant *, gpointer);
 *   data/ui/evolution-shell.eui <menu id='main-menu'>
 *                                 <submenu action='edit-menu'>
 *                                   <placeholder id='administrative-actions'/>
 *
 * The .eui fragment is embedded as a C string: a single call to
 * e_ui_manager_add_actions_with_eui_data() registers the action group
 * AND merges the menu fragment. No .eui file to install or to make
 * discoverable by Evolution.
 */

#include <glib-object.h>
#include <gmodule.h>
#include <gtk/gtk.h>

#include <e-util/e-util.h>            /* EExtension, EUIManager, EUIAction, EUIActionEntry */
#include <shell/e-shell.h>           /* EShell, e_shell_get_registry */
#include <shell/e-shell-view.h>      /* EShellView, e_shell_view_get_ui_manager */
#include <shell/e-shell-window.h>    /* EShellWindow, e_shell_window_get_shell */
#include <shell/e-shell-content.h>   /* EShellContent (mail view content = EMailReader) */
#include <mail/e-mail-reader.h>      /* EMailReader: selection + current folder */
#include <camel/camel.h>             /* CamelFolder, CamelMimeMessage, CamelInternetAddress */

#include "sieve-editor-dialog.h"
#include "sieve-config-page.h"
#include "sieve-model.h"             /* SieveRule: rule pre-filled from the message */

/* Required for any Evolution module: declares that the module is
 * compatible with the current ABI -- otherwise Evolution refuses to
 * load it. */
G_MODULE_EXPORT void e_module_load (GTypeModule *type_module);
G_MODULE_EXPORT void e_module_unload (GTypeModule *type_module);

#define SIEVE_TYPE_MENU_EXTENSION (sieve_menu_extension_get_type ())
G_DECLARE_FINAL_TYPE (SieveMenuExtension, sieve_menu_extension,
                      SIEVE, MENU_EXTENSION, EExtension)

struct _SieveMenuExtension {
  EExtension parent_instance;
};

G_DEFINE_DYNAMIC_TYPE (SieveMenuExtension, sieve_menu_extension, E_TYPE_EXTENSION)

/* .eui fragment: a single entry in the 'administrative-actions'
 * placeholder of the Edit menu ('edit-menu'), defined by
 * data/ui/evolution-shell.eui (loaded as a base by every EShellView
 * before extensions are instantiated, so always present here). This is
 * where Evolution already places "Accounts…", "Message Filters…" and
 * "Preferences". */
static const gchar *sieve_menu_eui =
  "<eui>"
  "  <menu id='main-menu'>"
  "    <submenu action='edit-menu'>"
  "      <placeholder id='administrative-actions'>"
  "        <item action='sieve-manage-filters'/>"
  "      </placeholder>"
  "    </submenu>"
  "  </menu>"
  "</eui>";

/* Separate fragment for a message's context menu (message list). It is
 * merged separately (e_ui_parser_merge_data) so a possible error can be
 * logged: 'mail-message-popup' and the "Create" submenu
 * ('mail-create-menu') come from evolution-mail.eui, loaded by the
 * "mail" EShellView. We target the 'mail-conversion-actions'
 * placeholder, where Evolution already places "Create a Meeting…" and
 * "Filter on Sender…". Fallback: if this placeholder is not a valid
 * merge target, we fall back to 'mail-message-popup-common-actions'
 * (top of the context menu). */
static const gchar *sieve_popup_eui =
  "<eui>"
  "  <menu id='mail-message-popup'>"
  "    <submenu action='mail-create-menu'>"
  "      <placeholder id='mail-conversion-actions'>"
  "        <item action='sieve-create-filter-from-message'/>"
  "      </placeholder>"
  "    </submenu>"
  "  </menu>"
  "</eui>";

static const gchar *sieve_popup_eui_fallback =
  "<eui>"
  "  <menu id='mail-message-popup'>"
  "    <placeholder id='mail-message-popup-common-actions'>"
  "      <item action='sieve-create-filter-from-message'/>"
  "    </placeholder>"
  "  </menu>"
  "</eui>";

/* ---- Context menu: "Create a Sieve Filter…" -------------------- */

/* Context carried from the click through to the return of the
 * message-fetch thread. `parent` is tracked with a weak pointer: if the
 * window is closed in the meantime, opening the dialog is abandoned. */
typedef struct {
  GtkWindow        *parent;       /* weak pointer (may go back to NULL) */
  EShell           *shell;        /* ref. */
  CamelFolder      *folder;       /* ref. */
  gchar            *uid;          /* selected message */
  gchar            *account_uid;  /* account's ESource UID (= CamelStore UID) */
  CamelMimeMessage *message;      /* thread result, ref., or NULL */
} SieveSeedCtx;

/* Builds a "From :contains <sender>" rule ("all criteria" mode, no
 * action -- the user chooses what to do with it). NULL if the message has
 * no usable sender. Must run on the main thread (no I/O). */
static SieveRule *
seed_rule_from_message (CamelMimeMessage *message)
{
  CamelInternetAddress *from;      /* borrowed */
  const gchar *name = NULL, *email = NULL;
  SieveRule *rule;
  SieveCondition *cond;
  gchar *rule_name;

  from = camel_mime_message_get_from (message);
  if (from == NULL || !camel_internet_address_get (from, 0, &name, &email) ||
      email == NULL || *email == '\0')
    return NULL;

  rule_name = g_strdup_printf ("Message from %s",
                               (name != NULL && *name != '\0') ? name : email);
  rule = sieve_rule_new (rule_name);
  g_free (rule_name);
  rule->mode = SIEVE_MATCH_MODE_ALL;

  cond = sieve_condition_new ();   /* field = From, match = :contains */
  g_free (cond->value);
  cond->value = g_strdup (email);
  g_ptr_array_add (rule->conditions, cond);

  return rule;
}

static void
sieve_seed_ctx_free (SieveSeedCtx *ctx)
{
  if (ctx->parent != NULL)
    g_object_remove_weak_pointer (G_OBJECT (ctx->parent), (gpointer *) &ctx->parent);
  g_clear_object (&ctx->shell);
  g_clear_object (&ctx->folder);
  g_clear_object (&ctx->message);
  g_free (ctx->uid);
  g_free (ctx->account_uid);
  g_free (ctx);
}

/* Worker thread: fetches the message (on IMAP it may not be cached yet --
 * we never block the GTK loop). */
static void
sieve_seed_fetch_thread (GTask *task, gpointer source_object,
                         gpointer task_data, GCancellable *cancellable)
{
  SieveSeedCtx *ctx = task_data;

  (void) source_object;
  ctx->message = camel_folder_get_message_sync (ctx->folder, ctx->uid,
                                                cancellable, NULL);
  g_task_return_boolean (task, ctx->message != NULL);
}

static void
sieve_seed_fetch_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  SieveSeedCtx *ctx = user_data;
  SieveRule *rule = NULL;

  (void) source;
  (void) res;

  g_debug ("sieve: seed fetch done (message=%s, window=%s)",
           ctx->message != NULL ? "OK" : "NULL",
           ctx->parent != NULL ? "alive" : "closed");

  if (ctx->message != NULL)
    rule = seed_rule_from_message (ctx->message);

  if (ctx->parent != NULL) {
    /* Open the dialog even without a usable rule (unreadable message or
     * no sender): the user can still compose their own. */
    sieve_editor_dialog_new_with_seed (ctx->parent, ctx->shell,
                                       ctx->account_uid, rule);
    rule = NULL;   /* ownership transferred to the dialog */
  }

  g_clear_pointer (&rule, sieve_rule_free);
  sieve_seed_ctx_free (ctx);
}

static void
action_sieve_manage_filters_cb (EUIAction *action,
                                GVariant *parameter,
                                gpointer user_data)
{
  EShellView *shell_view = E_SHELL_VIEW (user_data);
  EShellWindow *shell_window = e_shell_view_get_shell_window (shell_view);
  EShell *shell = e_shell_window_get_shell (shell_window);

  (void) action;
  (void) parameter;

  /* The EShell gives the dialog the source registry (list of mail
   * accounts + the account's OAuth2 token / password via EDS) and the
   * mail session (the account's folder tree, offered in the "fileinto"
   * action). ManageSieve connection settings, on the other hand, are
   * configured per account in the account editor ("Sieve Filters"
   * page). */
  sieve_editor_dialog_new (GTK_WINDOW (shell_window), shell);
}

/* Depending on the Evolution version, the EMailReader interface is
 * carried either by the "mail" view's EShellContent, or by the internal
 * view (EMailPanedView / EMailNotebookView), which is a descendant
 * widget. So we search depth-first for the first widget that implements
 * it -- without depending on the private header
 * e-mail-shell-content.h. */
static EMailReader *
sieve_find_mail_reader (GtkWidget *widget)
{
  GList *children, *link;
  EMailReader *found = NULL;

  if (widget == NULL)
    return NULL;
  if (E_IS_MAIL_READER (widget))
    return E_MAIL_READER (widget);
  if (!GTK_IS_CONTAINER (widget))
    return NULL;

  children = gtk_container_get_children (GTK_CONTAINER (widget));
  for (link = children; link != NULL && found == NULL; link = link->next)
    found = sieve_find_mail_reader (GTK_WIDGET (link->data));
  g_list_free (children);

  return found;
}

static void
action_sieve_create_filter_from_message_cb (EUIAction *action,
                                            GVariant *parameter,
                                            gpointer user_data)
{
  EShellView *shell_view = E_SHELL_VIEW (user_data);
  EShellWindow *shell_window = e_shell_view_get_shell_window (shell_view);
  EShell *shell = e_shell_window_get_shell (shell_window);
  EShellContent *shell_content = e_shell_view_get_shell_content (shell_view);
  EMailReader *reader;
  GPtrArray *uids = NULL;
  CamelFolder *folder = NULL;
  CamelStore *store;
  SieveSeedCtx *ctx;
  GTask *task;

  (void) action;
  (void) parameter;

  reader = sieve_find_mail_reader (GTK_WIDGET (shell_content));
  if (reader == NULL) {
    g_warning ("sieve: no EMailReader found under the \"mail\" view's content "
               "(%s) -- cannot create a filter from this message",
               shell_content != NULL ? G_OBJECT_TYPE_NAME (shell_content) : "NULL");
    return;
  }

  uids = e_mail_reader_get_selected_uids (reader);
  folder = e_mail_reader_ref_folder (reader);
  if (uids == NULL || uids->len == 0 || folder == NULL) {
    /* Context menu with no message under the cursor: nothing to do. */
    g_clear_pointer (&uids, g_ptr_array_unref);
    g_clear_object (&folder);
    return;
  }

  g_debug ("sieve: \"Create a Sieve Filter\" -- message %s, reader %s",
           (const gchar *) g_ptr_array_index (uids, 0),
           G_OBJECT_TYPE_NAME (reader));

  ctx = g_new0 (SieveSeedCtx, 1);
  ctx->parent = GTK_WINDOW (shell_window);
  g_object_add_weak_pointer (G_OBJECT (ctx->parent), (gpointer *) &ctx->parent);
  ctx->shell = g_object_ref (shell);
  ctx->folder = g_object_ref (folder);
  ctx->uid = g_strdup (g_ptr_array_index (uids, 0));

  /* The folder's CamelStore UID is that of the mail account's ESource:
   * sieve-editor-dialog uses it to preselect the right account in its
   * drop-down menu. */
  store = camel_folder_get_parent_store (folder);
  if (store != NULL)
    ctx->account_uid = g_strdup (camel_service_get_uid (CAMEL_SERVICE (store)));

  g_ptr_array_unref (uids);
  g_object_unref (folder);

  task = g_task_new (NULL, NULL, sieve_seed_fetch_done, ctx);
  g_task_set_task_data (task, ctx, NULL);   /* freed by sieve_seed_fetch_done */
  g_task_run_in_thread (task, sieve_seed_fetch_thread);
  g_object_unref (task);
}

/* No gettext infrastructure in this project (see meson.build): labels
 * are hardcoded in English, like the rest of the plugin's user-facing
 * messages (see AGENTS.md). */
static const EUIActionEntry sieve_menu_action_entries[] = {
  { "sieve-manage-filters",
    NULL,
    "_Sieve Filters…",
    NULL,
    "Manage server-side Sieve mail filters",
    action_sieve_manage_filters_cb,
    NULL, NULL, NULL },
  { "sieve-create-filter-from-message",
    NULL,
    "Create a _Sieve Filter…",
    NULL,
    "Create a server-side Sieve rule from the selected message's sender",
    action_sieve_create_filter_from_message_cb,
    NULL, NULL, NULL }
};

static void
sieve_menu_extension_constructed (GObject *object)
{
  EExtension *extension = E_EXTENSION (object);
  EShellView *shell_view;
  EUIManager *ui_manager;

  G_OBJECT_CLASS (sieve_menu_extension_parent_class)->constructed (object);

  shell_view = E_SHELL_VIEW (e_extension_get_extensible (extension));

  /* The extension is instantiated for every view (mail, calendar,
   * contacts, tasks, memos). Only mail concerns us. */
  if (g_strcmp0 (e_shell_view_get_name (shell_view), "mail") != 0)
    return;

  ui_manager = e_shell_view_get_ui_manager (shell_view);

  /* Registers the "sieve-filters" action group (BOTH actions) and merges
   * the Edit menu entry. user_data (shell_view) is passed to the
   * callbacks. */
  e_ui_manager_add_actions_with_eui_data (ui_manager,
                                          "sieve-filters",
                                          NULL,
                                          sieve_menu_action_entries,
                                          G_N_ELEMENTS (sieve_menu_action_entries),
                                          shell_view,
                                          sieve_menu_eui);

  /* A message's context menu: merged separately, with error reporting. */
  {
    EUIParser *parser = e_ui_manager_get_parser (ui_manager);
    GError *error = NULL;
    gboolean ok;

    ok = e_ui_parser_merge_data (parser, sieve_popup_eui, -1, &error);
    if (!ok) {
      g_warning ("sieve: merging the entry into the context menu's \"Create\" "
                 "submenu was refused (%s) -- falling back to top of menu",
                 error != NULL ? error->message : "unknown error");
      g_clear_error (&error);
      ok = e_ui_parser_merge_data (parser, sieve_popup_eui_fallback, -1, &error);
      if (!ok) {
        g_warning ("sieve: could not add the entry to a message's context "
                   "menu (%s)",
                   error != NULL ? error->message : "unknown error");
        g_clear_error (&error);
      }
    }

    e_ui_manager_changed (ui_manager);
  }
}

static void
sieve_menu_extension_class_init (SieveMenuExtensionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  EExtensionClass *extension_class = E_EXTENSION_CLASS (klass);

  object_class->constructed = sieve_menu_extension_constructed;
  extension_class->extensible_type = E_TYPE_SHELL_VIEW;
}

static void
sieve_menu_extension_class_finalize (SieveMenuExtensionClass *klass)
{
  (void) klass;
}

static void
sieve_menu_extension_init (SieveMenuExtension *self)
{
  (void) self;
}

/* ---- Module entry points ---------------------------------------------- */

void
e_module_load (GTypeModule *type_module)
{
  sieve_menu_extension_register_type (type_module);
  /* "Sieve Filters" page of the account editor (Edit -> Accounts): the
   * account's ManageSieve connection settings. Rule editing itself is
   * still carried by the menu entry above. */
  sieve_config_page_types_register (type_module);
}

void
e_module_unload (GTypeModule *type_module)
{
  (void) type_module;
}
