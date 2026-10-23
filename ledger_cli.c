#include "ledger.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
static const char *cmd_usage(const char *name);  /* from the s_cmds table */

static int unknown_option(const char *cmd, const char *opt) {
    fprintf(stderr, "ledger %s: unknown option, or missing value: '%s'\n",
            cmd, opt);
    fprintf(stderr, "Usage: %s\n", cmd_usage(cmd));
    return 1;
}

static int fail(const char *what) {
    fprintf(stderr, "%s: %s\n", what, ledger_last_error());
    ledger_close();
    return 1;
}
typedef struct {
    const char *db;
    const char *company;
} CompanyArgs;

static int company_option(int argc, char **argv, int *i, CompanyArgs *a) {
    if (*i + 1 >= argc) return 0;
    const char **slot = strcmp(argv[*i], "--db") == 0      ? &a->db
                      : strcmp(argv[*i], "--company") == 0 ? &a->company
                      : NULL;
    if (!slot) return 0;
    *slot = argv[++*i];
    return 1;
}

/* Opens an existing database.  ledger_open() creates a missing file,
   which is right for 'ledger init' and wrong everywhere else: a mistyped
   path would silently start empty books. */
static int open_existing(const char *db) {
    FILE *f = fopen(db, "rb");
    if (!f) {
        fprintf(stderr, "ledger: no database at '%s'"
                        " (create one with 'ledger init')\n", db);
        return 1;
    }
    fclose(f);
    if (ledger_open(db) != LEDGER_OK) return fail("ledger_open");
    return 0;
}

static int open_company(const CompanyArgs *a, int have_required,
                        const char *cmd) {
    if (!a->db || !a->company || !have_required) {
        fprintf(stderr, "Usage: %s\n", cmd_usage(cmd));
        return 1;
    }
    if (open_existing(a->db)) return 1;
    if (ledger_init(a->company) != LEDGER_OK) return fail("ledger_init");
    return 0;
}
/* A whole number > 0 written in full ("12", not "12x"), or 0 if not. */
static int64_t positive_int(const char *text) {
    char *end;
    long long v = strtoll(text, &end, 10);
    return (*text && !*end && v > 0) ? v : 0;
}
static char g_json_error[512];

#define JSON_ERROR(...) \
    (snprintf(g_json_error, sizeof g_json_error, __VA_ARGS__), g_json_error)

static void json_get_string(const cJSON *obj, const char *key,
                            char *buf, size_t size) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v)) snprintf(buf, size, "%s", v->valuestring);
}
/* A JSON number is a double: it holds every integer below 2^53 exactly.
   2^53 + 1 already rounds to 2^53, so 2^53 itself is suspect too: accept
   only whole numbers strictly below it. */
#define JSON_MAX_EXACT 9007199254740992.0   /* 2^53 */

static int json_whole_number(const cJSON *v, int64_t *out) {
    if (!cJSON_IsNumber(v) || v->valuedouble != floor(v->valuedouble) ||
        fabs(v->valuedouble) >= JSON_MAX_EXACT)
        return 0;
    *out = (int64_t)v->valuedouble;
    return 1;
}

static const char *posting_amount(const cJSON *line, LedgerPosting *p) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(line, "amount");
    if (cJSON_IsNumber(v)) {
        if (!json_whole_number(v, &p->amount))
            return JSON_ERROR("amount %g: write it as a decimal string,"
                              " e.g. \"12.50\"", v->valuedouble);
        return NULL;
    }
    if (!cJSON_IsString(v)) return JSON_ERROR("posting has no amount");

    LedgerAccount a;
    LedgerError err = p->account_id
        ? ledger_get_account_by_id(p->account_id, &a)
        : ledger_get_account_by_code(p->account_code, &a);
    if (err == LEDGER_OK)
        err = ledger_parse_amount(v->valuestring, a.decimal_places,
                                  &p->amount);
    return err == LEDGER_OK ? NULL : JSON_ERROR("%s", ledger_last_error());
}
static const char *postings_from_json(const cJSON *arr, const char *side,
                                      LedgerPosting **out, int *n) {
    *out = NULL; *n = 0;
    if (!cJSON_IsArray(arr))
        return JSON_ERROR("\"%s\" must be an array", side);

    int count = cJSON_GetArraySize(arr);
    LedgerPosting *p = calloc((size_t)(count ? count : 1), sizeof *p);
    if (!p) return JSON_ERROR("out of memory");
    *out = p; *n = count;

    for (int i = 0; i < count; i++) {
        const cJSON *line = cJSON_GetArrayItem(arr, i);
        const cJSON *id =
            cJSON_GetObjectItemCaseSensitive(line, "account_id");
        if (id && !json_whole_number(id, &p[i].account_id))
            return JSON_ERROR("%s line %d: account_id must be an integer",
                              side, i + 1);
        json_get_string(line, "account_code", p[i].account_code,
                        sizeof p[i].account_code);
        json_get_string(line, "currency", p[i].currency,
                        sizeof p[i].currency);
        json_get_string(line, "info", p[i].info, sizeof p[i].info);
        const char *msg = posting_amount(line, &p[i]);
        if (msg) {
            /* msg points into g_json_error: copy it before reusing it */
            char inner[sizeof g_json_error];
            snprintf(inner, sizeof inner, "%s", msg);
            return JSON_ERROR("%s line %d: %s", side, i + 1, inner);
        }
    }
    return NULL;
}
static int all_digits(const char *s, int n) {
    for (int i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

static int two_digits(const char *s) {
    return (s[0] - '0') * 10 + s[1] - '0';
}

/* An ISO-8601 date, "YYYY-MM-DD", optionally followed by a time
   "THH:MM[:SS]" and a zone "Z" or "+HH:MM".  Only such dates compare
   correctly as strings, which the taccount date filter relies on. */
static int valid_date(const char *d) {
    static const int days[] = {31,29,31,30,31,30,31,31,30,31,30,31};
    if (!all_digits(d, 4) || d[4] != '-' || !all_digits(d + 5, 2) ||
        d[7] != '-' || !all_digits(d + 8, 2))
        return 0;
    int y = atoi(d), m = two_digits(d + 5), day = two_digits(d + 8);
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (m < 1 || m > 12 || day < 1 || day > days[m - 1] ||
        (m == 2 && day == 29 && !leap))
        return 0;

    const char *t = d + 10;
    if (*t == '\0') return 1;
    if (*t != 'T' && *t != ' ') return 0;
    t++;
    if (!all_digits(t, 2) || t[2] != ':' || !all_digits(t + 3, 2) ||
        two_digits(t) > 23 || two_digits(t + 3) > 59)
        return 0;
    t += 5;
    if (*t == ':') {
        if (!all_digits(t + 1, 2) || two_digits(t + 1) > 59) return 0;
        t += 3;
    }
    if (*t == 'Z') t++;
    else if (*t == '+' || *t == '-') {
        if (!all_digits(t + 1, 2) || t[3] != ':' || !all_digits(t + 4, 2))
            return 0;
        t += 6;
    }
    return *t == '\0';
}

static void entry_free(LedgerEntry *e) {
    free(e->debits);
    free(e->credits);
    free(e->prop_keys);
    free(e->prop_vals);
    memset(e, 0, sizeof *e);
}
static const char *entry_from_json(const cJSON *root, LedgerEntry *e) {
    memset(e, 0, sizeof *e);
    if (!cJSON_IsObject(root)) return JSON_ERROR("entry must be an object");

    json_get_string(root, "date",    e->date,    sizeof e->date);
    json_get_string(root, "system",  e->system,  sizeof e->system);
    json_get_string(root, "end2end", e->end2end, sizeof e->end2end);
    json_get_string(root, "info",    e->info,    sizeof e->info);
    json_get_string(root, "description",
                    e->description, sizeof e->description);

    const char *msg = NULL;
    if (!valid_date(e->date))
        msg = JSON_ERROR("\"date\" must be YYYY-MM-DD, optionally with a time"
                     " such as T09:00:00Z");
    else if (!e->description[0])
        msg = JSON_ERROR("\"description\" is required");
    if (msg) return msg;
    msg = postings_from_json(
        cJSON_GetObjectItemCaseSensitive(root, "debits"), "debits",
        &e->debits, &e->n_debits);
    if (!msg) msg = postings_from_json(
        cJSON_GetObjectItemCaseSensitive(root, "credits"), "credits",
        &e->credits, &e->n_credits);
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(root, "properties");
    if (!msg && props && !cJSON_IsObject(props))
        msg = JSON_ERROR("\"properties\" must be an object of strings");
    if (!msg && cJSON_IsObject(props)) {
        int n = cJSON_GetArraySize(props);
        e->prop_keys = calloc((size_t)(n ? n : 1), sizeof(char *));
        e->prop_vals = calloc((size_t)(n ? n : 1), sizeof(char *));
        if (!e->prop_keys || !e->prop_vals) msg = JSON_ERROR("out of memory");
        const cJSON *p;
        cJSON_ArrayForEach(p, props) {
            if (msg) break;
            if (!cJSON_IsString(p)) {
                msg = JSON_ERROR("property \"%s\" must be a string",
                                 p->string);
                break;
            }
            for (int k = 0; k < e->n_props; k++)
                if (strcmp(e->prop_keys[k], p->string) == 0)
                    msg = JSON_ERROR("property \"%s\" appears twice",
                                     p->string);
            e->prop_keys[e->n_props] = p->string;
            e->prop_vals[e->n_props] = p->valuestring;
            e->n_props++;
        }
    }
    if (msg) entry_free(e);
    return msg;
}
static void print_json(const cJSON *value) {
    char *pretty = cJSON_Print(value);
    for (const char *c = pretty; c && *c; c++) {
        if (*c != '\t')                     putchar(*c);
        else if (c > pretty && c[-1] == ':') putchar(' ');
        else                                fputs("  ", stdout);
    }
    putchar('\n');
    free(pretty);
}
static cJSON *journal_to_json(int64_t journal_id) {
    LedgerJournalHeader h;
    if (ledger_get_journal(journal_id, &h) != LEDGER_OK) return NULL;

    LedgerJournalLine *lines = NULL; int n_lines = 0;
    char **keys = NULL, **vals = NULL; int n_props = 0;
    if (ledger_get_journal_lines(journal_id, &lines, &n_lines) != LEDGER_OK ||
        ledger_get_journal_props(journal_id, &keys, &vals, &n_props)
            != LEDGER_OK) {
        ledger_journal_lines_free(lines, n_lines);
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", (double)h.id);
    cJSON_AddStringToObject(root, "date", h.date);
    cJSON_AddStringToObject(root, "description", h.description);
    cJSON_AddStringToObject(root, "system", h.system);
    cJSON_AddStringToObject(root, "end2end", h.end2end);
    if (h.info[0]) cJSON_AddStringToObject(root, "info", h.info);
    cJSON *debits  = cJSON_AddArrayToObject(root, "debits");
    cJSON *credits = cJSON_AddArrayToObject(root, "credits");
    for (int i = 0; i < n_lines; i++) {
        const LedgerJournalLine *l = &lines[i];
        char amount[32];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "account_code", l->account_code);
        cJSON_AddStringToObject(j, "account_name", l->account_name);
        cJSON_AddStringToObject(j, "amount", ledger_format_amount(
            l->amount, l->decimal_places, amount, sizeof amount));
        cJSON_AddStringToObject(j, "currency", l->currency);
        if (l->info[0]) cJSON_AddStringToObject(j, "info", l->info);
        cJSON_AddItemToArray(l->side == 'D' ? debits : credits, j);
    }
    if (n_props > 0) {
        cJSON *props = cJSON_AddObjectToObject(root, "properties");
        for (int i = 0; i < n_props; i++)
            cJSON_AddStringToObject(props, keys[i], vals[i]);
    }

    ledger_journal_lines_free(lines, n_lines);
    ledger_journal_props_free(keys, vals, n_props);
    return root;
}
static int cmd_init(int argc, char **argv) {
    const char *db = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) db = argv[++i];
        else return unknown_option("init", argv[i]);
    }
    if (!db) { fprintf(stderr, "Usage: %s\n", cmd_usage("init")); return 1; }
    if (ledger_open(db) != LEDGER_OK) return fail("ledger_open");
    printf("Database initialized: %s\n", db);
    ledger_close();
    return 0;
}
static int cmd_company(int argc, char **argv) {
    const char *db = NULL, *nick = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) db = argv[++i];
        else if (argv[i][0] != '-' && !nick) nick = argv[i];
        else return unknown_option("company", argv[i]);
    }
    if (!db || !nick) {
        fprintf(stderr, "Usage: %s\n", cmd_usage("company"));
        return 1;
    }
    if (open_existing(db)) return 1;
    if (ledger_add_company(nick) != LEDGER_OK) return fail("company");
    printf("Company '%s' added.\n", nick);
    ledger_close();
    return 0;
}
static int cmd_currency(int argc, char **argv) {
    const char *db = NULL, *code = NULL, *dp = NULL, *price = NULL;
    for (int i = 2; i < argc; i++) {
        const char **slot = strcmp(argv[i], "--db") == 0    ? &db
                          : strcmp(argv[i], "--code") == 0  ? &code
                          : strcmp(argv[i], "--dp") == 0    ? &dp
                          : strcmp(argv[i], "--price") == 0 ? &price : NULL;
        if (!slot || i + 1 >= argc)
            return unknown_option("currency", argv[i]);
        *slot = argv[++i];
    }
    if (!db || !code || (!dp && !price)) {
        fprintf(stderr, "Usage: %s\n", cmd_usage("currency"));
        return 1;
    }
    size_t len = strlen(code);
    if (len == 0 || len > 15 || strspn(code, "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                             "0123456789") != len) {
        fprintf(stderr, "currency: --code must be 1 to 15 capital letters"
                        " or digits, such as BTC\n");
        return 1;
    }
    char *end;
    long places = dp ? strtol(dp, &end, 10) : 0;
    if (dp && (end == dp || *end || places < 0 || places > 18)) {
        fprintf(stderr, "currency: --dp must be 0 to 18\n");
        return 1;
    }
    double usd = price ? strtod(price, &end) : 0;
    if (price && (end == price || *end || !(usd >= 0) || !isfinite(usd))) {
        fprintf(stderr, "currency: --price must be a number >= 0\n");
        return 1;
    }

    if (open_existing(db)) return 1;
    if (dp && ledger_set_currency_decimal_places(code, (int)places)
                  != LEDGER_OK)
        return fail("currency");
    if (price && ledger_set_currency_price(code, usd) != LEDGER_OK)
        return fail("currency");
    printf("Currency %s updated.\n", code);
    ledger_close();
    return 0;
}
static int cmd_chart(int argc, char **argv) {
    CompanyArgs a = {0};
    for (int i = 2; i < argc; i++)
        if (!company_option(argc, argv, &i, &a))
            return unknown_option("chart", argv[i]);
    if (open_company(&a, 1, "chart"))
        return 1;

    int created = 0;
    if (ledger_create_minimal_chart(&created) != LEDGER_OK)
        return fail("chart");
    printf("Chart of accounts created: %d accounts.\n", created);
    ledger_close();
    return 0;
}
static int cmd_account(int argc, char **argv) {
    CompanyArgs a = {0};
    const char *code = NULL, *name = NULL, *currency = NULL, *nature = NULL;
    for (int i = 2; i < argc; i++) {
        if (company_option(argc, argv, &i, &a)) continue;
        const char **slot = strcmp(argv[i], "--code") == 0     ? &code
                          : strcmp(argv[i], "--name") == 0     ? &name
                          : strcmp(argv[i], "--currency") == 0 ? &currency
                          : strcmp(argv[i], "--nature") == 0   ? &nature
                          : NULL;
        if (!slot || i + 1 >= argc) return unknown_option("account", argv[i]);
        *slot = argv[++i];
    }
    if (open_company(&a, code && name && currency && nature, "account"))
        return 1;
    LedgerNature n = ledger_nature_parse(nature);
    if (n == LEDGER_NATURE_INVALID) {
        fprintf(stderr, "account: --nature must be DEBITNORMAL"
                        " or CREDITNORMAL\n");
        ledger_close();
        return 1;
    }
    if (ledger_add_account(name, code, currency, n, NULL) != LEDGER_OK)
        return fail("account");
    printf("Account %s %s added.\n", code, name);
    ledger_close();
    return 0;
}
/* Number of dots in a code: "1" is depth 0, "1.1.2" is depth 2. */
static int code_depth(const char *code) {
    int depth = 0;
    for (const char *p = code; *p; p++) depth += (*p == '.');
    return depth;
}

/* 1 if code lies strictly below ancestor in the tree ("1.1.2" below "1"). */
static int is_below(const char *code, const char *ancestor) {
    size_t n = strlen(ancestor);
    return strncmp(code, ancestor, n) == 0 && code[n] == '.';
}

/* 1 if code is ancestor itself or lies below it; any code if ancestor is
   NULL.  Used by the --under option of balance and the MCP tools. */
static int in_subtree(const char *code, const char *ancestor) {
    return !ancestor || strcmp(code, ancestor) == 0
                     || is_below(code, ancestor);
}
typedef struct { int leaf; int64_t balance; } BalanceRow;

/* Total of the leaves below accounts[i]; returns 0 if there are none or
   one of them is in another currency than the branch's own. */
static int subtree_total(const LedgerAccount *acc, const BalanceRow *row,
                         int n, int i, int64_t *total) {
    int leaves = 0;
    *total = 0;
    for (int j = i + 1; j < n && is_below(acc[j].code, acc[i].code); j++) {
        if (!row[j].leaf) continue;
        if (strcmp(acc[i].currency, acc[j].currency) != 0) return 0;
        leaves++;
        *total += acc[j].nature == acc[i].nature ? row[j].balance
                                                 : -row[j].balance;
    }
    return leaves > 0;
}
static void print_trial_balance_check(const LedgerAccount *acc,
                                      const BalanceRow *row, int n) {
    for (int i = 0; i < n; i++) {
        if (!row[i].leaf) continue;
        int seen = 0;   /* print each currency once, at its first leaf */
        for (int j = 0; j < i && !seen; j++)
            seen = row[j].leaf &&
                   strcmp(acc[j].currency, acc[i].currency) == 0;
        if (seen) continue;

        int64_t debit_side = 0, credit_side = 0;
        for (int j = i; j < n; j++) {
            if (!row[j].leaf || strcmp(acc[j].currency, acc[i].currency))
                continue;
            if (acc[j].nature == LEDGER_DEBITNORMAL)
                debit_side += row[j].balance;
            else
                credit_side += row[j].balance;
        }
        char d[32], c[32];
        int dp = acc[i].decimal_places;
        printf("Trial balance %s: debit-normal %s, credit-normal %s  %s\n",
               acc[i].currency,
               ledger_format_amount(debit_side, dp, d, sizeof d),
               ledger_format_amount(credit_side, dp, c, sizeof c),
               debit_side == credit_side ? "OK" : "UNBALANCED");
    }
}
static int cmd_balance(int argc, char **argv) {
    CompanyArgs a = {0};
    int level = 0, show_id = 0;
    const char *under = NULL;
    for (int i = 2; i < argc; i++) {
        if (company_option(argc, argv, &i, &a)) continue;
        if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
            level = (int)positive_int(argv[++i]);
            if (level <= 0) return unknown_option("balance", argv[i]);
        }
        else if (strcmp(argv[i], "--under") == 0 && i + 1 < argc)
            under = argv[++i];
        else if (strcmp(argv[i], "--id") == 0) show_id = 1;
        else return unknown_option("balance", argv[i]);
    }
    if (open_company(&a, 1, "balance"))
        return 1;

    LedgerAccount *acc = NULL;
    int n = 0;
    if (ledger_get_all_accounts(&acc, &n) != LEDGER_OK)
        return fail("balance");
    int under_found = !under;
    for (int i = 0; i < n && !under_found; i++)
        under_found = strcmp(acc[i].code, under) == 0;
    if (!under_found) {
        fprintf(stderr, "balance: no account with code '%s'\n", under);
        ledger_accounts_free(acc, n);
        ledger_close();
        return 1;
    }
    BalanceRow *row = calloc((size_t)(n ? n : 1), sizeof *row);
    if (!row) { ledger_accounts_free(acc, n); return fail("balance"); }
    for (int i = 0; i < n; i++) {
        row[i].leaf = ledger_has_children(acc[i].id) == 0;
        if (row[i].leaf) ledger_get_balance(acc[i].id, &row[i].balance);
    }
    for (int i = 0; i < n; i++) {
        int depth = code_depth(acc[i].code);
        if (level > 0 && depth + 1 > level) continue;
        if (!in_subtree(acc[i].code, under)) continue;

        char col1[32], amount[32] = "";
        if (show_id)
            snprintf(col1, sizeof col1, "%lld", (long long)acc[i].id);
        else
            snprintf(col1, sizeof col1, "%s", acc[i].code);

        int64_t total = row[i].balance;
        if (row[i].leaf || subtree_total(acc, row, n, i, &total))
            ledger_format_amount(total, acc[i].decimal_places,
                                 amount, sizeof amount);
        printf("%-15s  %*s%-*s  %14s %s\n", col1, depth * 2, "",
               40 - depth * 2, acc[i].name, amount,
               amount[0] ? acc[i].currency : "");
    }
    if (!under) print_trial_balance_check(acc, row, n);
    free(row);
    ledger_accounts_free(acc, n);
    ledger_close();
    return 0;
}
static int cmd_taccount(int argc, char **argv) {
    CompanyArgs a = {0};
    int64_t idc = 0;
    const char *code = NULL, *dtini = NULL, *dtend = NULL;
    for (int i = 2; i < argc; i++) {
        if (company_option(argc, argv, &i, &a)) continue;
        if (strcmp(argv[i], "--idc") == 0 && i + 1 < argc) {
            if (!(idc = positive_int(argv[++i])))
                return unknown_option("taccount", argv[i]);
        }
        else if (strcmp(argv[i], "--code") == 0 && i + 1 < argc)
            code = argv[++i];
        else if (strcmp(argv[i], "--dtini") == 0 && i + 1 < argc)
            dtini = argv[++i];
        else if (strcmp(argv[i], "--dtend") == 0 && i + 1 < argc)
            dtend = argv[++i];
        else return unknown_option("taccount", argv[i]);
    }
    if (open_company(&a, idc != 0 || code != NULL, "taccount"))
        return 1;
    LedgerAccount acct;
    LedgerError err = code ? ledger_get_account_by_code(code, &acct)
                           : ledger_get_account_by_id(idc, &acct);
    if (err != LEDGER_OK) return fail("taccount");
    idc = acct.id;
    LedgerTAccountLine *lines = NULL;
    int n = 0;
    if (ledger_get_taccount_lines(idc, &lines, &n) != LEDGER_OK)
        return fail("taccount");

    printf("T-Account: %s  %s  (%s)\n", acct.code, acct.name, acct.currency);
    printf("%-4s %-10s %-26s %10s %10s %10s\n",
           "JID", "Date", "Description", "Debit", "Credit", "Balance");
    int dp = acct.decimal_places;
    for (int i = 0; i < n; i++) {
        const LedgerTAccountLine *l = &lines[i];
        if (dtini && strcmp(l->date, dtini) < 0) continue;
        if (dtend && strncmp(l->date, dtend, strlen(dtend)) > 0) continue;

        char debit[32] = "", credit[32] = "", balance[32];
        if (l->debit)
            ledger_format_amount(l->debit, dp, debit, sizeof debit);
        if (l->credit)
            ledger_format_amount(l->credit, dp, credit, sizeof credit);
        ledger_format_amount(l->balance, dp, balance, sizeof balance);
        printf("%-4lld %-10.10s %-26.26s %10s %10s %10s\n",
               (long long)l->journal_id, l->date, l->description,
               debit, credit, balance);
    }
    ledger_taccount_lines_free(lines, n);
    ledger_close();
    return 0;
}
/* Reads a whole file into a string; "-" means standard input. */
static char *read_file(const char *path) {
    FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    size_t len = 0, cap = 0, got;
    do {
        if (len + 4096 + 1 > cap) {
            cap = cap ? cap * 2 : 8192;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); fclose(f); return NULL; }
            buf = tmp;
        }
        got = fread(buf + len, 1, 4096, f);
        len += got;
    } while (got > 0);
    if (f != stdin) fclose(f);
    buf[len] = '\0';
    return buf;
}
static int cmd_post(int argc, char **argv) {
    CompanyArgs a = {0};
    const char *file = NULL;
    for (int i = 2; i < argc; i++) {
        if (company_option(argc, argv, &i, &a)) continue;
        if (!file && (argv[i][0] != '-' || strcmp(argv[i], "-") == 0))
            file = argv[i];
        else return unknown_option("post", argv[i]);
    }
    if (open_company(&a, file != NULL, "post"))
        return 1;

    char *text = read_file(file);
    cJSON *root = text ? cJSON_ParseWithOpts(text, NULL, 1) : NULL;
    free(text);
    if (!root) {
        fprintf(stderr, "post: cannot read JSON from %s\n", file);
        ledger_close();
        return 1;
    }

    LedgerEntry entry;
    int64_t id = 0;
    const char *msg = entry_from_json(root, &entry);
    LedgerError err = msg ? LEDGER_ERR_ARGS
                          : ledger_post_journal(&entry, &id);
    if (!msg) entry_free(&entry);
    cJSON_Delete(root);

    if (msg) { fprintf(stderr, "post: %s\n", msg); ledger_close(); return 1; }
    if (err != LEDGER_OK) return fail("post");
    printf("Posted journal entry %lld.\n", (long long)id);
    ledger_close();
    return 0;
}
static int cmd_journal(int argc, char **argv) {
    CompanyArgs a = {0};
    int64_t jid = 0;
    for (int i = 2; i < argc; i++) {
        if (company_option(argc, argv, &i, &a)) continue;
        if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
            if (!(jid = positive_int(argv[++i])))
                return unknown_option("journal", argv[i]);
        }
        else return unknown_option("journal", argv[i]);
    }
    if (open_company(&a, jid != 0, "journal"))
        return 1;

    cJSON *json = journal_to_json(jid);
    if (!json) return fail("journal");
    print_json(json);
    cJSON_Delete(json);
    ledger_close();
    return 0;
}
static void print_inconsistency(const LedgerInconsistency *inc) {
    char x[32], y[32];
    int dp = inc->decimal_places;
    switch (inc->type) {
    case LEDGER_INC_UNBALANCED:
        printf("  entry %lld, %s: debits %s, credits %s\n",
               (long long)inc->journal_id, inc->currency,
               ledger_format_amount(inc->debits, dp, x, sizeof x),
               ledger_format_amount(inc->credits, dp, y, sizeof y));
        break;
    case LEDGER_INC_CACHE:
        printf("  %-12s %s: cached %s, calculated %s\n",
               inc->account_code, inc->account_name,
               ledger_format_amount(inc->cached, dp, x, sizeof x),
               ledger_format_amount(inc->calculated, dp, y, sizeof y));
        break;
    default:
        printf("  %-12s %s\n", inc->account_code, inc->account_name);
    }
}
typedef LedgerError (*CheckFn)(LedgerInconsistency **, int *);

static int g_check_fix = 0;

static LedgerError check_leaf_balances(LedgerInconsistency **out, int *n) {
    return ledger_check_leaf_balances(g_check_fix, out, n);
}

/* Runs one check and prints its findings.
   Returns 1 if problems were found, 0 if none, -1 on error. */
static int report_check(const char *title, CheckFn check) {
    LedgerInconsistency *inc = NULL;
    int n = 0;
    printf("Checking %s...\n", title);
    if (check(&inc, &n) != LEDGER_OK) {
        fprintf(stderr, "  Error: %s\n", ledger_last_error());
        return -1;
    }
    if (n == 0) printf("  OK\n");
    int fixing = check == check_leaf_balances && g_check_fix;
    for (int i = 0; i < n; i++) {
        print_inconsistency(&inc[i]);
        if (fixing && inc[i].calculated >= 0)
            printf("    corrected\n");
        else if (fixing)    /* the library leaves negatives alone */
            printf("    left alone: negative, find the altered postings\n");
    }
    ledger_inconsistencies_free(inc, n);
    return n > 0 ? 1 : 0;
}
static int cmd_check(int argc, char **argv) {
    CompanyArgs a = {0};
    for (int i = 2; i < argc; i++) {
        if (company_option(argc, argv, &i, &a)) continue;
        if (strcmp(argv[i], "--fixbal") == 0) g_check_fix = 1;
        else return unknown_option("check", argv[i]);
    }
    if (open_company(&a, 1, "check"))
        return 1;

    int r[4];
    r[0] = report_check("balanced entries", ledger_check_journal_balance);
    r[1] = report_check("cached leaf balances", check_leaf_balances);
    r[2] = report_check("postings to branch accounts",
                        ledger_check_non_leaf_entries);
    r[3] = report_check("account codes", ledger_check_code_prefixes);
    ledger_close();
    for (int i = 0; i < 4; i++)
        if (r[i] != 0) return 1;   /* problems found, or a check failed */
    return 0;
}
typedef struct {
    LedgerAccount acct;
    int           depth;
    int64_t       usd_cents;
    int           unpriced;   /* leaf in a currency with no price */
} MtmLine;

typedef struct {
    MtmLine *lines;
    int      n, cap;
} MtmReport;

static int mtm_add_line(MtmReport *r, const LedgerAccount *a, int depth) {
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 64;
        MtmLine *tmp = realloc(r->lines, (size_t)cap * sizeof(MtmLine));
        if (!tmp) return -1;
        r->lines = tmp; r->cap = cap;
    }
    MtmLine *l = &r->lines[r->n];
    memset(l, 0, sizeof(*l));
    l->acct  = *a;
    l->depth = depth;
    return r->n++;
}
static int64_t leaf_usd_value(const LedgerAccount *a, int *unpriced) {
    int64_t raw = 0;
    ledger_get_balance(a->id, &raw);
    if (strcmp(a->currency, "USD") == 0) return raw;

    double price = 0.0;
    if (ledger_get_currency_price(a->currency, &price) != LEDGER_OK
        || price <= 0.0) {
        *unpriced = 1;
        return 0;
    }
    double units = (double)raw / pow(10.0, a->decimal_places);
    return (int64_t)round(units * price * 100.0);
}
static int64_t dfs_usd_value(MtmReport *r, const LedgerAccount *acct,
                             int depth) {
    int idx = mtm_add_line(r, acct, depth);
    if (idx < 0) return 0;

    int64_t *children = NULL;
    int n_children = 0;
    ledger_get_children(acct->id, &children, &n_children);

    int64_t usd = 0;
    int unpriced = 0;
    if (n_children == 0) {
        usd = leaf_usd_value(acct, &unpriced);
    } else {
        for (int i = 0; i < n_children; i++) {
            LedgerAccount child = {0};
            if (ledger_get_account_by_id(children[i], &child) != LEDGER_OK)
                continue;
            int child_idx = r->n;   /* where the child's line will go */
            int64_t child_usd = dfs_usd_value(r, &child, depth + 1);
            if (child.nature == acct->nature)
                usd += child_usd;
            else
                usd -= child_usd;
            if (child_idx < r->n && r->lines[child_idx].unpriced)
                unpriced = 1;   /* a missing price below: incomplete */
        }
    }
    free(children);

    r->lines[idx].usd_cents = usd;   /* r->lines may have moved: index it */
    r->lines[idx].unpriced  = unpriced;
    return usd;
}
static void mtm_build(MtmReport *r) {
    int64_t *roots = NULL;
    int n = 0;
    ledger_get_children(0, &roots, &n);
    for (int i = 0; i < n; i++) {
        LedgerAccount root;
        if (ledger_get_account_by_id(roots[i], &root) == LEDGER_OK)
            dfs_usd_value(r, &root, 0);
    }
    free(roots);
}
static void mtm_print(const MtmReport *r, int level) {
    printf("%-15s  %-34s %12s\n", "CODE", "ACCOUNT", "USD VALUE");
    for (int i = 0; i < r->n; i++) {
        const MtmLine *l = &r->lines[i];
        if (level > 0 && l->depth + 1 > level) continue;
        char amount[32];
        ledger_format_amount(l->usd_cents, 2, amount, sizeof amount);
        const char *flag = !l->unpriced ? ""
                         : ledger_has_children(l->acct.id) == 0
                           ? "  (no price)" : "  (incomplete)";
        printf("%-15s  %*s%-*.*s %12s%s\n", l->acct.code,
               l->depth * 2, "", 34 - l->depth * 2, 34 - l->depth * 2,
               l->acct.name, amount, flag);
    }
}
static int cmd_mtm(int argc, char **argv) {
    CompanyArgs a = {0};
    int level = 0;
    for (int i = 2; i < argc; i++) {
        if (company_option(argc, argv, &i, &a)) continue;
        if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
            level = (int)positive_int(argv[++i]);
            if (level <= 0) return unknown_option("mtm", argv[i]);
        }
        else return unknown_option("mtm", argv[i]);
    }
    if (open_company(&a, 1, "mtm"))
        return 1;

    MtmReport report = {0};
    mtm_build(&report);
    mtm_print(&report, level);
    free(report.lines);
    ledger_close();
    return 0;
}
static void mcp_send(cJSON *msg) {
    char *text = cJSON_PrintUnformatted(msg);
    if (text) {
        fputs(text, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        free(text);
    }
    cJSON_Delete(msg);
}

static cJSON *mcp_message(const cJSON *id) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(msg, "id", cJSON_Duplicate(id, 1));
    else    cJSON_AddNullToObject(msg, "id");
    return msg;
}

static void mcp_reply(const cJSON *id, cJSON *result) {
    cJSON *msg = mcp_message(id);
    cJSON_AddItemToObject(msg, "result", result);
    mcp_send(msg);
}

static void mcp_error(const cJSON *id, int code, const char *message) {
    cJSON *msg = mcp_message(id);
    cJSON *err = cJSON_AddObjectToObject(msg, "error");
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    mcp_send(msg);
}
typedef const char *(*McpToolFn)(const cJSON *args, cJSON **out);

static const char *arg_string(const cJSON *args, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void add_amount(cJSON *obj, const char *key, int64_t amount,
                       int decimal_places) {
    char buf[32];
    cJSON_AddStringToObject(obj, key, ledger_format_amount(
        amount, decimal_places, buf, sizeof buf));
}
static const char *tool_list_accounts(const cJSON *args, cJSON **out) {
    const cJSON *u  = cJSON_GetObjectItemCaseSensitive(args, "under");
    const cJSON *nz = cJSON_GetObjectItemCaseSensitive(args, "nonzero");
    const cJSON *lv = cJSON_GetObjectItemCaseSensitive(args, "level");
    int64_t level = 0;
    if (u && !cJSON_IsString(u))
        return "under must be an account code (a string)";
    if (nz && !cJSON_IsBool(nz)) return "nonzero must be true or false";
    if (lv && (!json_whole_number(lv, &level) || level < 1))
        return "level must be a positive integer";
    const char *under = u ? u->valuestring : NULL;
    int nonzero = cJSON_IsTrue(nz);
    LedgerAccount *acc = NULL;
    int n = 0;
    if (ledger_get_all_accounts(&acc, &n) != LEDGER_OK)
        return ledger_last_error();
    int under_found = !under;
    for (int i = 0; i < n && !under_found; i++)
        under_found = strcmp(acc[i].code, under) == 0;
    if (!under_found) {
        ledger_accounts_free(acc, n);
        return "no account with that code; call list_accounts without"
               " 'under' to see the chart";
    }
    BalanceRow *row = calloc((size_t)(n ? n : 1), sizeof *row);
    if (!row) { ledger_accounts_free(acc, n); return "out of memory"; }
    for (int i = 0; i < n; i++) {
        row[i].leaf = ledger_has_children(acc[i].id) == 0;
        if (row[i].leaf) ledger_get_balance(acc[i].id, &row[i].balance);
    }
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        if (!in_subtree(acc[i].code, under)) continue;
        if (level && code_depth(acc[i].code) + 1 > level) continue;
        int64_t total = row[i].balance;
        int known = row[i].leaf || subtree_total(acc, row, n, i, &total);
        if (nonzero && known && total == 0) continue;

        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "code", acc[i].code);
        cJSON_AddStringToObject(j, "name", acc[i].name);
        cJSON_AddStringToObject(j, "nature",
                                ledger_nature_name(acc[i].nature));
        cJSON_AddStringToObject(j, "currency", acc[i].currency);
        cJSON_AddBoolToObject(j, "leaf", row[i].leaf);
        if (known) add_amount(j, "balance", total, acc[i].decimal_places);
        cJSON_AddItemToArray(arr, j);
    }
    free(row);
    ledger_accounts_free(acc, n);
    *out = arr;
    return NULL;
}
static const char *tool_account_history(const cJSON *args, cJSON **out) {
    const char *code = arg_string(args, "account_code");
    if (!code) return "account_code is required";
    LedgerAccount acct;
    if (ledger_get_account_by_code(code, &acct) != LEDGER_OK)
        return ledger_last_error();
    if (ledger_has_children(acct.id) != 0)
        return "this account is a branch and has no postings of its own;"
               " use list_accounts with 'under' for its total, or ask for"
               " the history of one of its leaves";

    LedgerTAccountLine *lines = NULL;
    int n = 0;
    if (ledger_get_taccount_lines(acct.id, &lines, &n) != LEDGER_OK)
        return ledger_last_error();

    int dp = acct.decimal_places;
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "journal_id", (double)lines[i].journal_id);
        cJSON_AddStringToObject(j, "date", lines[i].date);
        cJSON_AddStringToObject(j, "description", lines[i].description);
        add_amount(j, "debit",   lines[i].debit,   dp);
        add_amount(j, "credit",  lines[i].credit,  dp);
        add_amount(j, "balance", lines[i].balance, dp);
        cJSON_AddItemToArray(arr, j);
    }
    ledger_taccount_lines_free(lines, n);
    *out = arr;
    return NULL;
}
static const char *tool_list_journal(const cJSON *args, cJSON **out) {
    const cJSON *lim = cJSON_GetObjectItemCaseSensitive(args, "limit");
    int64_t limit = 20;
    if (lim && (!json_whole_number(lim, &limit) || limit < 1))
        return "limit must be a positive integer";

    LedgerJournalHeader *h = NULL;
    int n = 0;
    if (ledger_get_journals(&h, &n) != LEDGER_OK)
        return ledger_last_error();

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n && i < limit; i++) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "id", (double)h[i].id);
        cJSON_AddStringToObject(j, "date", h[i].date);
        cJSON_AddStringToObject(j, "description", h[i].description);
        cJSON_AddStringToObject(j, "end2end", h[i].end2end);
        cJSON_AddItemToArray(arr, j);
    }
    ledger_journals_free(h, n);
    *out = arr;
    return NULL;
}

static const char *tool_get_journal(const cJSON *args, cJSON **out) {
    int64_t id;
    if (!json_whole_number(cJSON_GetObjectItemCaseSensitive(args, "id"), &id))
        return "id is required and must be an integer";
    *out = journal_to_json(id);
    return *out ? NULL : ledger_last_error();
}
static const char *tool_usd_valuation(const cJSON *args, cJSON **out) {
    (void)args;
    MtmReport report = {0};
    mtm_build(&report);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < report.n; i++) {
        const MtmLine *l = &report.lines[i];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "code", l->acct.code);
        cJSON_AddStringToObject(j, "name", l->acct.name);
        add_amount(j, "usd", l->usd_cents, 2);
        if (l->unpriced) cJSON_AddBoolToObject(j, "unpriced", 1);
        cJSON_AddItemToArray(arr, j);
    }
    free(report.lines);
    *out = arr;
    return NULL;
}
static const char *with_entry(const cJSON *args, int post, cJSON **out) {
    LedgerEntry e;
    const char *msg = entry_from_json(args, &e);
    if (msg) return msg;
    if (!e.system[0]) snprintf(e.system, sizeof e.system, "mcp");

    int64_t id = 0;
    LedgerError err = post ? ledger_post_journal(&e, &id)
                           : ledger_validate_journal(&e);
    entry_free(&e);
    if (err != LEDGER_OK) return ledger_last_error();

    if (!post) {
        *out = cJSON_CreateObject();
        cJSON_AddBoolToObject(*out, "valid", 1);
        return NULL;
    }
    *out = journal_to_json(id);
    return *out ? NULL : ledger_last_error();
}

static const char *tool_validate_journal(const cJSON *args, cJSON **out) {
    return with_entry(args, 0, out);
}

static const char *tool_post_journal(const cJSON *args, cJSON **out) {
    return with_entry(args, 1, out);
}
static const char *tool_add_account(const cJSON *args, cJSON **out) {
    const char *code     = arg_string(args, "code");
    const char *name     = arg_string(args, "name");
    const char *currency = arg_string(args, "currency");
    LedgerNature nature  = ledger_nature_parse(arg_string(args, "nature"));
    if (!code || !name || !currency)
        return "code, name, currency and nature are required";
    if (nature == LEDGER_NATURE_INVALID)
        return "nature must be DEBITNORMAL or CREDITNORMAL";

    LedgerAccount a;
    if (ledger_add_account(name, code, currency, nature, &a) != LEDGER_OK)
        return ledger_last_error();
    *out = cJSON_CreateObject();
    cJSON_AddStringToObject(*out, "code", a.code);
    cJSON_AddStringToObject(*out, "name", a.name);
    cJSON_AddStringToObject(*out, "nature", ledger_nature_name(a.nature));
    cJSON_AddStringToObject(*out, "currency", a.currency);
    return NULL;
}
#define MCP_POSTINGS_SCHEMA                                              \
    "{\"type\":\"array\",\"minItems\":1,\"items\":{\"type\":\"object\","   \
    "\"properties\":{"                                                   \
    "\"account_code\":{\"type\":\"string\",\"description\":"             \
        "\"Code of a leaf account, e.g. 1.1.2\"},"                       \
    "\"account_id\":{\"type\":\"integer\"},"                           \
    "\"amount\":{\"type\":\"string\",\"description\":"                   \
        "\"Positive decimal, e.g. 8000.00\"},"                           \
    "\"currency\":{\"type\":\"string\",\"description\":"                 \
        "\"Must equal the account currency, e.g. USD\"},"                \
    "\"info\":{\"type\":\"string\"}},"                                   \
    "\"required\":[\"amount\",\"currency\"]}}"

#define MCP_ENTRY_SCHEMA                                                 \
    "{\"type\":\"object\",\"properties\":{"                              \
    "\"date\":{\"type\":\"string\",\"description\":"                     \
        "\"ISO-8601 date, e.g. 2025-06-30\"},"                           \
    "\"description\":{\"type\":\"string\"},"                             \
    "\"end2end\":{\"type\":\"string\",\"description\":"                  \
        "\"External reference: invoice or bank transaction id\"},"       \
    "\"system\":{\"type\":\"string\"},\"info\":{\"type\":\"string\"},"   \
    "\"debits\":" MCP_POSTINGS_SCHEMA ","                                \
    "\"credits\":" MCP_POSTINGS_SCHEMA ","                               \
    "\"properties\":{\"type\":\"object\",\"additionalProperties\":"      \
        "{\"type\":\"string\"}}},"                                       \
    "\"required\":[\"date\",\"description\",\"debits\",\"credits\"]}"

#define MCP_NO_ARGS "{\"type\":\"object\",\"properties\":{}}"
typedef struct {
    const char *name;
    const char *description;
    const char *input_schema;
    int         read_only;
    McpToolFn   fn;
} McpTool;

static const McpTool MCP_TOOLS[] = {
    { "list_accounts",
      "Chart of accounts with codes, names, natures, currencies and"
      " balances. Call it first to choose accounts; only leaf accounts"
      " accept postings. Optional: 'under' (a code) limits it to one"
      " subtree, e.g. \"5\" for expenses; 'level' keeps only the top"
      " levels; 'nonzero' hides zero balances.",
      "{\"type\":\"object\",\"properties\":{\"under\":"
      "{\"type\":\"string\",\"description\":\"Account code\"},"
      "\"level\":{\"type\":\"integer\",\"minimum\":1},"
      "\"nonzero\":{\"type\":\"boolean\"}}}",
      1, tool_list_accounts },
    { "account_history",
      "Every posting of one leaf account, with the running balance"
      " after each. Branches have no postings; use list_accounts"
      " with 'under' for their totals.",
      "{\"type\":\"object\",\"properties\":{\"account_code\":"
      "{\"type\":\"string\"}},\"required\":[\"account_code\"]}",
      1, tool_account_history },
    { "list_journal",
      "Most recent journal entries (id, date, description, end2end),"
      " most recently posted first.",
      "{\"type\":\"object\",\"properties\":{\"limit\":"
      "{\"type\":\"integer\",\"minimum\":1}}}",
      1, tool_list_journal },
    { "get_journal",
      "One journal entry in full, in the same shape post_journal accepts.",
      "{\"type\":\"object\",\"properties\":{\"id\":"
      "{\"type\":\"integer\"}},\"required\":[\"id\"]}",
      1, tool_get_journal },
    { "usd_valuation",
      "Value of every account in USD at the stored prices (read-only"
      " mark-to-market report).",
      MCP_NO_ARGS, 1, tool_usd_valuation },
    { "validate_journal",
      "Check a journal entry without writing it: balance per currency,"
      " leaf accounts, matching currencies, no negative balances. Always"
      " call this before post_journal.",
      MCP_ENTRY_SCHEMA, 1, tool_validate_journal },
    { "post_journal",
      "Write a journal entry to the books. Only call after"
      " validate_journal succeeded and the user approved the entry.",
      MCP_ENTRY_SCHEMA, 0, tool_post_journal },
    { "add_account",
      "Create an account. Its parent is the code without the last"
      " segment (1.1.2 -> 1.1) and must exist and have no postings; a"
      " code without a dot creates a new root, which is rarely wanted."
      " A currency other than USD needs its decimal places set by the"
      " user (ledger currency --dp) before the first posting.",
      "{\"type\":\"object\",\"properties\":{"
      "\"code\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},"
      "\"currency\":{\"type\":\"string\"},"
      "\"nature\":{\"type\":\"string\","
      "\"enum\":[\"DEBITNORMAL\",\"CREDITNORMAL\"]}},"
      "\"required\":[\"code\",\"name\",\"currency\",\"nature\"]}",
      0, tool_add_account },
    { NULL, NULL, NULL, 0, NULL }
};
static const char *MCP_VERSIONS[] = {
    "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05", NULL
};

static const char *g_mcp_company = "";

static cJSON *mcp_initialize(const cJSON *params) {
    const char *asked = arg_string(params, "protocolVersion");
    const char *version = MCP_VERSIONS[0];
    for (int i = 0; asked && MCP_VERSIONS[i]; i++)
        if (strcmp(asked, MCP_VERSIONS[i]) == 0) version = asked;

    char instructions[1024];
    snprintf(instructions, sizeof instructions,
        "Double-entry ledger of company '%s'. Amounts are decimal strings"
        " in the account's currency (\"8000.00\"). Every entry needs"
        " debits equal to credits per currency, on leaf accounts only."
        " Before posting: call list_accounts to pick accounts, call"
        " validate_journal, show the entry to the user, and call"
        " post_journal only after the user agrees.", g_mcp_company);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "protocolVersion", version);
    cJSON_AddObjectToObject(cJSON_AddObjectToObject(r, "capabilities"),
                            "tools");
    cJSON *info = cJSON_AddObjectToObject(r, "serverInfo");
    cJSON_AddStringToObject(info, "name", "libledger");
    cJSON_AddStringToObject(info, "version", "1.0");
    cJSON_AddStringToObject(r, "instructions", instructions);
    return r;
}
static cJSON *mcp_tools_list(void) {
    cJSON *r = cJSON_CreateObject();
    cJSON *tools = cJSON_AddArrayToObject(r, "tools");
    for (const McpTool *t = MCP_TOOLS; t->name; t++) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", t->name);
        cJSON_AddStringToObject(j, "description", t->description);
        cJSON_AddItemToObject(j, "inputSchema",
                              cJSON_Parse(t->input_schema));
        cJSON *ann = cJSON_AddObjectToObject(j, "annotations");
        cJSON_AddBoolToObject(ann, "readOnlyHint", t->read_only);
        cJSON_AddBoolToObject(ann, "destructiveHint", 0);
        cJSON_AddBoolToObject(ann, "idempotentHint", t->read_only);
        cJSON_AddBoolToObject(ann, "openWorldHint", 0);
        cJSON_AddItemToArray(tools, j);
    }
    return r;
}
static void mcp_tools_call(const cJSON *id, const cJSON *params) {
    const char *name = arg_string(params, "name");
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    const McpTool *t = MCP_TOOLS;
    while (t->name && (!name || strcmp(t->name, name) != 0)) t++;
    if (!t->name) { mcp_error(id, -32602, "unknown tool"); return; }

    cJSON *value = NULL;
    const char *err = t->fn(args, &value);
    char *text = err ? NULL : cJSON_PrintUnformatted(value);

    cJSON *r = cJSON_CreateObject();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", err ? err : text);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(r, "content"), item);
    cJSON_AddBoolToObject(r, "isError", err != NULL);
    free(text);
    cJSON_Delete(value);
    mcp_reply(id, r);
}
static void mcp_handle(const cJSON *msg) {
    const cJSON *id      = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const cJSON *method  = cJSON_GetObjectItemCaseSensitive(msg, "method");
    const cJSON *params  = cJSON_GetObjectItemCaseSensitive(msg, "params");
    const char  *version = arg_string(msg, "jsonrpc");
    int is_response = cJSON_HasObjectItem(msg, "result") ||
                      cJSON_HasObjectItem(msg, "error");

    int64_t whole;
    int good_id = !id || cJSON_IsString(id) ||
                  json_whole_number(id, &whole);   /* string or integer */
    if (!cJSON_IsObject(msg) || !version || strcmp(version, "2.0") != 0 ||
        (method && !cJSON_IsString(method)) || (!method && !is_response) ||
        !good_id) {
        mcp_error(cJSON_IsObject(msg) && id && good_id ? id : NULL,
                  -32600, "invalid request");
        return;
    }
    if (!method || !id) return;   /* a response, or a notification */

    const char *m = method->valuestring;
    if      (strcmp(m, "initialize") == 0)
        mcp_reply(id, mcp_initialize(params));
    else if (strcmp(m, "ping") == 0)
        mcp_reply(id, cJSON_CreateObject());
    else if (strcmp(m, "tools/list") == 0)
        mcp_reply(id, mcp_tools_list());
    else if (strcmp(m, "tools/call") == 0)
        mcp_tools_call(id, params);
    else
        mcp_error(id, -32601, "method not found");
}
static int mcp_call_by_hand(const char *name, const char *args_file) {
    const McpTool *t = MCP_TOOLS;
    while (t->name && strcmp(t->name, name) != 0) t++;
    if (!t->name) {
        fprintf(stderr, "ledger mcp: unknown tool '%s'\n", name);
        ledger_close();
        return 1;
    }

    char *text = args_file ? read_file(args_file) : NULL;   /* "-": stdin */
    cJSON *args = !args_file ? cJSON_CreateObject()
                : text ? cJSON_ParseWithOpts(text, NULL, 1) : NULL;
    free(text);
    if (!args) {
        fprintf(stderr, "ledger mcp: cannot read JSON from %s\n", args_file);
        ledger_close();
        return 1;
    }

    cJSON *value = NULL;
    const char *err = t->fn(args, &value);
    if (err) {
        printf("ERROR: %s\n", err);
    } else {
        print_json(value);
    }
    cJSON_Delete(value);
    cJSON_Delete(args);
    ledger_close();
    return err ? 1 : 0;
}

static int cmd_mcp(int argc, char **argv) {
    CompanyArgs a = {0};
    const char *call = NULL, *args_file = NULL;
    for (int i = 2; i < argc; i++) {
        if (company_option(argc, argv, &i, &a)) continue;
        if (strcmp(argv[i], "--call") == 0 && i + 1 < argc) call = argv[++i];
        else if (call && (argv[i][0] != '-' || !strcmp(argv[i], "-")))
            args_file = argv[i];
        else return unknown_option("mcp", argv[i]);
    }
    if (open_company(&a, 1, "mcp"))
        return 1;
    if (call) return mcp_call_by_hand(call, args_file);
    g_mcp_company = a.company;
    fprintf(stderr, "ledger mcp: serving company '%s' on stdio\n", a.company);

    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, stdin) != -1) {
        if (strspn(line, " \t\r\n") == strlen(line)) continue;
        line[strcspn(line, "\r\n")] = '\0';
        cJSON *msg = cJSON_ParseWithOpts(line, NULL, 1);
        if (!msg) { mcp_error(NULL, -32700, "parse error"); continue; }
        mcp_handle(msg);
        cJSON_Delete(msg);
    }
    free(line);
    ledger_close();
    return 0;
}
static int cmd_help(int argc, char **argv);

typedef struct {
    const char *name;
    const char *brief;
    const char *usage;
    int (*fn)(int, char **);
} Cmd;

#define DBC "--db <path> --company <nick>"

static const Cmd s_cmds[] = {
    { "init",     "Initialize the database schema",
      "ledger init --db <path>", cmd_init },
    { "company",  "Add a company",
      "ledger company --db <path> <nickname>", cmd_company },
    { "currency", "Set a currency's decimal places or USD price",
      "ledger currency --db <path> --code <ccy> [--dp <n>] [--price <usd>]",
      cmd_currency },
    { "chart",    "Create the default chart of accounts",
      "ledger chart " DBC, cmd_chart },
    { "account",  "Add one account to the chart",
      "ledger account " DBC " --code <code> --name <name>"
      " --currency <ccy> --nature DEBITNORMAL|CREDITNORMAL", cmd_account },
    { "balance",  "Print balances and the trial balance",
      "ledger balance " DBC " [--level <n>] [--under <code>] [--id]",
      cmd_balance },
    { "taccount", "Print T-account history for one account",
      "ledger taccount " DBC " (--code <code> | --idc <id>)"
      " [--dtini <date>] [--dtend <date>]", cmd_taccount },
    { "post",     "Post a journal entry from a JSON file",
      "ledger post " DBC " <file.json | ->", cmd_post },
    { "journal",  "Print a journal entry as JSON",
      "ledger journal " DBC " --id <id>", cmd_journal },
    { "check",    "Run all consistency checks",
      "ledger check " DBC " [--fixbal]", cmd_check },
    { "mtm",      "Mark-to-market USD valuation report",
      "ledger mtm " DBC " [--level <n>]", cmd_mtm },
    { "mcp",      "Serve the books to an AI assistant (MCP)",
      "ledger mcp " DBC " [--call <tool> [args.json | -]]", cmd_mcp },
    { "help",     "Show this help",
      "ledger help [subcommand]", cmd_help },
    { NULL, NULL, NULL, NULL }
};
static const char *cmd_usage(const char *name) {
    for (int i = 0; s_cmds[i].name; i++)
        if (strcmp(s_cmds[i].name, name) == 0) return s_cmds[i].usage;
    return "ledger help";
}

static int cmd_help(int argc, char **argv) {
    if (argc >= 3) {
        for (int i = 0; s_cmds[i].name; i++) {
            if (strcmp(s_cmds[i].name, argv[2]) == 0) {
                printf("%s\n\nUsage: %s\n", s_cmds[i].brief, s_cmds[i].usage);
                return 0;
            }
        }
        fprintf(stderr, "ledger: unknown subcommand '%s'\n", argv[2]);
        return 1;
    }
    printf("Usage: ledger <subcommand> [options]\n\nSubcommands:\n");
    for (int i = 0; s_cmds[i].name; i++)
        printf("  %-12s  %s\n", s_cmds[i].name, s_cmds[i].brief);
    printf("\nRun 'ledger help <subcommand>' for its options.\n");
    return 0;
}
int main(int argc, char **argv) {
    if (argc < 2) { cmd_help(argc, argv); return 1; }
    for (int i = 0; s_cmds[i].name; i++)
        if (strcmp(s_cmds[i].name, argv[1]) == 0)
            return s_cmds[i].fn(argc, argv);
    fprintf(stderr, "ledger: unknown subcommand '%s'\n", argv[1]);
    fprintf(stderr, "Run 'ledger help' for a list of subcommands.\n");
    return 1;
}
