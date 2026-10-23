#!/bin/sh
# The examples of the check chapter.
# Usage: sh check-demo.sh <path-to-ledger> <path-to-sql-edit>
L=${1:-./ledger}       # path to the ledger binary
EDIT=${2:-./sql-edit}  # path to the sql-edit tool above
show() { cmd=$1; shift; "$L" "$cmd" --db alex.db --company alex "$@" 2>&1; }
post() { show post -; }
edit() { "$EDIT" alex.db "$1"; }
"$L" init --db alex.db > /dev/null
"$L" company --db alex.db alex > /dev/null
show chart > /dev/null
post <<'EOF'
{"date": "2026-01-02T08:00:00Z", "description": "Opening balances",
 "debits":  [{"account_code": "1.1.2", "amount": "2000.00",
              "currency": "USD"}],
 "credits": [{"account_code": "3.1",   "amount": "2000.00",
              "currency": "USD"}]}
EOF
post <<'EOF'
{"date": "2026-01-02T18:00:00Z", "description": "Groceries",
 "debits":  [{"account_code": "5.2",   "amount": "50.00", "currency": "USD"}],
 "credits": [{"account_code": "1.1.2", "amount": "50.00", "currency": "USD"}]}
EOF
show check
echo "exit status: $?"
edit "UPDATE ledger_balance SET balance = 999999 WHERE account_id =
      (SELECT id FROM ledger_account WHERE code = '1.1.2')"
show balance --level 1
show check
echo "exit status: $?"
show check --fixbal
show check > /dev/null
echo "exit status of a second check: $?"
edit "UPDATE ledger_taccount SET credit = 4000
      WHERE journal_id = 2 AND credit > 0"
show check
echo "exit status: $?"
edit "UPDATE ledger_taccount SET credit = 300000
      WHERE journal_id = 2 AND credit > 0"
show check --fixbal
