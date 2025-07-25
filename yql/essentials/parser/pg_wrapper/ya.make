LIBRARY()

PROVIDES(
    yql_pg_runtime
)

CXXFLAGS(-DMKQL_DISABLE_CODEGEN)

YQL_LAST_ABI_VERSION()

ADDINCL(
    contrib/libs/lz4
    yql/essentials/parser/pg_wrapper/postgresql/src/backend/bootstrap
    yql/essentials/parser/pg_wrapper/postgresql/src/backend/parser
    yql/essentials/parser/pg_wrapper/postgresql/src/backend/replication
    yql/essentials/parser/pg_wrapper/postgresql/src/backend/replication/logical
    yql/essentials/parser/pg_wrapper/postgresql/src/backend/utils/adt
    yql/essentials/parser/pg_wrapper/postgresql/src/backend/utils/misc
    yql/essentials/parser/pg_wrapper/postgresql/src/backend/utils/sort
    yql/essentials/parser/pg_wrapper/postgresql/src/common
    yql/essentials/parser/pg_wrapper/postgresql/src/include
    yql/essentials/parser/pg_wrapper/postgresql/src/port
)

IF (NOT BUILD_POSTGRES_ONLY)
SRCS(
    arena_ctx.cpp
    arrow.cpp
    arrow_impl.cpp
    conversion.cpp
    parser.cpp
    thread_inits.c
    comp_factory.cpp
    type_cache.cpp
    pg_aggs.cpp
    read_table.cpp
    recovery.cpp
    superuser.cpp
    config.cpp
    cost_mocks.cpp
    syscache.cpp
    pg_utils_wrappers.cpp
    utils.cpp
    ctors.cpp
)
ENDIF()

IF (ARCH_X86_64)
    CFLAGS(
        -DHAVE__GET_CPUID=1
        -DUSE_SSE42_CRC32C_WITH_RUNTIME_CHECK=1
    )
    SRCS(
        postgresql/src/port/pg_crc32c_sse42.c
        postgresql/src/port/pg_crc32c_sse42_choose.c
    )
ENDIF()

INCLUDE(pg_sources.inc)

IF (NOT BUILD_POSTGRES_ONLY)
INCLUDE(pg_kernel_sources.inc)
ENDIF()

IF (NOT OS_WINDOWS AND NOT SANITIZER_TYPE AND NOT BUILD_TYPE == "DEBUG")
IF (NOT BUILD_POSTGRES_ONLY)
IF (YQL_USE_PG_BC)
USE_LLVM_BC16()
INCLUDE(pg_bc.all.inc)
ENDIF()
ENDIF()
ELSE()
CFLAGS(-DUSE_SLOW_PG_KERNELS)
ENDIF()

IF (BUILD_TYPE == "DEBUG")
CFLAGS(-DDISABLE_COMPLEX_MACRO)
ENDIF()

PEERDIR(
    library/cpp/resource
    library/cpp/yson
    library/cpp/string_utils/base64
    yql/essentials/core
    yql/essentials/minikql/arrow
    yql/essentials/minikql/computation
    yql/essentials/parser/pg_catalog
    yql/essentials/parser/pg_wrapper/interface
    yql/essentials/providers/common/codec
    yql/essentials/public/issue
    yql/essentials/public/udf
    yql/essentials/utils
    yql/essentials/public/decimal
    yql/essentials/public/result_format
    yql/essentials/types/binary_json
    yql/essentials/types/dynumber
    yql/essentials/types/uuid

    contrib/libs/icu
    contrib/libs/libc_compat
    contrib/libs/libxml
    contrib/libs/lz4
    contrib/libs/openssl
)

INCLUDE(cflags.inc)

IF (OS_LINUX)
    SRCS(
        postgresql/src/port/strlcat.c
        postgresql/src/port/strlcpy.c
    )
ENDIF()

IF (OS_LINUX OR OS_DARWIN)
    SRCS(
        postgresql/src/backend/port/posix_sema.c
        postgresql/src/backend/port/sysv_shmem.c
    )
ELSEIF (OS_WINDOWS)
    ADDINCL(
        yql/essentials/parser/pg_wrapper/postgresql/src/include
        yql/essentials/parser/pg_wrapper/postgresql/src/include/port/win32
        yql/essentials/parser/pg_wrapper/postgresql/src/include/port/win32_msvc
    )
    SRCS(
        postgresql/src/backend/port/win32/crashdump.c
        postgresql/src/backend/port/win32/signal.c
        postgresql/src/backend/port/win32/socket.c
        postgresql/src/backend/port/win32/timer.c
        postgresql/src/backend/port/win32_sema.c
        postgresql/src/backend/port/win32_shmem.c
        postgresql/src/port/dirmod.c
        postgresql/src/port/getopt.c
        postgresql/src/port/inet_aton.c
        postgresql/src/port/kill.c
        postgresql/src/port/open.c
        postgresql/src/port/pwritev.c
        postgresql/src/port/system.c
        postgresql/src/port/win32common.c
        postgresql/src/port/win32dlopen.c
        postgresql/src/port/win32env.c
        postgresql/src/port/win32error.c
        postgresql/src/port/win32fseek.c
        postgresql/src/port/win32gai_strerror.c
        postgresql/src/port/win32gettimeofday.c
        postgresql/src/port/win32getrusage.c
        postgresql/src/port/win32ntdll.c
        postgresql/src/port/win32pread.c
        postgresql/src/port/win32pwrite.c
        postgresql/src/port/win32security.c
        postgresql/src/port/win32setlocale.c
        postgresql/src/port/win32stat.c
    )
ENDIF()

# Service files must be listed as dependencies to be included in export
FILES(
    copy_src.py
    copy_src.sh
    generate_kernels.py
    source.patch
    vars.txt
    verify.sh
)

END()

RECURSE(
    interface
)

RECURSE_FOR_TESTS(
    ut
    test
)
