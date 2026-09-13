/* test-sieve-model.c
 *
 * Checks the sieve-model (de)serializer: model ⇄ script round trip,
 * stability of re-serialization, and clean rejection of scripts that
 * the visual editor cannot represent.
 */

#include <glib.h>
#include <string.h>

#include "sieve-model.h"

static void
test_roundtrip_basic (void)
{
  g_autoptr (SieveRuleSet) set = sieve_rule_set_new ();
  SieveRule *r = sieve_rule_new ("Newsletters");
  SieveCondition *c1 = sieve_condition_new ();
  SieveCondition *c2 = sieve_condition_new ();
  SieveAction *a1 = sieve_action_new (SIEVE_ACTION_FILEINTO);
  SieveAction *a2 = sieve_action_new (SIEVE_ACTION_STOP);
  g_autofree gchar *script = NULL;
  g_autofree gchar *script2 = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (SieveRuleSet) back = NULL;
  SieveRule *br;
  SieveCondition *bc;

  r->mode = SIEVE_MATCH_MODE_ANY;
  c1->field = SIEVE_FIELD_FROM;
  c1->match = SIEVE_MATCH_CONTAINS;
  g_free (c1->value);
  c1->value = g_strdup ("news@example.com");
  c2->field = SIEVE_FIELD_SUBJECT;
  c2->match = SIEVE_MATCH_MATCHES;
  g_free (c2->value);
  c2->value = g_strdup ("*promo*");
  g_ptr_array_add (r->conditions, c1);
  g_ptr_array_add (r->conditions, c2);
  a1->arg = g_strdup ("INBOX/News");
  g_ptr_array_add (r->actions, a1);
  g_ptr_array_add (r->actions, a2);
  g_ptr_array_add (set->rules, r);

  script = sieve_rule_set_to_script (set);
  g_assert_nonnull (strstr (script, "require [\"fileinto\"];"));
  g_assert_nonnull (strstr (script, "# rule:[Newsletters]"));
  g_assert_nonnull (strstr (script, "if anyof ("));
  g_assert_nonnull (strstr (script, "header :contains \"from\" \"news@example.com\""));
  g_assert_nonnull (strstr (script, "header :matches \"subject\" \"*promo*\""));
  g_assert_nonnull (strstr (script, "fileinto \"INBOX/News\";"));
  g_assert_nonnull (strstr (script, "stop;"));

  back = sieve_rule_set_parse (script, &error);
  g_assert_no_error (error);
  g_assert_nonnull (back);
  g_assert_cmpuint (back->rules->len, ==, 1);

  br = g_ptr_array_index (back->rules, 0);
  g_assert_cmpstr (br->name, ==, "Newsletters");
  g_assert_cmpint (br->mode, ==, SIEVE_MATCH_MODE_ANY);
  g_assert_cmpuint (br->conditions->len, ==, 2);
  g_assert_cmpuint (br->actions->len, ==, 2);

  bc = g_ptr_array_index (br->conditions, 0);
  g_assert_cmpint (bc->field, ==, SIEVE_FIELD_FROM);
  g_assert_cmpint (bc->match, ==, SIEVE_MATCH_CONTAINS);
  g_assert_cmpstr (bc->value, ==, "news@example.com");

  /* Re-serialization must be stable down to the last character. */
  script2 = sieve_rule_set_to_script (back);
  g_assert_cmpstr (script, ==, script2);
}

static void
test_roundtrip_size_body_header (void)
{
  g_autoptr (SieveRuleSet) set = sieve_rule_set_new ();
  SieveRule *r = sieve_rule_new ("Big or spam");
  SieveCondition *c1 = sieve_condition_new ();
  SieveCondition *c2 = sieve_condition_new ();
  SieveCondition *c3 = sieve_condition_new ();
  SieveAction *a = sieve_action_new (SIEVE_ACTION_ADDFLAG);
  g_autofree gchar *script = NULL;
  g_autofree gchar *script2 = NULL;
  g_autoptr (SieveRuleSet) back = NULL;
  SieveRule *br;
  SieveCondition *bc3;
  SieveAction *ba;

  c1->field = SIEVE_FIELD_SIZE;
  c1->match = SIEVE_MATCH_OVER;
  g_free (c1->value);
  c1->value = g_strdup ("2M");
  c2->field = SIEVE_FIELD_BODY;
  c2->match = SIEVE_MATCH_CONTAINS;
  g_free (c2->value);
  c2->value = g_strdup ("forbidden pattern");
  c3->field = SIEVE_FIELD_HEADER;
  c3->header_name = g_strdup ("X-Spam-Flag");
  c3->match = SIEVE_MATCH_IS;
  g_free (c3->value);
  c3->value = g_strdup ("YES");
  g_ptr_array_add (r->conditions, c1);
  g_ptr_array_add (r->conditions, c2);
  g_ptr_array_add (r->conditions, c3);
  a->arg = g_strdup ("\\Seen");
  g_ptr_array_add (r->actions, a);
  g_ptr_array_add (set->rules, r);

  script = sieve_rule_set_to_script (set);
  g_assert_nonnull (strstr (script, "\"body\""));
  g_assert_nonnull (strstr (script, "\"imap4flags\""));
  g_assert_nonnull (strstr (script, "size :over 2M"));
  g_assert_nonnull (strstr (script, "body :text :contains \"forbidden pattern\""));
  g_assert_nonnull (strstr (script, "header :is \"X-Spam-Flag\" \"YES\""));
  g_assert_nonnull (strstr (script, "addflag \"\\\\Seen\";"));

  back = sieve_rule_set_parse (script, NULL);
  g_assert_nonnull (back);
  br = g_ptr_array_index (back->rules, 0);
  g_assert_cmpuint (br->conditions->len, ==, 3);

  bc3 = g_ptr_array_index (br->conditions, 2);
  g_assert_cmpint (bc3->field, ==, SIEVE_FIELD_HEADER);
  g_assert_cmpstr (bc3->header_name, ==, "X-Spam-Flag");
  g_assert_cmpint (bc3->match, ==, SIEVE_MATCH_IS);

  ba = g_ptr_array_index (br->actions, 0);
  g_assert_cmpint (ba->type, ==, SIEVE_ACTION_ADDFLAG);
  g_assert_cmpstr (ba->arg, ==, "\\Seen");

  script2 = sieve_rule_set_to_script (back);
  g_assert_cmpstr (script, ==, script2);
}

static void
test_empty_and_comment_only (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (SieveRuleSet) set =
    sieve_rule_set_parse ("   \n\n# just a comment\n/* and another one */\n", &error);

  g_assert_no_error (error);
  g_assert_nonnull (set);
  g_assert_cmpuint (set->rules->len, ==, 0);
}

static void
test_rule_without_conditions (void)
{
  g_autoptr (SieveRuleSet) set = sieve_rule_set_new ();
  SieveRule *r = sieve_rule_new ("Keep everything");
  g_autofree gchar *script = NULL;
  g_autoptr (SieveRuleSet) back = NULL;

  g_ptr_array_add (r->actions, sieve_action_new (SIEVE_ACTION_KEEP));
  g_ptr_array_add (set->rules, r);

  script = sieve_rule_set_to_script (set);
  g_assert_nonnull (strstr (script, "if true"));

  back = sieve_rule_set_parse (script, NULL);
  g_assert_nonnull (back);
  g_assert_cmpuint (back->rules->len, ==, 1);
  g_assert_cmpuint (((SieveRule *) g_ptr_array_index (back->rules, 0))->conditions->len, ==, 0);
}

static void
test_parse_single_unwrapped_test (void)
{
  const gchar *script =
    "# rule:[Simple]\n"
    "if header :contains \"from\" \"boss@example.com\"\n"
    "{\n"
    "\tkeep;\n"
    "}\n";
  g_autoptr (SieveRuleSet) set = sieve_rule_set_parse (script, NULL);
  SieveRule *r;

  g_assert_nonnull (set);
  g_assert_cmpuint (set->rules->len, ==, 1);
  r = g_ptr_array_index (set->rules, 0);
  g_assert_cmpuint (r->conditions->len, ==, 1);
  g_assert_cmpuint (r->actions->len, ==, 1);
  g_assert_cmpint (((SieveAction *) g_ptr_array_index (r->actions, 0))->type, ==, SIEVE_ACTION_KEEP);
}

/* An `if` with no marker, with a test that cannot be represented (`not`):
 * kept verbatim in an opaque rule, and the leading `require` is preserved
 * (extensions re-injected on serialization). */
static void
test_handwritten_becomes_opaque (void)
{
  const gchar *hand =
    "require \"fileinto\";\n"
    "if not header :contains \"subject\" \"x\" {\n"
    "  fileinto \"Other\";\n"
    "}\n";
  g_autoptr (GError) error = NULL;
  g_autoptr (SieveRuleSet) set = sieve_rule_set_parse (hand, &error);
  g_autofree gchar *script = NULL;
  g_autofree gchar *script2 = NULL;
  SieveRule *r;

  g_assert_no_error (error);
  g_assert_nonnull (set);
  g_assert_cmpuint (set->rules->len, ==, 1);

  r = g_ptr_array_index (set->rules, 0);
  g_assert_true (r->opaque);
  g_assert_nonnull (r->raw);
  g_assert_nonnull (strstr (r->raw, "if not header :contains \"subject\" \"x\""));

  /* `fileinto` re-declared on serialization thanks to the leading require. */
  script = sieve_rule_set_to_script (set);
  g_assert_nonnull (strstr (script, "require [\"fileinto\"];"));
  g_assert_nonnull (strstr (script, "if not header :contains \"subject\" \"x\""));

  /* Re-serialization stable down to the last character. */
  {
    g_autoptr (SieveRuleSet) back = sieve_rule_set_parse (script, NULL);
    script2 = sieve_rule_set_to_script (back);
  }
  g_assert_cmpstr (script, ==, script2);
}

/* Marker "# rule:[…]" but an unhandled action (`vacation`): falls back to
 * an opaque rule, the marker's name still used for display. */
static void
test_unsupported_action_becomes_opaque (void)
{
  const gchar *script =
    "# rule:[Vacation]\n"
    "if true\n"
    "{\n"
    "\tvacation \"away\";\n"
    "}\n";
  g_autoptr (GError) error = NULL;
  g_autoptr (SieveRuleSet) set = sieve_rule_set_parse (script, &error);
  SieveRule *r;
  g_autofree gchar *out = NULL;

  g_assert_no_error (error);
  g_assert_nonnull (set);
  g_assert_cmpuint (set->rules->len, ==, 1);

  r = g_ptr_array_index (set->rules, 0);
  g_assert_true (r->opaque);
  g_assert_cmpstr (r->name, ==, "Vacation");
  g_assert_nonnull (strstr (r->raw, "vacation \"away\";"));

  out = sieve_rule_set_to_script (set);
  g_assert_nonnull (strstr (out, "# rule:[Vacation]"));
  g_assert_nonnull (strstr (out, "vacation \"away\";"));
}

/* "Nextcloud Mail" block (no marker, "### … ###" banners before AND
 * after the block): a single opaque rule, name taken from the
 * "# My filter" line, stable round trip. */
static void
test_nextcloud_block_opaque (void)
{
  const gchar *script =
    "### Nextcloud Mail: Filters ### DON'T EDIT ###\n"
    "# FILTER: [{\"name\":\"My filter\",\"enable\":true}]\n"
    "# My filter\n"
    "if header :is \"Subject\" [\"\"] {\n"
    "\tstop;\n"
    "}\n"
    "### Nextcloud Mail: Filters ### DON'T EDIT ###\n";
  g_autoptr (SieveRuleSet) set = sieve_rule_set_parse (script, NULL);
  g_autofree gchar *out = NULL;
  g_autofree gchar *out2 = NULL;
  SieveRule *r;

  g_assert_nonnull (set);
  g_assert_cmpuint (set->rules->len, ==, 1);

  r = g_ptr_array_index (set->rules, 0);
  g_assert_true (r->opaque);
  g_assert_cmpstr (r->name, ==, "My filter");
  /* Both banners are kept. */
  g_assert_cmpuint (
    strstr (r->raw, "### Nextcloud") == r->raw ? 1 : 0, ==, 1);
  {
    const gchar *first = strstr (r->raw, "### Nextcloud Mail");
    const gchar *last = g_strrstr (r->raw, "### Nextcloud Mail");
    g_assert_nonnull (first);
    g_assert_true (last != first); /* closing banner absorbed */
  }

  out = sieve_rule_set_to_script (set);
  {
    g_autoptr (SieveRuleSet) back = sieve_rule_set_parse (out, NULL);
    out2 = sieve_rule_set_to_script (back);
  }
  g_assert_cmpstr (out, ==, out2);
}

/* Mix: an editable structured rule followed by an opaque block. */
static void
test_mixed_structured_and_opaque (void)
{
  const gchar *script =
    "# rule:[Structured]\n"
    "if allof (header :contains \"from\" \"boss@example.com\")\n"
    "{\n"
    "\tkeep;\n"
    "}\n"
    "\n"
    "### Nextcloud Mail: Filters ### DON'T EDIT ###\n"
    "# My filter\n"
    "if header :is \"Subject\" [\"\"] {\n"
    "\tstop;\n"
    "}\n"
    "### Nextcloud Mail: Filters ### DON'T EDIT ###\n";
  g_autoptr (SieveRuleSet) set = sieve_rule_set_parse (script, NULL);
  g_autofree gchar *out = NULL;
  g_autofree gchar *out2 = NULL;
  SieveRule *r0, *r1;

  g_assert_nonnull (set);
  g_assert_cmpuint (set->rules->len, ==, 2);

  r0 = g_ptr_array_index (set->rules, 0);
  g_assert_false (r0->opaque);
  g_assert_cmpstr (r0->name, ==, "Structured");
  g_assert_cmpuint (r0->conditions->len, ==, 1);
  g_assert_cmpuint (r0->actions->len, ==, 1);

  r1 = g_ptr_array_index (set->rules, 1);
  g_assert_true (r1->opaque);
  g_assert_cmpstr (r1->name, ==, "My filter");

  out = sieve_rule_set_to_script (set);
  g_assert_nonnull (strstr (out, "# rule:[Structured]"));
  g_assert_nonnull (strstr (out, "### Nextcloud Mail: Filters ### DON'T EDIT ###"));

  {
    g_autoptr (SieveRuleSet) back = sieve_rule_set_parse (out, NULL);
    out2 = sieve_rule_set_to_script (back);
  }
  g_assert_cmpstr (out, ==, out2);
}

/* github.com/cyr-ius/evolution-sieve-filters/issues/1: ":regex" support
 * (RFE) and a round trip through the model (including the "regex"
 * require). */
static void
test_regex_roundtrip (void)
{
  g_autoptr (SieveRuleSet) set = sieve_rule_set_new ();
  SieveRule *r = sieve_rule_new ("Login alert");
  SieveCondition *c = sieve_condition_new ();
  g_autofree gchar *script = NULL;
  g_autofree gchar *script2 = NULL;
  g_autoptr (SieveRuleSet) back = NULL;
  SieveRule *br;
  SieveCondition *bc;

  c->field = SIEVE_FIELD_BODY;
  c->match = SIEVE_MATCH_REGEX;
  g_free (c->value);
  c->value = g_strdup ("(Foo|Bar), Example");
  g_ptr_array_add (r->conditions, c);
  g_ptr_array_add (r->actions, sieve_action_new (SIEVE_ACTION_KEEP));
  g_ptr_array_add (set->rules, r);

  script = sieve_rule_set_to_script (set);
  g_assert_nonnull (strstr (script, "\"regex\""));
  g_assert_nonnull (strstr (script, "body :text :regex \"(Foo|Bar), Example\""));

  back = sieve_rule_set_parse (script, NULL);
  g_assert_nonnull (back);
  br = g_ptr_array_index (back->rules, 0);
  g_assert_false (br->opaque);
  bc = g_ptr_array_index (br->conditions, 0);
  g_assert_cmpint (bc->match, ==, SIEVE_MATCH_REGEX);
  g_assert_cmpstr (bc->value, ==, "(Foo|Bar), Example");

  script2 = sieve_rule_set_to_script (back);
  g_assert_cmpstr (script, ==, script2);
}

/* github issue #1: a hand-written rule using ":regex" must NOT be
 * silently downgraded to ":contains". Now that ":regex" is a supported
 * match type (see test_regex_roundtrip), such a rule parses as a fully
 * structured, editable rule with the regex condition preserved exactly
 * — rather than either losing the ":regex" or falling back to opaque. */
static void
test_regex_in_handwritten_rule_kept_verbatim (void)
{
  const gchar *script =
    "# rule:[Login alert]\n"
    "if allof (header :contains \"subject\" \"Login attempt from new device\", "
    "body :text :regex \"(Foo|Bar), Example\")\n"
    "{\n"
    "\tkeep;\n"
    "}\n";
  g_autoptr (SieveRuleSet) set = sieve_rule_set_parse (script, NULL);
  SieveRule *r;
  SieveCondition *c1, *c2;
  g_autofree gchar *out = NULL;

  g_assert_nonnull (set);
  g_assert_cmpuint (set->rules->len, ==, 1);
  r = g_ptr_array_index (set->rules, 0);
  g_assert_false (r->opaque);
  g_assert_cmpuint (r->conditions->len, ==, 2);

  c1 = g_ptr_array_index (r->conditions, 0);
  g_assert_cmpint (c1->match, ==, SIEVE_MATCH_CONTAINS);

  c2 = g_ptr_array_index (r->conditions, 1);
  g_assert_cmpint (c2->match, ==, SIEVE_MATCH_REGEX);
  g_assert_cmpstr (c2->value, ==, "(Foo|Bar), Example");

  out = sieve_rule_set_to_script (set);
  g_assert_nonnull (strstr (out, ":regex \"(Foo|Bar), Example\""));
  g_assert_null (strstr (out, ":contains \"(Foo|Bar), Example\""));
}

/* github issue #1: a value list with more than one entry
 * (`["a","b"]`) must not be truncated to its first element — the whole
 * rule falls back to opaque, keeping every entry verbatim. */
static void
test_multi_value_list_kept_verbatim (void)
{
  const gchar *script =
    "# rule:[Cooker]\n"
    "if anyof (header :is \"sender\" [\"cooker-owner@linux-mandrake.com\",\"devel@mandrakesoft.com\"], "
    "header :is \"x-loop\" [\"cooker@linux-mandrake.com\",\"changelog@linux-mandrake.com\",\"cooker@\"])\n"
    "{\n"
    "\tkeep;\n"
    "}\n";
  g_autoptr (SieveRuleSet) set = sieve_rule_set_parse (script, NULL);
  SieveRule *r;

  g_assert_nonnull (set);
  g_assert_cmpuint (set->rules->len, ==, 1);
  r = g_ptr_array_index (set->rules, 0);
  g_assert_true (r->opaque);
  g_assert_nonnull (strstr (r->raw, "\"devel@mandrakesoft.com\""));
  g_assert_nonnull (strstr (r->raw, "\"changelog@linux-mandrake.com\""));
  g_assert_nonnull (strstr (r->raw, "\"cooker@\""));
}

/* A singleton list ("[\"a\"]") is semantically a bare string and stays
 * fully editable. */
static void
test_single_value_list_stays_editable (void)
{
  const gchar *script =
    "# rule:[List]\n"
    "if header :contains \"list-id\" [\"mplayer-users.mplayerhq.hu\"]\n"
    "{\n"
    "\tkeep;\n"
    "}\n";
  g_autoptr (SieveRuleSet) set = sieve_rule_set_parse (script, NULL);
  SieveRule *r;
  SieveCondition *c;

  g_assert_nonnull (set);
  g_assert_cmpuint (set->rules->len, ==, 1);
  r = g_ptr_array_index (set->rules, 0);
  g_assert_false (r->opaque);
  c = g_ptr_array_index (r->conditions, 0);
  g_assert_cmpstr (c->value, ==, "mplayer-users.mplayerhq.hu");
}

/* github issue #1: "if false # ..." must NOT be re-serialized as
 * "if true" — the model has no way to represent a standalone "false"
 * test (an empty condition list always means "true"), so it must fall
 * back to an opaque rule instead of silently inverting the logic. */
static void
test_if_false_kept_verbatim (void)
{
  const gchar *script =
    "# rule:[Fail2ban]\n"
    "if false # allof (header :matches \"subject\" \"[Fail2ban] *\")\n"
    "{\n"
    "\tdiscard;\n"
    "}\n";
  g_autoptr (SieveRuleSet) set = sieve_rule_set_parse (script, NULL);
  SieveRule *r;
  g_autofree gchar *out = NULL;

  g_assert_nonnull (set);
  g_assert_cmpuint (set->rules->len, ==, 1);
  r = g_ptr_array_index (set->rules, 0);
  g_assert_true (r->opaque);
  g_assert_nonnull (strstr (r->raw, "if false"));
  g_assert_null (strstr (r->raw, "if true"));

  out = sieve_rule_set_to_script (set);
  g_assert_nonnull (strstr (out, "if false"));
}

/* A truly broken script (unclosed brace): still rejected. */
static void
test_reject_unbalanced_braces (void)
{
  const gchar *broken =
    "# rule:[Broken]\n"
    "if true {\n"
    "\tkeep;\n";
  g_autoptr (GError) error = NULL;
  g_autoptr (SieveRuleSet) set = sieve_rule_set_parse (broken, &error);

  g_assert_null (set);
  g_assert_error (error, SIEVE_MODEL_ERROR, SIEVE_MODEL_ERROR_SYNTAX);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/sieve-model/roundtrip-basic", test_roundtrip_basic);
  g_test_add_func ("/sieve-model/roundtrip-size-body-header", test_roundtrip_size_body_header);
  g_test_add_func ("/sieve-model/empty-and-comment-only", test_empty_and_comment_only);
  g_test_add_func ("/sieve-model/rule-without-conditions", test_rule_without_conditions);
  g_test_add_func ("/sieve-model/parse-single-unwrapped-test", test_parse_single_unwrapped_test);
  g_test_add_func ("/sieve-model/handwritten-becomes-opaque", test_handwritten_becomes_opaque);
  g_test_add_func ("/sieve-model/unsupported-action-becomes-opaque", test_unsupported_action_becomes_opaque);
  g_test_add_func ("/sieve-model/nextcloud-block-opaque", test_nextcloud_block_opaque);
  g_test_add_func ("/sieve-model/mixed-structured-and-opaque", test_mixed_structured_and_opaque);
  g_test_add_func ("/sieve-model/reject-unbalanced-braces", test_reject_unbalanced_braces);
  g_test_add_func ("/sieve-model/regex-roundtrip", test_regex_roundtrip);
  g_test_add_func ("/sieve-model/regex-in-handwritten-rule-kept-verbatim",
                   test_regex_in_handwritten_rule_kept_verbatim);
  g_test_add_func ("/sieve-model/multi-value-list-kept-verbatim", test_multi_value_list_kept_verbatim);
  g_test_add_func ("/sieve-model/single-value-list-stays-editable", test_single_value_list_stays_editable);
  g_test_add_func ("/sieve-model/if-false-kept-verbatim", test_if_false_kept_verbatim);
  return g_test_run ();
}
