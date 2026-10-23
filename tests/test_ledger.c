#include "ledger.h"
#include "sqlite3.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The tests that need a database file write it to the current directory;
   make test runs this program from generated/. */

/* -------------------------------------------------------------------------
   Minimal test framework
   ---------------------------------------------------------------------- */
static int g_pass = 0;
static int g_fail = 0;

#define TEST(name) static void name(void)
#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "  FAIL %s:%d: expected %lld got %lld\n", \
                __FILE__, __LINE__, (long long)(b), (long long)(a)); \
        g_fail++; return; \
    } \
} while (0)
#define ASSERT_STR(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        fprintf(stderr, "  FAIL %s:%d: expected '%s' got '%s'\n", \
                __FILE__, __LINE__, (b), (a)); \
        g_fail++; return; \
    } \
} while (0)
#define ASSERT_OK(e) ASSERT_EQ((e), LEDGER_OK)
#define ASSERT_ERR(e, code) ASSERT_EQ((e), (code))

#define RUN(name) do { \
    int before = g_fail; \
    printf("  %-45s ", #name); fflush(stdout); \
    name(); \
    if (g_fail == before) { printf("OK\n"); g_pass++; } \
    else printf("\n"); \
} while (0)

/* -------------------------------------------------------------------------
   Each test opens its own fresh in-memory database with company "alex"
   ---------------------------------------------------------------------- */
static void setup(void) {
    ledger_close(); /* no-op if closed; cleans up after a failed test */
    ledger_open(":memory:");
    ledger_add_company("alex");
    ledger_init("alex");
}

static void teardown(void) {
    ledger_close();
}

/* Two roots with one leaf each:
     1   ASSETS       / 1.1  CHECKING   (DEBITNORMAL)
     2   LIABILITIES  / 2.1  CREDIT CARD (CREDITNORMAL) */
static void make_basic_accounts(int64_t *checking, int64_t *card) {
    LedgerAccount out = {0};
    ledger_add_account("ASSETS",      "1",   "USD", LEDGER_DEBITNORMAL,  NULL);
    ledger_add_account("LIABILITIES", "2",   "USD", LEDGER_CREDITNORMAL, NULL);
    ledger_add_account("CHECKING",    "1.1", "USD", LEDGER_DEBITNORMAL,  &out);
    *checking = out.id;
    ledger_add_account("CREDIT CARD", "2.1", "USD", LEDGER_CREDITNORMAL, &out);
    *card = out.id;
}

/* A one-debit, one-credit entry between two accounts given by id. */
static LedgerError post_simple(int64_t debit_id, int64_t credit_id,
                               int64_t amount) {
    LedgerPosting d = {.account_id = debit_id,  .amount = amount,
                       .currency = "USD"};
    LedgerPosting c = {.account_id = credit_id, .amount = amount,
                       .currency = "USD"};
    LedgerEntry e = {
        .date = "2025-01-01T00:00:00Z", .description = "test",
        .debits = &d, .n_debits = 1, .credits = &c, .n_credits = 1
    };
    return ledger_post_journal(&e, NULL);
}

/* One USD posting referencing an account by code. */
static LedgerPosting usd(const char *code, int64_t amount) {
    LedgerPosting p = {.amount = amount, .currency = "USD"};
    snprintf(p.account_code, sizeof p.account_code, "%s", code);
    return p;
}

static int64_t balance_of(const char *code) {
    LedgerAccount a = {0};
    int64_t bal = -1;
    if (ledger_get_account_by_code(code, &a) == LEDGER_OK)
        ledger_get_balance(a.id, &bal);
    return bal;
}

/* =========================================================================
   Chart of accounts
   ======================================================================= */

TEST(test_create_minimal_chart) {
    setup();
    int created = 0;
    ASSERT_OK(ledger_create_minimal_chart(&created));
    ASSERT_EQ(created, 49);   /* the 49 accounts of MINIMAL_CHART[] */

    int again = 0;            /* idempotent */
    ASSERT_OK(ledger_create_minimal_chart(&again));
    ASSERT_EQ(again, 0);

    LedgerAccount a = {0};
    ASSERT_OK(ledger_get_account_by_code("1.1.1", &a));
    ASSERT_STR(a.name, "CASH ON HAND");
    ASSERT_EQ(a.nature, LEDGER_DEBITNORMAL);
    ASSERT_EQ(a.decimal_places, 2);
    teardown();
}

TEST(test_parent_derived_from_code) {
    setup();
    LedgerAccount root = {0}, child = {0};
    ASSERT_OK(ledger_add_account("ASSETS", "1", "USD", LEDGER_DEBITNORMAL,
                                 &root));
    ASSERT_OK(ledger_add_account("CASH", "1.1", "USD", LEDGER_DEBITNORMAL,
                                 &child));
    ASSERT_EQ(root.parent_id, 0);
    ASSERT_EQ(child.parent_id, root.id);
    teardown();
}

TEST(test_add_account_missing_parent) {
    setup();
    ASSERT_ERR(ledger_add_account("ORPHAN", "9.1", "USD",
                                  LEDGER_DEBITNORMAL, NULL),
               LEDGER_ERR_ACCT_NOT_FOUND);
    teardown();
}

TEST(test_add_account_bad_code) {
    setup();
    const char *bad[] = {"", ".1", "1.", "1..2"};
    for (int i = 0; i < 4; i++)
        ASSERT_ERR(ledger_add_account("BAD", bad[i], "USD",
                                      LEDGER_DEBITNORMAL, NULL),
                   LEDGER_ERR_BAD_CODE);
    teardown();
}

TEST(test_add_account_bad_nature) {
    setup();
    ASSERT_ERR(ledger_add_account("X", "1", "USD", LEDGER_NATURE_INVALID,
                                  NULL),
               LEDGER_ERR_BAD_NATURE);
    teardown();
}

TEST(test_has_children) {
    setup();
    LedgerAccount root = {0}, leaf = {0};
    ASSERT_OK(ledger_add_account("ASSETS", "1", "USD", LEDGER_DEBITNORMAL,
                                 &root));
    ASSERT_EQ(ledger_has_children(root.id), 0);
    ASSERT_OK(ledger_add_account("CASH", "1.1", "USD", LEDGER_DEBITNORMAL,
                                 &leaf));
    ASSERT_EQ(ledger_has_children(root.id), 1);
    ASSERT_EQ(ledger_has_children(leaf.id), 0);
    teardown();
}

TEST(test_parent_has_entries_block_child) {
    setup();
    int64_t checking, card;
    make_basic_accounts(&checking, &card);
    ASSERT_OK(post_simple(checking, card, 100));

    /* CHECKING has been posted to: it must stay a leaf */
    ASSERT_ERR(ledger_add_account("SUB", "1.1.1", "USD", LEDGER_DEBITNORMAL,
                                  NULL),
               LEDGER_ERR_HAS_ENTRIES);
    teardown();
}

/* =========================================================================
   Posting
   ======================================================================= */

TEST(test_post_basic) {
    setup();
    int64_t checking, card;
    make_basic_accounts(&checking, &card);
    ASSERT_OK(post_simple(checking, card, 1000));

    int64_t b = 0;
    ASSERT_OK(ledger_get_balance(checking, &b)); ASSERT_EQ(b, 1000);
    ASSERT_OK(ledger_get_balance(card, &b));     ASSERT_EQ(b, 1000);
    teardown();
}

TEST(test_post_multicurrency) {
    setup();
    LedgerAccount usd_cash, usd_loan, btc_wallet, btc_loan;
    ledger_add_account("ASSETS",      "1",   "USD", LEDGER_DEBITNORMAL, NULL);
    ledger_add_account("LIABILITIES", "2",   "USD", LEDGER_CREDITNORMAL, NULL);
    ASSERT_OK(ledger_add_account("CASH", "1.1", "USD", LEDGER_DEBITNORMAL,
                                 &usd_cash));
    ASSERT_OK(ledger_add_account("BTC WALLET", "1.2", "BTC",
                                 LEDGER_DEBITNORMAL, &btc_wallet));
    ASSERT_OK(ledger_add_account("USD LOAN", "2.1", "USD",
                                 LEDGER_CREDITNORMAL, &usd_loan));
    ASSERT_OK(ledger_add_account("BTC LOAN", "2.2", "BTC",
                                 LEDGER_CREDITNORMAL, &btc_loan));

    LedgerPosting debits[2] = {
        {.account_id = usd_cash.id,   .amount = 500,       .currency = "USD"},
        {.account_id = btc_wallet.id, .amount = 100000000, .currency = "BTC"}
    };
    LedgerPosting credits[2] = {
        {.account_id = usd_loan.id, .amount = 500,       .currency = "USD"},
        {.account_id = btc_loan.id, .amount = 100000000, .currency = "BTC"}
    };
    LedgerEntry e = {
        .date = "2025-01-01T00:00:00Z", .description = "two loans",
        .debits = debits, .n_debits = 2, .credits = credits, .n_credits = 2
    };
    ASSERT_OK(ledger_post_journal(&e, NULL));

    int64_t b = 0;
    ASSERT_OK(ledger_get_balance(usd_cash.id, &b));   ASSERT_EQ(b, 500);
    ASSERT_OK(ledger_get_balance(btc_wallet.id, &b)); ASSERT_EQ(b, 100000000);
    teardown();
}

TEST(test_post_unbalanced) {
    setup();
    int64_t checking, card;
    make_basic_accounts(&checking, &card);
    LedgerPosting d = {.account_id = checking, .amount = 1000,
                       .currency = "USD"};
    LedgerPosting c = {.account_id = card, .amount = 999, .currency = "USD"};
    LedgerEntry e = {
        .date = "2025-01-01T00:00:00Z",
        .debits = &d, .n_debits = 1, .credits = &c, .n_credits = 1
    };
    ASSERT_ERR(ledger_post_journal(&e, NULL), LEDGER_ERR_CR_DB_EQUAL);
    teardown();
}

TEST(test_post_negative_balance) {
    setup();
    int64_t checking, card;
    make_basic_accounts(&checking, &card);
    /* Debiting the empty CREDIT CARD would drive it below zero */
    ASSERT_ERR(post_simple(card, checking, 500), LEDGER_ERR_NEG_BALANCE);
    ASSERT_EQ(balance_of("1.1"), 0);   /* nothing was written */
    teardown();
}

TEST(test_post_to_branch) {
    setup();
    int64_t checking, card;
    make_basic_accounts(&checking, &card);
    LedgerAccount assets;
    ASSERT_OK(ledger_get_account_by_code("1", &assets));
    ASSERT_ERR(post_simple(assets.id, card, 100), LEDGER_ERR_ACCT_CHILDREN);
    teardown();
}

TEST(test_post_wrong_currency) {
    setup();
    int64_t checking, card;
    make_basic_accounts(&checking, &card);
    LedgerPosting d = {.account_id = checking, .amount = 1000,
                       .currency = "EUR"};
    LedgerPosting c = {.account_id = card, .amount = 1000, .currency = "USD"};
    LedgerEntry e = {
        .date = "2025-01-01T00:00:00Z",
        .debits = &d, .n_debits = 1, .credits = &c, .n_credits = 1
    };
    ASSERT_ERR(ledger_post_journal(&e, NULL), LEDGER_ERR_DEBIT_CURR);
    teardown();
}

TEST(test_post_zero_amount) {
    setup();
    int64_t checking, card;
    make_basic_accounts(&checking, &card);
    ASSERT_ERR(post_simple(checking, card, 0), LEDGER_ERR_DEBIT_AMT);
    teardown();
}

TEST(test_repeated_posts) {
    setup();
    int64_t checking, card;
    make_basic_accounts(&checking, &card);
    for (int i = 0; i < 10; i++)
        ASSERT_OK(post_simple(checking, card, 100));
    ASSERT_EQ(balance_of("1.1"), 1000);
    ASSERT_EQ(balance_of("2.1"), 1000);
    teardown();
}

/* Two debit lines on the same account must both count. */
TEST(test_post_same_account_twice) {
    setup();
    ASSERT_OK(ledger_create_minimal_chart(NULL));
    LedgerPosting db[2] = { usd("1.1.2", 100), usd("1.1.2", 100) };
    LedgerPosting cr[1] = { usd("4.1", 200) };
    LedgerEntry e = {
        .date = "2025-01-01T00:00:00Z", .description = "two invoices",
        .debits = db, .n_debits = 2, .credits = cr, .n_credits = 1
    };
    ASSERT_OK(ledger_post_journal(&e, NULL));
    ASSERT_EQ(balance_of("1.1.2"), 200);

    LedgerInconsistency *inc = NULL; int n = 0;
    ASSERT_OK(ledger_check_leaf_balances(0, &inc, &n));
    ASSERT_EQ(n, 0);
    ledger_inconsistencies_free(inc, n);
    teardown();
}

/* A debit and a credit on the same empty account: the increase is applied
   first, so the entry is accepted and no intermediate balance is negative. */
TEST(test_post_same_account_debit_and_credit) {
    setup();
    ASSERT_OK(ledger_create_minimal_chart(NULL));
    LedgerPosting db[1] = { usd("1.1.2", 100) };
    LedgerPosting cr[2] = { usd("1.1.2", 40), usd("4.1", 60) };
    LedgerEntry e = {
        .date = "2025-01-01T00:00:00Z", .description = "net receipt",
        .debits = db, .n_debits = 1, .credits = cr, .n_credits = 2
    };
    ASSERT_OK(ledger_post_journal(&e, NULL));
    ASSERT_EQ(balance_of("1.1.2"), 60);

    LedgerAccount chk = {0};
    ASSERT_OK(ledger_get_account_by_code("1.1.2", &chk));
    LedgerTAccountLine *tl = NULL; int nt = 0;
    ASSERT_OK(ledger_get_taccount_lines(chk.id, &tl, &nt));
    ASSERT_EQ(nt, 2);
    ASSERT_EQ(tl[0].balance_prev, 0);
    ASSERT_EQ(tl[0].balance, 100);
    ASSERT_EQ(tl[1].balance_prev, 100);
    ASSERT_EQ(tl[1].balance, 60);
    ledger_taccount_lines_free(tl, nt);
    teardown();
}

/* Validation runs every check, including the balance check, and writes
   nothing. */
TEST(test_validate_journal) {
    setup();
    int64_t checking, card;
    make_basic_accounts(&checking, &card);
    LedgerPosting d = {.account_id = checking, .amount = 700,
                       .currency = "USD"};
    LedgerPosting c = {.account_id = card, .amount = 700, .currency = "USD"};
    LedgerEntry ok = {
        .date = "2025-01-01T00:00:00Z",
        .debits = &d, .n_debits = 1, .credits = &c, .n_credits = 1
    };
    ASSERT_OK(ledger_validate_journal(&ok));
    ASSERT_EQ(balance_of("1.1"), 0);

    LedgerJournalHeader *jh = NULL; int nj = -1;
    ASSERT_OK(ledger_get_journals(&jh, &nj));
    ASSERT_EQ(nj, 0);
    ledger_journals_free(jh, nj);

    LedgerEntry negative = ok;
    negative.debits = &c; negative.credits = &d;
    ASSERT_ERR(ledger_validate_journal(&negative), LEDGER_ERR_NEG_BALANCE);
    teardown();
}

TEST(test_journal_read_back) {
    setup();
    ASSERT_OK(ledger_create_minimal_chart(NULL));
    LedgerPosting db[1] = { usd("1.1.2", 800000) };
    LedgerPosting cr[1] = { usd("4.1", 800000) };
    char *keys[] = {"invoice"}, *vals[] = {"inv-2025-042"};
    LedgerEntry e = {
        .date = "2025-06-30T09:00:00Z", .description = "Design services",
        .debits = db, .n_debits = 1, .credits = cr, .n_credits = 1,
        .prop_keys = keys, .prop_vals = vals, .n_props = 1
    };
    int64_t id = 0;
    ASSERT_OK(ledger_post_journal(&e, &id));

    LedgerJournalHeader *jh = NULL; int nj = 0;
    ASSERT_OK(ledger_get_journals(&jh, &nj));
    ASSERT_EQ(nj, 1);
    ASSERT_EQ(jh[0].id, id);
    ledger_journals_free(jh, nj);

    LedgerJournalHeader h;
    ASSERT_OK(ledger_get_journal(id, &h));
    ASSERT_STR(h.description, "Design services");
    ASSERT_ERR(ledger_get_journal(id + 1, &h), LEDGER_ERR_JOURNAL_NOT_FOUND);

    LedgerJournalLine *jl = NULL; int nl = 0;
    ASSERT_OK(ledger_get_journal_lines(id, &jl, &nl));
    ASSERT_EQ(nl, 2);
    ASSERT_EQ(jl[0].side, 'D');
    ASSERT_STR(jl[0].account_code, "1.1.2");
    ASSERT_EQ(jl[0].decimal_places, 2);
    ledger_journal_lines_free(jl, nl);

    char **k = NULL, **v = NULL; int np = 0;
    ASSERT_OK(ledger_get_journal_props(id, &k, &v, &np));
    ASSERT_EQ(np, 1);
    ASSERT_STR(v[0], "inv-2025-042");
    ledger_journal_props_free(k, v, np);
    teardown();
}

/* One company must not read or post to another company's accounts. */
TEST(test_company_isolation) {
    setup();                                    /* company "alex" */
    ASSERT_OK(ledger_create_minimal_chart(NULL));
    LedgerPosting db[1] = { usd("1.1.2", 500) };
    LedgerPosting cr[1] = { usd("4.1", 500) };
    LedgerEntry e = {
        .date = "2025-01-01T00:00:00Z", .description = "fee",
        .debits = db, .n_debits = 1, .credits = cr, .n_credits = 1
    };
    ASSERT_OK(ledger_post_journal(&e, NULL));
    LedgerAccount chk = {0};
    ASSERT_OK(ledger_get_account_by_code("1.1.2", &chk));
    LedgerJournalHeader *jh = NULL; int nj = 0;
    ASSERT_OK(ledger_get_journals(&jh, &nj));
    int64_t journal_id = jh[0].id;
    ledger_journals_free(jh, nj);

    ASSERT_OK(ledger_add_company("other"));
    ASSERT_OK(ledger_init("other"));
    ASSERT_OK(ledger_create_minimal_chart(NULL));

    LedgerAccount a = {0};
    ASSERT_ERR(ledger_get_account_by_id(chk.id, &a),
               LEDGER_ERR_ACCT_NOT_FOUND);
    int64_t bal = 0;
    ASSERT_ERR(ledger_get_balance(chk.id, &bal), LEDGER_ERR_ACCT_NOT_FOUND);

    LedgerPosting foreign = {.account_id = chk.id, .amount = 50,
                             .currency = "USD"};
    LedgerPosting cr2[1] = { usd("4.1", 50) };
    e.debits = &foreign; e.credits = cr2;
    ASSERT_ERR(ledger_post_journal(&e, NULL), LEDGER_ERR_ACCT_NOT_FOUND);

    LedgerTAccountLine *tl = NULL; int nt = -1;
    ASSERT_OK(ledger_get_taccount_lines(chk.id, &tl, &nt));
    ASSERT_EQ(nt, 0);
    ledger_taccount_lines_free(tl, nt);
    LedgerJournalLine *jl = NULL; int nl = -1;
    ASSERT_OK(ledger_get_journal_lines(journal_id, &jl, &nl));
    ASSERT_EQ(nl, 0);
    ledger_journal_lines_free(jl, nl);
    LedgerJournalHeader h;
    ASSERT_ERR(ledger_get_journal(journal_id, &h),
               LEDGER_ERR_JOURNAL_NOT_FOUND);

    ASSERT_OK(ledger_init("alex"));
    ASSERT_EQ(balance_of("1.1.2"), 500);
    teardown();
}

TEST(test_init_unknown_company) {
    setup();
    ASSERT_ERR(ledger_init("nobody"), LEDGER_ERR_COMPANY_NOT_FOUND);
    teardown();
}

/* =========================================================================
   Consistency checks
   ======================================================================= */

TEST(test_checks_clean) {
    setup();
    int64_t checking, card;
    make_basic_accounts(&checking, &card);
    ASSERT_OK(post_simple(checking, card, 500));

    LedgerInconsistency *inc = NULL; int n = -1;
    ASSERT_OK(ledger_check_leaf_balances(0, &inc, &n));
    ASSERT_EQ(n, 0); ledger_inconsistencies_free(inc, n);
    ASSERT_OK(ledger_check_non_leaf_entries(&inc, &n));
    ASSERT_EQ(n, 0); ledger_inconsistencies_free(inc, n);
    ASSERT_OK(ledger_check_code_prefixes(&inc, &n));
    ASSERT_EQ(n, 0); ledger_inconsistencies_free(inc, n);
    teardown();
}

/* Corrupt the cache through a second SQLite connection, then let
   ledger_check_leaf_balances() find and repair it. */
TEST(test_check_detects_and_fixes_cache) {
    const char *path = "test_cache.db";
    remove(path);
    ledger_close();
    ASSERT_OK(ledger_open(path));
    ASSERT_OK(ledger_add_company("alex"));
    ASSERT_OK(ledger_init("alex"));
    int64_t checking, card;
    make_basic_accounts(&checking, &card);
    ASSERT_OK(post_simple(checking, card, 500));

    sqlite3 *raw = NULL;
    ASSERT_EQ(sqlite3_open(path, &raw), SQLITE_OK);
    char sql[128];
    snprintf(sql, sizeof sql,
             "UPDATE ledger_balance SET balance=999 WHERE account_id=%lld",
             (long long)checking);
    ASSERT_EQ(sqlite3_exec(raw, sql, NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(raw);

    LedgerInconsistency *inc = NULL; int n = 0;
    ASSERT_OK(ledger_check_leaf_balances(1, &inc, &n));
    ASSERT_EQ(n, 1);
    ASSERT_EQ(inc[0].cached, 999);
    ASSERT_EQ(inc[0].calculated, 500);
    ledger_inconsistencies_free(inc, n);

    ASSERT_OK(ledger_check_leaf_balances(0, &inc, &n));
    ASSERT_EQ(n, 0);
    ledger_inconsistencies_free(inc, n);
    ASSERT_EQ(balance_of("1.1"), 500);
    teardown();
    remove(path);
}

/* =========================================================================
   Review fixes
   ======================================================================= */

/* Sums that overflow int64 must be refused, not wrap into a fake balance. */
TEST(test_post_overflow) {
    setup();
    ASSERT_OK(ledger_create_minimal_chart(NULL));
    int64_t big = INT64_MAX;
    LedgerPosting db[2] = { usd("1.1.1", big), usd("1.1.2", big) };
    LedgerPosting cr[1] = { usd("4.1", 2) };
    LedgerEntry e = {
        .date = "2025-01-01", .description = "overflow",
        .debits = db, .n_debits = 2, .credits = cr, .n_credits = 1
    };
    ASSERT_ERR(ledger_post_journal(&e, NULL), LEDGER_ERR_OVERFLOW);
    teardown();
}

TEST(test_duplicate_code) {
    setup();
    ASSERT_OK(ledger_create_minimal_chart(NULL));
    ASSERT_ERR(ledger_add_account("AGAIN", "1.1.2", "USD",
                                  LEDGER_DEBITNORMAL, NULL),
               LEDGER_ERR_DUP_CODE);
    LedgerAccount a = {0};
    ASSERT_OK(ledger_add_account_if_not_exists("CHECKING", "1.1.2", "USD",
                                               LEDGER_DEBITNORMAL, &a));
    ASSERT_STR(a.name, "CHECKING ACCOUNT");
    ASSERT_ERR(ledger_add_account_if_not_exists("CHECKING", "1.1.2", "BTC",
                                                LEDGER_DEBITNORMAL, &a),
               LEDGER_ERR_DUP_CODE);
    teardown();
}

TEST(test_validate_rejects_null_property) {
    setup();
    int64_t checking, card;
    make_basic_accounts(&checking, &card);
    LedgerPosting d = {.account_id = checking, .amount = 5, .currency = "USD"};
    LedgerPosting c = {.account_id = card, .amount = 5, .currency = "USD"};
    char *keys[] = {"invoice"}, *vals[] = {NULL};
    LedgerEntry e = {
        .date = "2025-01-01", .description = "props",
        .debits = &d, .n_debits = 1, .credits = &c, .n_credits = 1,
        .prop_keys = keys, .prop_vals = vals, .n_props = 1
    };
    ASSERT_ERR(ledger_validate_journal(&e), LEDGER_ERR_ARGS);
    ASSERT_ERR(ledger_post_journal(&e, NULL), LEDGER_ERR_ARGS);
    teardown();
}

TEST(test_failed_init_selects_no_company) {
    setup();
    ASSERT_OK(ledger_create_minimal_chart(NULL));
    ASSERT_ERR(ledger_init("nobody"), LEDGER_ERR_COMPANY_NOT_FOUND);
    LedgerAccount a;
    ASSERT_ERR(ledger_get_account_by_code("1.1.2", &a), LEDGER_ERR_NO_COMPANY);
    teardown();
}

TEST(test_has_children_is_company_scoped) {
    setup();
    ASSERT_OK(ledger_create_minimal_chart(NULL));
    LedgerAccount assets;
    ASSERT_OK(ledger_get_account_by_code("1", &assets));
    ASSERT_OK(ledger_add_company("other"));
    ASSERT_OK(ledger_init("other"));
    ASSERT_EQ(ledger_has_children(assets.id), 0);
    teardown();
}

TEST(test_decimal_places_locked_after_posting) {
    setup();
    ASSERT_OK(ledger_set_currency_decimal_places("BTC", 8));
    LedgerAccount w, r;
    ledger_add_account("ASSETS", "1", "USD", LEDGER_DEBITNORMAL, NULL);
    ledger_add_account("REVENUE", "4", "USD", LEDGER_CREDITNORMAL, NULL);
    ASSERT_OK(ledger_add_account("WALLET", "1.1", "BTC", LEDGER_DEBITNORMAL,
                                 &w));
    ASSERT_OK(ledger_add_account("REWARDS", "4.1", "BTC", LEDGER_CREDITNORMAL,
                                 &r));
    ASSERT_EQ(w.decimal_places, 8);
    LedgerPosting d = {.account_id = w.id, .amount = 50000000,
                       .currency = "BTC"};
    LedgerPosting c = {.account_id = r.id, .amount = 50000000,
                       .currency = "BTC"};
    LedgerEntry e = {
        .date = "2025-01-01", .description = "reward",
        .debits = &d, .n_debits = 1, .credits = &c, .n_credits = 1
    };
    ASSERT_OK(ledger_post_journal(&e, NULL));
    ASSERT_ERR(ledger_set_currency_decimal_places("BTC", 2),
               LEDGER_ERR_HAS_ENTRIES);
    ASSERT_OK(ledger_set_currency_decimal_places("BTC", 8));  /* no change */
    teardown();
}

/* Corrupt a file database behind the library's back, then let the checks
   find an unbalanced entry and a wrong parent. */
TEST(test_checks_find_tampering) {
    const char *path = "test_tamper.db";
    remove(path);
    ledger_close();
    ASSERT_OK(ledger_open(path));
    ASSERT_OK(ledger_add_company("alex"));
    ASSERT_OK(ledger_init("alex"));
    ASSERT_OK(ledger_create_minimal_chart(NULL));
    LedgerPosting db[1] = { usd("1.1.2", 1000) };
    LedgerPosting cr[1] = { usd("4.1", 1000) };
    LedgerEntry e = {
        .date = "2025-01-01", .description = "fee",
        .debits = db, .n_debits = 1, .credits = cr, .n_credits = 1
    };
    int64_t id = 0;
    ASSERT_OK(ledger_post_journal(&e, &id));

    sqlite3 *raw = NULL;
    ASSERT_EQ(sqlite3_open(path, &raw), SQLITE_OK);
    char sql[256];
    snprintf(sql, sizeof sql,
             "UPDATE ledger_taccount SET credit=999 WHERE journal_id=%lld"
             " AND credit > 0;"
             "UPDATE ledger_account SET parent_id="
             "(SELECT id FROM ledger_account WHERE code='1')"
             " WHERE code='1.1.4';", (long long)id);
    ASSERT_EQ(sqlite3_exec(raw, sql, NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(raw);

    LedgerInconsistency *inc = NULL; int n = 0;
    ASSERT_OK(ledger_check_journal_balance(&inc, &n));
    ASSERT_EQ(n, 1);
    ASSERT_EQ(inc[0].journal_id, id);
    ASSERT_EQ(inc[0].debits, 1000);
    ASSERT_EQ(inc[0].credits, 999);
    ledger_inconsistencies_free(inc, n);

    ASSERT_OK(ledger_check_code_prefixes(&inc, &n));
    ASSERT_EQ(n, 1);
    ASSERT_STR(inc[0].account_code, "1.1.4");
    ledger_inconsistencies_free(inc, n);
    teardown();
    remove(path);
}

TEST(test_long_currency_refused) {
    setup();
    ledger_add_account("ASSETS", "1", "USD", LEDGER_DEBITNORMAL, NULL);
    ASSERT_ERR(ledger_add_account("X", "1.1", "LONGCURRENCY_ABC_X",
                                  LEDGER_DEBITNORMAL, NULL),
               LEDGER_ERR_ARGS);
    teardown();
}

TEST(test_duplicate_company) {
    setup();
    ASSERT_ERR(ledger_add_company("alex"), LEDGER_ERR_DUP_CODE);
    teardown();
}

TEST(test_chart_refuses_conflicting_account) {
    setup();
    ASSERT_OK(ledger_add_account("ASSETS", "1", "EUR", LEDGER_DEBITNORMAL,
                                 NULL));
    ASSERT_ERR(ledger_create_minimal_chart(NULL), LEDGER_ERR_DUP_CODE);
    teardown();
}

/* A recalculated balance below zero is reported but never written; an
   entry whose rows were all deleted is reported as unbalanced. */
TEST(test_fix_skips_negative_and_empty_entries_found) {
    const char *path = "test_negfix.db";
    remove(path);
    ledger_close();
    ASSERT_OK(ledger_open(path));
    ASSERT_OK(ledger_add_company("alex"));
    ASSERT_OK(ledger_init("alex"));
    ASSERT_OK(ledger_create_minimal_chart(NULL));
    LedgerPosting db[1] = { usd("1.1.2", 1000) };
    LedgerPosting cr[1] = { usd("4.1", 1000) };
    LedgerEntry e = {
        .date = "2025-01-01", .description = "fee",
        .debits = db, .n_debits = 1, .credits = cr, .n_credits = 1
    };
    int64_t id = 0;
    ASSERT_OK(ledger_post_journal(&e, &id));

    sqlite3 *raw = NULL;
    ASSERT_EQ(sqlite3_open(path, &raw), SQLITE_OK);
    char sql[256];
    snprintf(sql, sizeof sql,
             "UPDATE ledger_taccount SET debit=0, credit=500"
             " WHERE journal_id=%lld AND debit > 0;"
             "INSERT INTO ledger_journal(date, description, company_id)"
             " VALUES('2025-01-02', 'ghost', 1);", (long long)id);
    ASSERT_EQ(sqlite3_exec(raw, sql, NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(raw);

    LedgerInconsistency *inc = NULL; int n = 0;
    ASSERT_OK(ledger_check_leaf_balances(1, &inc, &n));
    ASSERT_EQ(n, 1);
    ASSERT_EQ(inc[0].calculated, -500);
    ledger_inconsistencies_free(inc, n);
    ASSERT_EQ(balance_of("1.1.2"), 1000);     /* left alone */

    ASSERT_OK(ledger_check_journal_balance(&inc, &n));
    ASSERT_EQ(n, 2);                           /* altered + empty entry */
    ledger_inconsistencies_free(inc, n);
    teardown();
    remove(path);
}

/* =========================================================================
   Money
   ======================================================================= */

TEST(test_format_amount) {
    char buf[32];
    ASSERT_STR(ledger_format_amount(800000, 2, buf, sizeof buf), "8000.00");
    ASSERT_STR(ledger_format_amount(5, 2, buf, sizeof buf), "0.05");
    ASSERT_STR(ledger_format_amount(-1250, 2, buf, sizeof buf), "-12.50");
    ASSERT_STR(ledger_format_amount(1500, 0, buf, sizeof buf), "1500");
    ASSERT_STR(ledger_format_amount(50000000, 8, buf, sizeof buf),
               "0.50000000");
}

TEST(test_parse_amount) {
    int64_t v = 0;
    ASSERT_OK(ledger_parse_amount("8000.00", 2, &v)); ASSERT_EQ(v, 800000);
    ASSERT_OK(ledger_parse_amount("8000.5", 2, &v));  ASSERT_EQ(v, 800050);
    ASSERT_OK(ledger_parse_amount("8000", 2, &v));    ASSERT_EQ(v, 800000);
    ASSERT_OK(ledger_parse_amount("0.5", 8, &v));     ASSERT_EQ(v, 50000000);

    const char *bad[] = {"", ".", ".5", "8000.", "-5", "1,000", "1.234",
                         "12a", " 5", "99999999999999999999"};
    for (int i = 0; i < 10; i++)
        ASSERT_ERR(ledger_parse_amount(bad[i], 2, &v), LEDGER_ERR_BAD_AMOUNT);
}

TEST(test_nature_names) {
    ASSERT_STR(ledger_nature_name(LEDGER_DEBITNORMAL), "DEBITNORMAL");
    ASSERT_EQ(ledger_nature_parse("CREDITNORMAL"), LEDGER_CREDITNORMAL);
    ASSERT_EQ(ledger_nature_parse("credit"), LEDGER_NATURE_INVALID);
    ASSERT_EQ(ledger_nature_name(LEDGER_NATURE_INVALID) == NULL, 1);
}

/* =========================================================================
   main
   ======================================================================= */
int main(void) {
    printf("=== libledger test suite ===\n");

    RUN(test_create_minimal_chart);
    RUN(test_parent_derived_from_code);
    RUN(test_add_account_missing_parent);
    RUN(test_add_account_bad_code);
    RUN(test_add_account_bad_nature);
    RUN(test_has_children);
    RUN(test_parent_has_entries_block_child);

    RUN(test_post_basic);
    RUN(test_post_multicurrency);
    RUN(test_post_unbalanced);
    RUN(test_post_negative_balance);
    RUN(test_post_to_branch);
    RUN(test_post_wrong_currency);
    RUN(test_post_zero_amount);
    RUN(test_repeated_posts);
    RUN(test_post_same_account_twice);
    RUN(test_post_same_account_debit_and_credit);
    RUN(test_validate_journal);
    RUN(test_journal_read_back);
    RUN(test_company_isolation);
    RUN(test_init_unknown_company);

    RUN(test_checks_clean);
    RUN(test_check_detects_and_fixes_cache);

    RUN(test_post_overflow);
    RUN(test_duplicate_code);
    RUN(test_validate_rejects_null_property);
    RUN(test_failed_init_selects_no_company);
    RUN(test_has_children_is_company_scoped);
    RUN(test_decimal_places_locked_after_posting);
    RUN(test_checks_find_tampering);
    RUN(test_long_currency_refused);
    RUN(test_duplicate_company);
    RUN(test_chart_refuses_conflicting_account);
    RUN(test_fix_skips_negative_and_empty_entries_found);

    RUN(test_format_amount);
    RUN(test_parse_amount);
    RUN(test_nature_names);

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
