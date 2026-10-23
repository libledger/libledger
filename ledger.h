#ifndef LEDGER_H
#define LEDGER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    LEDGER_OK                    =   0,
    LEDGER_ERR_ACCT_CHILDREN     =  -1,  /* account has children */
    LEDGER_ERR_ACCT_NOT_FOUND    =  -2,  /* account not found */
    LEDGER_ERR_CRED_AMT          =  -3,  /* credit amount must be > 0 */
    LEDGER_ERR_CRED_CURR         =  -4,  /* credit currency != account's */
    LEDGER_ERR_CR_DB_EQUAL       =  -5,  /* sum of debits != sum of credits */
    LEDGER_ERR_DEBIT_AMT         =  -6,  /* debit amount must be > 0 */
    LEDGER_ERR_DEBIT_CURR        =  -7,  /* debit currency != account's */
    LEDGER_ERR_MISSING_CREDIT    =  -8,  /* no credit lines */
    LEDGER_ERR_MISSING_DEBIT     =  -9,  /* no debit lines */
    LEDGER_ERR_NEG_BALANCE       = -10,  /* balance would go below zero */
    LEDGER_ERR_NO_COMPANY        = -11,  /* ledger_init() not called */
    LEDGER_ERR_HAS_ENTRIES       = -12,  /* already has postings */
    LEDGER_ERR_BAD_CODE          = -13,  /* code empty or malformed */
    LEDGER_ERR_BAD_NATURE        = -14,  /* invalid account nature */
    LEDGER_ERR_COMPANY_NOT_FOUND = -15,  /* no company with that nickname */
    LEDGER_ERR_BAD_AMOUNT        = -16,  /* unparseable decimal amount */
    LEDGER_ERR_JOURNAL_NOT_FOUND = -17,  /* no such journal entry */
    LEDGER_ERR_OVERFLOW          = -18,  /* amount sum exceeds int64 */
    LEDGER_ERR_DUP_CODE          = -19,  /* account code already in use */
    LEDGER_ERR_DB                = -99,  /* SQLite error or out of memory */
    LEDGER_ERR_ARGS              = -100, /* invalid arguments */
} LedgerError;
typedef enum {
    LEDGER_NATURE_INVALID = 0,
    LEDGER_DEBITNORMAL    = 1,  /* assets, expenses */
    LEDGER_CREDITNORMAL   = 2   /* liabilities, equity, revenues */
} LedgerNature;
typedef struct {
    int64_t      id;
    char         name[256];
    char         code[128];
    char         currency[16];
    int64_t      parent_id;       /* 0 = root */
    LedgerNature nature;
    int          decimal_places;  /* from ledger_currency */
} LedgerAccount;
typedef struct {
    int64_t  account_id;         /* non-zero: use this id */
    char     account_code[128];  /* used when account_id == 0 */
    int64_t  amount;             /* > 0, smallest currency unit */
    char     currency[16];
    char     info[256];
} LedgerPosting;
typedef struct {
    char           date[32];       /* ISO-8601 */
    char           description[512];
    char           system[128];
    char           end2end[256];
    char           info[512];
    LedgerPosting *debits;
    int            n_debits;
    LedgerPosting *credits;
    int            n_credits;
    char         **prop_keys;
    char         **prop_vals;
    int            n_props;
} LedgerEntry;
#define LEDGER_INC_CACHE       0  /* cached balance != recalculated */
#define LEDGER_INC_NOLEAF      1  /* branch account has postings */
#define LEDGER_INC_PREFIX      2  /* code does not match the parent */
#define LEDGER_INC_UNBALANCED  3  /* entry's debits != credits */

typedef struct {
    int      type;
    int64_t  account_id;          /* 0 for LEDGER_INC_UNBALANCED */
    char     account_code[128];
    char     account_name[256];
    char     currency[16];
    int      decimal_places;      /* to format the amounts below */
    int64_t  cached, calculated;  /* LEDGER_INC_CACHE */
    int64_t  journal_id;          /* LEDGER_INC_UNBALANCED */
    int64_t  debits, credits;     /* LEDGER_INC_UNBALANCED */
} LedgerInconsistency;
typedef struct {
    int64_t id;
    char    date[32];
    char    description[512];
    char    system[128];
    char    end2end[256];
    char    info[512];
} LedgerJournalHeader;
typedef struct {
    char    side;             /* 'D' = debit, 'C' = credit */
    char    account_code[128];
    char    account_name[256];
    int64_t amount;
    char    currency[16];
    int     decimal_places;
    char    info[256];
} LedgerJournalLine;
typedef struct {
    int64_t journal_id;
    char    date[32];
    char    description[512];
    int64_t debit;
    int64_t credit;
    int64_t balance_prev;
    int64_t balance;
} LedgerTAccountLine;
/* Lifecycle */
LedgerError  ledger_open(const char *path);
LedgerError  ledger_add_company(const char *nickname);
LedgerError  ledger_init(const char *company_nickname);
void         ledger_close(void);
const char  *ledger_last_error(void);
/* Money and currencies */
LedgerError ledger_set_currency_decimal_places(const char *currency,
                                               int decimal_places);
LedgerError ledger_set_currency_price(const char *currency, double price);
LedgerError ledger_get_currency_price(const char *currency,
                                      double *out_price);

char       *ledger_format_amount(int64_t amount, int decimal_places,
                                 char *buf, size_t size);
LedgerError ledger_parse_amount(const char *text, int decimal_places,
                                int64_t *out_amount);
/* Accounts */
const char  *ledger_nature_name(LedgerNature nature);
LedgerNature ledger_nature_parse(const char *name);

LedgerError ledger_add_account(const char *name, const char *code,
                               const char *currency, LedgerNature nature,
                               LedgerAccount *out);
LedgerError ledger_add_account_if_not_exists(const char *name,
                               const char *code, const char *currency,
                               LedgerNature nature, LedgerAccount *out);

LedgerError ledger_get_account_by_id(int64_t id, LedgerAccount *out);
LedgerError ledger_get_account_by_code(const char *code, LedgerAccount *out);
LedgerError ledger_get_all_accounts(LedgerAccount **out, int *count);
void        ledger_accounts_free(LedgerAccount *accounts, int count);

int         ledger_has_children(int64_t account_id);
LedgerError ledger_get_children(int64_t account_id, int64_t **ids,
                                int *count);

LedgerError ledger_create_minimal_chart(int *created);
/* Journal entries */
LedgerError ledger_validate_journal(const LedgerEntry *entry);
LedgerError ledger_post_journal(const LedgerEntry *entry, int64_t *out_id);
LedgerError ledger_get_balance(int64_t account_id, int64_t *out_balance);
/* Consistency checks */
LedgerError ledger_check_journal_balance(
    LedgerInconsistency **out, int *count);
LedgerError ledger_check_leaf_balances(int fix,
    LedgerInconsistency **out, int *count);
LedgerError ledger_check_non_leaf_entries(
    LedgerInconsistency **out, int *count);
LedgerError ledger_check_code_prefixes(
    LedgerInconsistency **out, int *count);
void        ledger_inconsistencies_free(LedgerInconsistency *arr, int count);
/* Reporting */
LedgerError ledger_get_journals(LedgerJournalHeader **out, int *count);
void        ledger_journals_free(LedgerJournalHeader *arr, int count);
LedgerError ledger_get_journal(int64_t journal_id, LedgerJournalHeader *out);
LedgerError ledger_get_journal_lines(int64_t journal_id,
    LedgerJournalLine **out, int *count);
void        ledger_journal_lines_free(LedgerJournalLine *arr, int count);
LedgerError ledger_get_journal_props(int64_t journal_id,
    char ***keys, char ***vals, int *count);
void        ledger_journal_props_free(char **keys, char **vals, int count);
LedgerError ledger_get_taccount_lines(int64_t account_id,
    LedgerTAccountLine **out, int *count);
void        ledger_taccount_lines_free(LedgerTAccountLine *arr, int count);

#endif /* LEDGER_H */
