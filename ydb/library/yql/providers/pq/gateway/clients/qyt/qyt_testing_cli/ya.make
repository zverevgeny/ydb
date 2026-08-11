PROGRAM(qyt_testing_cli)

SRCS(
    qyt_cli.cpp
)

PEERDIR(
    ydb/library/yql/providers/pq/gateway/clients/qyt
    ydb/library/yql/providers/pq/gateway/abstract
    ydb/public/sdk/cpp/src/client/topic
    yt/yt/client
    yt/yt/core
)

YQL_LAST_ABI_VERSION()

END()
