UNITTEST_FOR(ydb/library/yql/providers/pq/gateway/clients/qyt)

SIZE(MEDIUM)

SRCS(
    yql_qyt_blocking_queue_ut.cpp
)

PEERDIR(
    ydb/library/yql/providers/pq/gateway/clients/qyt
    library/cpp/testing/unittest
)

YQL_LAST_ABI_VERSION()

END()
