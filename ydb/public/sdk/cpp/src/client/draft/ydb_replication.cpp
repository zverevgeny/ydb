#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/draft/ydb_replication.h>

#define INCLUDE_YDB_INTERNAL_H
#include <ydb/public/sdk/cpp/src/client/impl/ydb_internal/make_request/make.h>
#undef INCLUDE_YDB_INTERNAL_H

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/library/issue/yql_issue.h>
#include <ydb/public/sdk/cpp/src/library/issue/yql_issue_message.h>
#include <ydb/public/api/grpc/draft/ydb_replication_v1.grpc.pb.h>
#include <ydb/public/sdk/cpp/src/client/common_client/impl/client.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/proto/accessor.h>

#include <google/protobuf/util/time_util.h>
#include <google/protobuf/repeated_field.h>

namespace NYdb::inline Dev {
namespace NReplication {

TConnectionParams::TConnectionParams(const Ydb::Replication::ConnectionParams& params) {
    DiscoveryEndpoint(params.endpoint());
    Database(params.database());
    SslCredentials(params.enable_ssl());

    switch (params.credentials_case()) {
    case Ydb::Replication::ConnectionParams::kStaticCredentials:
        Credentials_ = TStaticCredentials{
            .User = params.static_credentials().user(),
            .PasswordSecretName = params.static_credentials().password_secret_name(),
        };
        break;

    case Ydb::Replication::ConnectionParams::kOauth:
        Credentials_ = TOAuthCredentials{
            .TokenSecretName = params.oauth().token_secret_name(),
        };
        break;

    default:
        break;
    }
}

const std::string& TConnectionParams::GetDiscoveryEndpoint() const {
    return *DiscoveryEndpoint_;
}

const std::string& TConnectionParams::GetDatabase() const {
    return *Database_;
}

bool TConnectionParams::GetEnableSsl() const {
    return SslCredentials_->IsEnabled;
}

TConnectionParams::ECredentials TConnectionParams::GetCredentials() const {
    return static_cast<ECredentials>(Credentials_.index());
}

const TStaticCredentials& TConnectionParams::GetStaticCredentials() const {
    return std::get<TStaticCredentials>(Credentials_);
}

const TOAuthCredentials& TConnectionParams::GetOAuthCredentials() const {
    return std::get<TOAuthCredentials>(Credentials_);
}

static TDuration DurationToDuration(const google::protobuf::Duration& value) {
    return TDuration::MilliSeconds(google::protobuf::util::TimeUtil::DurationToMilliseconds(value));
}

TGlobalConsistency::TGlobalConsistency(const Ydb::Replication::ConsistencyLevelGlobal& proto)
    : CommitInterval_(DurationToDuration(proto.commit_interval()))
{
}

const TDuration& TGlobalConsistency::GetCommitInterval() const {
    return CommitInterval_;
}

TStats::TStats(const Ydb::Replication::DescribeReplicationResult_Stats& stats)
    : Lag_(stats.has_lag() ? std::make_optional(DurationToDuration(stats.lag())) : std::nullopt)
    , InitialScanProgress_(stats.has_initial_scan_progress() ? std::make_optional(stats.initial_scan_progress()) : std::nullopt)
{
}

const std::optional<TDuration>& TStats::GetLag() const {
    return Lag_;
}

const std::optional<float>& TStats::GetInitialScanProgress() const {
    return InitialScanProgress_;
}

TRunningState::TRunningState(const TStats& stats)
    : Stats_(stats)
{
}

const TStats& TRunningState::GetStats() const {
    return Stats_;
}

class TErrorState::TImpl {
public:
    NYdb::NIssue::TIssues Issues;

    explicit TImpl(NYdb::NIssue::TIssues&& issues)
        : Issues(std::move(issues))
    {
    }
};

TErrorState::TErrorState(NYdb::NIssue::TIssues&& issues)
    : Impl_(std::make_shared<TImpl>(std::move(issues)))
{
}

const NYdb::NIssue::TIssues& TErrorState::GetIssues() const {
    return Impl_->Issues;
}

NYdb::NIssue::TIssues IssuesFromMessage(const ::google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage>& message) {
    NYdb::NIssue::TIssues issues;
    NYdb::NIssue::IssuesFromMessage(message, issues);
    return issues;
}

TReplicationDescription::TReplicationDescription(const Ydb::Replication::DescribeReplicationResult& desc)
    : ConnectionParams_(desc.connection_params())
{
    Items_.reserve(desc.items_size());
    for (const auto& item : desc.items()) {
        Items_.push_back(TItem{
            .Id = item.id(),
            .SrcPath = item.source_path(),
            .DstPath = item.destination_path(),
            .Stats = TStats(item.stats()),
            .SrcChangefeedName = item.has_source_changefeed_name()
                ? std::make_optional(item.source_changefeed_name()) : std::nullopt,
        });
    }

    switch (desc.consistency_level_case()) {
    case Ydb::Replication::DescribeReplicationResult::kGlobalConsistency:
        ConsistencyLevel_ = TGlobalConsistency(desc.global_consistency());
        break;

    default:
        break;
    }

    switch (desc.state_case()) {
    case Ydb::Replication::DescribeReplicationResult::kRunning:
        State_ = TRunningState(desc.running().stats());
        break;

    case Ydb::Replication::DescribeReplicationResult::kError:
        State_ = TErrorState(IssuesFromMessage(desc.error().issues()));
        break;

    case Ydb::Replication::DescribeReplicationResult::kDone:
        State_ = TDoneState();
        break;

    case Ydb::Replication::DescribeReplicationResult::kPaused:
        State_ = TPausedState();
        break;

    default:
        break;
    }
}

const TConnectionParams& TReplicationDescription::GetConnectionParams() const {
    return ConnectionParams_;
}

const std::vector<TReplicationDescription::TItem> TReplicationDescription::GetItems() const {
    return Items_;
}

TReplicationDescription::EConsistencyLevel TReplicationDescription::GetConsistencyLevel() const {
    return static_cast<EConsistencyLevel>(ConsistencyLevel_.index());
}

const TGlobalConsistency& TReplicationDescription::GetGlobalConsistency() const {
    return std::get<TGlobalConsistency>(ConsistencyLevel_);
}


TReplicationDescription::EState TReplicationDescription::GetState() const {
    return static_cast<EState>(State_.index());
}

const TRunningState& TReplicationDescription::GetRunningState() const {
    return std::get<TRunningState>(State_);
}

const TErrorState& TReplicationDescription::GetErrorState() const {
    return std::get<TErrorState>(State_);
}

const TDoneState& TReplicationDescription::GetDoneState() const {
    return std::get<TDoneState>(State_);
}

const TPausedState& TReplicationDescription::GetPausedState() const {
    return std::get<TPausedState>(State_);
}

TDescribeReplicationResult::TDescribeReplicationResult(TStatus&& status, Ydb::Replication::DescribeReplicationResult&& desc)
    : NScheme::TDescribePathResult(std::move(status), desc.self())
    , ReplicationDescription_(desc)
    , Proto_(std::make_unique<Ydb::Replication::DescribeReplicationResult>())
{
    *Proto_ = std::move(desc);
}

const TReplicationDescription& TDescribeReplicationResult::GetReplicationDescription() const {
    return ReplicationDescription_;
}

const Ydb::Replication::DescribeReplicationResult& TDescribeReplicationResult::GetProto() const {
    return *Proto_;
}

class TReplicationClient::TImpl: public TClientImplCommon<TReplicationClient::TImpl> {
public:
    TImpl(std::shared_ptr<TGRpcConnectionsImpl>&& connections, const TCommonClientSettings& settings)
        : TClientImplCommon(std::move(connections), settings)
    {
    }

    TAsyncDescribeReplicationResult DescribeReplication(const std::string& path, const TDescribeReplicationSettings& settings) {
        using namespace Ydb::Replication;

        auto request = MakeOperationRequest<DescribeReplicationRequest>(settings);
        request.set_path(TStringType{path});
        request.set_include_stats(settings.IncludeStats_);

        auto promise = NThreading::NewPromise<TDescribeReplicationResult>();

        auto extractor = [promise]
            (google::protobuf::Any* any, TPlainStatus status) mutable {
                DescribeReplicationResult result;
                if (any) {
                    any->UnpackTo(&result);
                }

                TDescribeReplicationResult val(TStatus(std::move(status)), std::move(result));
                promise.SetValue(std::move(val));
            };

        Connections_->RunDeferred<V1::ReplicationService, DescribeReplicationRequest, DescribeReplicationResponse>(
            std::move(request),
            extractor,
            &V1::ReplicationService::Stub::AsyncDescribeReplication,
            DbDriverState_,
            INITIAL_DEFERRED_CALL_DELAY,
            TRpcRequestSettings::Make(settings));

        return promise.GetFuture();
    }

};

TReplicationClient::TReplicationClient(const TDriver& driver, const TCommonClientSettings& settings)
    : Impl_(std::make_shared<TImpl>(CreateInternalInterface(driver), settings))
{
}

TAsyncDescribeReplicationResult TReplicationClient::DescribeReplication(const std::string& path, const TDescribeReplicationSettings& settings) {
    return Impl_->DescribeReplication(path, settings);
}

} // NReplication

const Ydb::Replication::DescribeReplicationResult& TProtoAccessor::GetProto(const NReplication::TDescribeReplicationResult& result) {
    return result.GetProto();
}

} // NYdb
