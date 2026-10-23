# libledger

**A small double-entry accounting library in C, backed by SQLite.**

libledger is the companion implementation to **[Double-Entry Accounting: A Literate Programming Implementation](https://leanpub.com/literateledger)**, a book about building an accounting system from first principles using *literate programming*.

The project includes a C library, a `ledger` command-line application, and an MCP (Model Context Protocol) server that lets AI assistants interact with the books.

> **The book is the source.** The C implementation is extracted from the book's `noweb` source rather than maintained separately from its explanation.

## Features

- **Double-entry bookkeeping:** journal entries are checked for balanced debits and credits, separately for each currency.
- **SQLite storage:** multiple sets of books can be stored in a single database file.
- **Hierarchical chart of accounts:** accounts are organized using dotted codes; postings go to leaf accounts.
- **Exact money representation:** amounts are stored as integers in each currency's smallest unit, not floating-point values.
- **Atomic posting:** entries are validated and committed as database transactions.
- **Consistency checks:** tools to detect unbalanced entries and other accounting inconsistencies.
- **Command-line interface:** initialize books, manage accounts, post entries, inspect balances, and run checks.
- **Multi-currency support:** currency-specific balancing and USD mark-to-market reporting.
- **MCP integration:** expose accounting operations to an AI assistant through a Model Context Protocol server.

## The book

**[Double-Entry Accounting: A Literate Programming Implementation](https://leanpub.com/literateledger)**  
* C, SQLite and an AI bookkeeper, built chunk by chunk.

The book starts with accounting fundamentals—no prior accounting experience required—and develops the implementation step by step. It also follows a fictional freelancer through practical bookkeeping examples, first using the CLI and later an AI assistant.

The manuscript uses `noweb` to extract compilable source code from the explanatory text. Selected example sessions are also extracted and tested against the program's actual output, so the documentation can be checked as the software evolves.

**[Read the book on Leanpub →](https://leanpub.com/literateledger)**

## Project components

| Component | Purpose |
| --- | --- |
| `libledger` | C library implementing the accounting model and persistence |
| `ledger` | Command-line interface to the library |
| `ledger check` | Accounting consistency checks |
| `ledger mtm` | USD mark-to-market valuation |
| `ledger mcp` | MCP server for AI-assistant integration |

## Building and running

The implementation uses **C**, **SQLite**, and **cJSON**. The literate-programming workflow also uses **noweb** to extract code from the manuscript.

Build instructions and available targets depend on the files included in this repository. Consult the project's `Makefile` and source tree for the exact commands and dependencies in this release.

## Design principles

1. **Correctness before convenience.** Accounting invariants are checked when entries are posted.
2. **Exact arithmetic.** Money is represented with integers to avoid floating-point rounding errors.
3. **Small, understandable components.** The library is intentionally compact and uses SQLite for durable storage.
4. **Documentation and implementation together.** Literate programming keeps the explanation close to the code it describes.
5. **Verifiable examples.** Executable book examples help catch divergence between prose and implementation.

## Who is this for?

- C developers interested in building practical financial software.
- Readers learning double-entry bookkeeping through working code.
- Engineers interested in SQLite, accounting invariants, or literate programming.
- Developers exploring MCP integrations with stateful applications.

## Feedback and contributions

Bug reports, questions, and suggestions are welcome through GitHub Issues. When reporting a problem, please include the relevant command or API call, expected behavior, and observed result.

## License

See the repository's `LICENSE` file for the terms that apply to the code. The book is a separate publication; access to the repository does not by itself grant redistribution rights to the book's text.
