#include "ledger.h"
#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>      /* isfinite, for prices */
static const char *SCHEMA_DDL[] = {
    "PRAGMA journal_mode=WAL",
    "PRAGMA foreign_keys=ON",
    "CREATE TABLE IF NOT EXISTS ledger_company ("
    "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  nickname TEXT    NOT NULL UNIQUE"
    ")",
    "CREATE TABLE IF NOT EXISTS ledger_account ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name       TEXT    NOT NULL,"
    "  code       TEXT    NOT NULL,"
    "  currency   TEXT    NOT NULL,"
    "  parent_id  INTEGER REFERENCES ledger_account(id),"
    "  nature     TEXT    NOT NULL"
    "    CHECK(nature IN ('CREDITNORMAL','DEBITNORMAL')),"
    "  company_id INTEGER NOT NULL REFERENCES ledger_company(id)"
    ")",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_ledger_account_code_company"
    "  ON ledger_account(code, company_id)",
    "CREATE TABLE IF NOT EXISTS ledger_balance ("
    "  account_id INTEGER NOT NULL PRIMARY KEY REFERENCES ledger_account(id),"
    "  balance    INTEGER NOT NULL DEFAULT 0"
    ")",
    "CREATE TABLE IF NOT EXISTS ledger_journal ("
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  date        TEXT    NOT NULL,"
    "  description TEXT    NOT NULL DEFAULT '',"
    "  system      TEXT    NOT NULL DEFAULT '',"
    "  end2end     TEXT    NOT NULL DEFAULT '',"
    "  info        TEXT    NOT NULL DEFAULT '',"
    "  company_id  INTEGER NOT NULL REFERENCES ledger_company(id)"
    ")",
    "CREATE TABLE IF NOT EXISTS ledger_taccount ("
    "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  debit        INTEGER NOT NULL DEFAULT 0,"
    "  credit       INTEGER NOT NULL DEFAULT 0,"
    "  account_id   INTEGER NOT NULL REFERENCES ledger_account(id),"
    "  journal_id   INTEGER NOT NULL REFERENCES ledger_journal(id),"
    "  info         TEXT    NOT NULL DEFAULT '',"
    "  balance_prev INTEGER NOT NULL DEFAULT 0,"
    "  balance      INTEGER NOT NULL DEFAULT 0,"
    "  CHECK (debit >= 0 AND credit >= 0 AND (debit = 0) != (credit = 0))"
    ")",
    "CREATE INDEX IF NOT EXISTS idx_taccount_account"
    "  ON ledger_taccount(account_id)",
    "CREATE INDEX IF NOT EXISTS idx_taccount_journal"
    "  ON ledger_taccount(journal_id)",
    "CREATE TABLE IF NOT EXISTS ledger_accountattr ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  property   TEXT    NOT NULL,"
    "  value      TEXT    NOT NULL DEFAULT '',"
    "  account_id INTEGER NOT NULL REFERENCES ledger_account(id)"
    ")",
    "CREATE TABLE IF NOT EXISTS ledger_journalattr ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  property   TEXT    NOT NULL,"
    "  value      TEXT    NOT NULL DEFAULT '',"
    "  journal_id INTEGER NOT NULL REFERENCES ledger_journal(id)"
    ")",
    "CREATE TABLE IF NOT EXISTS ledger_currency ("
    "  code           TEXT    NOT NULL PRIMARY KEY,"
    "  decimal_places INTEGER NOT NULL DEFAULT 2,"
    "  price          REAL    NOT NULL DEFAULT 0.0"
    ")",
    NULL
};
static sqlite3 *g_db         = NULL;
static int64_t  g_company_id = 0;

static _Thread_local char g_last_error[512] = "";

#define SET_ERR(fmt, ...) \
    snprintf(g_last_error, sizeof(g_last_error), fmt, ##__VA_ARGS__)
#define SET_SQLERR() \
    SET_ERR("%s", sqlite3_errmsg(g_db))
#define CHECK_DB() \
    do { if (!g_db) { \
        SET_ERR("db not open"); return LEDGER_ERR_DB; \
    } } while (0)
#define CHECK_INIT() \
    do { if (!g_company_id) { \
        SET_ERR("no company selected; call ledger_init()"); \
        return LEDGER_ERR_NO_COMPANY; \
    } } while (0)
const char *ledger_last_error(void) {
    return g_last_error;
}
static sqlite3_stmt *db_prepare(const char *sql) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) {
        SET_SQLERR();
        return NULL;
    }
    return st;
}

static LedgerError db_done(sqlite3_stmt *st, int rc) {
    if (rc != SQLITE_DONE) SET_SQLERR();
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? LEDGER_OK : LEDGER_ERR_DB;
}

static LedgerError db_exec(sqlite3_stmt *st) {
    return db_done(st, sqlite3_step(st));
}

static void col_text(sqlite3_stmt *st, int col, char *buf, size_t size) {
    const unsigned char *t = sqlite3_column_text(st, col);
    snprintf(buf, size, "%s", t ? (const char *)t : "");
}

static int grow_array(void **arr, int *cap, int count, size_t elem_size) {
    if (count < *cap) return 0;
    int new_cap = *cap ? *cap * 2 : 16;
    void *tmp = realloc(*arr, (size_t)new_cap * elem_size);
    if (!tmp) { SET_ERR("out of memory"); return -1; }
    *arr = tmp;
    *cap = new_cap;
    return 0;
}

#define GROW(arr, count, cap) \
    grow_array((void **)&(arr), &(cap), (count), sizeof *(arr))
static LedgerError txn_run(const char *sql) {
    if (sqlite3_exec(g_db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        SET_SQLERR();
        return LEDGER_ERR_DB;
    }
    return LEDGER_OK;
}

static LedgerError txn_begin_immediate(void) {
    return txn_run("BEGIN IMMEDIATE");
}

static void txn_rollback(void);

static LedgerError txn_commit(void) {
    LedgerError err = txn_run("COMMIT");
    if (err != LEDGER_OK) txn_rollback();
    return err;
}

static void txn_rollback(void) {  /* errors ignored: nothing left to undo */
    sqlite3_exec(g_db, "ROLLBACK", NULL, NULL, NULL);
}
LedgerError ledger_open(const char *path) {
    if (!path) { SET_ERR("path is NULL"); return LEDGER_ERR_ARGS; }
    if (g_db)  { SET_ERR("already open"); return LEDGER_ERR_ARGS; }

    int rc = sqlite3_open_v2(path, &g_db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        NULL);
    if (rc != SQLITE_OK) goto fail;
    sqlite3_busy_timeout(g_db, 5000);  /* milliseconds */

    for (int i = 0; SCHEMA_DDL[i] != NULL; i++) {
        rc = sqlite3_exec(g_db, SCHEMA_DDL[i], NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto fail;
    }
    return LEDGER_OK;

fail:
    SET_SQLERR();
    sqlite3_close(g_db);
    g_db = NULL;
    return LEDGER_ERR_DB;
}
LedgerError ledger_add_company(const char *nickname) {
    CHECK_DB();
    if (!nickname || !*nickname) {
        SET_ERR("nickname is empty"); return LEDGER_ERR_ARGS;
    }

    sqlite3_stmt *st = db_prepare(
        "SELECT 1 FROM ledger_company WHERE nickname=?");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_text(st, 1, nickname, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        sqlite3_finalize(st);
        SET_ERR("company '%s' already exists", nickname);
        return LEDGER_ERR_DUP_CODE;
    }
    LedgerError err = db_done(st, rc);
    if (err != LEDGER_OK) return err;

    st = db_prepare("INSERT INTO ledger_company(nickname) VALUES(?)");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_text(st, 1, nickname, -1, SQLITE_STATIC);
    return db_exec(st);
}
LedgerError ledger_init(const char *company_nickname) {
    CHECK_DB();
    if (!company_nickname || !*company_nickname) {
        SET_ERR("nickname is empty"); return LEDGER_ERR_ARGS;
    }

    g_company_id = 0;   /* from here on, any failure leaves none selected */
    sqlite3_stmt *st = db_prepare(
        "SELECT id FROM ledger_company WHERE nickname=?");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_text(st, 1, company_nickname, -1, SQLITE_STATIC);

    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        g_company_id = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return LEDGER_OK;
    }
    if (rc != SQLITE_DONE) return db_done(st, rc);
    sqlite3_finalize(st);
    SET_ERR("company '%s' not found", company_nickname);
    return LEDGER_ERR_COMPANY_NOT_FOUND;
}
void ledger_close(void) {
    if (g_db) {
        sqlite3_close(g_db);
        g_db         = NULL;
        g_company_id = 0;
    }
}
static uint64_t pow10_u64(int n) {
    uint64_t p = 1;
    while (n-- > 0) p *= 10;
    return p;
}

char *ledger_format_amount(int64_t amount, int decimal_places,
                           char *buf, size_t size) {
    if (decimal_places < 0)  decimal_places = 0;
    if (decimal_places > 18) decimal_places = 18;

    uint64_t mag = amount < 0 ? -(uint64_t)amount : (uint64_t)amount;
    uint64_t div = pow10_u64(decimal_places);
    const char *sign = amount < 0 ? "-" : "";

    if (decimal_places == 0)
        snprintf(buf, size, "%s%llu", sign, (unsigned long long)mag);
    else
        snprintf(buf, size, "%s%llu.%0*llu", sign,
                 (unsigned long long)(mag / div), decimal_places,
                 (unsigned long long)(mag % div));
    return buf;
}
LedgerError ledger_parse_amount(const char *text, int decimal_places,
                                int64_t *out_amount) {
    if (!text || !out_amount || decimal_places < 0 || decimal_places > 18) {
        SET_ERR("invalid arguments to ledger_parse_amount");
        return LEDGER_ERR_ARGS;
    }

    int64_t value = 0;
    int digits = 0, frac_digits = 0, seen_dot = 0;
    for (const char *c = text; *c; c++) {
        if (*c == '.' && !seen_dot && digits > 0) { seen_dot = 1; continue; }
        if (*c < '0' || *c > '9') goto bad;
        if (seen_dot && ++frac_digits > decimal_places) goto bad;
        if (value > (INT64_MAX - (*c - '0')) / 10) goto bad;
        value = value * 10 + (*c - '0');
        digits++;
    }
    if (digits == 0 || (seen_dot && frac_digits == 0)) goto bad;

    for (; frac_digits < decimal_places; frac_digits++) {
        if (value > INT64_MAX / 10) goto bad;
        value *= 10;
    }
    *out_amount = value;
    return LEDGER_OK;

bad:
    SET_ERR("'%s' is not a valid amount with %d decimal places",
            text, decimal_places);
    return LEDGER_ERR_BAD_AMOUNT;
}
LedgerError ledger_set_currency_decimal_places(const char *currency,
                                               int decimal_places) {
    CHECK_DB();
    if (!currency || !*currency || strlen(currency) > 15 ||
        decimal_places < 0 || decimal_places > 18) {
        SET_ERR("invalid currency or decimal places");
        return LEDGER_ERR_ARGS;
    }
    LedgerError err = txn_begin_immediate();   /* check and write together */
    if (err != LEDGER_OK) return err;
    sqlite3_stmt *st = db_prepare(
        "SELECT EXISTS (SELECT 1 FROM ledger_taccount t"
        "               JOIN ledger_account a ON a.id = t.account_id"
        "               WHERE a.currency = ?1)"
        "   AND COALESCE((SELECT decimal_places FROM ledger_currency"
        "                 WHERE code = ?1), 2) != ?2");
    if (!st) { txn_rollback(); return LEDGER_ERR_DB; }
    sqlite3_bind_text(st, 1, currency, -1, SQLITE_STATIC);
    sqlite3_bind_int (st, 2, decimal_places);
    int rc = sqlite3_step(st);
    int refused = rc == SQLITE_ROW && sqlite3_column_int(st, 0);
    err = db_done(st, rc == SQLITE_ROW ? SQLITE_DONE : rc);
    if (err == LEDGER_OK && refused) {
        SET_ERR("%s already has postings; its decimal places cannot change",
                currency);
        err = LEDGER_ERR_HAS_ENTRIES;
    }
    if (err == LEDGER_OK) {
        st = db_prepare(
            "INSERT INTO ledger_currency(code, decimal_places) VALUES(?,?)"
            " ON CONFLICT(code)"
            " DO UPDATE SET decimal_places=excluded.decimal_places");
        if (!st) err = LEDGER_ERR_DB;
        else {
            sqlite3_bind_text(st, 1, currency, -1, SQLITE_STATIC);
            sqlite3_bind_int (st, 2, decimal_places);
            err = db_exec(st);
        }
    }
    if (err != LEDGER_OK) { txn_rollback(); return err; }
    return txn_commit();
}
LedgerError ledger_set_currency_price(const char *currency, double price) {
    CHECK_DB();
    if (!currency || !*currency || strlen(currency) > 15 ||
        !(price >= 0) || !isfinite(price)) {   /* !(>=) also catches NaN */
        SET_ERR("invalid currency or price"); return LEDGER_ERR_ARGS;
    }
    sqlite3_stmt *st = db_prepare(
        "INSERT INTO ledger_currency(code, price) VALUES(?,?)"
        " ON CONFLICT(code) DO UPDATE SET price=excluded.price");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_text  (st, 1, currency, -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 2, price);
    return db_exec(st);
}

LedgerError ledger_get_currency_price(const char *currency,
                                      double *out_price) {
    CHECK_DB();
    if (!currency) { SET_ERR("currency is NULL"); return LEDGER_ERR_ARGS; }
    sqlite3_stmt *st = db_prepare(
        "SELECT price FROM ledger_currency WHERE code=?");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_text(st, 1, currency, -1, SQLITE_STATIC);

    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        if (out_price) *out_price = sqlite3_column_double(st, 0);
        sqlite3_finalize(st);
        return LEDGER_OK;
    }
    if (rc != SQLITE_DONE) return db_done(st, rc);
    sqlite3_finalize(st);
    SET_ERR("unknown currency '%s'", currency);
    return LEDGER_ERR_ARGS;
}
const char *ledger_nature_name(LedgerNature nature) {
    switch (nature) {
    case LEDGER_DEBITNORMAL:  return "DEBITNORMAL";
    case LEDGER_CREDITNORMAL: return "CREDITNORMAL";
    default:                  return NULL;
    }
}

LedgerNature ledger_nature_parse(const char *name) {
    if (name && strcmp(name, "DEBITNORMAL") == 0)  return LEDGER_DEBITNORMAL;
    if (name && strcmp(name, "CREDITNORMAL") == 0) return LEDGER_CREDITNORMAL;
    return LEDGER_NATURE_INVALID;
}
#define ACCOUNT_SELECT                                                   \
    "SELECT a.id, a.name, a.code, a.currency, COALESCE(a.parent_id, 0),"  \
    "       a.nature, COALESCE(c.decimal_places, 2)"                      \
    " FROM ledger_account a"                                              \
    " LEFT JOIN ledger_currency c ON c.code = a.currency"

static void fill_account(sqlite3_stmt *st, LedgerAccount *a) {
    a->id = sqlite3_column_int64(st, 0);
    col_text(st, 1, a->name,     sizeof(a->name));
    col_text(st, 2, a->code,     sizeof(a->code));
    col_text(st, 3, a->currency, sizeof(a->currency));
    a->parent_id      = sqlite3_column_int64(st, 4);
    a->nature         = ledger_nature_parse(
                            (const char *)sqlite3_column_text(st, 5));
    a->decimal_places = sqlite3_column_int(st, 6);
}
/* Steps a single-row account query, fills out and finalizes. */
static LedgerError fetch_account(sqlite3_stmt *st, LedgerAccount *out) {
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        if (out) fill_account(st, out);
        sqlite3_finalize(st);
        return LEDGER_OK;
    }
    if (rc != SQLITE_DONE) return db_done(st, rc);
    sqlite3_finalize(st);
    return LEDGER_ERR_ACCT_NOT_FOUND;
}

/* Quiet lookup for internal use: 1 found, 0 not found, -1 error.
   Unlike the public lookups it does not record "not found" as an error,
   so a successful operation never leaves a stale message behind. */
static int find_account(const char *code, LedgerAccount *out) {
    sqlite3_stmt *st = db_prepare(ACCOUNT_SELECT
        " WHERE a.code=? AND a.company_id=?");
    if (!st) return -1;
    sqlite3_bind_text (st, 1, code, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, g_company_id);
    LedgerError err = fetch_account(st, out);
    return err == LEDGER_OK ? 1 : err == LEDGER_ERR_ACCT_NOT_FOUND ? 0 : -1;
}
LedgerError ledger_get_account_by_id(int64_t id, LedgerAccount *out) {
    CHECK_INIT();
    sqlite3_stmt *st = db_prepare(ACCOUNT_SELECT
        " WHERE a.id=? AND a.company_id=?");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, g_company_id);

    LedgerError err = fetch_account(st, out);
    if (err == LEDGER_ERR_ACCT_NOT_FOUND)
        SET_ERR("account %lld not found", (long long)id);
    return err;
}

LedgerError ledger_get_account_by_code(const char *code, LedgerAccount *out) {
    CHECK_INIT();
    if (!code) { SET_ERR("code is NULL"); return LEDGER_ERR_ARGS; }
    sqlite3_stmt *st = db_prepare(ACCOUNT_SELECT
        " WHERE a.code=? AND a.company_id=?");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_text (st, 1, code, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, g_company_id);

    LedgerError err = fetch_account(st, out);
    if (err == LEDGER_ERR_ACCT_NOT_FOUND)
        SET_ERR("account code '%s' not found", code);
    return err;
}
LedgerError ledger_get_all_accounts(LedgerAccount **out, int *count) {
    CHECK_INIT();
    *out = NULL; *count = 0;

    sqlite3_stmt *st = db_prepare(ACCOUNT_SELECT
        " WHERE a.company_id=? ORDER BY a.code");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_int64(st, 1, g_company_id);

    LedgerAccount *arr = NULL;
    int n = 0, cap = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (GROW(arr, n, cap) != 0) {
            free(arr); sqlite3_finalize(st); return LEDGER_ERR_DB;
        }
        fill_account(st, &arr[n++]);
    }
    if (db_done(st, rc) != LEDGER_OK) { free(arr); return LEDGER_ERR_DB; }
    *out = arr; *count = n;
    return LEDGER_OK;
}

void ledger_accounts_free(LedgerAccount *accounts, int count) {
    (void)count;
    free(accounts);
}
/* Runs a COUNT(*) query with ?1 = id and ?2 = the active company.
   Returns the count, or -1 on any error. */
static int count_rows(const char *sql, int64_t id) {
    if (!g_company_id) { SET_ERR("no company selected"); return -1; }
    sqlite3_stmt *st = db_prepare(sql);
    if (!st) return -1;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, g_company_id);
    int rc = sqlite3_step(st);
    int n = rc == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    if (rc != SQLITE_ROW) SET_SQLERR();
    sqlite3_finalize(st);
    return n;
}

int ledger_has_children(int64_t account_id) {
    int n = count_rows(
        "SELECT COUNT(*) FROM ledger_account"
        " WHERE parent_id=? AND company_id=?", account_id);
    return n < 0 ? -1 : n > 0;
}

static int has_postings(int64_t account_id) {
    int n = count_rows(
        "SELECT COUNT(*) FROM ledger_taccount t"
        " JOIN ledger_account a ON a.id = t.account_id"
        " WHERE t.account_id=? AND a.company_id=?", account_id);
    return n < 0 ? -1 : n > 0;
}

LedgerError ledger_get_children(int64_t account_id, int64_t **ids,
                                int *count) {
    CHECK_INIT();
    *ids = NULL; *count = 0;
    sqlite3_stmt *st = db_prepare(
        "SELECT id FROM ledger_account"
        " WHERE parent_id IS ? AND company_id=? ORDER BY code");
    if (!st) return LEDGER_ERR_DB;
    if (account_id) sqlite3_bind_int64(st, 1, account_id);
    else            sqlite3_bind_null (st, 1);
    sqlite3_bind_int64(st, 2, g_company_id);

    int64_t *arr = NULL;
    int n = 0, cap = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (GROW(arr, n, cap) != 0) {
            free(arr); sqlite3_finalize(st); return LEDGER_ERR_DB;
        }
        arr[n++] = sqlite3_column_int64(st, 0);
    }
    if (db_done(st, rc) != LEDGER_OK) { free(arr); return LEDGER_ERR_DB; }
    *ids = arr; *count = n;
    return LEDGER_OK;
}
/* Writes the parent's code into parent ("" for a root account).
   Returns 0 on success, -1 if code is malformed. */
static int parent_code_of(const char *code, char *parent, size_t size) {
    size_t len = strlen(code);
    if (len == 0 || code[0] == '.' || code[len - 1] == '.' ||
        strstr(code, "..") || len >= size)
        return -1;

    const char *last_dot = strrchr(code, '.');
    size_t plen = last_dot ? (size_t)(last_dot - code) : 0;
    memcpy(parent, code, plen);
    parent[plen] = '\0';
    return 0;
}
static LedgerError insert_account(const char *name, const char *code,
                                  const char *currency, LedgerNature nature,
                                  LedgerAccount *out)
{
    if (!name || !*name || !currency || !*currency) {
        SET_ERR("name and currency are required"); return LEDGER_ERR_ARGS;
    }
    if (strlen(name) >= sizeof(((LedgerAccount *)0)->name) ||
        strlen(currency) >= sizeof(((LedgerAccount *)0)->currency) ||
        (code && strlen(code) >= sizeof(((LedgerAccount *)0)->code))) {
        SET_ERR("name, code or currency too long"
                " (limits: 255, 127, 15 bytes)");
        return LEDGER_ERR_ARGS;
    }
    char parent_code[128];
    if (!code || parent_code_of(code, parent_code, sizeof(parent_code))) {
        SET_ERR("malformed code '%s'", code ? code : "(null)");
        return LEDGER_ERR_BAD_CODE;
    }
    if (!ledger_nature_name(nature)) {
        SET_ERR("invalid nature %d", (int)nature);
        return LEDGER_ERR_BAD_NATURE;
    }
    int64_t parent_id = 0;
    if (parent_code[0]) {
        LedgerAccount parent;
        int found = find_account(parent_code, &parent);
        if (found < 0) return LEDGER_ERR_DB;
        if (!found) {
            SET_ERR("parent account '%s' not found", parent_code);
            return LEDGER_ERR_ACCT_NOT_FOUND;
        }
        int posted = has_postings(parent.id);
        if (posted < 0) return LEDGER_ERR_DB;
        if (posted) {
            SET_ERR("parent account '%s' already has postings",
                    parent_code);
            return LEDGER_ERR_HAS_ENTRIES;
        }
        parent_id = parent.id;
    }

    int exists = find_account(code, NULL);
    if (exists < 0) return LEDGER_ERR_DB;
    if (exists) {
        SET_ERR("account code '%s' already exists", code);
        return LEDGER_ERR_DUP_CODE;
    }
    sqlite3_stmt *st = db_prepare(
    "INSERT INTO ledger_currency(code) VALUES(?)"
    " ON CONFLICT(code) DO NOTHING");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_text(st, 1, currency, -1, SQLITE_STATIC);
    LedgerError err = db_exec(st);
    if (err != LEDGER_OK) return err;
    st = db_prepare(
        "INSERT INTO ledger_account"
        "(name, code, currency, parent_id, nature, company_id)"
        " VALUES(?,?,?,?,?,?)");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_text(st, 1, name,     -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, code,     -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, currency, -1, SQLITE_STATIC);
    if (parent_id) sqlite3_bind_int64(st, 4, parent_id);
    else           sqlite3_bind_null (st, 4);
    sqlite3_bind_text (st, 5, ledger_nature_name(nature), -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 6, g_company_id);
    err = db_exec(st);
    if (err != LEDGER_OK) return err;

    int64_t new_id = sqlite3_last_insert_rowid(g_db);
    st = db_prepare(
        "INSERT INTO ledger_balance(account_id, balance) VALUES(?, 0)");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_int64(st, 1, new_id);
    err = db_exec(st);
    if (err != LEDGER_OK) return err;

    return out ? ledger_get_account_by_id(new_id, out) : LEDGER_OK;
}
LedgerError ledger_add_account(const char *name, const char *code,
                               const char *currency, LedgerNature nature,
                               LedgerAccount *out)
{
    CHECK_INIT();
    LedgerError err = txn_begin_immediate();
    if (err != LEDGER_OK) return err;
    err = insert_account(name, code, currency, nature, out);
    if (err != LEDGER_OK) { txn_rollback(); return err; }
    return txn_commit();
}
LedgerError ledger_add_account_if_not_exists(const char *name,
                               const char *code, const char *currency,
                               LedgerNature nature, LedgerAccount *out)
{
    CHECK_INIT();
    LedgerError err = txn_begin_immediate();
    if (err != LEDGER_OK) return err;

    LedgerAccount existing;
    int found = code ? find_account(code, &existing) : 0;
    if (found < 0) {
        err = LEDGER_ERR_DB;
    } else if (!found) {
        err = insert_account(name, code, currency, nature, out);
    } else if (!currency || strcmp(existing.currency, currency) != 0 ||
               existing.nature != nature) {
        SET_ERR("account '%s' exists with a different currency or nature",
                code);
        err = LEDGER_ERR_DUP_CODE;
    } else if (out) {
        *out = existing;
    }

    if (err != LEDGER_OK) { txn_rollback(); return err; }
    return txn_commit();
}
typedef struct {
    const char   *name;
    const char   *code;
    LedgerNature  nature;
} MinChartEntry;

/* Chart of accounts for a US professional / household, in the usual
   US textbook order: assets, liabilities, equity, revenue, expenses.
   Entries must appear in parent-before-child order. */
static const MinChartEntry MINIMAL_CHART[] = {
    /* ---- 1 Assets ---- */
    {"ASSETS",                        "1",             LEDGER_DEBITNORMAL},
    {"CURRENT ASSETS",                "1.1",           LEDGER_DEBITNORMAL},
    {"CASH ON HAND",                  "1.1.1",         LEDGER_DEBITNORMAL},
    {"CHECKING ACCOUNT",              "1.1.2",         LEDGER_DEBITNORMAL},
    {"SAVINGS ACCOUNT",               "1.1.3",         LEDGER_DEBITNORMAL},
    {"ACCOUNTS RECEIVABLE",           "1.1.4",         LEDGER_DEBITNORMAL},
    {"INVESTMENTS",                   "1.2",           LEDGER_DEBITNORMAL},
    {"FIXED INCOME",                  "1.2.1",         LEDGER_DEBITNORMAL},
    {"EQUITIES",                      "1.2.2",         LEDGER_DEBITNORMAL},
    {"PROPERTY AND EQUIPMENT",        "1.3",           LEDGER_DEBITNORMAL},
    {"VEHICLE",                       "1.3.1",         LEDGER_DEBITNORMAL},
    {"REAL ESTATE",                   "1.3.2",         LEDGER_DEBITNORMAL},
    {"COMPUTER EQUIPMENT",            "1.3.3",         LEDGER_DEBITNORMAL},
    /* ---- 2 Liabilities ---- */
    {"LIABILITIES",                   "2",             LEDGER_CREDITNORMAL},
    {"CURRENT LIABILITIES",           "2.1",           LEDGER_CREDITNORMAL},
    {"CREDIT CARD",                   "2.1.1",         LEDGER_CREDITNORMAL},
    {"LONG-TERM LIABILITIES",         "2.2",           LEDGER_CREDITNORMAL},
    {"AUTO LOAN",                     "2.2.1",         LEDGER_CREDITNORMAL},
    {"MORTGAGE",                      "2.2.2",         LEDGER_CREDITNORMAL},
    {"PERSONAL LOANS",                "2.2.3",         LEDGER_CREDITNORMAL},
    /* ---- 3 Equity ---- */
    {"EQUITY",                        "3",             LEDGER_CREDITNORMAL},
    {"OWNER'S CAPITAL",               "3.1",           LEDGER_CREDITNORMAL},
    {"RETAINED EARNINGS",             "3.2",           LEDGER_CREDITNORMAL},
    /* ---- 4 Revenue ---- */
    {"REVENUE",                       "4",             LEDGER_CREDITNORMAL},
    {"SALARY / PROFESSIONAL FEES",    "4.1",           LEDGER_CREDITNORMAL},
    {"INVESTMENT INCOME",             "4.2",           LEDGER_CREDITNORMAL},
    {"OTHER INCOME",                  "4.3",           LEDGER_CREDITNORMAL},
    /* ---- 5 Expenses ---- */
    {"EXPENSES",                      "5",             LEDGER_DEBITNORMAL},
    {"HOUSING",                       "5.1",           LEDGER_DEBITNORMAL},
    {"RENT / HOA FEES",               "5.1.1",         LEDGER_DEBITNORMAL},
    {"PROPERTY TAX",                  "5.1.2",         LEDGER_DEBITNORMAL},
    {"UTILITIES",                     "5.1.3",         LEDGER_DEBITNORMAL},
    {"GROCERIES",                     "5.2",           LEDGER_DEBITNORMAL},
    {"TRANSPORTATION",                "5.3",           LEDGER_DEBITNORMAL},
    {"FUEL",                          "5.3.1",         LEDGER_DEBITNORMAL},
    {"AUTO INSURANCE / REGISTRATION", "5.3.2",         LEDGER_DEBITNORMAL},
    {"AUTO MAINTENANCE",              "5.3.3",         LEDGER_DEBITNORMAL},
    {"PUBLIC TRANSIT / RIDESHARE",    "5.3.4",         LEDGER_DEBITNORMAL},
    {"EDUCATION",                     "5.4",           LEDGER_DEBITNORMAL},
    {"CHILDREN'S SCHOOL TUITION",     "5.4.1",         LEDGER_DEBITNORMAL},
    {"COURSES AND TRAINING",          "5.4.2",         LEDGER_DEBITNORMAL},
    {"HEALTHCARE",                    "5.5",           LEDGER_DEBITNORMAL},
    {"HEALTH INSURANCE PREMIUM",      "5.5.1",         LEDGER_DEBITNORMAL},
    {"MEDICAL AND DENTAL VISITS",     "5.5.2",         LEDGER_DEBITNORMAL},
    {"PRESCRIPTIONS",                 "5.5.3",         LEDGER_DEBITNORMAL},
    {"ENTERTAINMENT AND CULTURE",     "5.6",           LEDGER_DEBITNORMAL},
    {"CLOTHING",                      "5.7",           LEDGER_DEBITNORMAL},
    {"BUSINESS SOFTWARE",             "5.8",           LEDGER_DEBITNORMAL},
    {"MISCELLANEOUS",                 "5.9",           LEDGER_DEBITNORMAL},
    {NULL, NULL, LEDGER_NATURE_INVALID}
};
LedgerError ledger_create_minimal_chart(int *created) {
    CHECK_INIT();
    if (created) *created = 0;

    LedgerError err = txn_begin_immediate();
    if (err != LEDGER_OK) return err;

    for (const MinChartEntry *e = MINIMAL_CHART; e->name; e++) {
        LedgerAccount existing;
        int found = find_account(e->code, &existing);
        if (found < 0) { txn_rollback(); return LEDGER_ERR_DB; }
        if (found && (strcmp(existing.currency, "USD") != 0 ||
                      existing.nature != e->nature)) {
            SET_ERR("account '%s' exists with a different currency or nature",
                    e->code);
            txn_rollback();
            return LEDGER_ERR_DUP_CODE;
        }
        if (found) continue;
        err = insert_account(e->name, e->code, "USD", e->nature, NULL);
        if (err != LEDGER_OK) { txn_rollback(); return err; }
        if (created) (*created)++;
    }
    return txn_commit();
}
typedef struct {
    LedgerAccount acct;
    int64_t       amount;
    int           is_debit;
    const char   *info;
    int64_t       balance_prev;
    int64_t       balance;
} ResolvedPosting;
static LedgerError resolve_posting(const LedgerPosting *p, int is_debit,
                                   int line_no, ResolvedPosting *out)
{
    const char *side = is_debit ? "debit" : "credit";

    if (p->amount <= 0) {
        SET_ERR("%s line %d: amount must be > 0", side, line_no);
        return is_debit ? LEDGER_ERR_DEBIT_AMT : LEDGER_ERR_CRED_AMT;
    }

    LedgerError err;
    if (p->account_id != 0)
        err = ledger_get_account_by_id(p->account_id, &out->acct);
    else if (p->account_code[0] != '\0')
        err = ledger_get_account_by_code(p->account_code, &out->acct);
    else {
        SET_ERR("%s line %d: no account_id or account_code", side, line_no);
        return LEDGER_ERR_ARGS;
    }
    if (err != LEDGER_OK) {
        char why[256];
        snprintf(why, sizeof why, "%s", ledger_last_error());
        SET_ERR("%s line %d: %s", side, line_no, why);
        return err;
    }

    if (strcmp(p->currency, out->acct.currency) != 0) {
        SET_ERR("%s line %d: currency '%s' does not match account '%s' (%s)",
                side, line_no, p->currency,
                out->acct.code, out->acct.currency);
        return is_debit ? LEDGER_ERR_DEBIT_CURR : LEDGER_ERR_CRED_CURR;
    }

    int branch = ledger_has_children(out->acct.id);
    if (branch < 0) return LEDGER_ERR_DB;
    if (branch) {
        SET_ERR("%s line %d: account '%s' is not a leaf",
                side, line_no, out->acct.code);
        return LEDGER_ERR_ACCT_CHILDREN;
    }

    out->amount   = p->amount;
    out->is_debit = is_debit;
    out->info     = p->info;
    return LEDGER_OK;
}
#define MAX_ENTRY_CURRENCIES 16

static LedgerError check_currency_balance(const ResolvedPosting *p, int n) {
    struct { const char *currency; int64_t debits, credits; }
        sums[MAX_ENTRY_CURRENCIES];
    int n_curr = 0;

    for (int i = 0; i < n; i++) {
        int j = 0;
        while (j < n_curr && strcmp(sums[j].currency, p[i].acct.currency))
            j++;
        if (j == n_curr) {
            if (n_curr == MAX_ENTRY_CURRENCIES) {
                SET_ERR("entry has more than %d currencies",
                        MAX_ENTRY_CURRENCIES);
                return LEDGER_ERR_ARGS;
            }
            sums[j].currency = p[i].acct.currency;
            sums[j].debits = sums[j].credits = 0;
            n_curr++;
        }
        int64_t *sum = p[i].is_debit ? &sums[j].debits : &sums[j].credits;
        if (__builtin_add_overflow(*sum, p[i].amount, sum)) {
            SET_ERR("currency %s: total exceeds the largest amount",
                    sums[j].currency);
            return LEDGER_ERR_OVERFLOW;
        }
    }

    for (int j = 0; j < n_curr; j++) {
        if (sums[j].debits != sums[j].credits) {
            SET_ERR("currency %s: debits (%lld) != credits (%lld)",
                    sums[j].currency, (long long)sums[j].debits,
                    (long long)sums[j].credits);
            return LEDGER_ERR_CR_DB_EQUAL;
        }
    }
    return LEDGER_OK;
}
/* 1 if this line raises the account balance, 0 if it lowers it. */
static int increases_balance(const ResolvedPosting *p) {
    return p->is_debit == (p->acct.nature == LEDGER_DEBITNORMAL);
}

static int cmp_posting(const void *a, const void *b) {
    const ResolvedPosting *pa = a, *pb = b;
    if (pa->acct.id < pb->acct.id) return -1;
    if (pa->acct.id > pb->acct.id) return  1;
    return increases_balance(pb) - increases_balance(pa);
}
static LedgerError compute_balances(ResolvedPosting *p, int n) {
    for (int i = 0; i < n; i++) {
        if (i > 0 && p[i].acct.id == p[i - 1].acct.id) {
            p[i].balance_prev = p[i - 1].balance;
        } else {
            LedgerError err = ledger_get_balance(p[i].acct.id,
                                                 &p[i].balance_prev);
            if (err != LEDGER_OK) return err;
        }
        int64_t delta = increases_balance(&p[i]) ? p[i].amount
                                                 : -p[i].amount;
        if (__builtin_add_overflow(p[i].balance_prev, delta, &p[i].balance)) {
            SET_ERR("account %s %s: balance exceeds the largest amount",
                    p[i].acct.code, p[i].acct.name);
            return LEDGER_ERR_OVERFLOW;
        }
        if (p[i].balance < 0) {
            char amount[32];
            ledger_format_amount(p[i].balance, p[i].acct.decimal_places,
                                 amount, sizeof amount);
            SET_ERR("account %s %s would have a negative balance: %s %s",
                    p[i].acct.code, p[i].acct.name, amount,
                    p[i].acct.currency);
            return LEDGER_ERR_NEG_BALANCE;
        }
    }
    return LEDGER_OK;
}
static LedgerError prepare_entry(const LedgerEntry *e,
                                 ResolvedPosting **out, int *n_out)
{
    *out = NULL; *n_out = 0;
    if (!e) { SET_ERR("entry is NULL"); return LEDGER_ERR_ARGS; }
    if (e->n_debits <= 0) {
        SET_ERR("journal entry has no debit lines");
        return LEDGER_ERR_MISSING_DEBIT;
    }
    if (e->n_credits <= 0) {
        SET_ERR("journal entry has no credit lines");
        return LEDGER_ERR_MISSING_CREDIT;
    }

    if (!e->debits || !e->credits) {
        SET_ERR("entry has line counts but no line arrays");
        return LEDGER_ERR_ARGS;
    }
    for (int k = 0; k < e->n_props; k++) {
        if (!e->prop_keys || !e->prop_vals ||
            !e->prop_keys[k] || !e->prop_vals[k] || !*e->prop_keys[k]) {
            SET_ERR("property %d: key and value are required", k + 1);
            return LEDGER_ERR_ARGS;
        }
    }
    int n = e->n_debits + e->n_credits;
    ResolvedPosting *p = calloc((size_t)n, sizeof *p);
    if (!p) { SET_ERR("out of memory"); return LEDGER_ERR_DB; }

    LedgerError err = LEDGER_OK;
    for (int i = 0; i < e->n_debits && err == LEDGER_OK; i++)
        err = resolve_posting(&e->debits[i], 1, i + 1, &p[i]);
    for (int i = 0; i < e->n_credits && err == LEDGER_OK; i++)
        err = resolve_posting(&e->credits[i], 0, i + 1,
                              &p[e->n_debits + i]);
    if (err == LEDGER_OK)
        err = check_currency_balance(p, n);
    if (err == LEDGER_OK) {
        qsort(p, (size_t)n, sizeof *p, cmp_posting);
        err = compute_balances(p, n);
    }

    if (err != LEDGER_OK) { free(p); return err; }
    *out = p; *n_out = n;
    return LEDGER_OK;
}
static LedgerError insert_journal_header(const LedgerEntry *e,
                                         int64_t *journal_id) {
    sqlite3_stmt *st = db_prepare(
        "INSERT INTO ledger_journal"
        "(date, description, system, end2end, info, company_id)"
        " VALUES(?,?,?,?,?,?)");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_text (st, 1, e->date,        -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 2, e->description, -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 3, e->system,      -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 4, e->end2end,     -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 5, e->info,        -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 6, g_company_id);
    LedgerError err = db_exec(st);
    *journal_id = sqlite3_last_insert_rowid(g_db);
    return err;
}
static LedgerError insert_journal_props(const LedgerEntry *e,
                                        int64_t journal_id) {
    for (int k = 0; k < e->n_props; k++) {
        sqlite3_stmt *st = db_prepare(
            "INSERT INTO ledger_journalattr(property, value, journal_id)"
            " VALUES(?,?,?)");
        if (!st) return LEDGER_ERR_DB;
        sqlite3_bind_text (st, 1, e->prop_keys[k], -1, SQLITE_STATIC);
        sqlite3_bind_text (st, 2, e->prop_vals[k], -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 3, journal_id);
        LedgerError err = db_exec(st);
        if (err != LEDGER_OK) return err;
    }
    return LEDGER_OK;
}
static LedgerError insert_posting(const ResolvedPosting *p,
                                  int64_t journal_id) {
    sqlite3_stmt *st = db_prepare(
        "INSERT INTO ledger_taccount"
        "(debit, credit, account_id, journal_id, info, balance_prev, balance)"
        " VALUES(?,?,?,?,?,?,?)");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_int64(st, 1, p->is_debit ? p->amount : 0);
    sqlite3_bind_int64(st, 2, p->is_debit ? 0 : p->amount);
    sqlite3_bind_int64(st, 3, p->acct.id);
    sqlite3_bind_int64(st, 4, journal_id);
    sqlite3_bind_text (st, 5, p->info ? p->info : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 6, p->balance_prev);
    sqlite3_bind_int64(st, 7, p->balance);
    LedgerError err = db_exec(st);
    if (err != LEDGER_OK) return err;

    st = db_prepare("UPDATE ledger_balance SET balance=? WHERE account_id=?");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_int64(st, 1, p->balance);
    sqlite3_bind_int64(st, 2, p->acct.id);
    return db_exec(st);
}
LedgerError ledger_validate_journal(const LedgerEntry *entry) {
    CHECK_INIT();
    ResolvedPosting *p = NULL;
    int n = 0;
    LedgerError err = prepare_entry(entry, &p, &n);
    free(p);
    return err;
}

LedgerError ledger_post_journal(const LedgerEntry *entry, int64_t *out_id) {
    CHECK_INIT();
    LedgerError err = txn_begin_immediate();
    if (err != LEDGER_OK) return err;

    ResolvedPosting *p = NULL;
    int n = 0;
    int64_t journal_id = 0;
    err = prepare_entry(entry, &p, &n);
    if (err == LEDGER_OK) err = insert_journal_header(entry, &journal_id);
    if (err == LEDGER_OK) err = insert_journal_props(entry, journal_id);
    for (int i = 0; i < n && err == LEDGER_OK; i++)
        err = insert_posting(&p[i], journal_id);
    free(p);

    if (err != LEDGER_OK) { txn_rollback(); return err; }
    err = txn_commit();
    if (err == LEDGER_OK && out_id) *out_id = journal_id;
    return err;
}
LedgerError ledger_get_balance(int64_t account_id, int64_t *out_balance) {
    CHECK_INIT();
    sqlite3_stmt *st = db_prepare(
        "SELECT b.balance FROM ledger_balance b"
        " JOIN ledger_account a ON a.id = b.account_id"
        " WHERE b.account_id=? AND a.company_id=?");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_int64(st, 1, account_id);
    sqlite3_bind_int64(st, 2, g_company_id);

    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        if (out_balance) *out_balance = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return LEDGER_OK;
    }
    if (rc != SQLITE_DONE) return db_done(st, rc);
    sqlite3_finalize(st);
    SET_ERR("no balance row for account %lld", (long long)account_id);
    return LEDGER_ERR_ACCT_NOT_FOUND;
}
static LedgerError collect_inconsistencies(sqlite3_stmt *st, int type,
    LedgerInconsistency **out, int *count)
{
    LedgerInconsistency *arr = NULL;
    int n = 0, cap = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (GROW(arr, n, cap) != 0) {
            free(arr); sqlite3_finalize(st); return LEDGER_ERR_DB;
        }
        LedgerInconsistency *inc = &arr[n++];
        memset(inc, 0, sizeof *inc);
        inc->type       = type;
        inc->account_id = sqlite3_column_int64(st, 0);
        col_text(st, 1, inc->account_code, sizeof inc->account_code);
        col_text(st, 2, inc->account_name, sizeof inc->account_name);
        col_text(st, 3, inc->currency,     sizeof inc->currency);
        inc->decimal_places = sqlite3_column_int(st, 4);
        int64_t a = sqlite3_column_int64(st, 5);
        int64_t b = sqlite3_column_int64(st, 6);
        inc->journal_id = sqlite3_column_int64(st, 7);
        if (type == LEDGER_INC_CACHE) {
            inc->cached = a; inc->calculated = b;
        } else if (type == LEDGER_INC_UNBALANCED) {
            inc->debits = a; inc->credits = b;
        }
    }
    if (db_done(st, rc) != LEDGER_OK) { free(arr); return LEDGER_ERR_DB; }
    *out = arr; *count = n;
    return LEDGER_OK;
}

void ledger_inconsistencies_free(LedgerInconsistency *arr, int count) {
    (void)count;
    free(arr);
}
LedgerError ledger_check_journal_balance(
    LedgerInconsistency **out, int *count)
{
    CHECK_INIT();
    *out = NULL; *count = 0;

    sqlite3_stmt *st = db_prepare(
        "SELECT 0, NULL, NULL, a.currency, COALESCE(c.decimal_places, 2),"
        "       SUM(t.debit), SUM(t.credit), t.journal_id"
        " FROM ledger_taccount t"
        " JOIN ledger_account a ON a.id = t.account_id"
        " JOIN ledger_journal j ON j.id = t.journal_id"
        " LEFT JOIN ledger_currency c ON c.code = a.currency"
        " WHERE j.company_id=?1"
        " GROUP BY t.journal_id, a.currency"
        " HAVING SUM(t.debit) != SUM(t.credit)"
        " UNION ALL"
        " SELECT 0, NULL, NULL, '', 2, 0, 0, j.id"
        " FROM ledger_journal j"
        " WHERE j.company_id=?1 AND NOT EXISTS"
        "   (SELECT 1 FROM ledger_taccount t WHERE t.journal_id = j.id)"
        " ORDER BY 8");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_int64(st, 1, g_company_id);
    return collect_inconsistencies(st, LEDGER_INC_UNBALANCED, out, count);
}
LedgerError ledger_check_leaf_balances(int fix,
    LedgerInconsistency **out, int *count)
{
    CHECK_INIT();
    *out = NULL; *count = 0;

    LedgerError err = fix ? txn_begin_immediate() : LEDGER_OK;
    if (err != LEDGER_OK) return err;

    sqlite3_stmt *st = db_prepare(
        "SELECT a.id, a.code, a.name, a.currency,"
        "       COALESCE(c.decimal_places, 2), b.balance,"
        "       CASE a.nature"
        "         WHEN 'DEBITNORMAL'"
        "           THEN COALESCE(SUM(t.debit - t.credit), 0)"
        "         ELSE COALESCE(SUM(t.credit - t.debit), 0)"
        "       END AS calculated, 0"
        " FROM ledger_account a"
        " JOIN ledger_balance b ON b.account_id = a.id"
        " LEFT JOIN ledger_taccount t ON t.account_id = a.id"
        " LEFT JOIN ledger_currency c ON c.code = a.currency"
        " WHERE a.company_id=?"
        "   AND NOT EXISTS (SELECT 1 FROM ledger_account k"
        "                   WHERE k.parent_id = a.id)"
        " GROUP BY a.id"
        " HAVING b.balance != calculated"
        " ORDER BY a.code");
    if (!st) err = LEDGER_ERR_DB;
    else {
        sqlite3_bind_int64(st, 1, g_company_id);
        err = collect_inconsistencies(st, LEDGER_INC_CACHE, out, count);
    }
    for (int i = 0; fix && err == LEDGER_OK && i < *count; i++) {
        if ((*out)[i].calculated < 0) continue;   /* see the text above */
        st = db_prepare(
            "UPDATE ledger_balance SET balance=? WHERE account_id=?");
        if (!st) { err = LEDGER_ERR_DB; break; }
        sqlite3_bind_int64(st, 1, (*out)[i].calculated);
        sqlite3_bind_int64(st, 2, (*out)[i].account_id);
        err = db_exec(st);
    }
    if (fix) {
        if (err == LEDGER_OK) err = txn_commit();
        else                  txn_rollback();
    }
    if (err != LEDGER_OK) { free(*out); *out = NULL; *count = 0; }
    return err;
}
LedgerError ledger_check_non_leaf_entries(
    LedgerInconsistency **out, int *count)
{
    CHECK_INIT();
    *out = NULL; *count = 0;

    sqlite3_stmt *st = db_prepare(
        "SELECT a.id, a.code, a.name, a.currency, 0, 0, 0, 0"
        " FROM ledger_account a"
        " WHERE a.company_id=?"
        "   AND EXISTS (SELECT 1 FROM ledger_account k"
        "               WHERE k.parent_id = a.id)"
        "   AND EXISTS (SELECT 1 FROM ledger_taccount t"
        "               WHERE t.account_id = a.id)"
        " ORDER BY a.code");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_int64(st, 1, g_company_id);
    return collect_inconsistencies(st, LEDGER_INC_NOLEAF, out, count);
}
LedgerError ledger_check_code_prefixes(
    LedgerInconsistency **out, int *count)
{
    CHECK_INIT();
    *out = NULL; *count = 0;

    sqlite3_stmt *st = db_prepare(
        "SELECT c.id, c.code, c.name, c.currency, 0, 0, 0, 0"
        " FROM ledger_account c"
        " JOIN ledger_account p ON p.id = c.parent_id"
        " WHERE c.company_id=?1"
        "   AND (substr(c.code, 1, length(p.code) + 1) != (p.code || '.')"
        "        OR instr(substr(c.code, length(p.code) + 2), '.') > 0)"
        " UNION ALL"
        " SELECT c.id, c.code, c.name, c.currency, 0, 0, 0, 0"
        " FROM ledger_account c"
        " WHERE c.company_id=?1 AND c.parent_id IS NULL"
        "   AND instr(c.code, '.') > 0"
        " ORDER BY 2");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_int64(st, 1, g_company_id);
    return collect_inconsistencies(st, LEDGER_INC_PREFIX, out, count);
}
#define JOURNAL_SELECT \
    "SELECT id, date, description, system, end2end, info" \
    " FROM ledger_journal"

static void fill_journal_header(sqlite3_stmt *st, LedgerJournalHeader *h) {
    h->id = sqlite3_column_int64(st, 0);
    col_text(st, 1, h->date,        sizeof h->date);
    col_text(st, 2, h->description, sizeof h->description);
    col_text(st, 3, h->system,      sizeof h->system);
    col_text(st, 4, h->end2end,     sizeof h->end2end);
    col_text(st, 5, h->info,        sizeof h->info);
}
LedgerError ledger_get_journal(int64_t journal_id, LedgerJournalHeader *out) {
    CHECK_INIT();
    if (!out) { SET_ERR("out is NULL"); return LEDGER_ERR_ARGS; }
    sqlite3_stmt *st = db_prepare(JOURNAL_SELECT
        " WHERE id=? AND company_id=?");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_int64(st, 1, journal_id);
    sqlite3_bind_int64(st, 2, g_company_id);

    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        fill_journal_header(st, out);
        sqlite3_finalize(st);
        return LEDGER_OK;
    }
    if (rc != SQLITE_DONE) return db_done(st, rc);
    sqlite3_finalize(st);
    SET_ERR("journal entry %lld not found", (long long)journal_id);
    return LEDGER_ERR_JOURNAL_NOT_FOUND;
}
LedgerError ledger_get_journals(LedgerJournalHeader **out, int *count) {
    CHECK_INIT();
    *out = NULL; *count = 0;

    sqlite3_stmt *st = db_prepare(JOURNAL_SELECT
        " WHERE company_id=? ORDER BY id DESC");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_int64(st, 1, g_company_id);

    LedgerJournalHeader *arr = NULL;
    int n = 0, cap = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (GROW(arr, n, cap) != 0) {
            free(arr); sqlite3_finalize(st); return LEDGER_ERR_DB;
        }
        fill_journal_header(st, &arr[n++]);
    }
    if (db_done(st, rc) != LEDGER_OK) { free(arr); return LEDGER_ERR_DB; }
    *out = arr; *count = n;
    return LEDGER_OK;
}

void ledger_journals_free(LedgerJournalHeader *arr, int count) {
    (void)count;
    free(arr);
}
LedgerError ledger_get_journal_lines(int64_t journal_id,
    LedgerJournalLine **out, int *count)
{
    CHECK_INIT();
    *out = NULL; *count = 0;

    sqlite3_stmt *st = db_prepare(
        "SELECT t.debit > 0, a.code, a.name,"
        "       t.debit + t.credit, a.currency, t.info,"
        "       COALESCE(c.decimal_places, 2)"
        " FROM ledger_taccount t"
        " JOIN ledger_account a ON a.id = t.account_id"
        " LEFT JOIN ledger_currency c ON c.code = a.currency"
        " WHERE t.journal_id=? AND a.company_id=?"
        " ORDER BY t.id");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_int64(st, 1, journal_id);
    sqlite3_bind_int64(st, 2, g_company_id);

    LedgerJournalLine *arr = NULL;
    int n = 0, cap = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (GROW(arr, n, cap) != 0) {
            free(arr); sqlite3_finalize(st); return LEDGER_ERR_DB;
        }
        LedgerJournalLine *l = &arr[n++];
        l->side = sqlite3_column_int(st, 0) ? 'D' : 'C';
        col_text(st, 1, l->account_code, sizeof l->account_code);
        col_text(st, 2, l->account_name, sizeof l->account_name);
        l->amount = sqlite3_column_int64(st, 3);
        col_text(st, 4, l->currency, sizeof l->currency);
        col_text(st, 5, l->info,     sizeof l->info);
        l->decimal_places = sqlite3_column_int(st, 6);
    }
    if (db_done(st, rc) != LEDGER_OK) { free(arr); return LEDGER_ERR_DB; }
    *out = arr; *count = n;
    return LEDGER_OK;
}

void ledger_journal_lines_free(LedgerJournalLine *arr, int count) {
    (void)count;
    free(arr);
}
LedgerError ledger_get_journal_props(int64_t journal_id,
    char ***keys_out, char ***vals_out, int *count)
{
    CHECK_INIT();
    *keys_out = NULL; *vals_out = NULL; *count = 0;

    sqlite3_stmt *st = db_prepare(
        "SELECT p.property, p.value FROM ledger_journalattr p"
        " JOIN ledger_journal j ON j.id = p.journal_id"
        " WHERE p.journal_id=? AND j.company_id=? ORDER BY p.id");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_int64(st, 1, journal_id);
    sqlite3_bind_int64(st, 2, g_company_id);

    char **keys = NULL, **vals = NULL;
    int n = 0, cap_k = 0, cap_v = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (GROW(keys, n, cap_k) != 0 || GROW(vals, n, cap_v) != 0) {
            ledger_journal_props_free(keys, vals, n);
            sqlite3_finalize(st); return LEDGER_ERR_DB;
        }
        const char *k = (const char *)sqlite3_column_text(st, 0);
        const char *v = (const char *)sqlite3_column_text(st, 1);
        keys[n] = strdup(k ? k : "");
        vals[n] = strdup(v ? v : "");
        n++;
        if (!keys[n - 1] || !vals[n - 1]) {
            SET_ERR("out of memory");
            ledger_journal_props_free(keys, vals, n);
            sqlite3_finalize(st); return LEDGER_ERR_DB;
        }
    }
    if (db_done(st, rc) != LEDGER_OK) {
        ledger_journal_props_free(keys, vals, n);
        return LEDGER_ERR_DB;
    }
    *keys_out = keys; *vals_out = vals; *count = n;
    return LEDGER_OK;
}

void ledger_journal_props_free(char **keys, char **vals, int count) {
    for (int i = 0; i < count; i++) { free(keys[i]); free(vals[i]); }
    free(keys);
    free(vals);
}
LedgerError ledger_get_taccount_lines(int64_t account_id,
    LedgerTAccountLine **out, int *count)
{
    CHECK_INIT();
    *out = NULL; *count = 0;

    sqlite3_stmt *st = db_prepare(
        "SELECT t.journal_id, j.date, j.description,"
        "       t.debit, t.credit, t.balance_prev, t.balance"
        " FROM ledger_taccount t"
        " JOIN ledger_journal j ON j.id = t.journal_id"
        " WHERE t.account_id=? AND j.company_id=?"
        " ORDER BY t.id");
    if (!st) return LEDGER_ERR_DB;
    sqlite3_bind_int64(st, 1, account_id);
    sqlite3_bind_int64(st, 2, g_company_id);

    LedgerTAccountLine *arr = NULL;
    int n = 0, cap = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (GROW(arr, n, cap) != 0) {
            free(arr); sqlite3_finalize(st); return LEDGER_ERR_DB;
        }
        LedgerTAccountLine *l = &arr[n++];
        l->journal_id = sqlite3_column_int64(st, 0);
        col_text(st, 1, l->date,        sizeof l->date);
        col_text(st, 2, l->description, sizeof l->description);
        l->debit        = sqlite3_column_int64(st, 3);
        l->credit       = sqlite3_column_int64(st, 4);
        l->balance_prev = sqlite3_column_int64(st, 5);
        l->balance      = sqlite3_column_int64(st, 6);
    }
    if (db_done(st, rc) != LEDGER_OK) { free(arr); return LEDGER_ERR_DB; }
    *out = arr; *count = n;
    return LEDGER_OK;
}

void ledger_taccount_lines_free(LedgerTAccountLine *arr, int count) {
    (void)count;
    free(arr);
}
