LIBRARY()

SRCS(
    http_egress_actor.cpp
    http_egress_security.cpp
    http_lookup_actor.cpp
    http_lookup_source_factory.cpp
)

PEERDIR(
    ydb/library/actors/core
    ydb/library/actors/http
    ydb/library/actors/util
    ydb/library/yql/dq/actors/compute
    ydb/library/yql/dq/actors/protos
    ydb/library/yql/dq/proto
    ydb/library/yql/public/ydb_issue
    yql/essentials/minikql
    yql/essentials/minikql/computation
    yql/essentials/public/issue
    yql/essentials/public/udf
)

YQL_LAST_ABI_VERSION()

END()

RECURSE_FOR_TESTS(
    ut
)
