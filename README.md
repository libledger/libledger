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
*C, SQLite and an AI bookkeeper, built chunk by chunk.*

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

## What is in this repository

Everything here except the build files is extracted from the book, so you can build, run and test the program without **noweb**, which is only needed to write the book.

| Path | Contents | In the book |
| --- | --- | --- |
| `ledger.h` | The public header: types, error codes, functions | Chapters 3–10 |
| `ledger_impl.c` | The library | Chapters 3–10 |
| `ledger_cli.c` | The `ledger` command, `check`, `mtm` and the MCP server | Chapters 11–14 |
| `examples/` | The shell sessions of the executable chapters and the output the book prints; the C example of Chapter 8; `sql-edit.c` | Chapters 2, 8, 12, 13, 15 |
| `skills/libledger-bookkeeper/SKILL.md` | The bookkeeping skill for the AI assistant | Appendix A |
| `tests/` | The test suites of the library, the command line and the MCP server | — |
| `Makefile`, `compat/`, `.github/` | Portable build and CI (not from the book) | — |

## Building and running

The implementation uses **C**, **SQLite**, and **cJSON**.

### Requirements

- A C compiler (`gcc` or `clang`) and GNU Make
- Either the SQLite and cJSON development packages, **or** `curl`/`wget` and `unzip` so the Makefile can download them

### Build

```sh
make
./ledger help
```

The Makefile picks how to get the dependencies:

- If `pkg-config` finds `sqlite3` and `libcjson`, it links against the system libraries.
- Otherwise it downloads the SQLite amalgamation (3.46.1) and cJSON (1.7.19) into `deps/` and compiles them into the binary, so no system packages are needed. The download happens only on the first build.

You can force either mode:

```sh
make USE_SYSTEM_LIBS=0   # always use the bundled copies in deps/
make USE_SYSTEM_LIBS=1   # always use the system libraries
```

Targets:

| Target | Effect |
| --- | --- |
| `make` | Build `ledger` (and `libledger.a`) |
| `make check` | Run the test suites and every example of the book, comparing its output with the book's |
| `make deps` | Only download the bundled dependencies |
| `make clean` | Remove objects, the library and the binary |
| `make distclean` | Also remove `deps/` |

Prebuilt binaries for Linux, macOS and Windows are attached to each [release](https://github.com/libledger/libledger/releases).

## Following the book

`make check` runs every example of the book and checks that the program prints, byte for byte, what the book prints. To follow one by hand, run it in an empty directory of its own, since each example creates its database there:

```sh
make check                      # builds ledger and build/sql-edit too
mkdir week && cd week
sh ../examples/alex-week.sh ../ledger
```

| Example | Chapter | Arguments |
| --- | --- | --- |
| `examples/alex-week.sh` | 2, A Week in Alex's Books | `../ledger` |
| `examples/post-example.c` | 8, Journal Entries (Using the Library from C) | built as `build/post-example` |
| `examples/check-demo.sh` | 12, Reporting Problems (Damaged Books) | `../ledger ../build/sql-edit` |
| `examples/mtm-demo.sh` | 13, Valuing the Books in USD | `../ledger` |
| `examples/alex-day.sh` | 15, One Day, Two Ways | `../ledger` |

What each prints is in `examples/<name>.expected`, which is the output printed in the book.

To let an AI assistant keep the books (Chapter 14 and Appendix A):

- point your MCP client at the binary: command `/path/to/ledger`, arguments `["mcp", "--db", "/path/to/books.db", "--company", "<name>"]`;
- install the skill by copying `skills/libledger-bookkeeper/` into `~/.claude/skills/` (Claude Code) or the skills folder of your assistant.

### Platform notes

- **Linux (Debian/Ubuntu):** `sudo apt install build-essential libsqlite3-dev libcjson-dev` to use the system libraries, or just `make` to use the bundled copies.
- **macOS:** install the Xcode Command Line Tools (`xcode-select --install`) and run `make`. The stock GNU Make 3.81 works.
- **Windows:** build from an [MSYS2](https://www.msys2.org/) **UCRT64** shell:

  ```sh
  pacman -S make unzip curl mingw-w64-ucrt-x86_64-gcc
  make
  ./ledger.exe help
  ```

  MinGW lacks POSIX `getline()`, so on Windows the Makefile also builds a small shim from `compat/`.

### Continuous integration

GitHub Actions (`.github/workflows/build.yml`) builds and smoke-tests `ledger` on Linux, macOS and Windows for every push and pull request, and uploads the binaries as workflow artifacts. Pushing a tag such as `v1.0.0` (or publishing a release for a new tag in the GitHub UI) also attaches the zipped binaries for all three platforms to that tag's release, creating the release if it does not exist yet.

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

The C files, examples, skill and tests are generated from the book's source, so a fix is made there first and then exported here. Pull requests are still welcome as a way to show a fix; they are applied to the book, and the next export brings them back to this repository.

## License

See the repository's `LICENSE` file for the terms that apply to the code. The book is a separate publication; access to the repository does not by itself grant redistribution rights to the book's text.
