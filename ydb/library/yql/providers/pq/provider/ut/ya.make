UNITTEST_FOR(ydb/library/yql/providers/pq/provider)

SIZE(MEDIUM)

SRCS(
    yql_pq_datasource_ut.cpp
)

PEERDIR(
    ydb/library/yql/providers/pq/gateway/dummy
    yql/essentials/sql/pg_dummy
    yql/essentials/public/udf/service/stub
)

YQL_LAST_ABI_VERSION()

END()
