LIBRARY()

SRCS(
    yql_qyt_topic_client.cpp
    yql_yt_gateway.cpp
)

PEERDIR(
    ydb/library/yql/providers/pq/gateway/abstract
    ydb/public/sdk/cpp/src/client/driver
    ydb/public/sdk/cpp/src/client/topic
    yt/yt/client
)

YQL_LAST_ABI_VERSION()

END()
