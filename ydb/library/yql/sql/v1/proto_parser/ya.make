LIBRARY()

PEERDIR(
    ydb/library/yql/utils

    ydb/library/yql/parser/proto_ast/antlr3
    ydb/library/yql/parser/proto_ast/antlr4
    ydb/library/yql/parser/proto_ast/collect_issues
    ydb/library/yql/parser/proto_ast/gen/v1
    ydb/library/yql/parser/proto_ast/gen/v1_ansi
    ydb/library/yql/parser/proto_ast/gen/v1_proto_split
    ydb/library/yql/parser/proto_ast/gen/v1_antlr4
    ydb/library/yql/parser/proto_ast/gen/v1_ansi_antlr4
)

SRCS(
    proto_parser.cpp
)

END()
