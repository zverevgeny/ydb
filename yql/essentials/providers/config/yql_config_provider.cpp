#include "yql_config_provider.h"

#include <yql/essentials/providers/common/provider/yql_provider_names.h>
#include <yql/essentials/providers/common/provider/yql_data_provider_impl.h>
#include <yql/essentials/providers/common/proto/gateways_config.pb.h>
#include <yql/essentials/providers/common/provider/yql_provider.h>
#include <yql/essentials/providers/common/activation/yql_activation.h>
#include <yql/essentials/core/expr_nodes/yql_expr_nodes.h>
#include <yql/essentials/core/yql_execution.h>
#include <yql/essentials/core/yql_expr_optimize.h>
#include <yql/essentials/core/yql_expr_type_annotation.h>
#include <yql/essentials/core/type_ann/type_ann_core.h>
#include <yql/essentials/ast/yql_gc_nodes.h>
#include <yql/essentials/utils/log/log.h>
#include <yql/essentials/utils/fetch/fetch.h>
#include <yql/essentials/utils/retry.h>

#include <library/cpp/json/json_reader.h>

#include <util/string/cast.h>
#include <util/generic/hash.h>
#include <util/generic/utility.h>
#include <util/string/builder.h>

#include <vector>

namespace NYql {

namespace {
    using namespace NNodes;

    class TConfigCallableExecutionTransformer : public TSyncTransformerBase {
    public:
        TConfigCallableExecutionTransformer(const TTypeAnnotationContext& types)
            : Types_(types)
        {
            Y_UNUSED(Types_);
        }

        TStatus DoTransform(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) final {
            output = input;
            YQL_ENSURE(input->Type() == TExprNode::Callable);
            if (input->Content() == "Pull") {
                auto requireStatus = RequireChild(*input, 0);
                if (requireStatus.Level != TStatus::Ok) {
                    return requireStatus;
                }

                IDataProvider::TFillSettings fillSettings = NCommon::GetFillSettings(*input);
                YQL_ENSURE(fillSettings.Format == IDataProvider::EResultFormat::Yson);
                NYson::EYsonFormat ysonFormat = NCommon::GetYsonFormat(fillSettings);

                auto nodeToPull = input->Child(0)->Child(0);
                if (nodeToPull->IsCallable(ConfReadName)) {
                    auto key = nodeToPull->Child(2);
                    auto tag = key->Child(0)->Child(0)->Content();
                    if (tag == "data_sinks" || tag == "data_sources") {
                        TStringStream out;
                        NYson::TYsonWriter writer(&out, ysonFormat);
                        writer.OnBeginMap();
                        writer.OnKeyedItem("Data");
                        writer.OnBeginList();
                        if (tag == "data_sinks") {
                            for (const auto& ds: Types_.DataSinks) {
                                writer.OnListItem();
                                writer.OnStringScalar(ds->GetName());
                            }
                        } else if (tag == "data_sources") {
                            for (const auto& ds: Types_.DataSources) {
                                writer.OnListItem();
                                writer.OnStringScalar(ds->GetName());
                            }
                        }
                        writer.OnEndList();
                        writer.OnEndMap();

                        input->SetResult(ctx.NewAtom(input->Pos(), out.Str()));
                        input->SetState(TExprNode::EState::ExecutionComplete);
                        return TStatus::Ok;
                    } else {
                        ctx.AddError(TIssue(ctx.GetPosition(input->Pos()), TStringBuilder() << "Unsupported tag: " << tag));
                        return TStatus::Error;
                    }
                }

                ctx.AddError(TIssue(ctx.GetPosition(input->Pos()), TStringBuilder() << "Unknown node to pull, type: "
                    << nodeToPull->Type() << ", content: " << nodeToPull->Content()));
                return TStatus::Error;
            }

            if (input->Content() == ConfReadName) {
                auto requireStatus = RequireChild(*input, 0);
                if (requireStatus.Level != TStatus::Ok) {
                    return requireStatus;
                }

                input->SetState(TExprNode::EState::ExecutionComplete);
                input->SetResult(ctx.NewWorld(input->Pos()));
                return TStatus::Ok;
            }

            if (input->Content() == ConfigureName) {
                auto requireStatus = RequireChild(*input, 0);
                if (requireStatus.Level != TStatus::Ok) {
                    return requireStatus;
                }

                input->SetState(TExprNode::EState::ExecutionComplete);
                input->SetResult(ctx.NewWorld(input->Pos()));
                return TStatus::Ok;
            }

            ctx.AddError(TIssue(ctx.GetPosition(input->Pos()), TStringBuilder() << "Failed to execute node: " << input->Content()));
            return TStatus::Error;
        }
        void Rewind() final {
        }

    private:
        const TTypeAnnotationContext& Types_;
    };

    class TConfigProvider : public TDataProviderBase {
    public:
        struct TFunctions {
            THashSet<TStringBuf> Names;

            TFunctions() {
                Names.insert(ConfReadName);
            }
        };

        TConfigProvider(TTypeAnnotationContext& types, const TGatewaysConfig* config, const TString& username, const TAllowSettingPolicy& policy)
            : Types_(types)
            , CoreConfig_(config && config->HasYqlCore() ? &config->GetYqlCore() : nullptr)
            , Username_(username)
            , Policy_(policy)
        {}

        TStringBuf GetName() const override {
            return ConfigProviderName;
        }

        bool Initialize(TExprContext& ctx) override {
            std::unordered_set<std::string_view> groups;
            if (Types_.Credentials != nullptr) {
                groups.insert(Types_.Credentials->GetGroups().begin(), Types_.Credentials->GetGroups().end());
            }
            auto filter = [this, groups = std::move(groups)](const TCoreAttr& attr) {
                if (!attr.HasActivation() || !Username_) {
                    return true;
                }
                if (NConfig::Allow(attr.GetActivation(), Username_, groups)) {
                    Statistics_.Entries.emplace_back(TStringBuilder() << "Activation:" << attr.GetName(), 0, 0, 0, 0, 1);
                    return true;
                }
                return false;
            };
            if (CoreConfig_) {
                TPosition pos;
                for (auto& flag: CoreConfig_->GetFlags()) {
                    if (filter(flag)) {
                        TVector<TStringBuf> args;
                        for (auto& arg: flag.GetArgs()) {
                            args.push_back(arg);
                        }
                        if (!ApplyFlag(pos, flag.GetName(), args, ctx)) {
                            return false;
                        }
                    }
                }
            }
            return true;
        }

        bool CollectStatistics(NYson::TYsonWriter& writer, bool totalOnly) override {
            if (Statistics_.Entries.empty()) {
                return false;
            }

            THashMap<ui32, TOperationStatistics> tmp;
            tmp.emplace(Max<ui32>(), Statistics_);
            NCommon::WriteStatistics(writer, totalOnly, tmp);

            return true;
        }

        bool ValidateParameters(TExprNode& node, TExprContext& ctx, TMaybe<TString>& cluster) override {
            if (!EnsureArgsCount(node, 1, ctx)) {
                return false;
            }

            cluster = Nothing();
            return true;
        }

        bool MatchCategory(const TExprNode& node) {
            return (node.Child(1)->Child(0)->Content() == ConfigProviderName);
        }

        bool CanParse(const TExprNode& node) override {
            if (ConfigProviderFunctions().contains(node.Content()) ||
                node.Content() == ConfigureName)
            {
                return MatchCategory(node);
            }

            return false;
        }

        IGraphTransformer& GetConfigurationTransformer() override {
            if (ConfigurationTransformer_) {
                return *ConfigurationTransformer_;
            }

            ConfigurationTransformer_ = CreateFunctorTransformer(
                [this](const TExprNode::TPtr& input, TExprNode::TPtr& output, TExprContext& ctx) -> IGraphTransformer::TStatus {
                output = input;
                if (ctx.Step.IsDone(TExprStep::Configure)) {
                    return IGraphTransformer::TStatus::Ok;
                }

                bool hasPendingEvaluations = false;
                TOptimizeExprSettings settings(nullptr);
                settings.VisitChanges = true;
                auto status = OptimizeExpr(input, output, [&](const TExprNode::TPtr& node, TExprContext& ctx) -> TExprNode::TPtr {
                    auto res = node;
                    if (!hasPendingEvaluations && node->Content() == ConfigureName) {
                        if (!EnsureMinArgsCount(*node, 2, ctx)) {
                            return {};
                        }

                        if (!node->Child(1)->IsCallable("DataSource")) {
                            return node;
                        }

                        if (node->Child(1)->Child(0)->Content() != ConfigProviderName) {
                            return node;
                        }

                        if (!EnsureMinArgsCount(*node, 3, ctx)) {
                            return {};
                        }

                        if (!EnsureAtom(*node->Child(2), ctx)) {
                            return {};
                        }

                        TStringBuf command = node->Child(2)->Content();
                        if (command.length() && '_' == command[0]) {
                            ctx.AddError(TIssue(ctx.GetPosition(node->Child(2)->Pos()), "Flags started with underscore are not allowed"));
                            return {};
                        }

                        TVector<TStringBuf> args;
                        for (size_t i = 3; i < node->ChildrenSize(); ++i) {
                            if (node->Child(i)->IsCallable("EvaluateAtom")) {
                                hasPendingEvaluations = true;
                                return res;
                            }
                            if (!EnsureAtom(*node->Child(i), ctx)) {
                                return {};
                            }
                            args.push_back(node->Child(i)->Content());
                        }

                        if (!ApplyFlag(ctx.GetPosition(node->Child(2)->Pos()), command, args, ctx)) {
                            return {};
                        }

                        if (command == "PureDataSource") {
                            if (Types_.PureResultDataSource != node->Child(3)->Content()) {
                                res = ctx.ChangeChild(*node, 3, ctx.RenameNode(*node->Child(3), Types_.PureResultDataSource));
                            }
                        }
                    }

                    return res;
                }, ctx, settings);

                return status;
            });

            return *ConfigurationTransformer_;
        }

        IGraphTransformer& GetTypeAnnotationTransformer(bool instantOnly) override {
            Y_UNUSED(instantOnly);
            if (!TypeAnnotationTransformer_) {
                TypeAnnotationTransformer_ = CreateFunctorTransformer(
                    [&](const TExprNode::TPtr& input, TExprNode::TPtr& output, TExprContext& ctx) -> IGraphTransformer::TStatus {
                    output = input;
                    if (input->Content() == ConfReadName) {
                        if (!EnsureWorldType(*input->Child(0), ctx)) {
                            return IGraphTransformer::TStatus::Error;
                        }

                        if (!EnsureSpecificDataSource(*input->Child(1), ConfigProviderName, ctx)) {
                            return IGraphTransformer::TStatus::Error;
                        }

                        auto key = input->Child(2);
                        if (!key->IsCallable("Key")) {
                            ctx.AddError(TIssue(ctx.GetPosition(key->Pos()), "Expected key"));
                            return IGraphTransformer::TStatus::Error;
                        }

                        if (key->ChildrenSize() == 0) {
                            ctx.AddError(TIssue(ctx.GetPosition(key->Pos()), "Empty key is not allowed"));
                            return IGraphTransformer::TStatus::Error;
                        }

                        auto tag = key->Child(0)->Child(0)->Content();
                        if (key->Child(0)->ChildrenSize() > 1) {
                            ctx.AddError(TIssue(ctx.GetPosition(key->Child(0)->Pos()), "Only tag must be specified"));
                            return IGraphTransformer::TStatus::Error;
                        }

                        if (key->ChildrenSize() > 1) {
                            ctx.AddError(TIssue(ctx.GetPosition(key->Pos()), "Too many tags"));
                            return IGraphTransformer::TStatus::Error;
                        }

                        auto fields = input->Child(3);
                        if (!EnsureTuple(*fields, ctx)) {
                            return IGraphTransformer::TStatus::Error;
                        }

                        if (fields->ChildrenSize() != 0) {
                            ctx.AddError(TIssue(ctx.GetPosition(fields->Pos()), "Fields tuple must be empty"));
                            return IGraphTransformer::TStatus::Error;
                        }

                        if (!input->Child(3)->GetTypeAnn() || !input->Child(3)->IsComposable()) {
                            ctx.AddError(TIssue(ctx.GetPosition(input->Child(3)->Pos()), "Expected composable data"));
                            return IGraphTransformer::TStatus::Error;
                        }

                        auto settings = input->Child(4);
                        if (!EnsureTuple(*settings, ctx)) {
                            return IGraphTransformer::TStatus::Error;
                        }

                        if (settings->ChildrenSize() != 0) {
                            ctx.AddError(TIssue(ctx.GetPosition(settings->Pos()), "Unsupported settings"));
                            return IGraphTransformer::TStatus::Error;
                        }

                        auto stringAnnotation = ctx.MakeType<TDataExprType>(EDataSlot::String);
                        auto listOfString = ctx.MakeType<TListExprType>(stringAnnotation);
                        TTypeAnnotationNode::TListType children;
                        children.push_back(input->Child(0)->GetTypeAnn());
                        if (tag == "data_sources" || tag == "data_sinks") {
                            children.push_back(listOfString);
                        } else {
                            ctx.AddError(TIssue(ctx.GetPosition(key->Pos()), TStringBuilder() << "Unknown tag: " << tag));
                            return IGraphTransformer::TStatus::Error;
                        }

                        auto tupleAnn = ctx.MakeType<TTupleExprType>(children);
                        input->SetTypeAnn(tupleAnn);
                        return IGraphTransformer::TStatus::Ok;
                    }
                    else if (input->Content() == ConfigureName) {
                        if (!EnsureWorldType(*input->Child(0), ctx)) {
                            return IGraphTransformer::TStatus::Error;
                        }

                        input->SetTypeAnn(input->Child(0)->GetTypeAnn());
                        return IGraphTransformer::TStatus::Ok;
                    }

                    ctx.AddError(TIssue(ctx.GetPosition(input->Pos()), TStringBuilder() << "(Config) Unsupported function: " << input->Content()));
                    return IGraphTransformer::TStatus::Error;
                });
            }

            return *TypeAnnotationTransformer_;
        }

        TExprNode::TPtr RewriteIO(const TExprNode::TPtr& node, TExprContext& ctx) override {
            auto read = node->Child(0);
            TString newName;
            if (read->Content() == ReadName) {
                newName = ConfReadName;
            }
            else {
                YQL_ENSURE(false, "Expected Read!");
            }

            YQL_CLOG(INFO, ProviderConfig) << "RewriteIO";
            auto newRead = ctx.RenameNode(*read, newName);
            auto retChildren = node->ChildrenList();
            retChildren[0] = newRead;
            return ctx.ChangeChildren(*node, std::move(retChildren));
        }

        bool CanPullResult(const TExprNode& node, TSyncMap& syncList, bool& canRef) override {
            Y_UNUSED(syncList);

            if (node.IsCallable(RightName)) {
                if (node.Child(0)->IsCallable(ConfReadName)) {
                    canRef = false;
                    return true;
                }
            }

            return false;
        }

        bool CanExecute(const TExprNode& node) override {
            if (ConfigProviderFunctions().contains(node.Content()) ||
                node.Content() == ConfigureName)
            {
                return MatchCategory(node);
            }

            return false;
        }

        IGraphTransformer& GetCallableExecutionTransformer() override {
            if (!CallableExecutionTransformer_) {
                CallableExecutionTransformer_ = new TConfigCallableExecutionTransformer(Types_);
            }

            return *CallableExecutionTransformer_;
        }

        bool GetDependencies(const TExprNode& node, TExprNode::TListType& children, bool compact) override {
            Y_UNUSED(compact);
            if (CanExecute(node)) {
                children.push_back(node.ChildPtr(0));
            }

            return false;
        }

        void WritePullDetails(const TExprNode& node, NYson::TYsonWriter& writer) override {
            YQL_ENSURE(node.IsCallable(RightName));

            writer.OnKeyedItem("PullOperation");
            writer.OnStringScalar(node.Child(0)->Content());
        }

        TString GetProviderPath(const TExprNode& node) override {
            Y_UNUSED(node);
            return "config";
        }

    private:
        bool IsSettingAllowed(const TPosition& pos, TStringBuf name, TExprContext& ctx) {
            if (Policy_ && !Policy_(name)) {
                ctx.AddError(TIssue(pos, TStringBuilder() << "Changing setting " << name << " is not allowed"));
                return false;
            }
            return true;
        }

        bool ApplyFlag(const TPosition& pos, const TStringBuf name, const TVector<TStringBuf>& args, TExprContext& ctx) {
            if (!IsSettingAllowed(pos, name, ctx)) {
                return false;
            }

            if (name == "UnsecureCredential") {
                if (!AddCredential(pos, args, ctx)) {
                    return false;
                }
            } else if (name == "ImportUdfs") {
                if (!ImportUdfs(pos, args, ctx)) {
                    return false;
                }
            } else if (name == "AddFileByUrl") {
                if (!AddFileByUrl(pos, args, ctx)) {
                    return false;
                }
            } else if (name == "SetFileOption") {
                if (!SetFileOption(pos, args, ctx)) {
                    return false;
                }
            } else if (name == "AddFolderByUrl") {
                if (!AddFolderByUrl(pos, args, ctx)) {
                    return false;
                }
            } else if (name == "SetPackageVersion") {
                if (!SetPackageVersion(pos, args, ctx)) {
                    return false;
                }
            }
            else if (name == "ValidateUdf") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 1 argument, but got " << args.size()));
                    return false;
                }

                try {
                    Types_.ValidateMode = NKikimr::NUdf::ValidateModeByStr(TString(args[0]));
                } catch (const yexception& err) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << err.AsStrBuf() << ", available modes: " << NKikimr::NUdf::ValidateModeAvailables()));
                    return false;
                }
            }
            else if (name == "LLVM_OFF") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }
                Types_.OptLLVM = "OFF";
            }
            else if (name == "LLVM") {
                if (args.size() > 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected at most 1 argument, but got " << args.size()));
                    return false;
                }
                Types_.OptLLVM = args.empty() ? TString() : TString(args[0]);
            }
            else if (name == "RuntimeLogLevel") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 1 argument, but got " << args.size()));
                    return false;
                }
                auto value = NUdf::TryLevelFromString(args[0]);
                if (!value) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Invalid log level value: " << args[0]));
                    return false;
                }
                Types_.RuntimeLogLevel = *value;
            }
            else if (name == "NodesAllocationLimit") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 1 argument, but got " << args.size()));
                    return false;
                }
                if (!TryFromString(args[0], ctx.NodesAllocationLimit)) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected integer, but got: " << args[0]));
                    return false;
                }
            }
            else if (name == "StringsAllocationLimit") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 1 argument, but got " << args.size()));
                    return false;
                }
                if (!TryFromString(args[0], ctx.StringsAllocationLimit)) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected integer, but got: " << args[0]));
                    return false;
                }
            }
            else if (name == "RepeatTransformLimit") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 1 argument, but got " << args.size()));
                    return false;
                }
                if (!TryFromString(args[0], ctx.RepeatTransformLimit)) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected integer, but got: " << args[0]));
                    return false;
                }
            }
            else if (name == "TypeAnnNodeRepeatLimit") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 1 argument, but got " << args.size()));
                    return false;
                }
                if (!TryFromString(args[0], ctx.TypeAnnNodeRepeatLimit)) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected integer, but got: " << args[0]));
                    return false;
                }
            }
            else if (name == "PureDataSource") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 1 argument, but got " << args.size()));
                    return false;
                }

                auto dataSource = args[0];
                if (Find(Types_.AvailablePureResultDataSources, dataSource) == Types_.AvailablePureResultDataSources.end()) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Unsupported datasource for result provider: " << dataSource));
                    return false;
                }
                if (auto p = Types_.DataSourceMap.FindPtr(dataSource)) {
                    if ((*p)->GetName() != dataSource) {
                        dataSource = (*p)->GetName();
                    }
                } else {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Unknown datasource for result provider: " << dataSource));
                    return false;
                }

                Types_.PureResultDataSource = dataSource;
            }
            else if (name == "FullResultDataSink") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 1 argument, but got " << args.size()));
                    return false;
                }

                auto dataSink = args[0];
                if (auto p = Types_.DataSinkMap.FindPtr(dataSink)) {
                    if ((*p)->GetName() != dataSink) {
                        dataSink = (*p)->GetName();
                    }
                } else {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Unknown datasink for full result provider: " << dataSink));
                    return false;
                }

                Types_.FullResultDataSink = dataSink;
            }
            else if (name == "Diagnostics") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }

                Types_.Diagnostics = true;
            }
            else if (name == TStringBuf("Warning")) {
                if (!SetWarningRule(pos, args, ctx)) {
                    return false;
                }
            }
            else if (name == "UdfSupportsYield") {
                if (args.size() > 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected at most 1 argument, but got " << args.size()));
                    return false;
                }

                bool res = true;
                if (!args.empty()) {
                    if (!TryFromString(args[0], res)) {
                        ctx.AddError(TIssue(pos, TStringBuilder() << "Expected bool, but got: " << args[0]));
                        return false;
                    }
                }

                Types_.UdfSupportsYield = res;
            }
            else if (name == "EvaluateForLimit") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 1 argument, but got " << args.size()));
                    return false;
                }
                if (!TryFromString(args[0], Types_.EvaluateForLimit)) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected integer, but got: " << args[0]));
                    return false;
                }
            }
            else if (name == "EvaluateParallelForLimit") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 1 argument, but got " << args.size()));
                    return false;
                }
                if (!TryFromString(args[0], Types_.EvaluateParallelForLimit)) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected integer, but got: " << args[0]));
                    return false;
                }
            }
            else if (name == "DisablePullUpFlatMapOverJoin" || name == "PullUpFlatMapOverJoin") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }

                Types_.PullUpFlatMapOverJoin = (name == "PullUpFlatMapOverJoin");
            } else if (name == "DisableFilterPushdownOverJoinOptionalSide" || name == "FilterPushdownOverJoinOptionalSide") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }

                Types_.FilterPushdownOverJoinOptionalSide = (name == "FilterPushdownOverJoinOptionalSide");
            } else if (name == "RotateJoinTree") {
                if (args.size() > 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected at most 1 argument, but got " << args.size()));
                    return false;
                }

                bool res = true;
                if (!args.empty()) {
                    if (!TryFromString(args[0], res)) {
                        ctx.AddError(TIssue(pos, TStringBuilder() << "Expected bool, but got: " << args[0]));
                        return false;
                    }
                }

                Types_.RotateJoinTree = res;
            }
            else if (name == "SQL") {
                if (args.size() > 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected at most 1 argument, but got " << args.size()));
                    return false;
                }

                Types_.DeprecatedSQL = (args[0] == "0");
            }
            else if (name == "DisableConstraintCheck") {
                if (args.empty()) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected at least 1 argument, but got " << args.size()));
                    return false;
                }
                for (auto arg: args) {
                    Types_.DisableConstraintCheck.emplace(arg);
                }
            }
            else if (name == "EnableConstraintCheck") {
                if (args.empty()) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected at least 1 argument, but got " << args.size()));
                    return false;
                }
                for (auto arg: args) {
                    Types_.DisableConstraintCheck.erase(TString{arg});
                }
            }
            else if (name == "DisableConstraints") {
                if (args.empty()) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected at least 1 argument, but got " << args.size()));
                    return false;
                }
                for (auto arg: args) {
                    ctx.DisabledConstraints.emplace(arg);
                }
            }
            else if (name == "EnableConstraints") {
                if (args.empty()) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected at least 1 argument, but got " << args.size()));
                    return false;
                }
                for (auto arg: args) {
                    ctx.DisabledConstraints.erase(arg);
                }
            }
            else if (name == "UseTableMetaFromGraph") {
                if (args.size() > 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected at most 1 argument, but got " << args.size()));
                    return false;
                }

                bool res = true;
                if (!args.empty()) {
                    if (!TryFromString(args[0], res)) {
                        ctx.AddError(TIssue(pos, TStringBuilder() << "Expected bool, but got: " << args[0]));
                        return false;
                    }
                }

                Types_.UseTableMetaFromGraph = res;
            }
            else if (name == "DiscoveryMode") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }
                Types_.DiscoveryMode = true;
            }
            else if (name == "EnableSystemColumns") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }
            }
            else if (name == "UdfIgnoreCase" || name == "UdfStrictCase") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }

                if (!Types_.UdfIndex) {
                    ctx.AddError(TIssue(pos, "UdfIndex is not available"));
                    return false;
                }

                Types_.UdfIndex->SetCaseSentiveSearch(name == "UdfStrictCase");
            } else if (name == "DqEngine") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected at most 1 argument, but got " << args.size()));
                    return false;
                }

                auto arg = TString{args[0]};
                if (Types_.EngineType == EEngineType::Ytflow) {
                    if (arg == "force") {
                        ctx.AddError(TIssue(pos, TStringBuilder()
                            << "Expected `disable|auto` argument for DqEngine pragma "
                            << "with Engine pragma argument `ytflow`"));

                        return false;
                    }

                    arg = "disable";
                } else if (Types_.EngineType == EEngineType::Dq) {
                    arg = "force";
                }

                if (Find(Types_.AvailablePureResultDataSources, DqProviderName) == Types_.AvailablePureResultDataSources.end() || arg == "disable") {
                    ; // reserved
                } else if (arg == "auto") {
                    Types_.PureResultDataSource = DqProviderName;
                    Types_.ForceDq = false;
                } else if (arg == "force") {
                    Types_.PureResultDataSource = DqProviderName;
                    Types_.ForceDq = true;
                } else {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected `disable|auto|force', but got: " << args[0]));
                    return false;
                }
            }
            else if (name == "IssueCountLimit") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 1 argument, but got " << args.size()));
                    return false;
                }
                size_t limit = 0;
                if (!TryFromString(args[0], limit)) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected unsigned integer, but got: " << args[0]));
                    return false;
                }
                ctx.IssueManager.SetIssueCountLimit(limit);
            }
            else if (name == "StrictTableProps") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }
                Types_.StrictTableProps = true;
            }
            else if (name == "DisableStrictTableProps") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }
                Types_.StrictTableProps = false;
            }
            else if (name == "GeobaseDownloadUrl") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 1 argument, but got " << args.size()));
                    return false;
                }
                auto& userDataBlock = (Types_.UserDataStorageCrutches[TUserDataKey::File(TStringBuf("/home/geodata6.bin"))] = TUserDataBlock{EUserDataType::URL, {}, TString(args[0]), {}, {}});
                userDataBlock.Usage.Set(EUserDataBlockUsage::Path);
            } else if (name == "JsonQueryReturnsJsonDocument" || name == "DisableJsonQueryReturnsJsonDocument") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }

                Types_.JsonQueryReturnsJsonDocument = (name == "JsonQueryReturnsJsonDocument");
            } else if (name == "OrderedColumns" || name == "DisableOrderedColumns") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }
                Types_.DeriveColumnOrder = (name == "OrderedColumns");
                Types_.OrderedColumns = (name == "OrderedColumns");
            } else if (name == "DeriveColumnOrder" || name == "DisableDeriveColumnOrder") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }

                Types_.DeriveColumnOrder = (name == "DeriveColumnOrder");
            } else if (name == "FolderSubDirsLimit") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 1 argument, but got " << args.size()));
                    return false;
                }
                if (!TryFromString(args[0], Types_.FolderSubDirsLimit)) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected integer, but got: " << args[0]));
                    return false;
                }
            }
            else if (name == "YsonCastToString" || name == "DisableYsonCastToString") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }

                Types_.YsonCastToString = (name == "YsonCastToString");
            }
            else if (name == "UseBlocks" || name == "DisableUseBlocks") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }

                Types_.UseBlocks = (name == "UseBlocks");
            }
            else if (name == "DebugPositions" || name == "DisableDebugPositions") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }

                Types_.DebugPositions = (name == "DebugPositions");
            }
            else if (name == "PgEmitAggApply" || name == "DisablePgEmitAggApply") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }

                Types_.PgEmitAggApply = (name == "PgEmitAggApply");
            }
            else if (name == "CostBasedOptimizer") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected at most 1 argument, but got " << args.size()));
                    return false;
                }

                if (!TryFromString(args[0], Types_.CostBasedOptimizer)) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected `disable|pg|native', but got: " << args[0]));
                    return false;
                }
            }
            else if (name == "_EnableMatchRecognize" || name == "DisableMatchRecognize") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }
                Types_.MatchRecognize = name == "_EnableMatchRecognize";
            }
            else if (name == "TimeOrderRecoverDelay") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected one argument, but got " << args.size()));
                    return false;
                }
                if (!TryFromString(args[0], Types_.TimeOrderRecoverDelay)) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected integer, but got: " << args[0]));
                    return false;
                }
                if (Types_.TimeOrderRecoverDelay >= 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected negative value, but got: " << args[0]));
                    return false;
                }
            }
            else if (name == "TimeOrderRecoverAhead") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected one argument, but got " << args.size()));
                    return false;
                }
                if (!TryFromString(args[0], Types_.TimeOrderRecoverAhead)) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected integer, but got: " << args[0]));
                    return false;
                }
                if (Types_.TimeOrderRecoverAhead <= 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected positive value, but got: " << args[0]));
                    return false;
                }
            }
            else if (name == "TimeOrderRecoverRowLimit") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected one argument, but got " << args.size()));
                    return false;
                }
                if (!TryFromString(args[0], Types_.TimeOrderRecoverRowLimit)) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected integer, but got: " << args[0]));
                    return false;
                }
                if (Types_.TimeOrderRecoverRowLimit == 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected positive value, but got: " << args[0]));
                    return false;
                }
            }
            else if (name == "MatchRecognizeStream") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected at most 1 argument, but got " << args.size()));
                    return false;
                }
                const auto& arg = args[0];
                if (arg == "disable") {
                    Types_.MatchRecognizeStreaming = EMatchRecognizeStreamingMode::Disable;
                } else if (arg == "auto") {
                    Types_.MatchRecognizeStreaming = EMatchRecognizeStreamingMode::Auto;
                } else if (arg == "force") {
                    Types_.MatchRecognizeStreaming = EMatchRecognizeStreamingMode::Force;
                } else {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected `disable|auto|force', but got: " << args[0]));
                    return false;
                }
            }
            else if (name == "BlockEngine") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected at most 1 argument, but got " << args.size()));
                    return false;
                }

                auto arg = TString{args[0]};
                if (!TryFromString(arg, Types_.BlockEngineMode)) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected `disable|auto|force', but got: " << args[0]));
                    return false;
                }
            }
            else if (name == "OptimizerFlags") {
                for (auto& arg : args) {
                    if (arg.empty()) {
                        ctx.AddError(TIssue(pos, "Empty flags are not supported"));
                        return false;
                    }
                    Types_.OptimizerFlags.insert(to_lower(ToString(arg)));
                }
            }
            else if (name == "PeepholeFlags") {
                for (auto& arg : args) {
                    if (arg.empty()) {
                        ctx.AddError(TIssue(pos, "Empty flags are not supported"));
                        return false;
                    }
                    Types_.PeepholeFlags.insert(to_lower(ToString(arg)));
                }
            }
            else if (name == "_EnableStreamLookupJoin" || name == "DisableStreamLookupJoin") {
                if (args.size() != 0) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected no arguments, but got " << args.size()));
                    return false;
                }
                Types_.StreamLookupJoin = name == "_EnableStreamLookupJoin";
            } else if (name == "MaxAggPushdownPredicates") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected single numeric argument, but got " << args.size()));
                    return false;
                }
                ui32 value;
                if (!TryFromString(args[0], value)) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected non-negative integer, but got: " << args[0]));
                    return false;
                }
                const ui32 hardLimit = 10;
                if (value > hardLimit) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Hard limit for setting MaxAggPushdownPredicates is " << hardLimit << ", but got: " << args[0]));
                    return false;
                }
                Types_.MaxAggPushdownPredicates = value;
            } else if (name == "AndOverOrExpansionLimit") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected single numeric argument, but got " << args.size()));
                    return false;
                }
                ui32 value;
                if (!TryFromString(args[0], value)) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected non-negative integer, but got: " << args[0]));
                    return false;
                }
                Types_.AndOverOrExpansionLimit = value;
            } else if (name == "Engine") {
                if (args.size() != 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected at most 1 argument, but got " << args.size()));
                    return false;
                }

                auto arg = TString{args[0]};
                if (arg == "ytflow") {
                    if (Types_.ForceDq) {
                        ctx.AddError(TIssue(pos, TStringBuilder()
                            << "Expected `disable|auto` argument for DqEngine pragma "
                            << "with Engine pragma argument `ytflow`"));

                        return false;
                    }

                    if (Types_.PureResultDataSource == DqProviderName) {
                        Types_.PureResultDataSource.clear();
                    }

                    Types_.EngineType = EEngineType::Ytflow;
                } else if (arg == "dq") {
                    if (Find(Types_.AvailablePureResultDataSources, DqProviderName) == Types_.AvailablePureResultDataSources.end()) {
                        ; // reserved
                    } else {
                        Types_.PureResultDataSource = DqProviderName;
                        Types_.ForceDq = true;
                    }

                    Types_.EngineType = EEngineType::Dq;
                } else if (arg == "default") {
                    Types_.EngineType = EEngineType::Default;
                } else {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected `default|dq|ytflow', but got: " << arg));
                    return false;
                }
            } else if (name == "NormalizeDependsOn") {
                if (args.size() > 1) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Expected at most 1 argument, but got " << args.size()));
                    return false;
                }

                bool res = true;
                if (!args.empty()) {
                    if (!TryFromString(args[0], res)) {
                        ctx.AddError(TIssue(pos, TStringBuilder() << "Expected bool, but got: " << args[0]));
                        return false;
                    }
                }

                Types_.NormalizeDependsOn = res;
            } else {
                ctx.AddError(TIssue(pos, TStringBuilder() << "Unsupported command: " << name));
                return false;
            }
            return true;
        }

        bool ImportUdfs(const TPosition& pos, const TVector<TStringBuf>& args, TExprContext& ctx) {
            if (args.size() != 1 && args.size() != 2) {
                ctx.AddError(TIssue(pos, TStringBuilder()
                        << "Expected 1 or 2 arguments, but got " << args.size()));
                return false;
            }

            if (Types_.DisableNativeUdfSupport) {
                ctx.AddError(TIssue(pos, "Native UDF support is disabled"));
                return false;
            }

            // file alias
            const auto& fileAlias = args[0];
            TString customUdfPrefix = args.size() > 1 ? TString(args[1]) : "";
            const auto key = TUserDataStorage::ComposeUserDataKey(fileAlias);
            TString errorMessage;
            const TUserDataBlock* udfSource = nullptr;
            if (!Types_.QContext.CanRead()) {
                udfSource = Types_.UserDataStorage->FreezeUdfNoThrow(key, errorMessage, customUdfPrefix, Types_.RuntimeLogLevel, fileAlias);
                if (!udfSource) {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Unknown file: " << fileAlias << ", details: " << errorMessage));
                    return false;
                }
            }

            IUdfResolver::TImport import;
            import.Pos = pos;
            import.FileAlias = fileAlias;
            import.Block = udfSource;
            Types_.UdfImports.insert({ TString(fileAlias), import });
            return true;
        }

        bool AddCredential(const TPosition& pos, const TVector<TStringBuf>& args, TExprContext& ctx) {
            if (args.size() != 4) {
                ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 4 arguments, but got " << args.size()));
                return false;
            }

            if (Types_.Credentials->FindCredential(args[0])) {
                return true;
            }

            Types_.Credentials->AddCredential(TString(args[0]), TCredential(TString(args[1]), TString(args[2]), TString(args[3])));
            return true;
        }

        bool AddFileByUrlImpl(const TStringBuf alias, const TStringBuf url, const TStringBuf token, const TPosition pos, TExprContext& ctx) {
            if (url.empty()) {
                ctx.AddError(TIssue(pos, TStringBuilder() << "Empty URL for file '" << alias << "'."));
                return false;
            }

            auto key = TUserDataStorage::ComposeUserDataKey(alias);
            if (Types_.UserDataStorage->ContainsUserDataBlock(key)) {
                // Don't overwrite.
                return true;
            }

            TUserDataBlock block;
            if (Types_.QContext.CanRead()) {
                block.Type = EUserDataType::RAW_INLINE_DATA;
            } else {
                block.Type = EUserDataType::URL;
                block.Data = url;
                if (token) {
                    block.UrlToken = token;
                }
            }

            Types_.UserDataStorage->AddUserDataBlock(key, block);
            return true;
        }

        bool AddFileByUrl(const TPosition& pos, const TVector<TStringBuf>& args, TExprContext& ctx) {
            if (args.size() < 2 || args.size() > 3) {
                ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 2 or 3 arguments, but got " << args.size()));
                return false;
            }

            TStringBuf token = args.size() == 3 ? args[2] : TStringBuf();
            if (token) {
                if (auto cred = Types_.Credentials->FindCredential(token)) {
                    token = cred->Content;
                } else {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Unknown token name '" << token << "'."));
                    return false;
                }
            }

            return AddFileByUrlImpl(args[0], args[1], token, pos, ctx);
        }

        bool SetFileOptionImpl(const TStringBuf alias, const TString& key, const TString& value, const TPosition& pos, TExprContext& ctx) {
            const auto dataKey = TUserDataStorage::ComposeUserDataKey(alias);
            const auto dataBlock = Types_.UserDataStorage->FindUserDataBlock(dataKey);
            if (!dataBlock) {
                ctx.AddError(TIssue(pos, TStringBuilder() << "No such file '" << alias << "'"));
                return false;
            }
            dataBlock->Options[key] = value;
            return true;
        }

        bool SetFileOption(const TPosition& pos, const TVector<TStringBuf>& args, TExprContext& ctx) {
            if (args.size() != 3) {
                ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 3 arguments, but got " << args.size()));
                return false;
            }
            return SetFileOptionImpl(args[0], ToString(args[1]), ToString(args[2]), pos, ctx);
        }

        bool SetPackageVersion(const TPosition& pos, const TVector<TStringBuf>& args, TExprContext& ctx) {
            if (args.size() != 2) {
                ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 2 arguments, but got " << args.size()));
                return false;
            }

            ui32 version = 0;
            if (!TryFromString(args[1], version)) {
                ctx.AddError(TIssue(pos, TStringBuilder() << "Unable to parse package version from " << args[1]));
                return false;
            }

            if (!Types_.UdfIndexPackageSet || !Types_.UdfIndex) {
                ctx.AddError(TIssue(pos, TStringBuilder() << "UdfIndex is not initialized, unable to set version for package " << args[0]));
                return false;
            }

            if (!Types_.UdfIndexPackageSet->AddResourceTo(TString(args[0]), version, Types_.UdfIndex)) {
                ctx.AddError(TIssue(pos, TStringBuilder() << "Unable set default version to " << version << " for package " << args[0]));
                return false;
            }

            return true;
        }

        bool DoListSandboxFolder(const TStringBuf url, const TStringBuf token, const TPosition& pos, TExprContext& ctx, NJson::TJsonValue& content) {
            TString urlStr(url);
            if (!url.empty() && url.back() != '/') {
                urlStr += "/";
            }
            const THttpURL& httpUrl = ParseURL(urlStr);
            if (httpUrl.GetHost() != "proxy.sandbox.yandex-team.ru") {
                ctx.AddError(TIssue(pos, TStringBuilder() << "Adding folder by URL is currently supported only for proxy.sandbox.yandex-team.ru. Host " << httpUrl.GetHost() << " is not supported"));
                return false;
            }

            THttpHeaders headers;
            headers.AddHeader("Accept", "application/json");
            if (token) {
                headers.AddHeader("Authorization", TString("OAuth ").append(token));
            }
            auto result = Fetch(httpUrl, headers, TDuration::Seconds(30));
            if (!result) {
                ctx.AddError(TIssue(pos, TStringBuilder() << "Failed to fetch " << url << " for building folder"));
                return false;
            }
            if (result->GetRetCode() != 200) {
                ctx.AddError(TIssue(pos, TStringBuilder() << "Failed to fetch " << url << " for building folder (http code: " << result->GetRetCode() << ")"));
                return false;
            }

            if (!NJson::ReadJsonTree(result->GetStream().ReadAll(), &content)) {
                ctx.AddError(TIssue(pos, TStringBuilder() << "Failed to parse json from " << url << " for building folder"));
                return false;
            }

            return true;
        }

        bool ListSandboxFolder(const TStringBuf url, const TStringBuf token, const TPosition& pos, TExprContext& ctx, NJson::TJsonValue& content) {
            try {

                return WithRetry<std::exception>(3, [&]() {
                    return DoListSandboxFolder(url, token, pos, ctx, content);
                }, [&](const auto& e, int attempt, int attemptCount) {
                    YQL_CLOG(WARN, ProviderConfig) << "Error in loading sandbox folder " << url << ", attempt " << attempt << "/" << attemptCount << ", details: " << e.what();
                });

            } catch (const std::exception& e) {
                ctx.AddError(TIssue(pos, TStringBuilder() << "Failed to load sandbox folder content from " << url << ", details: " << e.what()));
                return false;
            }
        }

        static TString MakeHttps(const TString& url) {
            if (url.StartsWith("http:")) {
                return "https:" + url.substr(5);
            }
            return url;
        }

        bool AddFolderByUrl(const TPosition& pos, const TVector<TStringBuf>& args, TExprContext& ctx) {
            if (args.size() < 2 || args.size() > 3) {
                ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 2 or 3 arguments, but got " << args.size()));
                return false;
            }

            TStringBuf token = args.size() == 3 ? args[2] : TStringBuf();
            if (token) {
                if (auto cred = Types_.Credentials->FindCredential(token)) {
                    token = cred->Content;
                } else {
                    ctx.AddError(TIssue(pos, TStringBuilder() << "Unknown token name '" << token << "' for folder."));
                    return false;
                }
            } else if (auto cred = Types_.Credentials->FindCredential("default_sandbox")) {
                token = cred->Content;
            }

            std::vector<std::pair<TString, TString>> queue;
            queue.emplace_back(args[0], args[1]);

            size_t count = 0;
            while (!queue.empty()) {
                auto [prefix, url] = queue.back();
                queue.pop_back();

                YQL_CLOG(DEBUG, ProviderConfig) << "Listing sandbox folder " << prefix << ": " << url;
                NJson::TJsonValue content;
                if (!ListSandboxFolder(url, token, pos, ctx, content)) {
                    return false;
                }

                for (const auto& file : content.GetMap()) {
                    const auto& fileAttrs = file.second.GetMap();
                    auto fileUrl = fileAttrs.FindPtr("url");
                    if (fileUrl) {
                        TString type = "REGULAR";
                        if (auto t = fileAttrs.FindPtr("type")) {
                            type = t->GetString();
                        }
                        TStringBuilder alias;
                        if (!prefix.empty()) {
                            alias << prefix << "/";
                        }
                        alias << file.first;
                        if (type == "REGULAR") {
                            if (!AddFileByUrlImpl(alias, TStringBuf(MakeHttps(fileUrl->GetString())), token, pos, ctx)) {
                                return false;
                            }
                        } else if (type == "DIRECTORY") {
                            queue.emplace_back(alias, fileUrl->GetString());
                            if (++count > Types_.FolderSubDirsLimit) {
                                ctx.AddError(TIssue(pos, TStringBuilder() << "Sandbox resource has too many subfolders. Limit is " << Types_.FolderSubDirsLimit));
                                return false;
                            }
                        } else {
                            YQL_CLOG(WARN, ProviderConfig) << "Got unknown sandbox item type: " << type << ", name=" << alias;
                        }
                    }
                }
            }

            return true;

        }

        bool SetWarningRule(const TPosition& pos, const TVector<TStringBuf>& args, TExprContext& ctx) {
            if (args.size() != 2) {
                ctx.AddError(TIssue(pos, TStringBuilder() << "Expected 2 arguments, but got " << args.size()));
                return false;
            }

            TString codePattern = TString{args[0]};
            TString action = TString{args[1]};

            TWarningRule rule;
            TString parseError;
            auto parseResult = TWarningRule::ParseFrom(codePattern, action, rule, parseError);
            switch (parseResult) {
                case TWarningRule::EParseResult::PARSE_OK:
                    ctx.IssueManager.AddWarningRule(rule);
                    break;
                case TWarningRule::EParseResult::PARSE_PATTERN_FAIL:
                case TWarningRule::EParseResult::PARSE_ACTION_FAIL:
                    ctx.AddError(TIssue(pos, parseError));
                    break;
                default:
                    YQL_ENSURE(false, "Unknown parse result");
            }

            return parseResult == TWarningRule::EParseResult::PARSE_OK;
        }

    private:
        TTypeAnnotationContext& Types_;
        TAutoPtr<IGraphTransformer> TypeAnnotationTransformer_;
        TAutoPtr<IGraphTransformer> ConfigurationTransformer_;
        TAutoPtr<IGraphTransformer> CallableExecutionTransformer_;
        const TYqlCoreConfig* CoreConfig_;
        TString Username_;
        const TAllowSettingPolicy Policy_;
        TOperationStatistics Statistics_;
    };
}

TIntrusivePtr<IDataProvider> CreateConfigProvider(TTypeAnnotationContext& types, const TGatewaysConfig* config, const TString& username,
    const TAllowSettingPolicy& policy)
{
    return new TConfigProvider(types, config, username, policy);
}

const THashSet<TStringBuf>& ConfigProviderFunctions() {
    return Singleton<TConfigProvider::TFunctions>()->Names;
}

}
