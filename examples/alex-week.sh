#!/bin/sh
# Alex's first week, from Chapter 2 of the libledger book.
# Usage: sh alex-week.sh <path-to-ledger>   (run in an empty directory)
L=${1:-./ledger}     # path to the ledger binary
show() { cmd=$1; shift; "$L" "$cmd" --db alex.db --company alex "$@" 2>&1; }
post() { show post -; }
"$L" init --db alex.db
"$L" company --db alex.db alex
show chart
post <<'EOF'
{"date": "2025-06-30T08:00:00Z", "description": "Opening balances",
 "debits":  [{"account_code": "1.1.2", "amount": "2000.00",
              "currency": "USD"},
             {"account_code": "1.3.3", "amount": "5000.00",
              "currency": "USD"}],
 "credits": [{"account_code": "2.1.1", "amount": "1500.00",
              "currency": "USD"},
             {"account_code": "3.1",   "amount": "5500.00",
              "currency": "USD"}]}
EOF
post <<'EOF'
{"date": "2025-06-30T10:00:00Z", "description": "Invoice XYZ: brand identity",
 "end2end": "inv-2025-042",
 "properties": {"client": "XYZ", "project": "brand identity"},
 "debits":  [{"account_code": "1.1.4", "amount": "8000.00",
              "currency": "USD"}],
 "credits": [{"account_code": "4.1",   "amount": "8000.00",
              "currency": "USD"}]}
EOF
show journal --id 2
post <<'EOF'
{"date": "2025-06-30T18:30:00Z", "description": "Groceries",
 "debits":  [{"account_code": "5.2",   "amount": "86.40", "currency": "USD"}],
 "credits": [{"account_code": "2.1.1", "amount": "86.40", "currency": "USD"}]}
EOF
post <<'EOF'
{"date": "2025-07-01T09:00:00Z", "description": "Adobe Creative Cloud",
 "debits":  [{"account_code": "5.8",   "amount": "59.99", "currency": "USD"}],
 "credits": [{"account_code": "2.1.1", "amount": "59.99", "currency": "USD"}]}
EOF
post <<'EOF'
{"date": "2025-07-01T17:00:00Z", "description": "Fuel",
 "debits":  [{"account_code": "5.3",   "amount": "45.00", "currency": "USD"}],
 "credits": [{"account_code": "1.1.2", "amount": "45.00", "currency": "USD"}]}
EOF
post <<'EOF'
{"date": "2025-07-01T17:00:00Z", "description": "Fuel",
 "debits":  [{"account_code": "5.3.1", "amount": "45.00", "currency": "USD"}],
 "credits": [{"account_code": "1.1.2", "amount": "45.00", "currency": "USD"}]}
EOF
post <<'EOF'
{"date": "2025-07-02T11:00:00Z", "description": "XYZ pays inv-2025-042",
 "end2end": "inv-2025-042",
 "debits":  [{"account_code": "1.1.2", "amount": "8000.00",
              "currency": "USD"}],
 "credits": [{"account_code": "1.1.4", "amount": "8000.00",
              "currency": "USD"}]}
EOF
post <<'EOF'
{"date": "2025-07-03T09:00:00Z", "description": "July rent",
 "debits":  [{"account_code": "5.1.1", "amount": "1800.00",
              "currency": "USD"}],
 "credits": [{"account_code": "1.1.2", "amount": "1800.00",
              "currency": "USD"}]}
EOF
show taccount --code 2.1.1
post <<'EOF'
{"date": "2025-07-03T09:05:00Z", "description": "Credit card, paid in full",
 "debits":  [{"account_code": "2.1.1", "amount": "1646.39",
              "currency": "USD"}],
 "credits": [{"account_code": "1.1.2", "amount": "1646.39",
              "currency": "USD"}]}
EOF
post <<'EOF'
{"date": "2025-07-03T09:10:00Z", "description": "Transfer to savings",
 "debits":  [{"account_code": "1.1.3", "amount": "2000.00",
              "currency": "USD"}],
 "credits": [{"account_code": "1.1.2", "amount": "2000.00",
              "currency": "USD"}]}
EOF
post <<'EOF'
{"date": "2025-07-04T20:00:00Z", "description": "Dinner with friends",
 "debits":  [{"account_code": "5.6",   "amount": "42.00", "currency": "USD"}],
 "credits": [{"account_code": "1.1.1", "amount": "42.00", "currency": "USD"}]}
EOF
post <<'EOF'
{"date": "2025-07-04T19:30:00Z", "description": "ATM withdrawal",
 "debits":  [{"account_code": "1.1.1", "amount": "100.00",
              "currency": "USD"}],
 "credits": [{"account_code": "1.1.2", "amount": "100.00",
              "currency": "USD"}]}
EOF
post <<'EOF'
{"date": "2025-07-04T20:00:00Z", "description": "Dinner with friends",
 "debits":  [{"account_code": "5.6",   "amount": "42.00", "currency": "USD"}],
 "credits": [{"account_code": "1.1.1", "amount": "42.00", "currency": "USD"}]}
EOF
show balance --level 2
show taccount --code 1.1.2
show check
post <<'EOF'
{"date": "2025-07-04T23:59:00Z", "description": "Closing entry",
 "debits":  [{"account_code": "4.1",   "amount": "8000.00",
              "currency": "USD"}],
 "credits": [{"account_code": "5.1.1", "amount": "1800.00",
              "currency": "USD"},
             {"account_code": "5.2",   "amount": "86.40",
              "currency": "USD"},
             {"account_code": "5.3.1", "amount": "45.00",
              "currency": "USD"},
             {"account_code": "5.6",   "amount": "42.00",
              "currency": "USD"},
             {"account_code": "5.8",   "amount": "59.99",
              "currency": "USD"},
             {"account_code": "3.2",   "amount": "5966.61",
              "currency": "USD"}]}
EOF
show balance --level 1
