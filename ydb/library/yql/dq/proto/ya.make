PROTO_LIBRARY()
PROTOC_FATAL_WARNINGS()

PEERDIR(
    ydb/library/actors/protos
    yql/essentials/minikql/runtime_settings/proto
)

SRCS(
    dq_state_load_plan.proto
    dq_tasks.proto
    dq_transport.proto
    http_egress.proto
    http_lookup.proto
)

EXCLUDE_TAGS(GO_PROTO)

END()
