#include "yql_dq_http_datasource_type_ann.h"

#include "yql_dq_http_expr_nodes.h"

#include <yql/essentials/core/expr_nodes/yql_expr_nodes.h>
#include <yql/essentials/type_info/type.h>

namespace NYql {

    void RegisterDqHttpTypeAnnotation(IGraphTransformer::TAddHandlers& addHandlers) {
        addHandlers.AddHandler({NNodes::TDqHttpLookupSourceSettings::CallableName()},
            [&](const TExprNode::TPtr& input, TExprContext& ctx) {
                // Validate minimum argument count (Endpoint, PathTemplate, Method are required)
                if (!NCommon::EnsureArgsCount(*input, 3, ctx)) {
                    return IGraphTransformer::TStatus::Error;
                }

                // Endpoint must be an atom (string literal)
                if (!NCommon::EnsureAtom(*input->Child(NNodes::TDqHttpLookupSourceSettings::idx_Endpoint), ctx)) {
                    return IGraphTransformer::TStatus::Error;
                }

                // PathTemplate must be an atom (string literal)
                if (!NCommon::EnsureAtom(*input->Child(NNodes::TDqHttpLookupSourceSettings::idx_PathTemplate), ctx)) {
                    return IGraphTransformer::TStatus::Error;
                }

                // Method must be an atom (string literal: GET, POST, etc.)
                if (!NCommon::EnsureAtom(*input->Child(NNodes::TDqHttpLookupSourceSettings::idx_Method), ctx)) {
                    return IGraphTransformer::TStatus::Error;
                }

                // Validate Method value
                TString method = input->Child(NNodes::TDqHttpLookupSourceSettings::idx_Method)->Content();
                if (method != "GET" && method != "POST" && method != "PUT" && method != "PATCH" && method != "DELETE") {
                    ctx.AddError(TIssue(ctx.GetPosition(input->Child(NNodes::TDqHttpLookupSourceSettings::idx_Method)->Pos()),
                        "HTTP method must be one of: GET, POST, PUT, PATCH, DELETE"));
                    return IGraphTransformer::TStatus::Error;
                }

                // Optional: BodyTemplate (atom)
                if (input->ChildrenSize() > NNodes::TDqHttpLookupSourceSettings::idx_BodyTemplate &&
                    !input->Child(NNodes::TDqHttpLookupSourceSettings::idx_BodyTemplate)->IsCallable("Void")) {
                    if (!NCommon::EnsureAtom(*input->Child(NNodes::TDqHttpLookupSourceSettings::idx_BodyTemplate), ctx)) {
                        return IGraphTransformer::TStatus::Error;
                    }
                }

                // Optional: TimeoutMs (atom with numeric value)
                if (input->ChildrenSize() > NNodes::TDqHttpLookupSourceSettings::idx_TimeoutMs &&
                    !input->Child(NNodes::TDqHttpLookupSourceSettings::idx_TimeoutMs)->IsCallable("Void")) {
                    if (!NCommon::EnsureAtom(*input->Child(NNodes::TDqHttpLookupSourceSettings::idx_TimeoutMs), ctx)) {
                        return IGraphTransformer::TStatus::Error;
                    }
                }

                // Optional: CachePolicy (atom)
                if (input->ChildrenSize() > NNodes::TDqHttpLookupSourceSettings::idx_CachePolicy &&
                    !input->Child(NNodes::TDqHttpLookupSourceSettings::idx_CachePolicy)->IsCallable("Void")) {
                    if (!NCommon::EnsureAtom(*input->Child(NNodes::TDqHttpLookupSourceSettings::idx_CachePolicy), ctx)) {
                        return IGraphTransformer::TStatus::Error;
                    }
                }

                // Optional: CacheTtlSeconds (atom with numeric value)
                if (input->ChildrenSize() > NNodes::TDqHttpLookupSourceSettings::idx_CacheTtlSeconds &&
                    !input->Child(NNodes::TDqHttpLookupSourceSettings::idx_CacheTtlSeconds)->IsCallable("Void")) {
                    if (!NCommon::EnsureAtom(*input->Child(NNodes::TDqHttpLookupSourceSettings::idx_CacheTtlSeconds), ctx)) {
                        return IGraphTransformer::TStatus::Error;
                    }
                }

                // Optional: AuthTokenSecretName (atom)
                if (input->ChildrenSize() > NNodes::TDqHttpLookupSourceSettings::idx_AuthTokenSecretName &&
                    !input->Child(NNodes::TDqHttpLookupSourceSettings::idx_AuthTokenSecretName)->IsCallable("Void")) {
                    if (!NCommon::EnsureAtom(*input->Child(NNodes::TDqHttpLookupSourceSettings::idx_AuthTokenSecretName), ctx)) {
                        return IGraphTransformer::TStatus::Error;
                    }
                }

                // Optional: MaxBatchSize (atom with numeric value)
                if (input->ChildrenSize() > NNodes::TDqHttpLookupSourceSettings::idx_MaxBatchSize &&
                    !input->Child(NNodes::TDqHttpLookupSourceSettings::idx_MaxBatchSize)->IsCallable("Void")) {
                    if (!NCommon::EnsureAtom(*input->Child(NNodes::TDqHttpLookupSourceSettings::idx_MaxBatchSize), ctx)) {
                        return IGraphTransformer::TStatus::Error;
                    }
                }

                // Set output type: Struct<StatusCode: Uint32, Headers: Dict<String,String>, Body: String>
                auto statusCodeType = ctx.MakeType<TUint32ExprType>();
                auto headersType = ctx.MakeType<TDictExprType>(
                    ctx.MakeType<TStringExprType>(),
                    ctx.MakeType<TStringExprType>());
                auto bodyType = ctx.MakeType<TStringExprType>();

                auto structType = ctx.MakeType<TStructExprType>(
                    TStructExprType::TMembers{
                        {TString("StatusCode"), statusCodeType},
                        {TString("Headers"), headersType},
                        {TString("Body"), bodyType},
                    });

                input->SetTypeAnn(structType->Raw());
                return IGraphTransformer::TStatus::Ok;
            });
    }

} // namespace NYql
