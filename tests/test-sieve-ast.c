/* test-sieve-ast.c
 *
 * Checks the generic RFC 5228 parser (sieve-ast): lexical tokens,
 * grammar, byte offsets kept on every node, per-command recovery from
 * grammar errors, fatal lexical errors, nesting bound, and robustness
 * against truncated input.
 */

#include <glib.h>
#include <string.h>

#include "sieve-ast.h"
#include "sieve-model.h"

static SieveAst *
parse_ok (const gchar *script)
{
  g_autoptr (GError) error = NULL;
  SieveAst *ast = sieve_ast_parse (script, &error);

  g_assert_no_error (error);
  g_assert_nonnull (ast);
  return ast;
}

static void
parse_fails (const gchar *script, const gchar *expected_msg)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (SieveAst) ast = sieve_ast_parse (script, &error);

  g_assert_null (ast);
  g_assert_error (error, SIEVE_MODEL_ERROR, SIEVE_MODEL_ERROR_SYNTAX);
  if (strstr (error->message, expected_msg) == NULL)
    g_error ("\"%s\" not found in error \"%s\"", expected_msg, error->message);
}

static SieveAstCommand *
cmd_at (const SieveAst *ast, guint i)
{
  g_assert_cmpuint (i, <, ast->commands->len);
  return g_ptr_array_index (ast->commands, i);
}

static SieveAstArg *
arg_at (GPtrArray *args, guint i)
{
  g_assert_cmpuint (i, <, args->len);
  return g_ptr_array_index (args, i);
}

static SieveAstTest *
test_at (GPtrArray *tests, guint i)
{
  g_assert_nonnull (tests);
  g_assert_cmpuint (i, <, tests->len);
  return g_ptr_array_index (tests, i);
}

static gchar *
span (const SieveAst *ast, gsize start, gsize end)
{
  g_assert_cmpuint (start, <=, end);
  g_assert_cmpuint (end, <=, strlen (ast->source));
  return g_strndup (ast->source + start, end - start);
}

/* An "unparseable" node whose error mentions `msg`. */
static void
assert_error_node (const SieveAstCommand *cmd, const gchar *msg)
{
  g_assert_null (cmd->name);
  g_assert_nonnull (cmd->error);
  if (strstr (cmd->error, msg) == NULL)
    g_error ("\"%s\" not found in error node \"%s\"", msg, cmd->error);
}

/* ---- Lexical tokens ------------------------------------------------------ */

static void
test_empty (void)
{
  g_autoptr (SieveAst) a = parse_ok ("");
  g_autoptr (SieveAst) b = parse_ok ("  # comment\n/* block\n comment */\n");
  g_autoptr (SieveAst) c = parse_ok (NULL);

  g_assert_cmpuint (a->commands->len, ==, 0);
  g_assert_cmpuint (b->commands->len, ==, 0);
  g_assert_cmpuint (c->commands->len, ==, 0);
}

static void
test_require (void)
{
  g_autoptr (SieveAst) ast = parse_ok ("require [\"fileinto\", \"imap4flags\"];\n");
  SieveAstCommand *cmd = cmd_at (ast, 0);
  SieveAstArg *arg;

  g_assert_cmpuint (ast->commands->len, ==, 1);
  g_assert_cmpstr (cmd->name, ==, "require");
  g_assert_null (cmd->tests);
  g_assert_null (cmd->block);
  g_assert_cmpuint (cmd->args->len, ==, 1);
  arg = arg_at (cmd->args, 0);
  g_assert_cmpint (arg->kind, ==, SIEVE_AST_ARG_STRING_LIST);
  g_assert_cmpuint (arg->list->len, ==, 2);
  g_assert_cmpstr (g_ptr_array_index (arg->list, 0), ==, "fileinto");
  g_assert_cmpstr (g_ptr_array_index (arg->list, 1), ==, "imap4flags");
}

static void
test_numbers (void)
{
  g_autoptr (SieveAst) ast = parse_ok ("t 0 1K 10m 4G 100;");
  GPtrArray *args = cmd_at (ast, 0)->args;
  const guint64 expected[] = {
    0, 1024, G_GUINT64_CONSTANT (10) << 20, G_GUINT64_CONSTANT (4) << 30, 100
  };

  g_assert_cmpuint (args->len, ==, G_N_ELEMENTS (expected));
  for (guint i = 0; i < G_N_ELEMENTS (expected); i++) {
    g_assert_cmpint (arg_at (args, i)->kind, ==, SIEVE_AST_ARG_NUMBER);
    g_assert_cmpuint (arg_at (args, i)->number, ==, expected[i]);
  }
  g_assert_cmpstr (arg_at (args, 2)->str, ==, "10m");
}

static void
test_number_errors (void)
{
  g_autoptr (SieveAst) ast = parse_ok ("a 1X;\n"
                                       "b 99999999999999999999;\n"
                                       "c 17179869184G;\n"
                                       "d 17179869183G;\n");

  g_assert_cmpuint (ast->commands->len, ==, 4);
  assert_error_node (cmd_at (ast, 0), "invalid number");
  assert_error_node (cmd_at (ast, 1), "number too large");
  assert_error_node (cmd_at (ast, 2), "number too large");
  g_assert_cmpstr (cmd_at (ast, 3)->name, ==, "d");
  g_assert_cmpuint (arg_at (cmd_at (ast, 3)->args, 0)->number, ==,
                    G_GUINT64_CONSTANT (17179869183) << 30);
}

static void
test_quoted_strings (void)
{
  g_autoptr (SieveAst) ast = parse_ok ("s \"a\\\"b\" \"c\\\\d\" \"\\q\" \"\" \"é\";");
  GPtrArray *args = cmd_at (ast, 0)->args;

  g_assert_cmpstr (arg_at (args, 0)->str, ==, "a\"b");
  g_assert_cmpstr (arg_at (args, 1)->str, ==, "c\\d");
  g_assert_cmpstr (arg_at (args, 2)->str, ==, "q");
  g_assert_cmpstr (arg_at (args, 3)->str, ==, "");
  g_assert_cmpstr (arg_at (args, 4)->str, ==, "é");
  g_assert_false (arg_at (args, 0)->multiline);
}

static void
test_multiline_lf (void)
{
  /* A '"' and a '}' inside the text must not confuse anything, and a
   * dot-stuffed line loses its first dot. */
  g_autoptr (SieveAst) ast = parse_ok ("vacation :days 7 :subject \"Away\" text:\n"
                                       "Hello \"there\" }\n"
                                       "..dot\n"
                                       ".\n"
                                       ";\n"
                                       "keep;\n");
  SieveAstCommand *cmd = cmd_at (ast, 0);
  SieveAstArg *text;

  g_assert_cmpuint (ast->commands->len, ==, 2);
  g_assert_cmpstr (cmd->name, ==, "vacation");
  g_assert_cmpuint (cmd->args->len, ==, 5);
  g_assert_cmpstr (arg_at (cmd->args, 0)->str, ==, "days");
  g_assert_cmpuint (arg_at (cmd->args, 1)->number, ==, 7);
  text = arg_at (cmd->args, 4);
  g_assert_cmpint (text->kind, ==, SIEVE_AST_ARG_STRING);
  g_assert_true (text->multiline);
  g_assert_cmpstr (text->str, ==, "Hello \"there\" }\n.dot\n");
  g_assert_cmpstr (cmd_at (ast, 1)->name, ==, "keep");
}

static void
test_multiline_crlf_and_comment (void)
{
  g_autoptr (SieveAst) a = parse_ok ("x text:\r\nA\r\n\r\n.\r\n;");
  g_autoptr (SieveAst) b = parse_ok ("x TEXT:  # comment\nA\n.\n;");
  g_autoptr (SieveAst) c = parse_ok ("x [text:\n.\n, \"b\"];");
  SieveAstArg *list;

  g_assert_cmpstr (arg_at (cmd_at (a, 0)->args, 0)->str, ==, "A\r\n\r\n");
  g_assert_cmpstr (arg_at (cmd_at (b, 0)->args, 0)->str, ==, "A\n");

  /* text: inside a string list, empty body */
  list = arg_at (cmd_at (c, 0)->args, 0);
  g_assert_cmpint (list->kind, ==, SIEVE_AST_ARG_STRING_LIST);
  g_assert_cmpstr (g_ptr_array_index (list->list, 0), ==, "");
  g_assert_cmpstr (g_ptr_array_index (list->list, 1), ==, "b");
}

/* ---- Grammar ------------------------------------------------------------- */

static void
test_nested_tests (void)
{
  g_autoptr (SieveAst) ast = parse_ok (
    "if anyof (not exists \"X-Spam\",\n"
    "          allof (header :is \"a\" \"b\", size :over 1M)) {\n"
    "  fileinto \"x\";\n"
    "  stop;\n"
    "} elsif true {\n"
    "  keep;\n"
    "} else {\n"
    "  discard;\n"
    "}\n");
  SieveAstCommand *if_cmd = cmd_at (ast, 0);
  SieveAstTest *anyof, *not, *allof, *size;

  g_assert_cmpuint (ast->commands->len, ==, 3);
  g_assert_cmpstr (if_cmd->name, ==, "if");
  g_assert_cmpstr (cmd_at (ast, 1)->name, ==, "elsif");
  g_assert_cmpstr (cmd_at (ast, 2)->name, ==, "else");
  g_assert_null (cmd_at (ast, 2)->tests);

  g_assert_false (if_cmd->test_list);
  g_assert_cmpuint (if_cmd->tests->len, ==, 1);
  anyof = test_at (if_cmd->tests, 0);
  g_assert_cmpstr (anyof->name, ==, "anyof");
  g_assert_true (anyof->test_list);
  g_assert_cmpuint (anyof->tests->len, ==, 2);

  not = test_at (anyof->tests, 0);
  g_assert_cmpstr (not->name, ==, "not");
  g_assert_false (not->test_list);
  g_assert_cmpstr (test_at (not->tests, 0)->name, ==, "exists");
  g_assert_cmpstr (arg_at (test_at (not->tests, 0)->args, 0)->str, ==, "X-Spam");

  allof = test_at (anyof->tests, 1);
  g_assert_cmpstr (allof->name, ==, "allof");
  g_assert_cmpuint (allof->tests->len, ==, 2);
  size = test_at (allof->tests, 1);
  g_assert_cmpstr (size->name, ==, "size");
  g_assert_cmpint (arg_at (size->args, 0)->kind, ==, SIEVE_AST_ARG_TAG);
  g_assert_cmpstr (arg_at (size->args, 0)->str, ==, "over");
  g_assert_cmpuint (arg_at (size->args, 1)->number, ==, 1024 * 1024);
  g_assert_null (size->tests);

  g_assert_cmpuint (if_cmd->block->len, ==, 2);
  g_assert_cmpstr (((SieveAstCommand *) g_ptr_array_index (if_cmd->block, 0))->name, ==, "fileinto");
  g_assert_cmpstr (((SieveAstCommand *) g_ptr_array_index (if_cmd->block, 1))->name, ==, "stop");
}

static void
test_case_insensitive (void)
{
  g_autoptr (SieveAst) ast = parse_ok ("IF Header :IS \"Subject\" \"ABC\" { Keep; }");
  SieveAstCommand *cmd = cmd_at (ast, 0);
  SieveAstTest *t = test_at (cmd->tests, 0);

  g_assert_cmpstr (cmd->name, ==, "if");
  g_assert_cmpstr (t->name, ==, "header");
  g_assert_cmpstr (arg_at (t->args, 0)->str, ==, "is");
  /* string values are never touched */
  g_assert_cmpstr (arg_at (t->args, 1)->str, ==, "Subject");
  g_assert_cmpstr (arg_at (t->args, 2)->str, ==, "ABC");
  g_assert_cmpstr (((SieveAstCommand *) g_ptr_array_index (cmd->block, 0))->name, ==, "keep");
}

static void
test_offsets (void)
{
  g_autoptr (SieveAst) ast = parse_ok (
    "require \"fileinto\";\n"
    "\n"
    "# rule:[Spam]\n"
    "if header :contains \"subject\" \"spam\" { fileinto \"Junk\"; }\n"
    "# trailing\n");
  SieveAstCommand *req = cmd_at (ast, 0);
  SieveAstCommand *rule = cmd_at (ast, 1);
  SieveAstTest *t = test_at (rule->tests, 0);
  g_autofree gchar *req_text = span (ast, req->start, req->end);
  g_autofree gchar *rule_text = span (ast, rule->start, rule->end);
  g_autofree gchar *rule_trivia = span (ast, rule->trivia_start, rule->start);
  g_autofree gchar *test_text = span (ast, t->start, t->end);
  g_autofree gchar *arg_text = span (ast, arg_at (t->args, 1)->start, arg_at (t->args, 1)->end);
  SieveAstCommand *sub = g_ptr_array_index (rule->block, 0);
  g_autofree gchar *sub_text = span (ast, sub->start, sub->end);

  g_assert_cmpuint (req->trivia_start, ==, 0);
  g_assert_cmpstr (req_text, ==, "require \"fileinto\";");
  g_assert_cmpuint (rule->trivia_start, ==, req->end);
  g_assert_cmpstr (rule_trivia, ==, "\n\n# rule:[Spam]\n");
  g_assert_cmpstr (rule_text, ==,
                   "if header :contains \"subject\" \"spam\" { fileinto \"Junk\"; }");
  g_assert_cmpstr (test_text, ==, "header :contains \"subject\" \"spam\"");
  g_assert_cmpstr (arg_text, ==, "\"subject\"");
  g_assert_cmpstr (sub_text, ==, "fileinto \"Junk\";");
}

static void
test_multiline_offsets (void)
{
  g_autoptr (SieveAst) ast = parse_ok ("x text:\nA\n.\n;");
  SieveAstCommand *cmd = cmd_at (ast, 0);
  g_autofree gchar *arg_text = span (ast, arg_at (cmd->args, 0)->start,
                                     arg_at (cmd->args, 0)->end);
  g_autofree gchar *cmd_text = span (ast, cmd->start, cmd->end);

  g_assert_cmpstr (arg_text, ==, "text:\nA\n.\n");
  g_assert_cmpstr (cmd_text, ==, "x text:\nA\n.\n;");
}

/* ---- Recovery from grammar errors ---------------------------------------- */

static void
test_recovery_block (void)
{
  g_autoptr (SieveAst) ast = parse_ok ("keep;\n"
                                       "if anyof (true,) { stop; }\n"
                                       "discard;\n");
  SieveAstCommand *bad = cmd_at (ast, 1);
  g_autofree gchar *bad_text = NULL;

  g_assert_cmpuint (ast->commands->len, ==, 3);
  g_assert_cmpstr (cmd_at (ast, 0)->name, ==, "keep");
  assert_error_node (bad, "line 2, column 16: expected a test name, got \")\"");
  bad_text = span (ast, bad->start, bad->end);
  g_assert_cmpstr (bad_text, ==, "if anyof (true,) { stop; }");
  g_assert_cmpuint (bad->trivia_start, ==, cmd_at (ast, 0)->end);
  g_assert_cmpstr (cmd_at (ast, 2)->name, ==, "discard");
}

static void
test_recovery_simple (void)
{
  g_autoptr (SieveAst) ast = parse_ok ("fileinto \"a\" \"b\" ];\n"
                                       "}\n"
                                       "keep;\n");
  g_autofree gchar *first = NULL, *second = NULL;

  g_assert_cmpuint (ast->commands->len, ==, 3);
  assert_error_node (cmd_at (ast, 0), "expected \";\" or \"{\", got \"]\"");
  first = span (ast, cmd_at (ast, 0)->start, cmd_at (ast, 0)->end);
  g_assert_cmpstr (first, ==, "fileinto \"a\" \"b\" ];");
  /* a stray "}" is a unit of its own, not swallowing the rest */
  assert_error_node (cmd_at (ast, 1), "expected a command name");
  second = span (ast, cmd_at (ast, 1)->start, cmd_at (ast, 1)->end);
  g_assert_cmpstr (second, ==, "}");
  g_assert_cmpstr (cmd_at (ast, 2)->name, ==, "keep");
}

static void
test_recovery_bad_tokens (void)
{
  g_autoptr (SieveAst) ast = parse_ok ("keep @;\n"
                                       "x \"é\" ¤;\n"
                                       "y :;\n"
                                       "z [];\n"
                                       "if anyof () { }\n"
                                       "keep;\n");

  g_assert_cmpuint (ast->commands->len, ==, 6);
  assert_error_node (cmd_at (ast, 0), "line 1, column 6: unexpected character \"@\"");
  /* columns count characters, not bytes */
  assert_error_node (cmd_at (ast, 1), "line 2, column 7: unexpected character \"¤\"");
  assert_error_node (cmd_at (ast, 2), "expected a tag name after \":\"");
  assert_error_node (cmd_at (ast, 3), "expected a string, got \"]\"");
  assert_error_node (cmd_at (ast, 4), "expected a test name, got \")\"");
  g_assert_cmpstr (cmd_at (ast, 5)->name, ==, "keep");
}

static void
test_recovery_nested_block (void)
{
  /* A grammar error inside a nested block (missing ";" before "}"):
   * the whole top-level unit becomes the error node, up to the "}"
   * that closes its outermost block, not the inner one. */
  g_autoptr (SieveAst) ast = parse_ok ("if true {\n"
                                       "  if true { keep }\n"
                                       "}\n"
                                       "stop;\n");
  g_autofree gchar *bad_text = NULL;

  g_assert_cmpuint (ast->commands->len, ==, 2);
  assert_error_node (cmd_at (ast, 0), "line 2, column 18: expected \";\" or \"{\", got \"}\"");
  bad_text = span (ast, cmd_at (ast, 0)->start, cmd_at (ast, 0)->end);
  g_assert_cmpstr (bad_text, ==, "if true {\n  if true { keep }\n}");
  g_assert_cmpstr (cmd_at (ast, 1)->name, ==, "stop");
}

/* ---- Fatal (lexical) errors --------------------------------------------- */

static void
test_fatal_errors (void)
{
  parse_fails ("keep;\nfileinto \"abc;\n", "line 2, column 10: unterminated quoted string");
  parse_fails ("keep; /* never closed", "unterminated \"/* ... */\" comment");
  parse_fails ("if true {\n  keep;\n", "line 1, column 1: unclosed \"{\"");
  parse_fails ("keep; if true { if false { stop; }", "unclosed \"{\"");
  parse_fails ("x text:\nA\n", "unterminated multi-line string");
  parse_fails ("x text: junk\nA\n.\n;", "expected the end of the line after \"text:\"");
  parse_fails ("keep;\nx \"\xff\";", "line 2, column 4: invalid UTF-8");
}

/* ---- Robustness ---------------------------------------------------------- */

static gchar *
nested (const gchar *open, const gchar *middle, const gchar *close, guint n)
{
  GString *s = g_string_new (NULL);

  for (guint i = 0; i < n; i++)
    g_string_append (s, open);
  g_string_append (s, middle);
  for (guint i = 0; i < n; i++)
    g_string_append (s, close);
  return g_string_free (s, FALSE);
}

static void
test_depth_limit (void)
{
  /* "if" + N "not" + "true": N + 1 nested tests. */
  g_autofree gchar *ok_tests = nested ("not ", "true", "", SIEVE_AST_MAX_DEPTH - 1);
  g_autofree gchar *ok_script = g_strconcat ("if ", ok_tests, " { keep; }", NULL);
  g_autofree gchar *deep_tests = nested ("not ", "true", "", SIEVE_AST_MAX_DEPTH);
  g_autofree gchar *deep_script = g_strconcat ("if ", deep_tests, " { keep; }", NULL);
  g_autofree gchar *hostile_tests = nested ("not ", "true", "", 100000);
  g_autofree gchar *hostile_script = g_strconcat ("if ", hostile_tests, " { keep; }\nstop;", NULL);
  g_autofree gchar *hostile_blocks = nested ("if true { ", "keep;", " }", 100000);
  g_autofree gchar *hostile_lists = nested ("allof (", "true", ")", 100000);
  g_autofree gchar *lists_script = g_strconcat ("if ", hostile_lists, " { }", NULL);
  g_autoptr (SieveAst) a = parse_ok (ok_script);
  g_autoptr (SieveAst) b = parse_ok (deep_script);
  g_autoptr (SieveAst) c = parse_ok (hostile_script);
  g_autoptr (SieveAst) d = parse_ok (hostile_blocks);
  g_autoptr (SieveAst) e = parse_ok (lists_script);

  g_assert_cmpstr (cmd_at (a, 0)->name, ==, "if");
  assert_error_node (cmd_at (b, 0), "nesting deeper than");
  g_assert_cmpuint (c->commands->len, ==, 2);
  assert_error_node (cmd_at (c, 0), "nesting deeper than");
  g_assert_cmpstr (cmd_at (c, 1)->name, ==, "stop");
  g_assert_cmpuint (d->commands->len, ==, 1);
  assert_error_node (cmd_at (d, 0), "nesting deeper than");
  g_assert_cmpuint (e->commands->len, ==, 1);
  assert_error_node (cmd_at (e, 0), "nesting deeper than");
}

static void
check_spans (const SieveAst *ast)
{
  gsize prev_end = 0;
  gsize len = strlen (ast->source);

  for (guint i = 0; i < ast->commands->len; i++) {
    SieveAstCommand *cmd = cmd_at (ast, i);

    g_assert_cmpuint (cmd->trivia_start, ==, prev_end);
    g_assert_cmpuint (cmd->trivia_start, <=, cmd->start);
    g_assert_cmpuint (cmd->start, <, cmd->end);
    g_assert_cmpuint (cmd->end, <=, len);
    g_assert_true ((cmd->name == NULL) == (cmd->error != NULL));
    prev_end = cmd->end;
  }
}

static void
test_truncations (void)
{
  const gchar *script =
    "require [\"fileinto\", \"vacation\", \"imap4flags\"];\n"
    "# rule:[Été]\n"
    "if anyof (header :contains \"subject\" \"[SPAM]\", size :over 10M) {\n"
    "  fileinto \"Junk\"; addflag \"\\\\Seen\"; stop;\n"
    "}\n"
    "/* block comment */\n"
    "vacation :days 3 text:\n"
    "Back soon. }\"\n"
    "..\n"
    ".\n"
    ";\n";
  gsize len = strlen (script);

  /* Every prefix either parses (with consistent spans) or fails cleanly
   * with a syntax error: never a crash, never an out-of-bounds span. */
  for (gsize n = 0; n <= len; n++) {
    g_autofree gchar *prefix = g_strndup (script, n);
    g_autoptr (GError) error = NULL;
    g_autoptr (SieveAst) ast = sieve_ast_parse (prefix, &error);

    if (ast == NULL)
      g_assert_error (error, SIEVE_MODEL_ERROR, SIEVE_MODEL_ERROR_SYNTAX);
    else
      check_spans (ast);
  }

  {
    g_autoptr (SieveAst) full = parse_ok (script);

    check_spans (full);
    g_assert_cmpuint (full->commands->len, ==, 3);
    for (guint i = 0; i < full->commands->len; i++)
      g_assert_nonnull (cmd_at (full, i)->name);
  }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/sieve-ast/empty", test_empty);
  g_test_add_func ("/sieve-ast/require", test_require);
  g_test_add_func ("/sieve-ast/numbers", test_numbers);
  g_test_add_func ("/sieve-ast/number-errors", test_number_errors);
  g_test_add_func ("/sieve-ast/quoted-strings", test_quoted_strings);
  g_test_add_func ("/sieve-ast/multiline-lf", test_multiline_lf);
  g_test_add_func ("/sieve-ast/multiline-crlf-and-comment", test_multiline_crlf_and_comment);
  g_test_add_func ("/sieve-ast/nested-tests", test_nested_tests);
  g_test_add_func ("/sieve-ast/case-insensitive", test_case_insensitive);
  g_test_add_func ("/sieve-ast/offsets", test_offsets);
  g_test_add_func ("/sieve-ast/multiline-offsets", test_multiline_offsets);
  g_test_add_func ("/sieve-ast/recovery-block", test_recovery_block);
  g_test_add_func ("/sieve-ast/recovery-simple", test_recovery_simple);
  g_test_add_func ("/sieve-ast/recovery-bad-tokens", test_recovery_bad_tokens);
  g_test_add_func ("/sieve-ast/recovery-nested-block", test_recovery_nested_block);
  g_test_add_func ("/sieve-ast/fatal-errors", test_fatal_errors);
  g_test_add_func ("/sieve-ast/depth-limit", test_depth_limit);
  g_test_add_func ("/sieve-ast/truncations", test_truncations);

  return g_test_run ();
}
