cmake_minimum_required(VERSION 3.10)
project(NovaDB VERSION 1.0.0 LANGUAGES C)

# ── C11 with strict warnings ───────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Werror=implicit-function-declaration
        -Werror=return-type
        -Werror=uninitialized
        -Wstrict-prototypes
        -Wmissing-prototypes
        -Wno-unused-parameter
        -fno-strict-aliasing
        -fvisibility=hidden
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g3 -O0 -fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    else()
        add_compile_options(-O2 -DNDEBUG)
    endif()
endif()

# ── Feature detection ──────────────────────────────────────────
include(CheckIncludeFile)
include(CheckFunctionExists)
include(CheckSymbolExists)

check_include_file("sys/epoll.h"    HAVE_EPOLL)
check_include_file("sys/event.h"    HAVE_KQUEUE)
check_include_file("fcntl.h"        HAVE_FCNTL)
check_include_file("sys/mman.h"     HAVE_MMAP)
check_include_file("byteswap.h"     HAVE_BYTESWAP_H)

check_function_exists("pread"       HAVE_PREAD)
check_function_exists("pwrite"      HAVE_PWRITE)
check_function_exists("fallocate"   HAVE_FALLOCATE)
check_function_exists("posix_fadvise" HAVE_FADVISE)

# ── Configuration header ───────────────────────────────────────
configure_file(
    include/novadb/config.h.in
    ${CMAKE_BINARY_DIR}/generated/config.h
)

# ── Library: libnovadb ─────────────────────────────────────────
set(NVDB_SOURCES
    # Common
    src/common/memory.c
    src/common/logging.c

    # Storage
    src/storage/page.c
    src/storage/btree.c
    src/storage/wal.c
    src/storage/buffer.c

    # Transactions
    src/txn/transaction.c
    src/txn/mvcc.c

    # SQL
    src/sql/lexer.c
    src/sql/parser.c
    src/sql/executor.c
    src/sql/planner.c

    # Network
    src/network/server.c
    src/network/protocol.c
)

set(NVDB_HEADERS
    include/novadb/nvdb.h
    include/novadb/types.h
    include/novadb/error.h
    ${CMAKE_BINARY_DIR}/generated/config.h
)

add_library(novadb STATIC ${NVDB_SOURCES})
target_include_directories(novadb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(novadb PRIVATE pthread m)

# ── Server binary ──────────────────────────────────────────────
add_executable(novadb-server src/main.c)
target_link_libraries(novadb-server novadb)

# ── CLI client ─────────────────────────────────────────────────
add_executable(novadb-cli contrib/novacli.c)
target_link_libraries(novadb-cli novadb)

# ── Tests ──────────────────────────────────────────────────────
option(BUILD_TESTS "Build test suite" ON)
if(BUILD_TESTS)
    enable_testing()
    add_executable(novadb-test
        test/test_btree.c
        test/test_wal.c
        test/test_sql.c
        test/test_integration.c
        test/test_main.c
    )
    target_link_libraries(novadb-test novadb)
    add_test(NAME novadb-test COMMAND novadb-test)
endif()

# ── Install ────────────────────────────────────────────────────
install(TARGETS novadb novadb-server novadb-cli
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
)
install(DIRECTORY include/novadb DESTINATION include)
