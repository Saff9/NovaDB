# NovaDB — Production SQL Database Engine
# Build: make release (optimised) or make debug (sanitizers)

CC       ?= cc
CFLAGS   = -std=c11 -Wall -Wextra -Wpedantic \
           -Werror=implicit-function-declaration \
           -Werror=return-type \
           -Werror=uninitialized \
           -Wstrict-prototypes \
           -Wmissing-prototypes \
           -fno-strict-aliasing \
           -D_DEFAULT_SOURCE -D_GNU_SOURCE
DBGFLAGS = -g3 -O0 -fsanitize=address,undefined
RELFLAGS = -O2 -DNDEBUG
LDFLAGS  = -lpthread -lm

SRCDIR   = src
INCDIR   = include
BLDDIR   = build
BINDIR   = bin

INCLUDES = -I$(INCDIR) -I$(SRCDIR)

# ── Source files ────────────────────────────────────────────────
ENGINE_SRCS = \
    $(SRCDIR)/common/memory.c \
    $(SRCDIR)/common/logging.c \
    $(SRCDIR)/storage/page.c \
    $(SRCDIR)/storage/buffer.c \
    $(SRCDIR)/storage/btree.c \
    $(SRCDIR)/storage/wal.c \
    $(SRCDIR)/txn/transaction.c \
    $(SRCDIR)/sql/lexer.c \
    $(SRCDIR)/sql/parser.c \
    $(SRCDIR)/sql/executor.c \
    $(SRCDIR)/network/protocol.c \
    $(SRCDIR)/network/server.c \
    $(SRCDIR)/engine.c

ENGINE_OBJS = $(ENGINE_SRCS:$(SRCDIR)/%.c=$(BLDDIR)/%.o)

.PHONY: all debug release clean server cli test

all: release

# ── Debug ───────────────────────────────────────────────────────
debug: CFLAGS += $(DBGFLAGS)
debug: $(BINDIR)/novadb-server $(BINDIR)/novadb-cli

# ── Release ─────────────────────────────────────────────────────
release: CFLAGS += $(RELFLAGS)
release: $(BINDIR)/novadb-server $(BINDIR)/novadb-cli

# ── Server binary ───────────────────────────────────────────────
$(BINDIR)/novadb-server: $(ENGINE_OBJS) $(BLDDIR)/main.o | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ── CLI client ──────────────────────────────────────────────────
$(BINDIR)/novadb-cli: $(BLDDIR)/cli.o | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ── Pattern rules ───────────────────────────────────────────────
$(BLDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BLDDIR)/main.o: $(SRCDIR)/main.c
	@mkdir -p $(BLDDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BLDDIR)/cli.o: contrib/novacli.c
	@mkdir -p $(BLDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Directories ─────────────────────────────────────────────────
$(BINDIR):
	mkdir -p $(BINDIR)

test: debug
	$(CC) $(CFLAGS) $(DBGFLAGS) $(INCLUDES) \
		-o $(BINDIR)/novadb-test \
		test/test_main.c test/test_btree.c test/test_wal.c \
		test/test_sql.c test/test_integration.c \
		$(ENGINE_SRCS) $(LDFLAGS)
	./$(BINDIR)/novadb-test

# ── Clean ───────────────────────────────────────────────────────
clean:
	rm -rf $(BLDDIR) $(BINDIR)

# ── Help ────────────────────────────────────────────────────────
help:
	@echo "NovaDB Build System"
	@echo "  make release  — optimised build"
	@echo "  make debug    — with sanitizers"
	@echo "  make test     — build and run all tests"
	@echo "  make clean    — remove build artifacts"
