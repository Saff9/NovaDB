# Security Policy

## Reporting a Vulnerability

Do **not** open a public GitHub issue for security vulnerabilities.

Email: `security@novadb.dev` (set this up for your domain)

We respond within 48 hours. We will confirm receipt, investigate, and keep you
updated throughout the process. Once resolved, we publish a security advisory
and credit the reporter (unless you prefer to remain anonymous).

## Supported Versions

| Version | Supported |
|---------|-----------|
| 1.0.x   | Yes       |
| < 1.0   | No        |

## What We Consider a Vulnerability

- Buffer overflows, use-after-free, double-free
- SQL injection (though NovaDB uses parameterised queries internally)
- Denial of service via crafted input
- Data corruption that causes silent data loss
- Anything that allows unauthorised access

## What We Don't Consider a Vulnerability

- The lack of built-in authentication (NovaDB is designed to run behind a proxy
  for auth — this is documented)
- The lack of built-in TLS (use stunnel or a reverse proxy)
- Resource exhaustion via unbounded connections (there is a configurable
  `NVDB_MAX_CONNECTIONS` limit)

## Security Design

NovaDB takes security seriously within its scope:

- **CRC-32C on every WAL record** — corrupt records are detected and logged are
  truncated at the point of corruption during recovery
- **No `memcpy` on untrusted lengths** — all buffer operations are bounded
- **Panic on unrecoverable corruption** — we crash loudly rather than silently
  continue with bad data
- **AddressSanitizer + UBSan in CI** — every commit is tested with sanitizers
- **Valgrind memcheck in CI** — leak checks on main branch pushes
- **CodeQL analysis** — GitHub's static analysis runs on every PR

## Supply Chain

NovaDB has **zero runtime dependencies**. The only build dependency is a C
compiler and make. There is no npm, no pip, no cargo, no vendored libraries.
This dramatically reduces the supply chain attack surface.

All releases include SHA256 checksums. Verify with:

```bash
sha256sum -c novadb-server-linux-x86_64.sha256
```
