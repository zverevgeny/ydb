#include "parser.h"

#include "ast_builder.h"
#include "type_check.h"

#include <yql/essentials/core/issue/protos/issue_id.pb.h>
#include <yql/essentials/parser/proto_ast/gen/jsonpath/JsonPathLexer.h>
#include <yql/essentials/parser/proto_ast/gen/jsonpath/JsonPathParser.h>
#include <yql/essentials/parser/proto_ast/gen/jsonpath/JsonPathParser.pb.h>
#include <yql/essentials/parser/proto_ast/antlr3/proto_ast_antlr3.h>

#include <google/protobuf/message.h>

#if defined(_tsan_enabled_)
#include <util/system/mutex.h>
#endif

#include <util/string/strip.h>

namespace {

using namespace NYql;

#if defined(_tsan_enabled_)
TMutex SanitizerJsonPathTranslationMutex;
#endif

class TParseErrorsCollector : public NProtoAST::IErrorCollector {
public:
    TParseErrorsCollector(TIssues& issues, size_t maxErrors)
        : IErrorCollector(maxErrors)
        , Issues_(issues)
    {
    }

private:
    void AddError(ui32 line, ui32 column, const TString& message) override {
        Issues_.AddIssue(TPosition(column, line, "jsonpath"), StripString(message));
        Issues_.back().SetCode(TIssuesIds::JSONPATH_PARSE_ERROR, TSeverityIds::S_ERROR);
    }

    TIssues& Issues_;
};

}

namespace NYql::NJsonPath {

const TAstNodePtr ParseJsonPathAst(const TStringBuf path, TIssues& issues, size_t maxParseErrors) {
    if (!IsUtf(path)) {
        issues.AddIssue(TPosition(1, 1, "jsonpath"), "JsonPath must be UTF-8 encoded string");
        issues.back().SetCode(TIssuesIds::JSONPATH_PARSE_ERROR, TSeverityIds::S_ERROR);
        return {};
    }

    google::protobuf::Arena arena;
    const google::protobuf::Message* rawAst = nullptr;
    {
    #if defined(_tsan_enabled_)
        TGuard<TMutex> guard(SanitizerJsonPathTranslationMutex);
    #endif
        NProtoAST::TProtoASTBuilder3<NALP::JsonPathParser, NALP::JsonPathLexer> builder(path, "JsonPath", &arena);
        TParseErrorsCollector collector(issues, maxParseErrors);
        rawAst = builder.BuildAST(collector);
    }

    if (rawAst == nullptr) {
        return nullptr;
    }

    const google::protobuf::Descriptor* descriptor = rawAst->GetDescriptor();
    if (descriptor && descriptor->name() != "TJsonPathParserAST") {
        return nullptr;
    }

    const auto* protoAst = static_cast<const NJsonPathGenerated::TJsonPathParserAST*>(rawAst);
    TAstBuilder astBuilder(issues);
    TAstNodePtr ast = astBuilder.Build(*protoAst);
    if (!issues.Empty()) {
        return nullptr;
    }

    // At this point AST is guaranteed to be valid. We return it even if
    // type checker finds some logical errors.
    TJsonPathTypeChecker checker(issues);
    ast->Accept(checker);
    return ast;
}

const TJsonPathPtr PackBinaryJsonPath(const TAstNodePtr ast) {
    TJsonPathBuilder builder;
    ast->Accept(builder);
    return builder.ShrinkAndGetResult();
}

const TJsonPathPtr ParseJsonPath(const TStringBuf path, TIssues& issues, size_t maxParseErrors) {
    const auto ast = ParseJsonPathAst(path, issues, maxParseErrors);
    if (!issues.Empty()) {
        return {};
    }
    return PackBinaryJsonPath(ast);
}

}
