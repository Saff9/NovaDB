# Contributing to NovaDB

Thanks for wanting to help. This file tells you everything you need.

## The One Rule

**Keep it simple.** NovaDB is ~7,000 lines of C. That's the point. Every line should earn its place. If a feature adds 2,000 lines, it better be worth 2,000 lines.

## Quick Setup

```bash
git clone https://github.com/Saff9/NovaDB.git
cd NovaDB
./setup.sh          # builds everything, runs tests
```

Or manually:
```bash
make debug          # build with sanitizers
make test           # run all tests
```

## Where to Start

| You're good at | Look at |
|---------------|---------|
| C / systems | `src/storage/btree.c` — the B+Tree needs internal node splitting |
| SQL / parsers | `src/sql/parser.c` — JOIN, subquery, and aggregation support |
| Networking | `src/network/server.c` — connection pooling, TLS, auth |
| Testing / tooling | `test/` — benchmarks, fuzzing, CI pipelines |
| Docs / writing | `README.md`, `website.html`, this file |
| Language bindings | Write a client library for Python, Go, Rust, Zig, etc. |

## Code Style

- **C11** — no GNU extensions, no VLAs, no `//` comments in headers
- **4 spaces** — never tabs
- **snake_case** for functions, **CamelCase** for types
- **Every function gets a comment** — one line above it describing what it does
- **Zero warnings** — `-Wall -Wextra -Wpedantic` must pass clean
- **Return codes** — functions that can fail return `nvdb_errcode_t`; the caller checks it

Example:
```c
/* Return the leaf page that would contain 'key', or an error. */
static int navigate_to_leaf(NVDBBTree *tree, pgno_t from,
                             const void *key, uint16_t keylen,
                             pgno_t *leaf_out) {
    ...
}
```

## Submitting a Pull Request

1. **Fork** the repo
2. **Branch** off `main` — name it `fix/short-desc` or `feature/short-desc`
3. **Write code** — keep each PR focused on one thing
4. **Add tests** — new features need tests in `test/`, bug fixes need a regression test
5. **Run `make test`** — everything must pass
6. **Push and open a PR** — describe what you changed and why

## What We Need Most

| Priority | Task | Why |
|----------|------|-----|
| High | Internal node splitting in B+Tree | Tree height currently limited to 2 levels |
| High | JOIN support in SQL parser/executor | Biggest missing SQL feature |
| High | Prepared statements | Security + performance |
| Medium | Secondary indexes | Query performance on non-primary columns |
| Medium | Aggregation (SUM, AVG, COUNT across rows) | Basic analytics |
| Medium | Benchmark suite comparing against SQLite | Credibility |
| Medium | Python/Go/Rust client libraries | Adoption |
| Low | Replication (leader-follower) | Production deployments |
| Low | Authentication + TLS | Multi-tenant safety |
| Low | ODBC driver | BI tool integration |

## Need Help?

Open a GitHub issue with the `question` tag. Or start a Discussion. Someone will respond.

## License

By contributing, you agree your code will be licensed under Apache 2.0 — same as the rest of NovaDB.
