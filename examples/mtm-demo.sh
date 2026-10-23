#!/bin/sh
# The example of the mtm chapter.  Usage: sh mtm-demo.sh <path-to-ledger>
L=${1:-./ledger}     # path to the ledger binary
show() { cmd=$1; shift; "$L" "$cmd" --db alex.db --company alex "$@" 2>&1; }
post() { show post -; }
"$L" init --db alex.db > /dev/null
"$L" company --db alex.db alex > /dev/null
show chart > /dev/null
"$L" currency --db alex.db --code BTC --dp 8
show account --code 1.2.3 --name "BITCOIN" --currency BTC \
     --nature DEBITNORMAL
show account --code 3.3 --name "OWNER'S CAPITAL (BTC)" --currency BTC \
     --nature CREDITNORMAL
post <<'EOF'
{"date": "2026-01-02T08:00:00Z", "description": "Opening balances",
 "debits":  [{"account_code": "1.1.2", "amount": "2000.00",
              "currency": "USD"},
             {"account_code": "1.2.3", "amount": "0.05",
              "currency": "BTC"}],
 "credits": [{"account_code": "3.1",   "amount": "2000.00",
              "currency": "USD"},
             {"account_code": "3.3",   "amount": "0.05",
              "currency": "BTC"}]}
EOF
show mtm --level 2
"$L" currency --db alex.db --code BTC --price 60000
show mtm --level 2
