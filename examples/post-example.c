#include "ledger.h"
#include <stdio.h>

int main(void) {
    char amount[32];
    if (ledger_open(":memory:") != LEDGER_OK ||
        ledger_add_company("alex") != LEDGER_OK ||
        ledger_init("alex") != LEDGER_OK ||
        ledger_create_minimal_chart(NULL) != LEDGER_OK) {
        fprintf(stderr, "setup failed: %s\n", ledger_last_error());
        return 1;
    }

    LedgerPosting debit  = {.account_code = "1.1.2", .amount = 80000,
                            .currency = "USD"};          /* $800.00 */
    LedgerPosting credit = {.account_code = "4.1",   .amount = 80000,
                            .currency = "USD"};
    LedgerEntry entry = {
        .date = "2025-06-27T15:00:00Z",
        .description = "Logo for a walk-in client, paid on the spot",
        .debits = &debit,   .n_debits = 1,
        .credits = &credit, .n_credits = 1
    };
    int64_t id = 0;
    if (ledger_post_journal(&entry, &id) != LEDGER_OK) {
        fprintf(stderr, "post failed: %s\n", ledger_last_error());
        return 1;
    }

    LedgerAccount checking;
    int64_t balance = 0;
    ledger_get_account_by_code("1.1.2", &checking);
    ledger_get_balance(checking.id, &balance);
    printf("entry %lld posted; %s is now %s %s\n", (long long)id,
           checking.name,
           ledger_format_amount(balance, checking.decimal_places,
                                amount, sizeof amount),
           checking.currency);

    snprintf(debit.account_code, sizeof debit.account_code, "1.1");
    LedgerError err = ledger_post_journal(&entry, NULL);
    printf("posting to 1.1: error %d, %s\n", err, ledger_last_error());

    ledger_close();
    return 0;
}
