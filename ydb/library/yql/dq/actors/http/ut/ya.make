UNITTEST_FOR(ydb/library/yql/dq/actors/http)

SRCS(
    http_egress_actor_ut.cpp
)

PEERDIR(
    ydb/library/actors/testlib
    ydb/library/actors/http
)

END()
