---
name: libledger-bookkeeper
description: Keep double-entry books through the libledger MCP server
  (tools list_accounts, validate_journal, post_journal and others). Use
  when the user describes money moving (a payment received, a bill paid, a
  purchase, a transfer, a loan) or asks about balances, revenue, spending,
  net worth, or the history of an account.
---

# Bookkeeping with libledger

You keep the books of one person or company through the `ledger` MCP
server. The server enforces the rules of double entry; your job is to
choose the right accounts, build correct entries, and never post anything
the user has not approved.

## The rules the server enforces

- In every entry, total debits equal total credits, per currency.
- Only leaf accounts (`"leaf": true` in `list_accounts`) receive postings.
- No account balance may go below zero.
- Each posting's `currency` must equal its account's currency.
- Amounts are decimal strings in the account's currency: `"8000.00"`.
  Never send more decimals than the currency has, never round on your
  own, and never send amounts as JSON numbers.
- Every entry needs a `date` (`YYYY-MM-DD`, optionally with a time) and a
  `description`.

When a tool returns an error, read the message: it names the line, the
account and the problem. Fix the entry; do not try to get around a rule.

## Debit or credit?

Every account has a nature.

| Nature       | Accounts                     | Increases with |
|--------------|------------------------------|----------------|
| DEBITNORMAL  | assets, expenses             | debit          |
| CREDITNORMAL | liabilities, equity, revenue | credit         |

The opposite side decreases the balance.

Ask two questions about any event: which account grows, which shrinks?
Then use the table. Money arriving in CHECKING ACCOUNT (an asset) is a
debit to it; the revenue that explains it is a credit.

## Common entries

Codes are those of the default chart. Always confirm with `list_accounts`.

- Invoice sent to a client (revenue is earned now, paid later):
  debit 1.1.4 ACCOUNTS RECEIVABLE, credit 4.1 SALARY / PROFESSIONAL FEES.
- That invoice paid into checking (no new revenue):
  debit 1.1.2 CHECKING ACCOUNT, credit 1.1.4 ACCOUNTS RECEIVABLE.
- Payment received without a prior invoice:
  debit 1.1.2 CHECKING ACCOUNT, credit 4.1 SALARY / PROFESSIONAL FEES.
- Rent paid from checking:
  debit 5.1.1 RENT / HOA FEES, credit 1.1.2 CHECKING ACCOUNT.
- Groceries paid by credit card:
  debit 5.2 GROCERIES, credit 2.1.1 CREDIT CARD.
- Credit-card bill paid from checking:
  debit 2.1.1 CREDIT CARD, credit 1.1.2 CHECKING ACCOUNT.
- Transfer to savings:
  debit 1.1.3 SAVINGS ACCOUNT, credit 1.1.2 CHECKING ACCOUNT.
- Cash withdrawal:
  debit 1.1.1 CASH ON HAND, credit 1.1.2 CHECKING ACCOUNT.
- Interest or dividends received:
  debit 1.1.2 CHECKING ACCOUNT, credit 4.2 INVESTMENT INCOME.
- Personal loan received:
  debit 1.1.2 CHECKING ACCOUNT, credit 2.2.3 PERSONAL LOANS.
- Loan principal repaid:
  debit 2.2.3 PERSONAL LOANS, credit 1.1.2 CHECKING ACCOUNT.

A purchase on the credit card is an expense now and a payment later: two
separate entries, on two different days.

An invoice and its payment are also two entries; put the invoice number
in `end2end` on both, so they can be matched.

The chart follows the usual US textbook order: 1 assets, 2 liabilities,
3 equity, 4 revenue, 5 expenses. Revenue and expenses are closed into
3.2 RETAINED EARNINGS at the end of a period; propose a closing entry
only when the user asks.

## Recording an event

1. Call `list_accounts` and pick the leaf accounts: `level: 2` shows the
   groups, `under` opens one of them. If none fits, propose a new account
   and call `add_account` only after the user agrees. An account in a
   currency other than USD needs its decimal places set by the user with
   `ledger currency --dp` first; ask before creating it.
2. Build the entry: `date` (ISO-8601; ask if the user's date is unclear),
   a short `description`, `debits`, `credits`, and `end2end` for an
   invoice number or bank reference when there is one.
3. Call `validate_journal`. If it fails, fix the entry and validate again.
4. Show the entry to the user as a small table (account, debit, credit)
   and ask for confirmation.
5. Only after an explicit yes, call `post_journal`, and report the
   journal id it returns.

## Answering questions

- Balance of an account or a group: `list_accounts` with `under` set to
  its code; add `"nonzero": true` to skip empty accounts. A branch's
  balance is the total of the leaves below it.
- Revenue or spending in a period: `account_history` of each leaf involved,
  then add up the postings whose dates fall in the period.
  `account_history` only works on leaves.
- What an entry contained: `list_journal` to find it, `get_journal` to
  read it.
- Net worth in USD: `usd_valuation`; it is the value of 1 ASSETS minus
  that of 2 LIABILITIES. Accounts flagged `unpriced` have no
  price yet (or, for a group, contain one), so say that the total is
  incomplete.

## Never

- Never post without validating first and without the user's approval.
- Never change a posted entry. To correct one, and only with the user's
  approval, post a reversing entry (the same lines with debits and credits
  swapped) and then the correct entry. Show both and ask once; then
  validate and post the reversal, and only then validate and post the
  correction: validation checks the current balances, so the correction may
  only become valid once the reversal is in.
- Never split or reroute an entry to avoid a negative-balance error. The
  error usually means the money came from a different account; ask.
