#!/bin/sh
# One day, two ways: Chapter "One Day, Two Ways" of the libledger book.
# Usage: sh alex-day.sh <path-to-ledger>   (run in an empty directory)
L=${1:-./ledger}     # path to the ledger binary
show() { cmd=$1; shift; "$L" "$cmd" --db alex.db --company alex "$@" 2>&1; }
post() { show post -; }
tool() { "$L" mcp --db alex-ai.db --company alex --call "$1" "${2:--}" 2>&1; }
"$L" init --db alex.db > /dev/null
"$L" company --db alex.db alex > /dev/null
show chart > /dev/null
post <<'EOF'
{"date": "2026-01-05T07:00:00Z", "description": "Opening balances",
 "debits":  [{"account_code": "1.1.2", "amount": "3000.00",
              "currency": "USD"},
             {"account_code": "1.1.1", "amount": "40.00",
              "currency": "USD"},
             {"account_code": "1.3.3", "amount": "5000.00",
              "currency": "USD"}],
 "credits": [{"account_code": "3.1",   "amount": "8040.00",
              "currency": "USD"}]}
EOF
cp alex.db alex-ai.db      # the same Monday morning, for the second half
show balance --under 1.1
post <<'EOF'
{"date": "2026-01-05T09:00:00Z", "description": "Electricity bill",
 "debits":  [{"account_code": "5.1.3", "amount": "120.35",
              "currency": "USD"}],
 "credits": [{"account_code": "1.1.2", "amount": "120.35",
              "currency": "USD"}]}
EOF
post <<'EOF'
{"date": "2026-01-05T11:00:00Z", "description": "Invoice ABC Studio: logo",
 "end2end": "inv-2026-001",
 "debits":  [{"account_code": "1.1.4", "amount": "2500.00",
              "currency": "USD"}],
 "credits": [{"account_code": "4.1",   "amount": "2500.00",
              "currency": "USD"}]}
EOF
post <<'EOF'
{"date": "2026-01-05T13:00:00Z", "description": "Fuel",
 "debits":  [{"account_code": "5.3.1", "amount": "52.00", "currency": "USD"}],
 "credits": [{"account_code": "1.1.1", "amount": "52.00", "currency": "USD"}]}
EOF
post <<'EOF'
{"date": "2026-01-05T13:00:00Z", "description": "Fuel",
 "debits":  [{"account_code": "5.3.1", "amount": "52.00", "currency": "USD"}],
 "credits": [{"account_code": "1.1.2", "amount": "52.00", "currency": "USD"}]}
EOF
post <<'EOF'
{"date": "2026-01-05T17:00:00Z", "description": "Groceries",
 "debits":  [{"account_code": "5.2",   "amount": "63.70", "currency": "USD"}],
 "credits": [{"account_code": "2.1.1", "amount": "63.70", "currency": "USD"}]}
EOF
post <<'EOF'
{"date": "2026-01-05T19:00:00Z", "description": "Reverses entry 5 (typo)",
 "debits":  [{"account_code": "2.1.1", "amount": "63.70", "currency": "USD"}],
 "credits": [{"account_code": "5.2",   "amount": "63.70", "currency": "USD"}]}
EOF
post <<'EOF'
{"date": "2026-01-05T17:00:00Z", "description": "Groceries",
 "debits":  [{"account_code": "5.2",   "amount": "36.70", "currency": "USD"}],
 "credits": [{"account_code": "2.1.1", "amount": "36.70", "currency": "USD"}]}
EOF
show taccount --code 5.2
show balance --under 5 --level 2
tool list_accounts <<'EOF'
{"under": "1.1", "nonzero": true}
EOF
tool list_accounts <<'EOF'
{"under": "5", "level": 2}
EOF
tool list_accounts <<'EOF'
{"under": "5.1"}
EOF
cat > bill.json <<'EOF'
{"date": "2026-01-05T09:00:00Z", "description": "Electricity bill",
 "debits":  [{"account_code": "5.1.3", "amount": "120.35",
              "currency": "USD"}],
 "credits": [{"account_code": "1.1.2", "amount": "120.35",
              "currency": "USD"}]}
EOF
tool validate_journal bill.json
tool post_journal bill.json
cat > invoice.json <<'EOF'
{"date": "2026-01-05T11:00:00Z", "description": "Invoice ABC Studio: logo",
 "end2end": "inv-2026-001",
 "debits":  [{"account_code": "1.1.4", "amount": "2500.00",
              "currency": "USD"}],
 "credits": [{"account_code": "4.1",   "amount": "2500.00",
              "currency": "USD"}]}
EOF
tool validate_journal invoice.json
tool post_journal invoice.json > /dev/null && echo "posted"
tool validate_journal <<'EOF'
{"date": "2026-01-05T13:00:00Z", "description": "Fuel",
 "debits":  [{"account_code": "5.3.1", "amount": "52.00", "currency": "USD"}],
 "credits": [{"account_code": "1.1.1", "amount": "52.00", "currency": "USD"}]}
EOF
cat > fuel.json <<'EOF'
{"date": "2026-01-05T13:00:00Z", "description": "Fuel",
 "debits":  [{"account_code": "5.3.1", "amount": "52.00", "currency": "USD"}],
 "credits": [{"account_code": "1.1.2", "amount": "52.00", "currency": "USD"}]}
EOF
tool validate_journal fuel.json
tool post_journal fuel.json > /dev/null && echo "posted"
cat > groceries.json <<'EOF'
{"date": "2026-01-05T17:00:00Z", "description": "Groceries",
 "debits":  [{"account_code": "5.2",   "amount": "63.70", "currency": "USD"}],
 "credits": [{"account_code": "2.1.1", "amount": "63.70", "currency": "USD"}]}
EOF
tool validate_journal groceries.json
tool post_journal groceries.json > /dev/null && echo "posted"
tool list_journal <<'EOF'
{"limit": 2}
EOF
tool get_journal <<'EOF'
{"id": 5}
EOF
cat > reversal.json <<'EOF'
{"date": "2026-01-05T19:00:00Z", "description": "Reverses entry 5 (typo)",
 "debits":  [{"account_code": "2.1.1", "amount": "63.70", "currency": "USD"}],
 "credits": [{"account_code": "5.2",   "amount": "63.70", "currency": "USD"}]}
EOF
tool validate_journal reversal.json
tool post_journal reversal.json > /dev/null && echo "posted"
cat > corrected.json <<'EOF'
{"date": "2026-01-05T17:00:00Z", "description": "Groceries",
 "debits":  [{"account_code": "5.2",   "amount": "36.70", "currency": "USD"}],
 "credits": [{"account_code": "2.1.1", "amount": "36.70", "currency": "USD"}]}
EOF
tool validate_journal corrected.json
tool post_journal corrected.json > /dev/null && echo "posted"
tool list_accounts <<'EOF'
{"under": "5", "nonzero": true}
EOF
show balance > by-hand.txt
"$L" balance --db alex-ai.db --company alex > by-assistant.txt
diff by-hand.txt by-assistant.txt \
  && echo "The two books have the same balances."
