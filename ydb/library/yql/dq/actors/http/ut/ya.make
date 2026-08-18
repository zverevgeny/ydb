UNITTEST_FOR(ydb/library/yql/dq/actors/http)

FORK_SUBTESTS()

SIZE(MEDIUM)

SRCS(
    http_egress_actor_ut.cpp
    http_lookup_source_ut.cpp
)

YQL_LAST_ABI_VERSION()

PEERDIR(
    library/cpp/testing/unittest
    ydb/library/actors/testlib
    ydb/library/actors/core
    ydb/library/actors/http
    ydb/library/yql/dq/actors/http
)

END()
