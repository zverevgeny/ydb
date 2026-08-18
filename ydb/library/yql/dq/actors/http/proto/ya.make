PROTO_LIBRARY()
PROTOC_FATAL_WARNINGS()

SRCS(
    http_egress.proto
    http_lookup.proto
)

PEERDIR(
    ydb/library/actors/protos
    ydb/public/api/protos
    ydb/public/api/protos/annotations
    yql/essentials/public/issue/protos
    ydb/library/yql/dq/actors/protos
    yql/essentials/public/types
)

EXCLUDE_TAGS(GO_PROTO)

END()
