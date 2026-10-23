#!/bin/sh
# End-to-end test of `ledger mcp`: speaks JSON-RPC over stdio like an MCP
# client and checks the answers with grep.
# Usage: tests/test_mcp.sh ./ledger [work-dir, default generated]
set -u
LEDGER=${1:-./ledger}
DB=${2:-generated}/test_mcp.db
rm -f "$DB" "$DB-wal" "$DB-shm"

"$LEDGER" init    --db "$DB" >/dev/null || exit 1
"$LEDGER" company --db "$DB" alex >/dev/null || exit 1
"$LEDGER" chart   --db "$DB" --company alex >/dev/null || exit 1

ENTRY='{"date":"2025-06-30","description":"Design services",
  "debits":[{"account_code":"1.1.2","amount":"8000.00","currency":"USD"}],
  "credits":[{"account_code":"4.1","amount":"8000.00","currency":"USD"}]}'
ENTRY=$(printf '%s' "$ENTRY" | tr -d '\n')
BAD='{"date":"2025-07-01","description":"Overspend",
  "debits":[{"account_code":"5.2","amount":"9000.00","currency":"USD"}],
  "credits":[{"account_code":"1.1.2","amount":"9000.00","currency":"USD"}]}'
BAD=$(printf '%s' "$BAD" | tr -d '\n')

OUT=$("$LEDGER" mcp --db "$DB" --company alex 2>/dev/null <<EOF
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"0"}}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":2,"method":"tools/list"}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"validate_journal","arguments":$ENTRY}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"post_journal","arguments":$ENTRY}}
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"post_journal","arguments":$BAD}}
{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"account_history","arguments":{"account_code":"1.1.2"}}}
{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"nope","arguments":{}}}
{"jsonrpc":"2.0","id":8,"method":"no/such/method"}
not json
{"jsonrpc":"2.0","id":"s-9","method":"ping"}
{"jsonrpc":"2.0","id":10}
{"jsonrpc":"1.0","id":11,"method":"ping"}
[{"jsonrpc":"2.0","id":12,"method":"ping"}]
{"jsonrpc":"2.0","id":13,"method":"ping"} {"jsonrpc":"2.0","id":99,"method":"ping"}
{"jsonrpc":"2.0","id":14,"method":"tools/call","params":{"name":"get_journal","arguments":{"id":1}}}
{"jsonrpc":"2.0","id":15,"method":"tools/call","params":{"name":"add_account","arguments":{"code":"4.1.1","name":"CLIENT X","currency":"USD","nature":"debit"}}}
{"jsonrpc":"2.0","id":16,"method":"tools/call","params":{"name":"post_journal","arguments":{"date":"2025-07-01","description":"Huge","debits":[{"account_code":"1.1.2","amount":9007199254740993,"currency":"USD"}],"credits":[{"account_code":"4.1","amount":9007199254740993,"currency":"USD"}]}}}
{"jsonrpc":"2.0","id":17,"method":"tools/call","params":{"name":"account_history","arguments":{"account_code":"1.1"}}}
{"jsonrpc":"2.0","method":"notifications/cancelled","params":{}}
{"jsonrpc":"2.0","id":1.5,"method":"ping"}
{"jsonrpc":"2.0","id":18,"method":"tools/call","params":{"name":"get_journal","arguments":{"id":1.9}}}
{"jsonrpc":"2.0","id":19,"method":"tools/call","params":{"name":"list_accounts","arguments":{"under":5}}}
{"jsonrpc":"2.0","id":20,"method":"tools/call","params":{"name":"list_accounts","arguments":{"under":"5","level":2}}}
{"jsonrpc":"2.0","id":21,"method":"tools/call","params":{"name":"list_journal","arguments":{"limit":"1"}}}
EOF
)

fail=0
expect() {  # expect <description> <pattern>
    if printf '%s\n' "$OUT" | grep -q -- "$2"; then
        printf '  %-45s OK\n' "$1"
    else
        printf '  %-45s FAIL (no match for %s)\n' "$1" "$2"; fail=1
    fi
}

echo "=== ledger mcp test suite ==="
expect "initialize echoes protocol version" '"id":1,"result":{"protocolVersion":"2025-06-18"'
expect "tools/list includes post_journal"  '"name":"post_journal"'
expect "validate_journal accepts entry"    '"id":3,.*\\"valid\\":true'
expect "post_journal returns stored entry" '"id":4,.*"isError":false'
expect "negative balance is a tool error"  '"id":5,.*negative balance.*"isError":true'
expect "history shows the posting"         '"id":6,.*\\"balance\\":\\"8000.00\\"'
expect "unknown tool is a protocol error"  '"id":7,"error":{"code":-32602'
expect "unknown method is -32601"          '"id":8,"error":{"code":-32601'
expect "garbage line is a parse error"     '"id":null,"error":{"code":-32700'
expect "string ids are echoed"              '"id":"s-9","result":{}'
expect "missing method is invalid"         '"id":10,"error":{"code":-32600'
expect "wrong jsonrpc version is invalid"  '"id":11,"error":{"code":-32600'
expect "a batch is an invalid request"     '"id":null,"error":{"code":-32600'
expect "trailing data is a parse error"    '"id":null,"error":{"code":-32700,"message":"parse error"}}$'
expect "a fractional id is invalid"        '"id":null,"error":{"code":-32600'
expect "get_journal wants an integer id"   '"id":18,.*must be an integer'
expect "under must be a string"            '"id":19,.*under must be an account code'
expect "level keeps the top levels"        '"id":20,.*TRANSPORTATION'
expect "limit must be an integer"          '"id":21,.*limit must be a positive integer'
expect "get_journal returns the entry"     '"id":14,.*inv-\|"id":14,.*Design services'
expect "add_account checks the nature"     '"id":15,.*nature must be DEBITNORMAL'
expect "amounts beyond 2^53 are refused"   '"id":16,.*write it as a decimal string'
expect "history of a branch is explained"  '"id":17,.*this account is a branch'
# Responses: 19 requests with a valid id, plus one answer each for the
# garbage line, the batch, the two-message line and the fractional id.
# Notifications: none.
lines=$(printf '%s\n' "$OUT" | wc -l)
[ "$lines" -eq 23 ] && printf '  %-45s OK\n' "one response per request, none per notification" \
    || { printf '  %-45s FAIL (%s lines)\n' "one response per request, none per notification" "$lines"; fail=1; }

# --call runs one tool by hand and prints what the model would read
CALL=$(echo '{"under":"1.1.2"}' | "$LEDGER" mcp --db "$DB" --company alex --call list_accounts -)
printf '%s\n' "$CALL" | grep -q '"balance": "8000.00"' \
    && printf '  %-45s OK\n' "--call prints the tool result" \
    || { printf '  %-45s FAIL\n' "--call prints the tool result"; fail=1; }
"$LEDGER" mcp --db "$DB" --company alex --call list_accounts nothere.json \
    >/dev/null 2>&1 \
    && { printf '  %-45s FAIL\n' "--call refuses a missing file"; fail=1; } \
    || printf '  %-45s OK\n' "--call refuses a missing file"

rm -f "$DB" "$DB-wal" "$DB-shm"
[ $fail -eq 0 ] && echo "=== mcp: all passed ===" || echo "=== mcp: FAILED ==="
exit $fail
