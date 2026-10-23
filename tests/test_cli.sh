#!/bin/sh
# Error paths of the ledger command line.
# Usage: tests/test_cli.sh ./ledger [work-dir, default generated]
set -u
LEDGER=${1:-./ledger}
DIR=${2:-generated}/test-cli
rm -rf "$DIR" && mkdir -p "$DIR"
DB=$DIR/alex.db
"$LEDGER" init --db "$DB" >/dev/null || exit 1
"$LEDGER" company --db "$DB" alex >/dev/null || exit 1
"$LEDGER" chart --db "$DB" --company alex >/dev/null || exit 1

fail=0
# refuses <description> <command...>: the command must exit non-zero
refuses() {
    d=$1; shift
    if "$@" >/dev/null 2>&1; then
        printf '  %-45s FAIL (accepted)\n' "$d"; fail=1
    else
        printf '  %-45s OK\n' "$d"
    fi
}
entry() {  # entry <date> [trailing text]: a valid entry, optionally broken
    printf '{"date":"%s","description":"t","debits":[{"account_code":"1.1.2",' "$1"
    printf '"amount":"1.00","currency":"USD"}],"credits":[{"account_code":"4.1",'
    printf '"amount":"1.00","currency":"USD"}]}%s' "${2:-}"
}

echo "=== ledger cli test suite ==="
refuses "a mistyped --db does not create a file" \
    "$LEDGER" balance --db "$DIR/typo.db" --company alex
[ -e "$DIR/typo.db" ] && { echo "  typo.db was created: FAIL"; fail=1; }
refuses "a duplicate company is refused" "$LEDGER" company --db "$DB" alex
refuses "two company names are refused" "$LEDGER" company --db "$DB" a b
refuses "--level must be a whole number" \
    "$LEDGER" balance --db "$DB" --company alex --level 2x
refuses "--under needs an existing code" \
    "$LEDGER" balance --db "$DB" --company alex --under 7
refuses "February 30 is not a date" \
    sh -c "printf '%s' '$(entry 2025-02-30)' | $LEDGER post --db $DB --company alex -"
refuses "garbage after the time is refused" \
    sh -c "printf '%s' '$(entry 2025-07-01Tgarbage)' | $LEDGER post --db $DB --company alex -"
refuses "trailing data after the entry is refused" \
    sh -c "printf '%s' '$(entry 2025-07-01 ' {}')' | $LEDGER post --db $DB --company alex -"
refuses "currency codes are capitals" \
    "$LEDGER" currency --db "$DB" --code usd --dp 2
refuses "an empty price is refused" \
    "$LEDGER" currency --db "$DB" --code GBP --price ""
refuses "an infinite price is refused" \
    "$LEDGER" currency --db "$DB" --code GBP --price inf
if printf '%s' "$(entry 2025-07-01T09:00:00Z)" | \
       "$LEDGER" post --db "$DB" --company alex - >/dev/null; then
    printf '  %-45s OK\n' "a valid entry with a time is accepted"
else
    printf '  %-45s FAIL\n' "a valid entry with a time is accepted"; fail=1
fi
printf '%s' "$(entry 2025-07-02T09:00:00Z)" | sed 's/"description"/"info":"memo","description"/' | \
    "$LEDGER" post --db "$DB" --company alex - >/dev/null
if "$LEDGER" journal --db "$DB" --company alex --id 2 | grep -q '"info": "memo"'; then
    printf '  %-45s OK\n' "an entry's info is printed back"
else
    printf '  %-45s FAIL\n' "an entry's info is printed back"; fail=1
fi

# a USD branch whose only leaf is in BTC has no total in USD
"$LEDGER" currency --db "$DB" --code BTC --dp 8 >/dev/null
"$LEDGER" account --db "$DB" --company alex --code 1.4 --name CRYPTO \
    --currency USD --nature DEBITNORMAL >/dev/null
"$LEDGER" account --db "$DB" --company alex --code 1.4.1 --name BITCOIN \
    --currency BTC --nature DEBITNORMAL >/dev/null
if "$LEDGER" balance --db "$DB" --company alex --under 1.4 | grep '^1\.4 ' | grep -q 'USD'; then
    printf '  %-45s FAIL\n' "a BTC leaf gives a USD branch no total"; fail=1
else
    printf '  %-45s OK\n' "a BTC leaf gives a USD branch no total"
fi

rm -rf "$DIR"
[ $fail -eq 0 ] && echo "=== cli: all passed ===" || echo "=== cli: FAILED ==="
exit $fail
