UNITTEST_FOR(ydb/services/ydb)

FORK_SUBTESTS()
SPLIT_FACTOR(60)

IF (SANITIZER_TYPE == "thread" OR WITH_VALGRIND)
    SIZE(LARGE)
    TAG(ya:fat)
ELSE()
    SIZE(MEDIUM)
ENDIF()

SRCS(
    ydb_bulk_upsert_ut.cpp
    ydb_bulk_upsert_olap_ut.cpp
    ydb_coordination_ut.cpp
    ydb_index_table_ut.cpp
    ydb_import_ut.cpp
    ydb_ut.cpp
    ydb_register_node_ut.cpp
    ydb_scripting_ut.cpp
    ydb_table_ut.cpp
    ydb_stats_ut.cpp
    ydb_logstore_ut.cpp
    ydb_olapstore_ut.cpp
    ydb_monitoring_ut.cpp
    ydb_query_ut.cpp
    ydb_read_rows_ut.cpp
    ydb_ldap_login_ut.cpp
    ydb_login_ut.cpp
    ydb_object_storage_ut.cpp
)

PEERDIR(
    contrib/libs/apache/arrow
    library/cpp/getopt
    ydb/public/sdk/cpp/src/library/grpc/client
    library/cpp/regex/pcre
    library/cpp/svnversion
    ydb/core/kqp/ut/common
    ydb/core/testlib/pg
    ydb/core/tx/datashard/ut_common
    ydb/core/grpc_services/base
    ydb/core/testlib
    ydb/core/security
    yql/essentials/minikql/dom
    yql/essentials/minikql/jsonpath
    ydb/library/testlib/service_mocks/ldap_mock
    ydb/public/lib/experimental
    ydb/public/lib/json_value
    ydb/public/lib/yson_value
    ydb/public/lib/ut_helpers
    ydb/public/lib/ydb_cli/commands
    ydb/public/sdk/cpp/src/client/draft
    ydb/public/sdk/cpp/src/client/coordination
    ydb/public/sdk/cpp/src/client/export
    ydb/public/sdk/cpp/src/client/extension_common
    ydb/public/sdk/cpp/src/client/operation
    ydb/public/sdk/cpp/src/client/scheme
    ydb/public/sdk/cpp/src/client/monitoring
    ydb/services/ydb
)

YQL_LAST_ABI_VERSION()

END()
