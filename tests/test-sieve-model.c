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
  sieve_condition_set_value (c1, "news@example.com");
  c2->field = SIEVE_FIELD_SUBJECT;
  c2->match = SIEVE_MATCH_MATCHES;
  sieve_condition_set_value (c2, "*promo*");
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
  g_assert_cmpstr (sieve_condition_get_value (bc), ==, "news@example.com");

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
  sieve_condition_set_value (c1, "2M");
  c2->field = SIEVE_FIELD_BODY;
  c2->match = SIEVE_MATCH_CONTAINS;
  sieve_condition_set_value (c2, "forbidden pattern");
  c3->field = SIEVE_FIELD_HEADER;
  c3->header_name = g_strdup ("X-Spam-Flag");
  c3->match = SIEVE_MATCH_IS;
  sieve_condition_set_value (c3, "YES");
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
  sieve_condition_set_value (c, "(Foo|Bar), Example");
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
  g_assert_cmpstr (sieve_condition_get_value (bc), ==, "(Foo|Bar), Example");

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
  g_assert_cmpstr (sieve_condition_get_value (c2), ==, "(Foo|Bar), Example");

  out = sieve_rule_set_to_script (set);
  g_assert_nonnull (strstr (out, ":regex \"(Foo|Bar), Example\""));
  g_assert_null (strstr (out, ":contains \"(Foo|Bar), Example\""));
}

/* github issue #1: a value list with more than one entry
 * (`["a","b"]`) must not be truncated to its first element. The model
 * now represents the full list (SieveCondition.values): the rule stays
 * fully structured/editable and every entry survives the round trip. */
static void
test_multi_value_list_stays_editable (void)
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
  SieveCondition *c0, *c1;
  g_autofree gchar *out = NULL;
  g_autofree gchar *out2 = NULL;

  g_assert_nonnull (set);
  g_assert_cmpuint (set->rules->len, ==, 1);
  r = g_ptr_array_index (set->rules, 0);
  g_assert_false (r->opaque);
  g_assert_cmpuint (r->conditions->len, ==, 2);

  c0 = g_ptr_array_index (r->conditions, 0);
  g_assert_cmpuint (c0->values->len, ==, 2);
  g_assert_cmpstr (g_ptr_array_index (c0->values, 0), ==, "cooker-owner@linux-mandrake.com");
  g_assert_cmpstr (g_ptr_array_index (c0->values, 1), ==, "devel@mandrakesoft.com");

  c1 = g_ptr_array_index (r->conditions, 1);
  g_assert_cmpuint (c1->values->len, ==, 3);
  g_assert_cmpstr (g_ptr_array_index (c1->values, 2), ==, "cooker@");

  out = sieve_rule_set_to_script (set);
  g_assert_nonnull (strstr (out,
    "header :is \"sender\" [\"cooker-owner@linux-mandrake.com\", \"devel@mandrakesoft.com\"]"));
  g_assert_nonnull (strstr (out, "\"changelog@linux-mandrake.com\""));
  g_assert_nonnull (strstr (out, "\"cooker@\""));

  {
    g_autoptr (SieveRuleSet) back = sieve_rule_set_parse (out, NULL);
    out2 = sieve_rule_set_to_script (back);
  }
  g_assert_cmpstr (out, ==, out2);
}

/* github issue #1: the exact "list-id" example — a single unwrapped
 * `header :contains "list-id" [...]` test with three values — must stay
 * fully editable with all three values, not get wrapped into
 * "allof (... single value)" nor lose any entry. (A "# rule:[...]"
 * marker is required for structured parsing to even be attempted — see
 * test_parse_single_unwrapped_test — the plugin always writes one when
 * re-serializing, which is why a round-tripped script always has it.) */
static void
test_list_id_example_stays_editable (void)
{
  const gchar *script =
    "# rule:[List-Id]\n"
    "if header :contains \"list-id\" [\"mplayer-users.mplayerhq.hu\","
    "\"mplayer-dev-eng.mplayerhq.hu\",\"mplayer-matrox.lists.sourceforge.net\"]\n"
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
  g_assert_cmpuint (r->conditions->len, ==, 1);

  c = g_ptr_array_index (r->conditions, 0);
  g_assert_cmpuint (c->values->len, ==, 3);
  g_assert_cmpstr (g_ptr_array_index (c->values, 0), ==, "mplayer-users.mplayerhq.hu");
  g_assert_cmpstr (g_ptr_array_index (c->values, 1), ==, "mplayer-dev-eng.mplayerhq.hu");
  g_assert_cmpstr (g_ptr_array_index (c->values, 2), ==, "mplayer-matrox.lists.sourceforge.net");
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
  g_assert_cmpstr (sieve_condition_get_value (c), ==, "mplayer-users.mplayerhq.hu");
}

/* github issue #1: "if false # <test>" (the exact convention Roundcube's
 * managesieve plugin uses to disable a rule without deleting it) must
 * NOT be re-serialized as "if true", inverting the logic. It's now a
 * fully editable, disabled rule: `enabled` is FALSE, and the original
 * test is recovered from the trailing comment so it survives
 * re-enabling and round-trips exactly. */
static void
test_disabled_rule_recovers_condition (void)
{
  const gchar *script =
    "# rule:[Fail2ban]\n"
    "if false # allof (header :matches \"subject\" \"[Fail2ban] *\")\n"
    "{\n"
    "\tdiscard;\n"
    "}\n";
  g_autoptr (SieveRuleSet) set = sieve_rule_set_parse (script, NULL);
  SieveRule *r;
  SieveCondition *c;
  g_autofree gchar *out = NULL;
  g_autofree gchar *out2 = NULL;

  g_assert_nonnull (set);
  g_assert_cmpuint (set->rules->len, ==, 1);
  r = g_ptr_array_index (set->rules, 0);
  g_assert_false (r->opaque);
  g_assert_false (r->enabled);
  g_assert_cmpuint (r->conditions->len, ==, 1);

  c = g_ptr_array_index (r->conditions, 0);
  g_assert_cmpint (c->field, ==, SIEVE_FIELD_SUBJECT);
  g_assert_cmpint (c->match, ==, SIEVE_MATCH_MATCHES);
  g_assert_cmpstr (sieve_condition_get_value (c), ==, "[Fail2ban] *");

  out = sieve_rule_set_to_script (set);
  g_assert_null (strstr (out, "if true"));
  g_assert_nonnull (strstr (out,
    "if false # allof (header :matches \"subject\" \"[Fail2ban] *\")"));

  {
    g_autoptr (SieveRuleSet) back = sieve_rule_set_parse (out, NULL);
    out2 = sieve_rule_set_to_script (back);
  }
  g_assert_cmpstr (out, ==, out2);

  /* Re-enabling turns it back into a normal "if allof (...)" test. */
  r->enabled = TRUE;
  {
    g_autofree gchar *enabled_out = sieve_rule_set_to_script (set);
    g_assert_nonnull (strstr (enabled_out,
      "if allof (header :matches \"subject\" \"[Fail2ban] *\")"));
  }
}

/* A disabled rule with no trailing comment (no original test to
 * recover — e.g. one newly created, then disabled, in this editor)
 * stays disabled with no conditions, and round-trips stably. */
static void
test_disabled_rule_without_comment (void)
{
  const gchar *script =
    "# rule:[Off]\n"
    "if false\n"
    "{\n"
    "\tstop;\n"
    "}\n";
  g_autoptr (SieveRuleSet) set = sieve_rule_set_parse (script, NULL);
  SieveRule *r;
  g_autofree gchar *out = NULL;
  g_autofree gchar *out2 = NULL;

  g_assert_nonnull (set);
  r = g_ptr_array_index (set->rules, 0);
  g_assert_false (r->opaque);
  g_assert_false (r->enabled);
  g_assert_cmpuint (r->conditions->len, ==, 0);

  out = sieve_rule_set_to_script (set);
  g_assert_nonnull (strstr (out, "if false\n"));

  {
    g_autoptr (SieveRuleSet) back = sieve_rule_set_parse (out, NULL);
    out2 = sieve_rule_set_to_script (back);
  }
  g_assert_cmpstr (out, ==, out2);
}

/* A disabled rule whose trailing comment is NOT a recognizable test
 * (e.g. it uses "not", which the visual editor doesn't support): rather
 * than silently discard that comment on the first save — which would
 * permanently lose the original, disabled test — the whole rule falls
 * back to opaque, verbatim. */
static void
test_disabled_rule_unparseable_comment_becomes_opaque (void)
{
  const gchar *script =
    "# rule:[Weird]\n"
    "if false # not header :contains \"subject\" \"x\"\n"
    "{\n"
    "\tstop;\n"
    "}\n";
  g_autoptr (SieveRuleSet) set = sieve_rule_set_parse (script, NULL);
  SieveRule *r;

  g_assert_nonnull (set);
  r = g_ptr_array_index (set->rules, 0);
  g_assert_true (r->opaque);
  g_assert_nonnull (strstr (r->raw, "if false # not header :contains \"subject\" \"x\""));
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
  g_test_add_func ("/sieve-model/multi-value-list-stays-editable", test_multi_value_list_stays_editable);
  g_test_add_func ("/sieve-model/list-id-example-stays-editable", test_list_id_example_stays_editable);
  g_test_add_func ("/sieve-model/single-value-list-stays-editable", test_single_value_list_stays_editable);
  g_test_add_func ("/sieve-model/disabled-rule-recovers-condition", test_disabled_rule_recovers_condition);
  g_test_add_func ("/sieve-model/disabled-rule-without-comment", test_disabled_rule_without_comment);
  g_test_add_func ("/sieve-model/disabled-rule-unparseable-comment-becomes-opaque",
                   test_disabled_rule_unparseable_comment_becomes_opaque);
  return g_test_run ();
}
