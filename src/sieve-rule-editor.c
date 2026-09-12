/* sieve-rule-editor.c — see sieve-rule-editor.h for the overview.
 *
 * Implementation choice: the internal SieveRuleSet is the single source
 * of truth. "Structural" changes (adding/removing a rule, a condition,
 * an action, changing a field or an action type) fully rebuild the
 * detail panel from the model; "value" changes (typing text, choosing a
 * match type) write in place without rebuilding. The `updating` flag
 * neutralizes callbacks during a rebuild.
 */

#include "sieve-rule-editor.h"

struct _SieveRuleEditor {
  GtkBox parent_instance;

  SieveRuleSet *model;
  gint          selected;   /* index into model->rules, or -1 */
  gboolean      updating;
  GStrv         mailboxes;  /* account folder paths, or NULL */

  GtkWidget    *rule_list;   /* GtkListBox */
  GtkWidget    *detail;      /* GtkBox rebuilt on demand */
  GtkWidget    *toolbar;     /* +/-/reload bar, placed by the container
                             * (ref kept: survives any reparenting) */
  GtkWidget    *remove_rule_button;
  GtkWidget    *refresh_rule_button;
};

G_DEFINE_TYPE (SieveRuleEditor, sieve_rule_editor, GTK_TYPE_BOX)

enum { SIG_CHANGED, SIG_REFRESH, N_SIGNALS };
static guint signals[N_SIGNALS];

static void rebuild_detail    (SieveRuleEditor *self);
static void rebuild_rule_list (SieveRuleEditor *self);

/* ---- Small utilities ------------------------------------------------- */

static SieveRule *
current_rule (SieveRuleEditor *self)
{
  if (self->selected < 0 || self->selected >= (gint) self->model->rules->len)
    return NULL;
  return g_ptr_array_index (self->model->rules, self->selected);
}

static void
emit_changed (SieveRuleEditor *self)
{
  if (!self->updating)
    g_signal_emit (self, signals[SIG_CHANGED], 0);
}

static void
container_clear (GtkContainer *container)
{
  GList *children = gtk_container_get_children (container);
  for (GList *l = children; l != NULL; l = l->next)
    gtk_widget_destroy (GTK_WIDGET (l->data));
  g_list_free (children);
}

static GtkWidget *
make_combo (const gchar * const *labels, gint active)
{
  GtkWidget *combo = gtk_combo_box_text_new ();
  for (gint i = 0; labels[i] != NULL; i++)
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), labels[i]);
  if (active >= 0)
    gtk_combo_box_set_active (GTK_COMBO_BOX (combo), active);
  return combo;
}

/* Indices aligned with SieveField / SieveActionType (see sieve-model.h). */
static const gchar * const field_labels[] = {
  "Sender (From)", "Recipient (To)", "Copy (Cc)", "Subject",
  "Header…", "Size", "Message body", NULL
};
static const gchar * const text_match_labels[] = {
  "contains", "is exactly", "matches pattern", NULL
};
static const gchar * const size_match_labels[] = {
  "is over", "is under", NULL
};
static const gchar * const action_labels[] = {
  "Keep", "Discard", "File into",
  "Redirect to", "Add IMAP flag",
  "Stop processing", NULL
};

/* IMAP flags offered for "addflag": the RFC 3501 system flags followed
 * by a few common keywords. The list is indicative -- the field remains
 * editable so an arbitrary keyword can be entered. */
static const gchar * const imap_flag_labels[] = {
  "\\Seen", "\\Answered", "\\Flagged", "\\Deleted", "\\Draft",
  "$Junk", "$NotJunk", "$Forwarded", "$MDNSent", "$Phishing", NULL
};

/* Editable GtkComboBoxText pre-filled with `items` (NULL-terminated
 * array): a list of suggestions without forbidding free entry. The
 * internal entry holds the value; that's what gets connected / read
 * back. `current` pre-fills the entry (may be NULL). Used for both IMAP
 * flags ("addflag") and account folders ("fileinto"). */
static GtkWidget *
make_entry_combo (const gchar * const *items, const gchar *current)
{
  GtkWidget *combo = gtk_combo_box_text_new_with_entry ();
  for (gint i = 0; items != NULL && items[i] != NULL; i++)
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), items[i]);
  if (current != NULL)
    gtk_entry_set_text (GTK_ENTRY (gtk_bin_get_child (GTK_BIN (combo))), current);
  return combo;
}

typedef struct {
  SieveRuleEditor *self;
  gpointer         item;   /* SieveCondition* or SieveAction* */
} RowCtx;

static RowCtx *
row_ctx_new (SieveRuleEditor *self, gpointer item)
{
  RowCtx *ctx = g_new0 (RowCtx, 1);
  ctx->self = self;
  ctx->item = item;
  return ctx;
}

static gboolean
action_takes_arg (SieveActionType type)
{
  return type == SIEVE_ACTION_FILEINTO ||
         type == SIEVE_ACTION_REDIRECT ||
         type == SIEVE_ACTION_ADDFLAG;
}

/* ---- "Condition" row ------------------------------------------------- */

static void
on_cond_field_changed (GtkComboBox *combo, RowCtx *ctx)
{
  SieveRuleEditor *self = ctx->self;
  SieveCondition *c = ctx->item;
  gint idx;

  if (self->updating)
    return;
  idx = gtk_combo_box_get_active (combo);
  if (idx < 0)
    return;

  c->field = (SieveField) idx;
  if (c->field == SIEVE_FIELD_SIZE) {
    if (c->match != SIEVE_MATCH_OVER && c->match != SIEVE_MATCH_UNDER)
      c->match = SIEVE_MATCH_OVER;
  } else if (c->match == SIEVE_MATCH_OVER || c->match == SIEVE_MATCH_UNDER) {
    c->match = SIEVE_MATCH_CONTAINS;
  }

  rebuild_detail (self);
  emit_changed (self);
}

static void
on_cond_match_changed (GtkComboBox *combo, RowCtx *ctx)
{
  SieveCondition *c = ctx->item;
  gint idx;

  if (ctx->self->updating)
    return;
  idx = gtk_combo_box_get_active (combo);
  if (idx < 0)
    return;

  if (c->field == SIEVE_FIELD_SIZE)
    c->match = (idx == 1) ? SIEVE_MATCH_UNDER : SIEVE_MATCH_OVER;
  else
    c->match = (SieveMatch) idx; /* 0..2 == CONTAINS / IS / MATCHES */

  emit_changed (ctx->self);
}

static void
on_cond_header_changed (GtkEntry *entry, RowCtx *ctx)
{
  SieveCondition *c = ctx->item;

  if (ctx->self->updating)
    return;
  g_free (c->header_name);
  c->header_name = g_strdup (gtk_entry_get_text (entry));
  emit_changed (ctx->self);
}

static void
on_cond_value_changed (GtkEntry *entry, RowCtx *ctx)
{
  SieveCondition *c = ctx->item;

  if (ctx->self->updating)
    return;
  g_free (c->value);
  c->value = g_strdup (gtk_entry_get_text (entry));
  emit_changed (ctx->self);
}

static void
on_cond_remove (GtkButton *button, RowCtx *ctx)
{
  SieveRuleEditor *self = ctx->self;
  SieveRule *rule = current_rule (self);

  (void) button;
  if (self->updating || rule == NULL)
    return;
  g_ptr_array_remove (rule->conditions, ctx->item);
  rebuild_detail (self);
  emit_changed (self);
}

static GtkWidget *
build_condition_row (SieveRuleEditor *self, SieveCondition *c)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  RowCtx *ctx = row_ctx_new (self, c);
  gboolean is_size = (c->field == SIEVE_FIELD_SIZE);
  GtkWidget *field_combo, *match_combo, *header_entry, *value_entry, *remove_btn;

  g_object_set_data_full (G_OBJECT (row), "ctx", ctx, g_free);

  field_combo = make_combo (field_labels, (gint) c->field);

  match_combo = make_combo (is_size ? size_match_labels : text_match_labels,
                            is_size ? (c->match == SIEVE_MATCH_UNDER ? 1 : 0)
                                    : (gint) c->match);

  header_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (header_entry), "X-Header");
  if (c->header_name != NULL)
    gtk_entry_set_text (GTK_ENTRY (header_entry), c->header_name);
  gtk_widget_set_no_show_all (header_entry, TRUE);
  gtk_widget_set_visible (header_entry, c->field == SIEVE_FIELD_HEADER);

  value_entry = gtk_entry_new ();
  gtk_widget_set_hexpand (value_entry, TRUE);
  gtk_entry_set_placeholder_text (GTK_ENTRY (value_entry),
                                  is_size ? "1M" : "value to match");
  if (c->value != NULL)
    gtk_entry_set_text (GTK_ENTRY (value_entry), c->value);

  remove_btn = gtk_button_new_from_icon_name ("list-remove-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text (remove_btn, "Remove this condition");

  gtk_box_pack_start (GTK_BOX (row), field_combo, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (row), header_entry, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (row), match_combo, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (row), value_entry, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (row), remove_btn, FALSE, FALSE, 0);

  g_signal_connect (field_combo, "changed", G_CALLBACK (on_cond_field_changed), ctx);
  g_signal_connect (match_combo, "changed", G_CALLBACK (on_cond_match_changed), ctx);
  g_signal_connect (header_entry, "changed", G_CALLBACK (on_cond_header_changed), ctx);
  g_signal_connect (value_entry, "changed", G_CALLBACK (on_cond_value_changed), ctx);
  g_signal_connect (remove_btn, "clicked", G_CALLBACK (on_cond_remove), ctx);

  return row;
}

/* ---- "Action" row ------------------------------------------------- */

static void
on_action_type_changed (GtkComboBox *combo, RowCtx *ctx)
{
  SieveRuleEditor *self = ctx->self;
  SieveAction *a = ctx->item;
  gint idx;

  if (self->updating)
    return;
  idx = gtk_combo_box_get_active (combo);
  if (idx < 0)
    return;
  a->type = (SieveActionType) idx;
  /* rebuild_detail() destroys this row, freeing `ctx` (attached via
   * g_object_set_data_full). We cache `self` beforehand: touching it
   * through `ctx` after the rebuild would be a use-after-free. */
  rebuild_detail (self);
  emit_changed (self);
}

static void
on_action_arg_changed (GtkEntry *entry, RowCtx *ctx)
{
  SieveAction *a = ctx->item;

  if (ctx->self->updating)
    return;
  g_free (a->arg);
  a->arg = g_strdup (gtk_entry_get_text (entry));
  emit_changed (ctx->self);
}

static void
on_action_remove (GtkButton *button, RowCtx *ctx)
{
  SieveRuleEditor *self = ctx->self;
  SieveRule *rule = current_rule (self);

  (void) button;
  if (self->updating || rule == NULL)
    return;
  g_ptr_array_remove (rule->actions, ctx->item);
  rebuild_detail (self);
  emit_changed (self);
}

static GtkWidget *
build_action_row (SieveRuleEditor *self, SieveAction *a)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  RowCtx *ctx = row_ctx_new (self, a);
  GtkWidget *type_combo, *arg_widget, *arg_entry, *remove_btn;
  gboolean is_flag = (a->type == SIEVE_ACTION_ADDFLAG);
  gboolean is_fileinto_list = (a->type == SIEVE_ACTION_FILEINTO &&
                               self->mailboxes != NULL &&
                               self->mailboxes[0] != NULL);
  const gchar *placeholder = "";

  g_object_set_data_full (G_OBJECT (row), "ctx", ctx, g_free);

  type_combo = make_combo (action_labels, (gint) a->type);

  switch (a->type) {
    case SIEVE_ACTION_FILEINTO: placeholder = "INBOX/Folder"; break;
    case SIEVE_ACTION_REDIRECT: placeholder = "address@example.tld"; break;
    case SIEVE_ACTION_ADDFLAG:  placeholder = "\\Seen"; break;
    default: break;
  }

  /* addflag: editable drop-down list of common IMAP flags. fileinto (if
   * the account's folders are known): editable drop-down list of the
   * folder tree. Otherwise: plain text field. In every case
   * `arg_entry` designates the entry to connect / read back (the
   * combo's internal entry where applicable). */
  if (is_flag) {
    arg_widget = make_entry_combo (imap_flag_labels, a->arg);
    arg_entry = gtk_bin_get_child (GTK_BIN (arg_widget));
  } else if (is_fileinto_list) {
    arg_widget = make_entry_combo ((const gchar * const *) self->mailboxes, a->arg);
    arg_entry = gtk_bin_get_child (GTK_BIN (arg_widget));
  } else {
    arg_widget = arg_entry = gtk_entry_new ();
    if (a->arg != NULL)
      gtk_entry_set_text (GTK_ENTRY (arg_entry), a->arg);
  }
  gtk_widget_set_hexpand (arg_widget, TRUE);
  gtk_entry_set_placeholder_text (GTK_ENTRY (arg_entry), placeholder);
  gtk_widget_set_no_show_all (arg_widget, TRUE);
  gtk_widget_set_visible (arg_widget, action_takes_arg (a->type));

  remove_btn = gtk_button_new_from_icon_name ("list-remove-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text (remove_btn, "Remove this action");

  gtk_box_pack_start (GTK_BOX (row), type_combo, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (row), arg_widget, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (row), remove_btn, FALSE, FALSE, 0);

  g_signal_connect (type_combo, "changed", G_CALLBACK (on_action_type_changed), ctx);
  g_signal_connect (arg_entry, "changed", G_CALLBACK (on_action_arg_changed), ctx);
  g_signal_connect (remove_btn, "clicked", G_CALLBACK (on_action_remove), ctx);

  return row;
}

/* ---- Name / mode / additions ------------------------------------------------- */

static void
on_name_changed (GtkEntry *entry, SieveRuleEditor *self)
{
  SieveRule *rule = current_rule (self);
  GtkListBoxRow *row;

  if (self->updating || rule == NULL)
    return;
  g_free (rule->name);
  rule->name = g_strdup (gtk_entry_get_text (entry));

  row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (self->rule_list), self->selected);
  if (row != NULL) {
    GtkWidget *label = gtk_bin_get_child (GTK_BIN (row));
    /* Opaque rules have no "Name" field: this callback doesn't concern
     * them, but their row is a GtkBox rather than a GtkLabel. */
    if (GTK_IS_LABEL (label))
      gtk_label_set_text (GTK_LABEL (label),
                          (*rule->name != '\0') ? rule->name : "(unnamed)");
  }
  emit_changed (self);
}

static void
on_mode_changed (GtkComboBox *combo, SieveRuleEditor *self)
{
  SieveRule *rule = current_rule (self);

  if (self->updating || rule == NULL)
    return;
  rule->mode = (gtk_combo_box_get_active (combo) == 1)
                 ? SIEVE_MATCH_MODE_ANY : SIEVE_MATCH_MODE_ALL;
  emit_changed (self);
}

static void
on_add_condition (GtkButton *button, SieveRuleEditor *self)
{
  SieveRule *rule = current_rule (self);

  (void) button;
  if (rule == NULL)
    return;
  g_ptr_array_add (rule->conditions, sieve_condition_new ());
  rebuild_detail (self);
  emit_changed (self);
}

static void
on_add_action (GtkButton *button, SieveRuleEditor *self)
{
  SieveRule *rule = current_rule (self);

  (void) button;
  if (rule == NULL)
    return;
  g_ptr_array_add (rule->actions, sieve_action_new (SIEVE_ACTION_KEEP));
  rebuild_detail (self);
  emit_changed (self);
}

/* ---- Rule list -------------------------------------------------- */

static void
on_row_selected (GtkListBox *box, GtkListBoxRow *row, SieveRuleEditor *self)
{
  (void) box;
  if (self->updating)
    return;
  self->selected = (row != NULL) ? gtk_list_box_row_get_index (row) : -1;
  rebuild_detail (self);
}

static void
on_add_rule (GtkButton *button, SieveRuleEditor *self)
{
  (void) button;
  g_ptr_array_add (self->model->rules, sieve_rule_new ("New rule"));
  self->selected = (gint) self->model->rules->len - 1;
  rebuild_rule_list (self);
  rebuild_detail (self);
  emit_changed (self);
}

static void
on_refresh_rules (GtkButton *button, SieveRuleEditor *self)
{
  (void) button;
  /* The editor doesn't know where the rules come from: it just signals
   * the request. The dialog reloads from the server. */
  g_signal_emit (self, signals[SIG_REFRESH], 0);
}

static void
on_remove_rule (GtkButton *button, SieveRuleEditor *self)
{
  (void) button;
  if (self->selected < 0 || self->selected >= (gint) self->model->rules->len)
    return;
  /* Safety net: the button is already desensitized for an opaque rule. */
  if (((SieveRule *) g_ptr_array_index (self->model->rules, self->selected))->opaque)
    return;
  g_ptr_array_remove_index (self->model->rules, self->selected);
  if (self->selected >= (gint) self->model->rules->len)
    self->selected = (gint) self->model->rules->len - 1;
  rebuild_rule_list (self);
  rebuild_detail (self);
  emit_changed (self);
}

/* ---- Rebuilding the UI from the model -------------------------- */

static void
rebuild_detail (SieveRuleEditor *self)
{
  static const gchar * const mode_labels[] = {
    "all of the criteria", "at least one criterion", NULL
  };
  SieveRule *rule = current_rule (self);
  GtkWidget *name_row, *name_entry, *mode_row, *mode_combo;
  GtkWidget *cond_frame, *cond_box, *add_cond_btn;
  GtkWidget *act_frame, *act_box, *add_act_btn;

  self->updating = TRUE;
  container_clear (GTK_CONTAINER (self->detail));
  /* An opaque rule is entirely read-only: no editing or removal from
   * the visual editor (only the raw text tab can). */
  gtk_widget_set_sensitive (self->remove_rule_button,
                            rule != NULL && !rule->opaque);

  if (rule == NULL) {
    GtkWidget *hint = gtk_label_new ("Select a rule on the left, "
                                     "or click \"+\" to create one.");
    gtk_widget_set_halign (hint, GTK_ALIGN_START);
    gtk_box_pack_start (GTK_BOX (self->detail), hint, FALSE, FALSE, 0);
    gtk_widget_show_all (self->detail);
    self->updating = FALSE;
    return;
  }

  if (rule->opaque) {
    GtkWidget *info, *scroll, *view;
    GtkTextBuffer *buf;

    info = gtk_label_new (
      "This rule comes from another tool (Nextcloud Mail, Roundcube, "
      "hand-written script…) or uses constructs the visual editor "
      "cannot represent.\n"
      "It is shown here read-only and copied verbatim. "
      "To edit or remove it, use the \"Raw text\" tab.");
    gtk_label_set_line_wrap (GTK_LABEL (info), TRUE);
    gtk_label_set_xalign (GTK_LABEL (info), 0.0);
    gtk_box_pack_start (GTK_BOX (self->detail), info, FALSE, FALSE, 0);

    scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_widget_set_vexpand (scroll, TRUE);
    view = gtk_text_view_new ();
    gtk_text_view_set_editable (GTK_TEXT_VIEW (view), FALSE);
    gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (view), FALSE);
    gtk_text_view_set_monospace (GTK_TEXT_VIEW (view), TRUE);
    buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
    gtk_text_buffer_set_text (buf, rule->raw != NULL ? rule->raw : "", -1);
    gtk_container_add (GTK_CONTAINER (scroll), view);
    gtk_box_pack_start (GTK_BOX (self->detail), scroll, TRUE, TRUE, 0);

    gtk_widget_show_all (self->detail);
    self->updating = FALSE;
    return;
  }

  name_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_pack_start (GTK_BOX (name_row), gtk_label_new ("Name:"), FALSE, FALSE, 0);
  name_entry = gtk_entry_new ();
  gtk_widget_set_hexpand (name_entry, TRUE);
  gtk_entry_set_text (GTK_ENTRY (name_entry), rule->name != NULL ? rule->name : "");
  gtk_box_pack_start (GTK_BOX (name_row), name_entry, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (self->detail), name_row, FALSE, FALSE, 0);
  g_signal_connect (name_entry, "changed", G_CALLBACK (on_name_changed), self);

  mode_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_pack_start (GTK_BOX (mode_row), gtk_label_new ("Match"), FALSE, FALSE, 0);
  mode_combo = make_combo (mode_labels, rule->mode == SIEVE_MATCH_MODE_ANY ? 1 : 0);
  gtk_box_pack_start (GTK_BOX (mode_row), mode_combo, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (mode_row), gtk_label_new ("of the following:"), FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (self->detail), mode_row, FALSE, FALSE, 0);
  g_signal_connect (mode_combo, "changed", G_CALLBACK (on_mode_changed), self);

  cond_frame = gtk_frame_new ("Criteria");
  cond_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_container_set_border_width (GTK_CONTAINER (cond_box), 6);
  for (guint i = 0; i < rule->conditions->len; i++)
    gtk_box_pack_start (GTK_BOX (cond_box),
                        build_condition_row (self, g_ptr_array_index (rule->conditions, i)),
                        FALSE, FALSE, 0);
  add_cond_btn = gtk_button_new_with_label ("Add a condition");
  gtk_widget_set_halign (add_cond_btn, GTK_ALIGN_START);
  gtk_box_pack_start (GTK_BOX (cond_box), add_cond_btn, FALSE, FALSE, 0);
  gtk_container_add (GTK_CONTAINER (cond_frame), cond_box);
  gtk_box_pack_start (GTK_BOX (self->detail), cond_frame, FALSE, FALSE, 0);
  g_signal_connect (add_cond_btn, "clicked", G_CALLBACK (on_add_condition), self);

  act_frame = gtk_frame_new ("Actions");
  act_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_container_set_border_width (GTK_CONTAINER (act_box), 6);
  for (guint i = 0; i < rule->actions->len; i++)
    gtk_box_pack_start (GTK_BOX (act_box),
                        build_action_row (self, g_ptr_array_index (rule->actions, i)),
                        FALSE, FALSE, 0);
  add_act_btn = gtk_button_new_with_label ("Add an action");
  gtk_widget_set_halign (add_act_btn, GTK_ALIGN_START);
  gtk_box_pack_start (GTK_BOX (act_box), add_act_btn, FALSE, FALSE, 0);
  gtk_container_add (GTK_CONTAINER (act_frame), act_box);
  gtk_box_pack_start (GTK_BOX (self->detail), act_frame, FALSE, FALSE, 0);
  g_signal_connect (add_act_btn, "clicked", G_CALLBACK (on_add_action), self);

  gtk_widget_show_all (self->detail);
  self->updating = FALSE;
}

static void
rebuild_rule_list (SieveRuleEditor *self)
{
  self->updating = TRUE;
  container_clear (GTK_CONTAINER (self->rule_list));

  for (guint i = 0; i < self->model->rules->len; i++) {
    SieveRule *r = g_ptr_array_index (self->model->rules, i);
    GtkWidget *label = gtk_label_new ((r->name != NULL && *r->name != '\0')
                                        ? r->name : "(unnamed)");
    GtkWidget *item;

    gtk_widget_set_halign (label, GTK_ALIGN_START);

    if (r->opaque) {
      /* Rule imported from another tool: lock icon + label, not editable
       * in the visual editor (see rebuild_detail). */
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
      GtkWidget *lock =
        gtk_image_new_from_icon_name ("changes-prevent-symbolic", GTK_ICON_SIZE_MENU);

      gtk_box_pack_start (GTK_BOX (box), lock, FALSE, FALSE, 0);
      gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
      gtk_widget_set_tooltip_text (
        box, "Rule imported from another tool — editable only "
             "in the \"Raw text\" tab");
      item = box;
    } else {
      item = label;
    }

    gtk_widget_set_margin_top (item, 3);
    gtk_widget_set_margin_bottom (item, 3);
    gtk_widget_set_margin_start (item, 6);
    gtk_widget_set_margin_end (item, 6);
    gtk_widget_show_all (item);
    gtk_list_box_insert (GTK_LIST_BOX (self->rule_list), item, -1);
  }

  if (self->selected >= 0 && self->selected < (gint) self->model->rules->len) {
    GtkListBoxRow *row =
      gtk_list_box_get_row_at_index (GTK_LIST_BOX (self->rule_list), self->selected);
    if (row != NULL)
      gtk_list_box_select_row (GTK_LIST_BOX (self->rule_list), row);
  }

  self->updating = FALSE;
}

/* ---- Public API --------------------------------------------------------
 *
 * Deep copy via a script round-trip: this way the internal model only
 * ever contains representable constructs, and we avoid a field-by-field
 * clone to maintain in parallel with the (de)serializer.
 */

static SieveRuleSet *
deep_copy (const SieveRuleSet *set)
{
  gchar *script;
  SieveRuleSet *copy;

  if (set == NULL)
    return sieve_rule_set_new ();

  script = sieve_rule_set_to_script (set);
  copy = sieve_rule_set_parse (script, NULL);
  g_free (script);
  return (copy != NULL) ? copy : sieve_rule_set_new ();
}

void
sieve_rule_editor_set_rule_set (SieveRuleEditor *self, const SieveRuleSet *set)
{
  g_return_if_fail (SIEVE_IS_RULE_EDITOR (self));

  sieve_rule_set_free (self->model);
  self->model = deep_copy (set);
  self->selected = (self->model->rules->len > 0) ? 0 : -1;
  rebuild_rule_list (self);
  rebuild_detail (self);
}

SieveRuleSet *
sieve_rule_editor_dup_rule_set (SieveRuleEditor *self)
{
  g_return_val_if_fail (SIEVE_IS_RULE_EDITOR (self), NULL);
  return deep_copy (self->model);
}

void
sieve_rule_editor_select_rule (SieveRuleEditor *self, gint index)
{
  GtkListBoxRow *row;

  g_return_if_fail (SIEVE_IS_RULE_EDITOR (self));

  if (index < 0 || index >= (gint) self->model->rules->len)
    self->selected = -1;
  else
    self->selected = index;

  rebuild_rule_list (self);   /* visually re-selects the row */
  rebuild_detail (self);

  if (self->selected < 0)
    return;

  row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (self->rule_list),
                                       self->selected);
  if (row != NULL) {
    /* grab_focus on the row: the GtkListBox scrolls it into view and
     * makes it active for keyboard navigation (up/down arrows). */
    gtk_widget_grab_focus (GTK_WIDGET (row));
  }
}

void
sieve_rule_editor_set_mailboxes (SieveRuleEditor     *self,
                                 const gchar * const *mailbox_paths)
{
  g_return_if_fail (SIEVE_IS_RULE_EDITOR (self));

  g_clear_pointer (&self->mailboxes, g_strfreev);
  if (mailbox_paths != NULL && mailbox_paths[0] != NULL)
    self->mailboxes = g_strdupv ((gchar **) mailbox_paths);

  /* The detail panel materializes the "fileinto" choice: rebuild it so
   * its rows switch between a drop-down list and a free text field. */
  rebuild_detail (self);
}

void
sieve_rule_editor_set_refresh_sensitive (SieveRuleEditor *self, gboolean sensitive)
{
  g_return_if_fail (SIEVE_IS_RULE_EDITOR (self));
  gtk_widget_set_sensitive (self->refresh_rule_button, sensitive);
}

GtkWidget *
sieve_rule_editor_get_toolbar (SieveRuleEditor *self)
{
  g_return_val_if_fail (SIEVE_IS_RULE_EDITOR (self), NULL);
  return self->toolbar;
}

/* ---- GObject boilerplate --------------------------------------------- */

static void
sieve_rule_editor_finalize (GObject *object)
{
  SieveRuleEditor *self = SIEVE_RULE_EDITOR (object);

  g_clear_pointer (&self->model, sieve_rule_set_free);
  g_clear_pointer (&self->mailboxes, g_strfreev);
  g_clear_object (&self->toolbar);

  G_OBJECT_CLASS (sieve_rule_editor_parent_class)->finalize (object);
}

static void
sieve_rule_editor_class_init (SieveRuleEditorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = sieve_rule_editor_finalize;

  signals[SIG_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /* Emitted when the user clicks "Reload": the container must re-read
   * the rules from the server. */
  signals[SIG_REFRESH] =
    g_signal_new ("refresh-requested", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
sieve_rule_editor_init (SieveRuleEditor *self)
{
  GtkWidget *list_scroll, *buttons, *add_btn, *detail_scroll;

  self->model = sieve_rule_set_new ();
  self->selected = -1;

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_HORIZONTAL);
  gtk_box_set_spacing (GTK_BOX (self), 6);

  /* Left column: nothing but the list, full height -- this keeps it
   * aligned with the detail panel on the right (no "step" at the
   * bottom). The +/-/reload bar is built here but NOT placed: the
   * container retrieves it via sieve_rule_editor_get_toolbar() and
   * inserts it into its own action bar, alongside "Save". */
  list_scroll = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (list_scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (list_scroll),
                                       GTK_SHADOW_IN);
  gtk_widget_set_size_request (list_scroll, 200, -1);
  gtk_widget_set_vexpand (list_scroll, TRUE);
  self->rule_list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->rule_list), GTK_SELECTION_SINGLE);
  gtk_container_add (GTK_CONTAINER (list_scroll), self->rule_list);
  gtk_box_pack_start (GTK_BOX (self), list_scroll, FALSE, FALSE, 0);

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  add_btn = gtk_button_new_from_icon_name ("list-add-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text (add_btn, "New rule");
  self->remove_rule_button =
    gtk_button_new_from_icon_name ("list-remove-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text (self->remove_rule_button, "Remove the selected rule");
  self->refresh_rule_button =
    gtk_button_new_from_icon_name ("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text (self->refresh_rule_button,
                               "Reload rules from the server");
  /* Nothing to reload until a session is open: the dialog enables it via
   * sieve_rule_editor_set_refresh_sensitive(). */
  gtk_widget_set_sensitive (self->refresh_rule_button, FALSE);
  gtk_box_pack_start (GTK_BOX (buttons), add_btn, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (buttons), self->remove_rule_button, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (buttons), self->refresh_rule_button, FALSE, FALSE, 0);
  /* Ownership stays with the editor, regardless of its future parent. */
  self->toolbar = g_object_ref_sink (buttons);

  detail_scroll = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (detail_scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (detail_scroll),
                                       GTK_SHADOW_IN);
  gtk_widget_set_hexpand (detail_scroll, TRUE);
  gtk_widget_set_vexpand (detail_scroll, TRUE);
  self->detail = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width (GTK_CONTAINER (self->detail), 6);
  gtk_container_add (GTK_CONTAINER (detail_scroll), self->detail);
  /* gtk_container_add inserts a GtkViewport (self->detail is not
   * scrollable); its default GTK_SHADOW_IN frame would stack with the
   * GtkScrolledWindow's own and offset the panel's bottom edge relative
   * to the list (which has no viewport). Neutralize it. */
  {
    GtkWidget *viewport = gtk_bin_get_child (GTK_BIN (detail_scroll));
    if (GTK_IS_VIEWPORT (viewport))
      gtk_viewport_set_shadow_type (GTK_VIEWPORT (viewport), GTK_SHADOW_NONE);
  }
  gtk_box_pack_start (GTK_BOX (self), detail_scroll, TRUE, TRUE, 0);

  g_signal_connect (self->rule_list, "row-selected", G_CALLBACK (on_row_selected), self);
  g_signal_connect (add_btn, "clicked", G_CALLBACK (on_add_rule), self);
  g_signal_connect (self->remove_rule_button, "clicked", G_CALLBACK (on_remove_rule), self);
  g_signal_connect (self->refresh_rule_button, "clicked",
                    G_CALLBACK (on_refresh_rules), self);

  rebuild_detail (self);
}

GtkWidget *
sieve_rule_editor_new (void)
{
  return g_object_new (SIEVE_TYPE_RULE_EDITOR, NULL);
}
