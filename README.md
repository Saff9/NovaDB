# NovaDB — Production-Grade SQL Database in C

A full-featured, ACID-compliant SQL database engine written from scratch in
standard C11. No dependencies beyond libc, pthreads, and POSIX.

## What This Is

NovaDB is a complete database engine — every component is implemented here:

| Layer | Files | Lines | Description |
|-------|-------|-------|-------------|
| **Slotted Pages** | `page.c/h` | ~400 | 8KB pages, variable-length records, LE binary encoding |
| **B+Tree** | `btree.c/h` | ~500 | Copy-on-write, leaf chaining, 500-key stress tested |
| **Buffer Pool** | `buffer.c/h` | ~340 | Clock-sweep eviction with page pinning |
| **Write-Ahead Log** | `wal.c/h` | ~360 | CRC-32C, two-pass recovery, checkpointing |
| **MVCC Transactions** | `transaction.c/h` | ~300 | Snapshot isolation, write-set, atomic commit |
| **SQL Lexer** | `lexer.c/h` | ~410 | 60+ keywords, line/block comments, string escaping |
| **SQL Parser** | `parser.c/h` | ~700 | Recursive-descent + Pratt expression parsing |
| **SQL Executor** | `executor.c/h` | ~630 | SELECT/INSERT/UPDATE/DELETE/CREATE/DROP, WHERE eval |
| **TCP Server** | `server.c/h` | ~520 | epoll + poll(2) fallback, non-blocking I/O |
| **Wire Protocol** | `protocol.c/h` | ~90 | Text-framed protocol with JSON payloads |
| **CLI Client** | `novacli.c` | ~176 | Interactive `psql`-like SQL shell |
| **Tests** | 4 files | ~530 | B+Tree, WAL recovery, SQL parser, full integration |
| **Public API** | `nvdb.h`, `types.h`, `error.h` | ~400 | Clean C API, embeddable |

**Total: ~6,800 lines across 38 files.**

## Quick Start

```bash
make release        # optimised build
./bin/novadb-server --data-dir ./mydata --port 9876 &
./bin/novadb-cli
```

### Example

```sql
CREATE TABLE users (
    id INTEGER PRIMARY KEY,
    name VARCHAR(255) NOT NULL,
    email VARCHAR(255)
);

INSERT INTO users (id, name, email) VALUES (1, 'Alice', 'alice@example.com');
INSERT INTO users (id, name, email) VALUES (2, 'Bob', 'bob@example.com');

SELECT * FROM users;
SELECT name, email FROM users WHERE id = 1;

UPDATE users SET email = 'alice@nova.io' WHERE name = 'Alice';
DELETE FROM users WHERE id = 2;
```

## Architecture

```
┌─────────────────────────────────────────────────┐
│  novadb-cli   netcat   Your Application          │
└────────────────────┬────────────────────────────┘
                     │ TCP (port 9876)
┌────────────────────▼────────────────────────────┐
│  Network Server (epoll / poll)                   │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│  SQL: Lexer → Parser → Executor                  │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│  Transaction Manager (Snapshot Isolation)        │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│  B+Tree (CoW) + WAL (CRC-32C) + Buffer Pool      │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│  OS: pread / pwrite / fsync / epoll              │
└─────────────────────────────────────────────────┘
```

## Project Structure

```
novadb-c/
├── Makefile                   # GNU Make (debug + release)
├── CMakeLists.txt             # CMake alternative
├── README.md
├── LICENSE                    # Apache 2.0
├── include/novadb/
│   ├── nvdb.h                 # Public C API
│   ├── types.h                # Core types, NVDBValue, page/wal structs
│   └── error.h                # Error handling + diagnostics
├── src/
│   ├── common/
│   │   ├── memory.c/h         # Arena + slab allocators
│   │   └── logging.c/h        # Timestamped level-gated logging
│   ├── storage/
│   │   ├── page.c/h           # Slotted 8KB pages
│   │   ├── btree.c/h          # B+Tree (CoW)
│   │   ├── buffer.c/h         # Buffer pool (clock-sweep)
│   │   └── wal.c/h            # Write-Ahead Log (CRC-32C)
│   ├── txn/
│   │   └── transaction.c/h    # MVCC transaction manager
│   ├── sql/
│   │   ├── lexer.c/h          # SQL tokeniser
│   │   ├── parser.c/h         # Recursive-descent parser
│   │   └── executor.c/h       # Query executor
│   ├── network/
│   │   ├── server.c/h         # TCP server (epoll + poll)
│   │   └── protocol.c/h       # Wire protocol
│   └── main.c                 # Entry point + engine assembly
├── contrib/
│   └── novacli.c              # Interactive CLI client
└── test/
    ├── test_main.c            # Test runner
    ├── test_btree.c           # B+Tree (500 keys)
    ├── test_wal.c             # WAL recovery
    ├── test_sql.c             # Lexer + parser
    └── test_integration.c     # Full SQL lifecycle
```

## Key Design Decisions

### Slotted Pages (not fixed records)

Each 8KB page has a 32-byte header, a slot directory growing downward,
and record data growing upward from the page end. Same design as PostgreSQL's
`bufpage.h` and SQLite's `btreeInt.h`. Variable-length keys and values,
no external fragmentation.

### Copy-on-Write B+Tree

Every insert clones the path from root to leaf. Old pages stay accessible
to concurrent readers. MVCC snapshots come for free — no version chain,
no undo log, no read locks.

### Clock-Sweep Eviction (not LRU)

O(1) per access. One reference bit per cached page, a hand that sweeps,
eviction when it finds an unreferenced page. Nearly identical to LRU for
database workloads at a fraction of the overhead.

### Text Wire Protocol

`QRY 27\r\nSELECT * FROM users` — debuggable with netcat, clients in any
language without a special driver. JSON result payloads.

### No External Dependencies

Just C11, libc, and POSIX. Compiles on Linux, macOS, FreeBSD.

## Comparison

| | NovaDB | SQLite | PostgreSQL | LMDB |
|---|--------|--------|------------|------|
| Type | SQL | SQL | SQL | KV |
| Language | C | C | C | C |
| Storage | B+Tree CoW | B-Tree | Heap+BTree | B+Tree CoW |
| WAL | CRC-32C | Custom | CRC-32C | None |
| MVCC | ✓ | ✗ | ✓ | ✓ |
| SQL | ✓ | ✓ | ✓ | ✗ |
| Embeddable | ✓ | ✓ | ✗ | ✓ |
| Dependencies | None | None | Many | None |
| Code size | ~6,800 loc | ~155k loc | ~1.2M loc | ~12k loc |

## Building & Testing

```bash
make release       # optimised build
make debug         # with AddressSanitizer + UBSan
make test          # build + run all tests
make clean         # remove build artifacts
```

## License

Apache License 2.0.
#   N o v a D B  
 