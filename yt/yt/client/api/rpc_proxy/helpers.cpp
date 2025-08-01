#include "helpers.h"

#include <yt/yt/client/api/distributed_table_session.h>
#include <yt/yt/client/api/rowset.h>
#include <yt/yt/client/api/table_client.h>

#include <yt/yt/client/sequoia_client/public.h>

#include <yt/yt/client/signature/signature.h>

#include <yt/yt/client/table_client/columnar_statistics.h>
#include <yt/yt/client/table_client/column_sort_schema.h>
#include <yt/yt/client/table_client/logical_type.h>
#include <yt/yt/client/table_client/name_table.h>
#include <yt/yt/client/table_client/row_base.h>
#include <yt/yt/client/table_client/row_buffer.h>
#include <yt/yt/client/table_client/schema.h>
#include <yt/yt/client/table_client/unversioned_row.h>

#include <yt/yt/client/tablet_client/table_mount_cache.h>
#include <yt/yt/client/table_client/wire_protocol.h>

#include <yt/yt/client/ypath/rich.h>

#include <yt/yt/core/yson/protobuf_helpers.h>

namespace NYT::NApi::NRpcProxy {

using namespace NTableClient;
using namespace NTabletClient;
using namespace NYson;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

void ThrowUnimplemented(const TString& method)
{
    THROW_ERROR_EXCEPTION("%Qv method is not implemented in RPC proxy",
        method);
}

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

using NYT::ToProto;
using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////
// OPTIONS
////////////////////////////////////////////////////////////////////////////////

void ToProto(
    NProto::TTransactionalOptions* proto,
    const NApi::TTransactionalOptions& options)
{
    if (options.TransactionId) {
        ToProto(proto->mutable_transaction_id(), options.TransactionId);
    }
    proto->set_ping(options.Ping);
    proto->set_ping_ancestors(options.PingAncestors);
    proto->set_suppress_transaction_coordinator_sync(options.SuppressTransactionCoordinatorSync);
    proto->set_suppress_upstream_sync(options.SuppressUpstreamSync);
}

void FromProto(
    NApi::TTransactionalOptions* options,
    const NProto::TTransactionalOptions& proto)
{
    FromProto(&options->TransactionId, proto.transaction_id());
    options->Ping = proto.ping();
    options->PingAncestors = proto.ping_ancestors();
    options->SuppressTransactionCoordinatorSync = proto.suppress_transaction_coordinator_sync();
    options->SuppressUpstreamSync = proto.suppress_upstream_sync();
}

void ToProto(
    NProto::TPrerequisiteOptions* proto,
    const NApi::TPrerequisiteOptions& options)
{
    for (const auto& item : options.PrerequisiteTransactionIds) {
        auto* protoItem = proto->add_transactions();
        ToProto(protoItem->mutable_transaction_id(), item);
    }
    for (const auto& item : options.PrerequisiteRevisions) {
        auto* protoItem = proto->add_revisions();
        protoItem->set_path(item->Path);
        protoItem->set_revision(ToProto(item->Revision));
    }
}

void ToProto(
    NProto::TMasterReadOptions* proto,
    const NApi::TMasterReadOptions& options)
{
    proto->set_read_from(static_cast<NProto::EMasterReadKind>(options.ReadFrom));
    proto->set_disable_per_user_cache(options.DisablePerUserCache);
    proto->set_expire_after_successful_update_time(ToProto(options.ExpireAfterSuccessfulUpdateTime));
    proto->set_expire_after_failed_update_time(ToProto(options.ExpireAfterFailedUpdateTime));
    proto->set_success_staleness_bound(ToProto(options.SuccessStalenessBound));
    YT_OPTIONAL_SET_PROTO(proto, cache_sticky_group_size, options.CacheStickyGroupSize);
}

void ToProto(
    NProto::TMutatingOptions* proto,
    const NApi::TMutatingOptions& options)
{
    ToProto(proto->mutable_mutation_id(), options.GetOrGenerateMutationId());
    proto->set_retry(options.Retry);
}

void ToProto(
    NProto::TSuppressableAccessTrackingOptions* proto,
    const NApi::TSuppressableAccessTrackingOptions& options)
{
    proto->set_suppress_access_tracking(options.SuppressAccessTracking);
    proto->set_suppress_modification_tracking(options.SuppressModificationTracking);
    proto->set_suppress_expiration_timeout_renewal(options.SuppressExpirationTimeoutRenewal);
}

void ToProto(
    NProto::TTabletRangeOptions* proto,
    const NApi::TTabletRangeOptions& options)
{
    YT_OPTIONAL_SET_PROTO(proto, first_tablet_index, options.FirstTabletIndex);
    YT_OPTIONAL_SET_PROTO(proto, last_tablet_index, options.LastTabletIndex);
}

void ToProto(
    NProto::TTabletReadOptions* protoOptions,
    const NApi::TTabletReadOptionsBase& options)
{
    protoOptions->set_read_from(static_cast<NProto::ETabletReadKind>(options.ReadFrom));
    YT_OPTIONAL_SET_PROTO(protoOptions, cached_sync_replicas_timeout, options.CachedSyncReplicasTimeout);
}

////////////////////////////////////////////////////////////////////////////////
// CONFIGS
////////////////////////////////////////////////////////////////////////////////

void ToProto(
    NProto::TRetentionConfig* protoConfig,
    const NTableClient::TRetentionConfig& config)
{
    protoConfig->set_min_data_versions(config.MinDataVersions);
    protoConfig->set_max_data_versions(config.MaxDataVersions);
    protoConfig->set_min_data_ttl(config.MinDataTtl.GetValue());
    protoConfig->set_max_data_ttl(config.MaxDataTtl.GetValue());
    protoConfig->set_ignore_major_timestamp(config.IgnoreMajorTimestamp);
}

void FromProto(
    NTableClient::TRetentionConfig* config,
    const NProto::TRetentionConfig& protoConfig)
{
    config->MinDataVersions = protoConfig.min_data_versions();
    config->MaxDataVersions = protoConfig.max_data_versions();
    config->MinDataTtl = TDuration::FromValue(protoConfig.min_data_ttl());
    config->MaxDataTtl = TDuration::FromValue(protoConfig.max_data_ttl());
    config->IgnoreMajorTimestamp = protoConfig.ignore_major_timestamp();
}

////////////////////////////////////////////////////////////////////////////////
// RESULTS
////////////////////////////////////////////////////////////////////////////////

void ToProto(
    NProto::TGetFileFromCacheResult* proto,
    const NApi::TGetFileFromCacheResult& result)
{
    proto->set_path(result.Path);
}

void FromProto(
    NApi::TGetFileFromCacheResult* result,
    const NProto::TGetFileFromCacheResult& proto)
{
    result->Path = proto.path();
}

void ToProto(
    NProto::TPutFileToCacheResult* proto,
    const NApi::TPutFileToCacheResult& result)
{
    proto->set_path(result.Path);
}

void FromProto(
    NApi::TPutFileToCacheResult* result,
    const NProto::TPutFileToCacheResult& proto)
{
    result->Path = proto.path();
}

void ToProto(
    NProto::TCheckPermissionResult* proto,
    const NApi::TCheckPermissionResult& result)
{
    proto->Clear();

    proto->set_action(static_cast<NProto::ESecurityAction>(result.Action));

    ToProto(proto->mutable_object_id(), result.ObjectId);
    YT_OPTIONAL_TO_PROTO(proto, object_name, result.ObjectName);

    ToProto(proto->mutable_subject_id(), result.SubjectId);
    YT_OPTIONAL_TO_PROTO(proto, subject_name, result.SubjectName);
}

void FromProto(
    NApi::TCheckPermissionResult* result,
    const NProto::TCheckPermissionResult& proto)
{
    result->Action = static_cast<NSecurityClient::ESecurityAction>(proto.action());

    FromProto(&result->ObjectId, proto.object_id());
    result->ObjectName = YT_OPTIONAL_FROM_PROTO(proto, object_name);

    FromProto(&result->SubjectId, proto.subject_id());
    result->SubjectName = YT_OPTIONAL_FROM_PROTO(proto, subject_name);
}

void ToProto(
    NProto::TCheckPermissionByAclResult* proto,
    const NApi::TCheckPermissionByAclResult& result)
{
    proto->Clear();

    proto->set_action(static_cast<NProto::ESecurityAction>(result.Action));

    ToProto(proto->mutable_subject_id(), result.SubjectId);
    YT_OPTIONAL_TO_PROTO(proto, subject_name, result.SubjectName);

    ToProto(proto->mutable_missing_subjects(), result.MissingSubjects);
}

void FromProto(
    NApi::TCheckPermissionByAclResult* result,
    const NProto::TCheckPermissionByAclResult& proto)
{
    result->Action = static_cast<NSecurityClient::ESecurityAction>(proto.action());

    FromProto(&result->SubjectId, proto.subject_id());
    result->SubjectName = YT_OPTIONAL_FROM_PROTO(proto, subject_name);

    FromProto(&result->MissingSubjects, proto.missing_subjects());
}

void ToProto(
    NProto::TListOperationsResult* proto,
    const NApi::TListOperationsResult& result)
{
    proto->Clear();
    ToProto(proto->mutable_operations(), result.Operations);

    if (result.PoolTreeCounts) {
        auto* poolTreeCounts = proto->mutable_pool_tree_counts()->mutable_entries();
        for (const auto& entry : *result.PoolTreeCounts) {
            (*poolTreeCounts)[entry.first] = entry.second;
        }
    }
    if (result.PoolCounts) {
        for (const auto& entry : *result.PoolCounts) {
            auto* newPoolCount = proto->mutable_pool_counts()->add_entries();
            newPoolCount->set_pool(entry.first);
            newPoolCount->set_count(entry.second);
        }
    }
    if (result.UserCounts) {
        for (const auto& entry : *result.UserCounts) {
            auto* newUserCount = proto->mutable_user_counts()->add_entries();
            newUserCount->set_user(entry.first);
            newUserCount->set_count(entry.second);
        }
    }

    if (result.StateCounts) {
        for (const auto& state : TEnumTraits<NScheduler::EOperationState>::GetDomainValues()) {
            if ((*result.StateCounts)[state] != 0) {
                auto* newStateCount = proto->mutable_state_counts()->add_entries();
                newStateCount->set_state(ConvertOperationStateToProto(state));
                newStateCount->set_count((*result.StateCounts)[state]);
            }
        }
    }
    if (result.TypeCounts) {
        for (const auto& type : TEnumTraits<NScheduler::EOperationType>::GetDomainValues()) {
            if ((*result.TypeCounts)[type] != 0) {
                auto* newTypeCount = proto->mutable_type_counts()->add_entries();
                newTypeCount->set_type(ConvertOperationTypeToProto(type));
                newTypeCount->set_count((*result.TypeCounts)[type]);
            }
        }
    }

    YT_OPTIONAL_SET_PROTO(proto, failed_jobs_count, result.FailedJobsCount);
    proto->set_incomplete(result.Incomplete);
}

void FromProto(
    NApi::TListOperationsResult* result,
    const NProto::TListOperationsResult& proto)
{
    FromProto(&result->Operations, proto.operations());

    if (proto.has_pool_tree_counts()) {
        result->PoolTreeCounts.emplace();
        for (const auto& [poolTree, count]: proto.pool_tree_counts().entries()) {
            YT_VERIFY((*result->PoolTreeCounts)[poolTree] == 0);
            (*result->PoolTreeCounts)[poolTree] = count;
        }
    } else {
        result->PoolTreeCounts.reset();
    }

    if (proto.has_pool_counts()) {
        result->PoolCounts.emplace();
        for (const auto& poolCount : proto.pool_counts().entries()) {
            auto pool = poolCount.pool();
            YT_VERIFY((*result->PoolCounts)[pool] == 0);
            (*result->PoolCounts)[pool] = poolCount.count();
        }
    } else {
        result->PoolCounts.reset();
    }
    if (proto.has_user_counts()) {
        result->UserCounts.emplace();
        for (const auto& userCount : proto.user_counts().entries()) {
            auto user = userCount.user();
            YT_VERIFY((*result->UserCounts)[user] == 0);
            (*result->UserCounts)[user] = userCount.count();
        }
    } else {
        result->UserCounts.reset();
    }

    if (proto.has_state_counts()) {
        result->StateCounts.emplace();
        std::fill(result->StateCounts->begin(), result->StateCounts->end(), 0);
        for (const auto& stateCount : proto.state_counts().entries()) {
            auto state = ConvertOperationStateFromProto(stateCount.state());
            YT_VERIFY(result->StateCounts->IsValidIndex(state));
            YT_VERIFY((*result->StateCounts)[state] == 0);
            (*result->StateCounts)[state] = stateCount.count();
        }
    } else {
        result->StateCounts.reset();
    }
    if (proto.has_type_counts()) {
        result->TypeCounts.emplace();
        std::fill(result->TypeCounts->begin(), result->TypeCounts->end(), 0);
        for (const auto& typeCount : proto.type_counts().entries()) {
            auto type = ConvertOperationTypeFromProto(typeCount.type());
            YT_VERIFY(result->TypeCounts->IsValidIndex(type));
            YT_VERIFY((*result->TypeCounts)[type] == 0);
            (*result->TypeCounts)[type] = typeCount.count();
        }
    } else {
        result->TypeCounts.reset();
    }

    if (proto.has_failed_jobs_count()) {
        result->FailedJobsCount = proto.failed_jobs_count();
    } else {
        result->FailedJobsCount.reset();
    }
    result->Incomplete = proto.incomplete();
}

void ToProto(
    NProto::TListJobsResult* proto,
    const NApi::TListJobsResult& result)
{
    proto->Clear();
    ToProto(proto->mutable_jobs(), result.Jobs);

    YT_OPTIONAL_SET_PROTO(proto, cypress_job_count, result.CypressJobCount);
    YT_OPTIONAL_SET_PROTO(proto, controller_agent_job_count, result.ControllerAgentJobCount);
    YT_OPTIONAL_SET_PROTO(proto, archive_job_count, result.ArchiveJobCount);
    if (result.ContinuationToken) {
        proto->set_continuation_token(*result.ContinuationToken);
    }

    ToProto(proto->mutable_statistics(), result.Statistics);
    ToProto(proto->mutable_errors(), result.Errors);
}

void FromProto(
    NApi::TListJobsResult* result,
    const NProto::TListJobsResult& proto)
{
    FromProto(&result->Jobs, proto.jobs());

    result->CypressJobCount = YT_OPTIONAL_FROM_PROTO(proto, cypress_job_count);
    result->ControllerAgentJobCount = YT_OPTIONAL_FROM_PROTO(proto, controller_agent_job_count);
    result->ArchiveJobCount = YT_OPTIONAL_FROM_PROTO(proto, archive_job_count);
    result->ContinuationToken = YT_OPTIONAL_FROM_PROTO(proto, continuation_token);

    FromProto(&result->Statistics, proto.statistics());
    FromProto(&result->Errors, proto.errors());
}

void ToProto(
    NProto::TJobTraceEvent* proto,
    const NApi::TJobTraceEvent& result)
{
    ToProto(proto->mutable_operation_id(), result.OperationId);
    ToProto(proto->mutable_job_id(), result.JobId);
    ToProto(proto->mutable_trace_id(), result.TraceId);
    proto->set_event_index(result.EventIndex);
    proto->set_event(result.Event);
    proto->set_event_time(ToProto(result.EventTime));
}

void FromProto(
    NApi::TJobTraceEvent* result,
    const NProto::TJobTraceEvent& proto)
{
    FromProto(&result->OperationId, proto.operation_id());
    FromProto(&result->JobId, proto.job_id());
    FromProto(&result->TraceId, proto.trace_id());
    result->EventIndex = proto.event_index();
    result->Event = proto.event();
    result->EventTime = TInstant::FromValue(proto.event_time());
}

void ToProto(
    NProto::TOperationEvent* proto,
    const NApi::TOperationEvent& result)
{
    proto->set_timestamp(ToProto(result.Timestamp));
    proto->set_event_type(ConvertOperationEventTypeToProto(result.EventType));

    YT_OPTIONAL_TO_PROTO(proto, incarnation, result.Incarnation);

    if (result.IncarnationSwitchReason) {
        proto->set_incarnation_switch_reason(ConvertIncarnationSwitchReasonToProto(*result.IncarnationSwitchReason));
    }

    if (result.IncarnationSwitchInfo) {
        proto->set_incarnation_switch_info(result.IncarnationSwitchInfo->ToString());
    }
}

void FromProto(
    NApi::TOperationEvent* result,
    const NProto::TOperationEvent& proto)
{
    FromProto(&result->Timestamp, proto.timestamp());
    result->EventType = ConvertOperationEventTypeFromProto(proto.event_type());
    result->Incarnation = YT_OPTIONAL_FROM_PROTO(proto, incarnation);
    if (proto.has_incarnation_switch_reason()) {
        result->IncarnationSwitchReason = ConvertIncarnationSwitchReasonFromProto(proto.incarnation_switch_reason());
    }
    if (proto.has_incarnation_switch_info()) {
        result->IncarnationSwitchInfo = TYsonString(proto.incarnation_switch_info());
    }
}

////////////////////////////////////////////////////////////////////////////////
// MISC
////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TColumnSchema* protoSchema, const NTableClient::TColumnSchema& schema)
{
    protoSchema->set_stable_name(ToProto(schema.StableName()));
    protoSchema->set_name(ToProto(schema.Name()));
    protoSchema->set_type(ToProto(GetPhysicalType(schema.CastToV1Type())));
    auto typeV3Yson = ConvertToYsonString(TTypeV3LogicalTypeWrapper{schema.LogicalType()});
    protoSchema->set_type_v3(typeV3Yson.ToString());
    YT_OPTIONAL_TO_PROTO(protoSchema, lock, schema.Lock());
    YT_OPTIONAL_TO_PROTO(protoSchema, expression, schema.Expression());
    YT_OPTIONAL_SET_PROTO(protoSchema, materialized, schema.Materialized());
    YT_OPTIONAL_TO_PROTO(protoSchema, aggregate, schema.Aggregate());
    YT_OPTIONAL_SET_PROTO(protoSchema, sort_order, schema.SortOrder());
    YT_OPTIONAL_TO_PROTO(protoSchema, group, schema.Group());
    if (schema.Required()) {
        protoSchema->set_required(schema.Required());
    } else {
        protoSchema->clear_required();
    }
    YT_OPTIONAL_SET_PROTO(protoSchema, max_inline_hunk_size, schema.MaxInlineHunkSize());
}

void FromProto(NTableClient::TColumnSchema* schema, const NProto::TColumnSchema& protoSchema)
{
    schema->SetName(protoSchema.name());
    schema->SetStableName(
        protoSchema.has_stable_name()
        ? TColumnStableName(protoSchema.stable_name())
        : TColumnStableName(protoSchema.name()));

    auto physicalType = FromProto<EValueType>(protoSchema.type());

    TLogicalTypePtr columnType;
    if (protoSchema.has_type_v3()) {
        columnType = ConvertTo<TTypeV3LogicalTypeWrapper>(TYsonStringBuf(protoSchema.type_v3())).LogicalType;
        auto [v1Type, v1Required] = CastToV1Type(columnType);
        if (protoSchema.has_required() && protoSchema.required() != v1Required) {
            THROW_ERROR_EXCEPTION("Fields \"type_v3\" and \"required\" do not match")
                << TErrorAttribute("type_v3", ToString(*columnType))
                << TErrorAttribute("required", protoSchema.required());
        }
        if (protoSchema.has_logical_type() && v1Type != FromProto<ESimpleLogicalValueType>(protoSchema.logical_type())) {
            THROW_ERROR_EXCEPTION("Fields \"type_v3\" and \"logical_type\" do not match")
                << TErrorAttribute("type_v3", ToString(*columnType))
                << TErrorAttribute("logical_type", FromProto<ESimpleLogicalValueType>(protoSchema.logical_type()));
        }
        if (protoSchema.has_type() && GetPhysicalType(v1Type) != physicalType) {
            THROW_ERROR_EXCEPTION("Fields \"type_v3\" and \"logical_type\" do not match")
                << TErrorAttribute("type_v3", ToString(*columnType))
                << TErrorAttribute("type", protoSchema.type());
        }
    } else if (protoSchema.has_logical_type()) {
        auto logicalType = FromProto<ESimpleLogicalValueType>(protoSchema.logical_type());
        columnType = MakeLogicalType(logicalType, protoSchema.required());
        if (protoSchema.has_type() && GetPhysicalType(logicalType) != physicalType) {
            THROW_ERROR_EXCEPTION("Fields \"logical_type\" and \"type\" do not match")
                << TErrorAttribute("logical_type", ToString(*columnType))
                << TErrorAttribute("type", protoSchema.type());
        }
    } else if (protoSchema.has_type()) {
        columnType = MakeLogicalType(GetLogicalType(physicalType), protoSchema.required());
    }

    if (!columnType) {
        THROW_ERROR_EXCEPTION("Type is not specified");
    }

    schema->SetLogicalType(std::move(columnType));
    schema->SetLock(YT_OPTIONAL_FROM_PROTO(protoSchema, lock));
    schema->SetExpression(YT_OPTIONAL_FROM_PROTO(protoSchema, expression));
    schema->SetMaterialized(YT_OPTIONAL_FROM_PROTO(protoSchema, materialized));
    schema->SetAggregate(YT_OPTIONAL_FROM_PROTO(protoSchema, aggregate));
    schema->SetSortOrder(YT_APPLY_PROTO_OPTIONAL(protoSchema, sort_order, FromProto<ESortOrder>));
    schema->SetGroup(YT_OPTIONAL_FROM_PROTO(protoSchema, group));
    schema->SetMaxInlineHunkSize(YT_OPTIONAL_FROM_PROTO(protoSchema, max_inline_hunk_size));
}

void ToProto(NProto::TTableSchema* protoSchema, const NTableClient::TTableSchema& schema)
{
    ToProto(protoSchema->mutable_columns(), schema.Columns());
    protoSchema->set_strict(schema.IsStrict());
    protoSchema->set_unique_keys(schema.IsUniqueKeys());
}

void FromProto(NTableClient::TTableSchema* schema, const NProto::TTableSchema& protoSchema)
{
    *schema = NTableClient::TTableSchema(
        FromProto<std::vector<NTableClient::TColumnSchema>>(protoSchema.columns()),
        protoSchema.strict(),
        protoSchema.unique_keys());
}

void ToProto(NProto::TTableSchema* protoSchema, const NTableClient::TTableSchemaPtr& schema)
{
    ToProto(protoSchema, *schema);
}

void FromProto(NTableClient::TTableSchemaPtr* schema, const NProto::TTableSchema& protoSchema)
{
    *schema = New<NTableClient::TTableSchema>();
    FromProto(schema->Get(), protoSchema);
}

void ToProto(NProto::TTabletInfo* protoTabletInfo, const NTabletClient::TTabletInfo& tabletInfo)
{
    ToProto(protoTabletInfo->mutable_tablet_id(), tabletInfo.TabletId);
    protoTabletInfo->set_mount_revision(ToProto(tabletInfo.MountRevision));
    protoTabletInfo->set_state(ToProto(tabletInfo.State));
    ToProto(protoTabletInfo->mutable_pivot_key(), tabletInfo.PivotKey);
    if (tabletInfo.CellId) {
        ToProto(protoTabletInfo->mutable_cell_id(), tabletInfo.CellId);
    }
}

void FromProto(NTabletClient::TTabletInfo* tabletInfo, const NProto::TTabletInfo& protoTabletInfo)
{
    tabletInfo->TabletId = FromProto<TTabletId>(protoTabletInfo.tablet_id());
    tabletInfo->MountRevision = FromProto<NHydra::TRevision>(protoTabletInfo.mount_revision());
    tabletInfo->State = FromProto<ETabletState>(protoTabletInfo.state());
    tabletInfo->PivotKey = FromProto<NTableClient::TLegacyOwningKey>(protoTabletInfo.pivot_key());
    tabletInfo->CellId = FromProto<TTabletCellId>(protoTabletInfo.cell_id());
}

void ToProto(
    NProto::TQueryStatistics* protoStatistics,
    const NQueryClient::TQueryStatistics& statistics)
{
    protoStatistics->set_rows_read(statistics.RowsRead);
    protoStatistics->set_data_weight_read(statistics.DataWeightRead);
    protoStatistics->set_rows_written(statistics.RowsWritten);
    protoStatistics->set_sync_time(statistics.SyncTime.GetValue());
    protoStatistics->set_async_time(statistics.AsyncTime.GetValue());
    protoStatistics->set_execute_time(statistics.ExecuteTime.GetValue());
    protoStatistics->set_read_time(statistics.ReadTime.GetValue());
    protoStatistics->set_write_time(statistics.WriteTime.GetValue());
    protoStatistics->set_codegen_time(statistics.CodegenTime.GetValue());
    protoStatistics->set_wait_on_ready_event_time(statistics.WaitOnReadyEventTime.GetValue());
    protoStatistics->set_incomplete_input(statistics.IncompleteInput);
    protoStatistics->set_incomplete_output(statistics.IncompleteOutput);
    protoStatistics->set_memory_usage(statistics.MemoryUsage);
    protoStatistics->set_total_grouped_row_count(statistics.TotalGroupedRowCount);

    ToProto(protoStatistics->mutable_inner_statistics(), statistics.InnerStatistics);
}

void FromProto(
    NQueryClient::TQueryStatistics* statistics,
    const NProto::TQueryStatistics& protoStatistics)
{
    statistics->RowsRead = protoStatistics.rows_read();
    statistics->DataWeightRead = protoStatistics.data_weight_read();
    statistics->RowsWritten = protoStatistics.rows_written();
    statistics->SyncTime = TDuration::FromValue(protoStatistics.sync_time());
    statistics->AsyncTime = TDuration::FromValue(protoStatistics.async_time());
    statistics->ExecuteTime = TDuration::FromValue(protoStatistics.execute_time());
    statistics->ReadTime = TDuration::FromValue(protoStatistics.read_time());
    statistics->WriteTime = TDuration::FromValue(protoStatistics.write_time());
    statistics->CodegenTime = TDuration::FromValue(protoStatistics.codegen_time());
    statistics->WaitOnReadyEventTime = TDuration::FromValue(protoStatistics.wait_on_ready_event_time());
    statistics->IncompleteInput = protoStatistics.incomplete_input();
    statistics->IncompleteOutput = protoStatistics.incomplete_output();
    statistics->MemoryUsage = protoStatistics.memory_usage();
    statistics->TotalGroupedRowCount = protoStatistics.total_grouped_row_count();

    FromProto(&statistics->InnerStatistics, protoStatistics.inner_statistics());
}

void ToProto(NProto::TOperation* protoOperation, const NApi::TOperation& operation)
{
    protoOperation->Clear();

    if (operation.Id) {
        ToProto(protoOperation->mutable_id(), *operation.Id);
    }
    if (operation.Type) {
        protoOperation->set_type(ConvertOperationTypeToProto(*operation.Type));
    }
    if (operation.State) {
        protoOperation->set_state(ConvertOperationStateToProto(*operation.State));
    }

    if (operation.StartTime) {
        protoOperation->set_start_time(ToProto(*operation.StartTime));
    }
    if (operation.FinishTime) {
        protoOperation->set_finish_time(ToProto(*operation.FinishTime));
    }

    if (operation.AuthenticatedUser) {
        protoOperation->set_authenticated_user(*operation.AuthenticatedUser);
    }

    if (operation.BriefSpec) {
        protoOperation->set_brief_spec(ToProto(operation.BriefSpec));
    }
    if (operation.Spec) {
        protoOperation->set_spec(ToProto(operation.Spec));
    }
    if (operation.ProvidedSpec) {
        protoOperation->set_provided_spec(ToProto(operation.ProvidedSpec));
    }
    if (operation.ExperimentAssignments) {
        protoOperation->set_experiment_assignments(ToProto(operation.ExperimentAssignments));
    }
    if (operation.ExperimentAssignmentNames) {
        protoOperation->set_experiment_assignment_names(ToProto(operation.ExperimentAssignmentNames));
    }
    if (operation.FullSpec) {
        protoOperation->set_full_spec(ToProto(operation.FullSpec));
    }
    if (operation.UnrecognizedSpec) {
        protoOperation->set_unrecognized_spec(ToProto(operation.UnrecognizedSpec));
    }

    if (operation.BriefProgress) {
        protoOperation->set_brief_progress(ToProto(operation.BriefProgress));
    }
    if (operation.Progress) {
        protoOperation->set_progress(ToProto(operation.Progress));
    }

    if (operation.RuntimeParameters) {
        protoOperation->set_runtime_parameters(ToProto(operation.RuntimeParameters));
    }

    if (operation.Suspended) {
        protoOperation->set_suspended(*operation.Suspended);
    }

    if (operation.SuspendReason) {
        protoOperation->set_suspend_reason(*operation.SuspendReason);
    }

    if (operation.Events) {
        protoOperation->set_events(ToProto(operation.Events));
    }
    if (operation.Result) {
        protoOperation->set_result(ToProto(operation.Result));
    }

    if (operation.SlotIndexPerPoolTree) {
        protoOperation->set_slot_index_per_pool_tree(ToProto(operation.SlotIndexPerPoolTree));
    }

    if (operation.SchedulingAttributesPerPoolTree) {
        protoOperation->set_scheduling_attributes_per_pool_tree(ToProto(operation.SchedulingAttributesPerPoolTree));
    }

    if (operation.TaskNames) {
        protoOperation->set_task_names(ToProto(operation.TaskNames));
    }

    if (operation.Alerts) {
        protoOperation->set_alerts(ToProto(operation.Alerts));
    }
    if (operation.AlertEvents) {
        protoOperation->set_alert_events(ToProto(operation.AlertEvents));
    }

    if (operation.ControllerFeatures) {
        protoOperation->set_controller_features(ToProto(operation.ControllerFeatures));
    }

    if (operation.OtherAttributes) {
        protoOperation->set_other_attributes(ToProto(ConvertToYsonString(operation.OtherAttributes)));
    }
}

void FromProto(NApi::TOperation* operation, const NProto::TOperation& protoOperation)
{
    operation->Id = YT_APPLY_PROTO_OPTIONAL(protoOperation, id, FromProto<NScheduler::TOperationId>);
    operation->Type = YT_APPLY_PROTO_OPTIONAL(protoOperation, type, ConvertOperationTypeFromProto);
    operation->State = YT_APPLY_PROTO_OPTIONAL(protoOperation, state, ConvertOperationStateFromProto);

    operation->StartTime = YT_OPTIONAL_FROM_PROTO(protoOperation, start_time, TInstant);
    operation->FinishTime = YT_OPTIONAL_FROM_PROTO(protoOperation, finish_time, TInstant);
    operation->AuthenticatedUser = YT_OPTIONAL_FROM_PROTO(protoOperation, authenticated_user);

    if (protoOperation.has_brief_spec()) {
        operation->BriefSpec = TYsonString(protoOperation.brief_spec());
    } else {
        operation->BriefSpec = TYsonString();
    }
    if (protoOperation.has_spec()) {
        operation->Spec = TYsonString(protoOperation.spec());
    } else {
        operation->Spec = TYsonString();
    }
    if (protoOperation.has_provided_spec()) {
        operation->ProvidedSpec = TYsonString(protoOperation.provided_spec());
    } else {
        operation->ProvidedSpec = TYsonString();
    }
    if (protoOperation.has_full_spec()) {
        operation->FullSpec = TYsonString(protoOperation.full_spec());
    } else {
        operation->FullSpec = TYsonString();
    }
    if (protoOperation.has_unrecognized_spec()) {
        operation->UnrecognizedSpec = TYsonString(protoOperation.unrecognized_spec());
    } else {
        operation->UnrecognizedSpec = TYsonString();
    }

    if (protoOperation.has_experiment_assignments()) {
        operation->ExperimentAssignments = TYsonString(protoOperation.experiment_assignments());
    } else {
        operation->ExperimentAssignments = TYsonString();
    }
    if (protoOperation.has_experiment_assignment_names()) {
        operation->ExperimentAssignmentNames = TYsonString(protoOperation.experiment_assignment_names());
    } else {
        operation->ExperimentAssignmentNames = TYsonString();
    }

    if (protoOperation.has_brief_progress()) {
        operation->BriefProgress = TYsonString(protoOperation.brief_progress());
    } else {
        operation->BriefProgress = TYsonString();
    }
    if (protoOperation.has_progress()) {
        operation->Progress = TYsonString(protoOperation.progress());
    } else {
        operation->Progress = TYsonString();
    }

    if (protoOperation.has_runtime_parameters()) {
        operation->RuntimeParameters = TYsonString(protoOperation.runtime_parameters());
    } else {
        operation->RuntimeParameters = TYsonString();
    }

    operation->Suspended = YT_OPTIONAL_FROM_PROTO(protoOperation, suspended);

    if (protoOperation.has_events()) {
        operation->Events = TYsonString(protoOperation.events());
    } else {
        operation->Events = TYsonString();
    }
    if (protoOperation.has_result()) {
        operation->Result = TYsonString(protoOperation.result());
    } else {
        operation->Result = TYsonString();
    }

    if (protoOperation.has_slot_index_per_pool_tree()) {
        operation->SlotIndexPerPoolTree = TYsonString(protoOperation.slot_index_per_pool_tree());
    } else {
        operation->SlotIndexPerPoolTree = TYsonString();
    }

    if (protoOperation.has_scheduling_attributes_per_pool_tree()) {
        operation->SchedulingAttributesPerPoolTree = TYsonString(protoOperation.scheduling_attributes_per_pool_tree());
    } else {
        operation->SchedulingAttributesPerPoolTree = TYsonString();
    }

    if (protoOperation.has_task_names()) {
        operation->TaskNames = TYsonString(protoOperation.task_names());
    } else {
        operation->TaskNames = TYsonString();
    }

    if (protoOperation.has_alerts()) {
        operation->Alerts = TYsonString(protoOperation.alerts());
    } else {
        operation->Alerts = TYsonString();
    }
    if (protoOperation.has_alert_events()) {
        operation->AlertEvents = TYsonString(protoOperation.alert_events());
    } else {
        operation->AlertEvents = TYsonString();
    }

    if (protoOperation.has_controller_features()) {
        operation->ControllerFeatures = TYsonString(protoOperation.controller_features());
    } else {
        operation->ControllerFeatures = TYsonString();
    }

    if (protoOperation.has_other_attributes()) {
        operation->OtherAttributes = ConvertToAttributes(TYsonStringBuf(protoOperation.other_attributes()));
    } else {
        operation->OtherAttributes = {};
    }
}

void ToProto(NProto::TJob* protoJob, const NApi::TJob& job)
{
    protoJob->Clear();

    if (job.Id) {
        ToProto(protoJob->mutable_id(), job.Id);
    }
    if (job.OperationId) {
        ToProto(protoJob->mutable_operation_id(), job.OperationId);
    }
    if (job.Type) {
        protoJob->set_type(ConvertJobTypeToProto(*job.Type));
    }
    if (auto state = job.GetState()) {
        protoJob->set_state(ConvertJobStateToProto(*state));
    }
    if (job.ControllerState) {
        protoJob->set_controller_state(
            ConvertJobStateToProto(*job.ControllerState));
    }
    if (job.ArchiveState) {
        protoJob->set_archive_state(ConvertJobStateToProto(*job.ArchiveState));
    }

    YT_OPTIONAL_SET_PROTO(protoJob, start_time, job.StartTime);
    YT_OPTIONAL_SET_PROTO(protoJob, finish_time, job.FinishTime);

    YT_OPTIONAL_TO_PROTO(protoJob, address, job.Address);
    YT_OPTIONAL_TO_PROTO(protoJob, addresses, job.Addresses);
    if (job.Progress) {
        protoJob->set_progress(*job.Progress);
    }
    YT_OPTIONAL_SET_PROTO(protoJob, stderr_size, job.StderrSize);
    YT_OPTIONAL_SET_PROTO(protoJob, fail_context_size, job.FailContextSize);
    YT_OPTIONAL_SET_PROTO(protoJob, has_spec, job.HasSpec);

    if (job.Error) {
        protoJob->set_error(ToProto(job.Error));
    }
    if (job.InterruptionInfo) {
        protoJob->set_interruption_info(ToProto(job.InterruptionInfo));
    }

    if (job.BriefStatistics) {
        protoJob->set_brief_statistics(ToProto(job.BriefStatistics));
    }
    if (job.InputPaths) {
        protoJob->set_input_paths(ToProto(job.InputPaths));
    }
    if (job.CoreInfos) {
        protoJob->set_core_infos(ToProto(job.CoreInfos));
    }
    if (job.JobCompetitionId) {
        ToProto(protoJob->mutable_job_competition_id(), job.JobCompetitionId);
    }
    if (job.ProbingJobCompetitionId) {
        ToProto(protoJob->mutable_probing_job_competition_id(), job.ProbingJobCompetitionId);
    }
    YT_OPTIONAL_SET_PROTO(protoJob, has_competitors, job.HasCompetitors);
    YT_OPTIONAL_SET_PROTO(protoJob, has_probing_competitors, job.HasProbingCompetitors);
    YT_OPTIONAL_SET_PROTO(protoJob, is_stale, job.IsStale);
    if (job.ExecAttributes) {
        protoJob->set_exec_attributes(ToProto(job.ExecAttributes));
    }
    YT_OPTIONAL_TO_PROTO(protoJob, task_name, job.TaskName);
    YT_OPTIONAL_TO_PROTO(protoJob, pool_tree, job.PoolTree);
    YT_OPTIONAL_TO_PROTO(protoJob, pool, job.Pool);
    YT_OPTIONAL_SET_PROTO(protoJob, job_cookie, job.JobCookie);
    if (job.ArchiveFeatures) {
        protoJob->set_archive_features(ToProto(job.ArchiveFeatures));
    }
    YT_OPTIONAL_TO_PROTO(protoJob, monitoring_descriptor, job.MonitoringDescriptor);
    YT_OPTIONAL_SET_PROTO(protoJob, operation_incarnation, job.OperationIncarnation);
    YT_OPTIONAL_TO_PROTO(protoJob, allocation_id, job.AllocationId);
    if (job.Events) {
        protoJob->set_events(ToProto(job.Events));
    }
    if (job.Statistics) {
        protoJob->set_statistics(ToProto(job.Statistics));
    }
    YT_OPTIONAL_SET_PROTO(protoJob, gang_rank, job.GangRank);
}

void FromProto(NApi::TJob* job, const NProto::TJob& protoJob)
{
    if (protoJob.has_id()) {
        FromProto(&job->Id, protoJob.id());
    } else {
        job->Id = {};
    }
    if (protoJob.has_operation_id()) {
        FromProto(&job->OperationId, protoJob.operation_id());
    } else {
        job->OperationId = {};
    }
    job->Type = YT_APPLY_PROTO_OPTIONAL(protoJob, type, ConvertJobTypeFromProto);
    job->ControllerState = YT_APPLY_PROTO_OPTIONAL(protoJob, controller_state, ConvertJobStateFromProto);
    job->ArchiveState = YT_APPLY_PROTO_OPTIONAL(protoJob, archive_state, ConvertJobStateFromProto);
    job->StartTime = YT_OPTIONAL_FROM_PROTO(protoJob, start_time, TInstant);
    job->FinishTime = YT_OPTIONAL_FROM_PROTO(protoJob, finish_time, TInstant);
    job->Address = YT_OPTIONAL_FROM_PROTO(protoJob, address);
    if (protoJob.has_addresses()) {
        job->Addresses = FromProto<NNodeTrackerClient::TAddressMap>(protoJob.addresses());
    } else {
        job->Addresses = {};
    }
    job->Progress = YT_OPTIONAL_FROM_PROTO(protoJob, progress);
    job->StderrSize = YT_OPTIONAL_FROM_PROTO(protoJob, stderr_size);
    job->FailContextSize = YT_OPTIONAL_FROM_PROTO(protoJob, fail_context_size);
    if (protoJob.has_has_spec()) {
        job->HasSpec = protoJob.has_spec();
    }
    if (protoJob.has_error()) {
        job->Error = TYsonString(protoJob.error());
    } else {
        job->Error = TYsonString();
    }
    if (protoJob.has_interruption_info()) {
        job->InterruptionInfo = TYsonString(protoJob.interruption_info());
    } else {
        job->InterruptionInfo = TYsonString();
    }
    if (protoJob.has_brief_statistics()) {
        job->BriefStatistics = TYsonString(protoJob.brief_statistics());
    } else {
        job->BriefStatistics = TYsonString();
    }
    if (protoJob.has_input_paths()) {
        job->InputPaths = TYsonString(protoJob.input_paths());
    } else {
        job->InputPaths = TYsonString();
    }
    if (protoJob.has_core_infos()) {
        job->CoreInfos = TYsonString(protoJob.core_infos());
    } else {
        job->CoreInfos = TYsonString();
    }
    if (protoJob.has_job_competition_id()) {
        FromProto(&job->JobCompetitionId, protoJob.job_competition_id());
    } else {
        job->JobCompetitionId = {};
    }
    if (protoJob.has_probing_job_competition_id()) {
        FromProto(&job->ProbingJobCompetitionId, protoJob.probing_job_competition_id());
    } else {
        job->ProbingJobCompetitionId = {};
    }
    if (protoJob.has_has_competitors()) {
        job->HasCompetitors = protoJob.has_competitors();
    }
    job->HasProbingCompetitors = YT_OPTIONAL_FROM_PROTO(protoJob, has_probing_competitors);
    job->IsStale = YT_OPTIONAL_FROM_PROTO(protoJob, is_stale);
    if (protoJob.has_exec_attributes()) {
        job->ExecAttributes = TYsonString(protoJob.exec_attributes());
    } else {
        job->ExecAttributes = TYsonString();
    }
    if (protoJob.has_events()) {
        job->Events = TYsonString(protoJob.events());
    } else {
        job->Events = TYsonString();
    }
    job->TaskName = YT_OPTIONAL_FROM_PROTO(protoJob, task_name);
    job->PoolTree = YT_OPTIONAL_FROM_PROTO(protoJob, pool_tree);
    job->Pool = YT_OPTIONAL_FROM_PROTO(protoJob, pool);
    job->JobCookie = YT_OPTIONAL_FROM_PROTO(protoJob, job_cookie);
    if (protoJob.has_archive_features()) {
        job->ArchiveFeatures = TYsonString(protoJob.archive_features());
    } else {
        job->ArchiveFeatures = TYsonString();
    }
    job->MonitoringDescriptor = YT_OPTIONAL_FROM_PROTO(protoJob, monitoring_descriptor);
    job->OperationIncarnation = YT_OPTIONAL_FROM_PROTO(protoJob, operation_incarnation);
    if (protoJob.has_allocation_id()) {
        job->AllocationId = NScheduler::TAllocationId(FromProto<TGuid>(protoJob.allocation_id()));
    } else {
        job->AllocationId = {};
    }
    if (protoJob.has_statistics()) {
        job->Statistics = TYsonString(protoJob.statistics());
    } else {
        job->Statistics = TYsonString();
    }
    job->GangRank = YT_OPTIONAL_FROM_PROTO(protoJob, gang_rank);
}

void ToProto(
    NProto::TListJobsStatistics* protoStatistics,
    const NApi::TListJobsStatistics& statistics)
{
    protoStatistics->mutable_state_counts()->clear_entries();
    for (const auto& state : TEnumTraits<NJobTrackerClient::EJobState>::GetDomainValues()) {
        if (statistics.StateCounts[state] != 0) {
            auto* newStateCount = protoStatistics->mutable_state_counts()->add_entries();
            newStateCount->set_state(ConvertJobStateToProto(state));
            newStateCount->set_count(statistics.StateCounts[state]);
        }
    }

    protoStatistics->mutable_type_counts()->clear_entries();
    for (const auto& type : TEnumTraits<NJobTrackerClient::EJobType>::GetDomainValues()) {
        if (statistics.TypeCounts[type] != 0) {
            auto* newTypeCount = protoStatistics->mutable_type_counts()->add_entries();
            newTypeCount->set_type(ConvertJobTypeToProto(type));
            newTypeCount->set_count(statistics.TypeCounts[type]);
        }
    }
}

void FromProto(
    NApi::TListJobsStatistics* statistics,
    const NProto::TListJobsStatistics& protoStatistics)
{
    std::fill(statistics->StateCounts.begin(), statistics->StateCounts.end(), 0);
    for (const auto& stateCount : protoStatistics.state_counts().entries()) {
        auto state = ConvertJobStateFromProto(stateCount.state());
        YT_VERIFY(statistics->StateCounts.IsValidIndex(state));
        YT_VERIFY(statistics->StateCounts[state] == 0);
        statistics->StateCounts[state] = stateCount.count();
    }

    std::fill(statistics->TypeCounts.begin(), statistics->TypeCounts.end(), 0);
    for (const auto& typeCount : protoStatistics.type_counts().entries()) {
        auto type = ConvertJobTypeFromProto(typeCount.type());
        YT_VERIFY(statistics->TypeCounts.IsValidIndex(type));
        YT_VERIFY(statistics->TypeCounts[type] == 0);
        statistics->TypeCounts[type] = typeCount.count();
    }
}

void ToProto(
    NProto::TFetchChunkSpecConfig* protoFetchChunkSpecConfig,
    const NChunkClient::TFetchChunkSpecConfigPtr& fetchChunkSpecConfig)
{
    protoFetchChunkSpecConfig->set_max_chunk_per_fetch(
        fetchChunkSpecConfig->MaxChunksPerFetch);
    protoFetchChunkSpecConfig->set_max_chunk_per_locate_request(
        fetchChunkSpecConfig->MaxChunksPerLocateRequest);
}

void FromProto(
    const NChunkClient::TFetchChunkSpecConfigPtr& fetchChunkSpecConfig,
    const NProto::TFetchChunkSpecConfig& protoFetchChunkSpecConfig)
{
    FromProto(
        &fetchChunkSpecConfig->MaxChunksPerFetch,
        protoFetchChunkSpecConfig.max_chunk_per_fetch());
    FromProto(
        &fetchChunkSpecConfig->MaxChunksPerLocateRequest,
        protoFetchChunkSpecConfig.max_chunk_per_locate_request());
}

void ToProto(
    NProto::TFetcherConfig* protoFetcherConfig,
    const NChunkClient::TFetcherConfigPtr& fetcherConfig)
{
    protoFetcherConfig->set_node_rpc_timeout(
        ToProto(fetcherConfig->NodeRpcTimeout));
}

void FromProto(
    const NChunkClient::TFetcherConfigPtr& fetcherConfig,
    const NProto::TFetcherConfig& protoFetcherConfig)
{
    fetcherConfig->NodeRpcTimeout = TDuration::FromValue(protoFetcherConfig.node_rpc_timeout());
}

void ToProto(
    NProto::TColumnarStatistics* protoStatistics,
    const NTableClient::TColumnarStatistics& statistics)
{
    protoStatistics->Clear();

    ToProto(protoStatistics->mutable_column_data_weights(), statistics.ColumnDataWeights);
    YT_OPTIONAL_SET_PROTO(protoStatistics, timestamp_total_weight, statistics.TimestampTotalWeight);
    protoStatistics->set_legacy_chunk_data_weight(statistics.LegacyChunkDataWeight);

    NYT::NTableClient::ToProto(protoStatistics->mutable_column_min_values(), statistics.ColumnMinValues);
    NYT::NTableClient::ToProto(protoStatistics->mutable_column_max_values(), statistics.ColumnMaxValues);
    ToProto(protoStatistics->mutable_column_non_null_value_counts(), statistics.ColumnNonNullValueCounts);

    YT_OPTIONAL_SET_PROTO(protoStatistics, chunk_row_count, statistics.ChunkRowCount);
    YT_OPTIONAL_SET_PROTO(protoStatistics, legacy_chunk_row_count, statistics.LegacyChunkRowCount);

    ToProto(protoStatistics->mutable_column_hyperloglog_digests(), statistics.LargeStatistics.ColumnHyperLogLogDigests);
}

void FromProto(
    NTableClient::TColumnarStatistics* statistics,
    const NProto::TColumnarStatistics& protoStatistics)
{
    FromProto(&statistics->ColumnDataWeights, protoStatistics.column_data_weights());
    statistics->TimestampTotalWeight = YT_OPTIONAL_FROM_PROTO(protoStatistics, timestamp_total_weight);
    statistics->LegacyChunkDataWeight = protoStatistics.legacy_chunk_data_weight();

    NYT::NTableClient::FromProto(&statistics->ColumnMinValues, protoStatistics.column_min_values());
    NYT::NTableClient::FromProto(&statistics->ColumnMaxValues, protoStatistics.column_max_values());
    FromProto(&statistics->ColumnNonNullValueCounts, protoStatistics.column_non_null_value_counts());

    statistics->ChunkRowCount = YT_OPTIONAL_FROM_PROTO(protoStatistics, chunk_row_count);
    statistics->LegacyChunkRowCount = YT_OPTIONAL_FROM_PROTO(protoStatistics, legacy_chunk_row_count);

    FromProto(&statistics->LargeStatistics.ColumnHyperLogLogDigests, protoStatistics.column_hyperloglog_digests());
}

void ToProto(
    NProto::TMultiTablePartition* protoMultiTablePartition,
    const NApi::TMultiTablePartition& multiTablePartition)
{
    protoMultiTablePartition->Clear();

    for (const auto& range : multiTablePartition.TableRanges) {
        protoMultiTablePartition->add_table_ranges(ToString(range));
    }

    auto aggregateStatistics = protoMultiTablePartition->mutable_aggregate_statistics();
    aggregateStatistics->set_chunk_count(multiTablePartition.AggregateStatistics.ChunkCount);
    aggregateStatistics->set_data_weight(multiTablePartition.AggregateStatistics.DataWeight);
    aggregateStatistics->set_row_count(multiTablePartition.AggregateStatistics.RowCount);

    if (multiTablePartition.Cookie) {
        ToProto(protoMultiTablePartition->mutable_cookie(), multiTablePartition.Cookie);
    }
}

void ToProto(
    TProtobufString* protoCookie,
    const TTablePartitionCookiePtr& cookie)
{
    auto cookieBytes = ConvertToYsonString(cookie);
    *protoCookie = cookieBytes.ToString();
}

void FromProto(
    NApi::TMultiTablePartition* multiTablePartition,
    const NProto::TMultiTablePartition& protoMultiTablePartition)
{
    for (const auto& range : protoMultiTablePartition.table_ranges()) {
        multiTablePartition->TableRanges.emplace_back(NYPath::TRichYPath::Parse(FromProto<TString>(range)));
    }

    if (protoMultiTablePartition.has_aggregate_statistics()) {
        const auto& aggregateStatistics = protoMultiTablePartition.aggregate_statistics();
        multiTablePartition->AggregateStatistics.ChunkCount = aggregateStatistics.chunk_count();
        multiTablePartition->AggregateStatistics.DataWeight = aggregateStatistics.data_weight();
        multiTablePartition->AggregateStatistics.RowCount = aggregateStatistics.row_count();
    }

    if (protoMultiTablePartition.has_cookie()) {
        FromProto(&multiTablePartition->Cookie, protoMultiTablePartition.cookie());
    }
}

void FromProto(
    NApi::TMultiTablePartitions* multiTablePartitions,
    const NProto::TRspPartitionTables& protoRspPartitionTables)
{
    FromProto(
        &multiTablePartitions->Partitions,
        protoRspPartitionTables.partitions());
}

void FromProto(
    TTablePartitionCookiePtr* cookie,
    const TProtobufString& protoCookie)
{
    *cookie = ConvertTo<TTablePartitionCookiePtr>(TYsonStringBuf(protoCookie));
}

void ToProto(
    NProto::TRowBatchReadOptions* proto,
    const NQueueClient::TQueueRowBatchReadOptions& result)
{
    proto->set_max_row_count(result.MaxRowCount);
    proto->set_max_data_weight(result.MaxDataWeight);
    YT_OPTIONAL_SET_PROTO(proto, data_weight_per_row_hint, result.DataWeightPerRowHint);
}

void FromProto(
    NQueueClient::TQueueRowBatchReadOptions* result,
    const NProto::TRowBatchReadOptions& proto)
{
    result->MaxRowCount = proto.max_row_count();
    result->MaxDataWeight = proto.max_data_weight();
    result->DataWeightPerRowHint = YT_OPTIONAL_FROM_PROTO(proto, data_weight_per_row_hint);
}

void ToProto(
    NProto::TTableBackupManifest* protoManifest,
    const NApi::TTableBackupManifestPtr& manifest)
{
    protoManifest->set_source_path(manifest->SourcePath);
    protoManifest->set_destination_path(manifest->DestinationPath);
    protoManifest->set_ordered_mode(ToProto(manifest->OrderedMode));
}

void FromProto(
    NApi::TTableBackupManifestPtr* manifest,
    const NProto::TTableBackupManifest& protoManifest)
{
    *manifest = New<NApi::TTableBackupManifest>();

    (*manifest)->SourcePath = protoManifest.source_path();
    (*manifest)->DestinationPath = protoManifest.destination_path();
    (*manifest)->OrderedMode = FromProto<EOrderedTableBackupMode>(protoManifest.ordered_mode());
}

void ToProto(
    NProto::TBackupManifest::TClusterManifest* protoEntry,
    const std::pair<std::string, std::vector<NApi::TTableBackupManifestPtr>>& entry)
{
    protoEntry->set_cluster(entry.first);
    ToProto(protoEntry->mutable_table_manifests(), entry.second);
}

void FromProto(
    std::pair<std::string, std::vector<NApi::TTableBackupManifestPtr>>* entry,
    const NProto::TBackupManifest::TClusterManifest& protoEntry)
{
    entry->first = protoEntry.cluster();
    FromProto(&entry->second, protoEntry.table_manifests());
}

void ToProto(
    NProto::TBackupManifest* protoManifest,
    const NApi::TBackupManifest& manifest)
{
    ToProto(protoManifest->mutable_clusters(), manifest.Clusters);
}

void FromProto(
    NApi::TBackupManifest* manifest,
    const NProto::TBackupManifest& protoManifest)
{
    FromProto(&manifest->Clusters, protoManifest.clusters());
}

void ToProto(
    NProto::TQuery* protoQuery,
    const NApi::TQuery& query)
{
    protoQuery->Clear();

    ToProto(protoQuery->mutable_id(), query.Id);

    if (query.Engine) {
        protoQuery->set_engine(ConvertQueryEngineToProto(*query.Engine));
    }
    YT_OPTIONAL_TO_PROTO(protoQuery, query, query.Query);
    if (query.Files) {
        protoQuery->set_files(ToProto(*query.Files));
    }
    YT_OPTIONAL_SET_PROTO(protoQuery, start_time, query.StartTime);
    YT_OPTIONAL_SET_PROTO(protoQuery, finish_time, query.FinishTime);
    if (query.Settings) {
        protoQuery->set_settings(ToProto(query.Settings));
    }
    YT_OPTIONAL_TO_PROTO(protoQuery, user, query.User);
    if (query.AccessControlObject) {
        protoQuery->set_access_control_object(*query.AccessControlObject);
    }
    protoQuery->set_access_control_objects(ToProto(*query.AccessControlObjects));

    if (query.State) {
        protoQuery->set_state(ConvertQueryStateToProto(*query.State));
    }
    YT_OPTIONAL_SET_PROTO(protoQuery, result_count, query.ResultCount);
    if (query.Progress) {
        protoQuery->set_progress(ToProto(query.Progress));
    }
    YT_OPTIONAL_TO_PROTO(protoQuery, error, query.Error);
    if (query.Annotations) {
        protoQuery->set_annotations(ToProto(query.Annotations));
    }
    if (query.OtherAttributes) {
        ToProto(protoQuery->mutable_other_attributes(), *query.OtherAttributes);
    }
}

void FromProto(
    NApi::TQuery* query,
    const NProto::TQuery& protoQuery)
{
    FromProto(&query->Id, protoQuery.id());

    query->Engine = YT_APPLY_PROTO_OPTIONAL(protoQuery, engine, ConvertQueryEngineFromProto);
    query->Query = YT_OPTIONAL_FROM_PROTO(protoQuery, query);
    query->Files = YT_APPLY_PROTO_OPTIONAL(protoQuery, files, TYsonString);
    query->StartTime = YT_OPTIONAL_FROM_PROTO(protoQuery, start_time, TInstant);
    query->FinishTime = YT_OPTIONAL_FROM_PROTO(protoQuery, finish_time, TInstant);
    if (protoQuery.has_settings()) {
        query->Settings = TYsonString(protoQuery.settings());
    } else {
        query->Settings = TYsonString{};
    }
    query->User = YT_OPTIONAL_FROM_PROTO(protoQuery, user);
    query->AccessControlObject = YT_OPTIONAL_FROM_PROTO(protoQuery, access_control_object);
    query->AccessControlObjects = YT_APPLY_PROTO_OPTIONAL(protoQuery, access_control_objects, TYsonString);
    query->State = YT_APPLY_PROTO_OPTIONAL(protoQuery, state, ConvertQueryStateFromProto);
    query->ResultCount = YT_OPTIONAL_FROM_PROTO(protoQuery, result_count);
    if (protoQuery.has_progress()) {
        query->Progress = TYsonString(protoQuery.progress());
    } else {
        query->Progress = TYsonString{};
    }
    query->Error = YT_APPLY_PROTO_OPTIONAL(protoQuery, error, FromProto<TError>);
    if (protoQuery.has_annotations()) {
        query->Annotations = TYsonString(protoQuery.annotations());
    } else {
        query->Annotations = TYsonString{};
    }
    if (protoQuery.has_other_attributes()) {
        query->OtherAttributes = NYTree::FromProto(protoQuery.other_attributes());
    } else if (query->OtherAttributes) {
        query->OtherAttributes->Clear();
    }
}

////////////////////////////////////////////////////////////////////////////////
// ENUMS
////////////////////////////////////////////////////////////////////////////////

NProto::EOperationType ConvertOperationTypeToProto(
    NScheduler::EOperationType operationType)
{
    switch (operationType) {
        case NScheduler::EOperationType::Map:
            return NProto::EOperationType::OT_MAP;
        case NScheduler::EOperationType::Merge:
            return NProto::EOperationType::OT_MERGE;
        case NScheduler::EOperationType::Erase:
            return NProto::EOperationType::OT_ERASE;
        case NScheduler::EOperationType::Sort:
            return NProto::EOperationType::OT_SORT;
        case NScheduler::EOperationType::Reduce:
            return NProto::EOperationType::OT_REDUCE;
        case NScheduler::EOperationType::MapReduce:
            return NProto::EOperationType::OT_MAP_REDUCE;
        case NScheduler::EOperationType::RemoteCopy:
            return NProto::EOperationType::OT_REMOTE_COPY;
        case NScheduler::EOperationType::JoinReduce:
            return NProto::EOperationType::OT_JOIN_REDUCE;
        case NScheduler::EOperationType::Vanilla:
            return NProto::EOperationType::OT_VANILLA;
    }
    YT_ABORT();
}

NScheduler::EOperationType ConvertOperationTypeFromProto(
    NProto::EOperationType proto)
{
    switch (proto) {
        case NProto::EOperationType::OT_MAP:
            return NScheduler::EOperationType::Map;
        case NProto::EOperationType::OT_MERGE:
            return NScheduler::EOperationType::Merge;
        case NProto::EOperationType::OT_ERASE:
            return NScheduler::EOperationType::Erase;
        case NProto::EOperationType::OT_SORT:
            return NScheduler::EOperationType::Sort;
        case NProto::EOperationType::OT_REDUCE:
            return NScheduler::EOperationType::Reduce;
        case NProto::EOperationType::OT_MAP_REDUCE:
            return NScheduler::EOperationType::MapReduce;
        case NProto::EOperationType::OT_REMOTE_COPY:
            return NScheduler::EOperationType::RemoteCopy;
        case NProto::EOperationType::OT_JOIN_REDUCE:
            return NScheduler::EOperationType::JoinReduce;
        case NProto::EOperationType::OT_VANILLA:
            return NScheduler::EOperationType::Vanilla;
        case NProto::EOperationType::OT_UNKNOWN:
            THROW_ERROR_EXCEPTION("Protobuf contains unknown value for operation type");
    }
    YT_ABORT();
}

NProto::EOperationState ConvertOperationStateToProto(
    NScheduler::EOperationState operationState)
{
    switch (operationState) {
        case NScheduler::EOperationState::None:
            return NProto::EOperationState::OS_NONE;
        case NScheduler::EOperationState::Starting:
            return NProto::EOperationState::OS_STARTING;
        case NScheduler::EOperationState::Orphaned:
            return NProto::EOperationState::OS_ORPHANED;
        case NScheduler::EOperationState::WaitingForAgent:
            return NProto::EOperationState::OS_WAITING_FOR_AGENT;
        case NScheduler::EOperationState::Initializing:
            return NProto::EOperationState::OS_INITIALIZING;
        case NScheduler::EOperationState::Preparing:
            return NProto::EOperationState::OS_PREPARING;
        case NScheduler::EOperationState::Materializing:
            return NProto::EOperationState::OS_MATERIALIZING;
        case NScheduler::EOperationState::Reviving:
            return NProto::EOperationState::OS_REVIVING;
        case NScheduler::EOperationState::RevivingJobs:
            return NProto::EOperationState::OS_REVIVING_JOBS;
        case NScheduler::EOperationState::Pending:
            return NProto::EOperationState::OS_PENDING;
        case NScheduler::EOperationState::Running:
            return NProto::EOperationState::OS_RUNNING;
        case NScheduler::EOperationState::Completing:
            return NProto::EOperationState::OS_COMPLETING;
        case NScheduler::EOperationState::Completed:
            return NProto::EOperationState::OS_COMPLETED;
        case NScheduler::EOperationState::Aborting:
            return NProto::EOperationState::OS_ABORTING;
        case NScheduler::EOperationState::Aborted:
            return NProto::EOperationState::OS_ABORTED;
        case NScheduler::EOperationState::Failing:
            return NProto::EOperationState::OS_FAILING;
        case NScheduler::EOperationState::Failed:
            return NProto::EOperationState::OS_FAILED;
        case NScheduler::EOperationState::ReviveInitializing:
            return NProto::EOperationState::OS_REVIVE_INITIALIZING;
    }
    YT_ABORT();
}

NScheduler::EOperationState ConvertOperationStateFromProto(
    NProto::EOperationState proto)
{
    switch (proto) {
        case NProto::EOperationState::OS_NONE:
            return NScheduler::EOperationState::None;
        case NProto::EOperationState::OS_STARTING:
            return NScheduler::EOperationState::Starting;
        case NProto::EOperationState::OS_ORPHANED:
            return NScheduler::EOperationState::Orphaned;
        case NProto::EOperationState::OS_WAITING_FOR_AGENT:
            return NScheduler::EOperationState::WaitingForAgent;
        case NProto::EOperationState::OS_INITIALIZING:
            return NScheduler::EOperationState::Initializing;
        case NProto::EOperationState::OS_PREPARING:
            return NScheduler::EOperationState::Preparing;
        case NProto::EOperationState::OS_MATERIALIZING:
            return NScheduler::EOperationState::Materializing;
        case NProto::EOperationState::OS_REVIVING:
            return NScheduler::EOperationState::Reviving;
        case NProto::EOperationState::OS_REVIVING_JOBS:
            return NScheduler::EOperationState::RevivingJobs;
        case NProto::EOperationState::OS_PENDING:
            return NScheduler::EOperationState::Pending;
        case NProto::EOperationState::OS_RUNNING:
            return NScheduler::EOperationState::Running;
        case NProto::EOperationState::OS_COMPLETING:
            return NScheduler::EOperationState::Completing;
        case NProto::EOperationState::OS_COMPLETED:
            return NScheduler::EOperationState::Completed;
        case NProto::EOperationState::OS_ABORTING:
            return NScheduler::EOperationState::Aborting;
        case NProto::EOperationState::OS_ABORTED:
            return NScheduler::EOperationState::Aborted;
        case NProto::EOperationState::OS_FAILING:
            return NScheduler::EOperationState::Failing;
        case NProto::EOperationState::OS_FAILED:
            return NScheduler::EOperationState::Failed;
        case NProto::EOperationState::OS_REVIVE_INITIALIZING:
            return NScheduler::EOperationState::ReviveInitializing;
        case NProto::EOperationState::OS_UNKNOWN:
            THROW_ERROR_EXCEPTION("Protobuf contains unknown value for operation state");
    }
    YT_ABORT();
}

NProto::EOperationEventType ConvertOperationEventTypeToProto(
    NApi::EOperationEventType operationEventType)
{
    switch (operationEventType) {
        case NApi::EOperationEventType::IncarnationStarted:
            return NProto::EOperationEventType::OET_INCARNATION_STARTED;
    }
}

NApi::EOperationEventType ConvertOperationEventTypeFromProto(
    NProto::EOperationEventType proto)
{
    switch (proto) {
        case NProto::EOperationEventType::OET_INCARNATION_STARTED:
            return NApi::EOperationEventType::IncarnationStarted;
    }
}

NProto::EIncarnationSwitchReason ConvertIncarnationSwitchReasonToProto(
    NControllerAgent::EOperationIncarnationSwitchReason operationEventType)
{
    switch (operationEventType) {
        case NYT::NControllerAgent::EOperationIncarnationSwitchReason::JobAborted:
            return NProto::EIncarnationSwitchReason::ISR_JOB_ABORTED;
        case NYT::NControllerAgent::EOperationIncarnationSwitchReason::JobFailed:
            return NProto::EIncarnationSwitchReason::ISR_JOB_FAILED;
        case NYT::NControllerAgent::EOperationIncarnationSwitchReason::JobInterrupted:
            return NProto::EIncarnationSwitchReason::ISR_JOB_INTERRUPTED;
        case NYT::NControllerAgent::EOperationIncarnationSwitchReason::JobLackAfterRevival:
            return NProto::EIncarnationSwitchReason::ISR_JOB_LACK_AFTER_REVIVAL;
    }
}

NControllerAgent::EOperationIncarnationSwitchReason ConvertIncarnationSwitchReasonFromProto(
    NProto::EIncarnationSwitchReason proto)
{
    switch (proto) {
        case NProto::EIncarnationSwitchReason::ISR_JOB_ABORTED:
            return NYT::NControllerAgent::EOperationIncarnationSwitchReason::JobAborted;
        case NProto::EIncarnationSwitchReason::ISR_JOB_FAILED:
            return NYT::NControllerAgent::EOperationIncarnationSwitchReason::JobFailed;
        case NProto::EIncarnationSwitchReason::ISR_JOB_INTERRUPTED:
            return NYT::NControllerAgent::EOperationIncarnationSwitchReason::JobInterrupted;
        case NProto::EIncarnationSwitchReason::ISR_JOB_LACK_AFTER_REVIVAL:
            return NYT::NControllerAgent::EOperationIncarnationSwitchReason::JobLackAfterRevival;
    }
}

NProto::EJobType ConvertJobTypeToProto(
    NJobTrackerClient::EJobType jobType)
{
    switch (jobType) {
        case NJobTrackerClient::EJobType::Map:
            return NProto::EJobType::JT_MAP;
        case NJobTrackerClient::EJobType::PartitionMap:
            return NProto::EJobType::JT_PARTITION_MAP;
        case NJobTrackerClient::EJobType::SortedMerge:
            return NProto::EJobType::JT_SORTED_MERGE;
        case NJobTrackerClient::EJobType::OrderedMerge:
            return NProto::EJobType::JT_ORDERED_MERGE;
        case NJobTrackerClient::EJobType::UnorderedMerge:
            return NProto::EJobType::JT_UNORDERED_MERGE;
        case NJobTrackerClient::EJobType::Partition:
            return NProto::EJobType::JT_PARTITION;
        case NJobTrackerClient::EJobType::SimpleSort:
            return NProto::EJobType::JT_SIMPLE_SORT;
        case NJobTrackerClient::EJobType::FinalSort:
            return NProto::EJobType::JT_FINAL_SORT;
        case NJobTrackerClient::EJobType::SortedReduce:
            return NProto::EJobType::JT_SORTED_REDUCE;
        case NJobTrackerClient::EJobType::PartitionReduce:
            return NProto::EJobType::JT_PARTITION_REDUCE;
        case NJobTrackerClient::EJobType::ReduceCombiner:
            return NProto::EJobType::JT_REDUCE_COMBINER;
        case NJobTrackerClient::EJobType::RemoteCopy:
            return NProto::EJobType::JT_REMOTE_COPY;
        case NJobTrackerClient::EJobType::IntermediateSort:
            return NProto::EJobType::JT_INTERMEDIATE_SORT;
        case NJobTrackerClient::EJobType::OrderedMap:
            return NProto::EJobType::JT_ORDERED_MAP;
        case NJobTrackerClient::EJobType::JoinReduce:
            return NProto::EJobType::JT_JOIN_REDUCE;
        case NJobTrackerClient::EJobType::Vanilla:
            return NProto::EJobType::JT_VANILLA;
        case NJobTrackerClient::EJobType::SchedulerUnknown:
            return NProto::EJobType::JT_SCHEDULER_UNKNOWN;
        case NJobTrackerClient::EJobType::ReplicateChunk:
            return NProto::EJobType::JT_REPLICATE_CHUNK;
        case NJobTrackerClient::EJobType::RemoveChunk:
            return NProto::EJobType::JT_REMOVE_CHUNK;
        case NJobTrackerClient::EJobType::RepairChunk:
            return NProto::EJobType::JT_REPAIR_CHUNK;
        case NJobTrackerClient::EJobType::SealChunk:
            return NProto::EJobType::JT_SEAL_CHUNK;
        case NJobTrackerClient::EJobType::MergeChunks:
            return NProto::EJobType::JT_MERGE_CHUNKS;
        case NJobTrackerClient::EJobType::AutotomizeChunk:
            return NProto::EJobType::JT_AUTOTOMIZE_CHUNK;
        case NJobTrackerClient::EJobType::ShallowMerge:
            return NProto::EJobType::JT_SHALLOW_MERGE;
        case NJobTrackerClient::EJobType::ReincarnateChunk:
            return NProto::EJobType::JT_REINCARNATE_CHUNK;
    }
    YT_ABORT();
}

NJobTrackerClient::EJobType ConvertJobTypeFromProto(
    NProto::EJobType proto)
{
    switch (proto) {
        case NProto::EJobType::JT_MAP:
            return NJobTrackerClient::EJobType::Map;
        case NProto::EJobType::JT_PARTITION_MAP:
            return NJobTrackerClient::EJobType::PartitionMap;
        case NProto::EJobType::JT_SORTED_MERGE:
            return NJobTrackerClient::EJobType::SortedMerge;
        case NProto::EJobType::JT_ORDERED_MERGE:
            return NJobTrackerClient::EJobType::OrderedMerge;
        case NProto::EJobType::JT_UNORDERED_MERGE:
            return NJobTrackerClient::EJobType::UnorderedMerge;
        case NProto::EJobType::JT_PARTITION:
            return NJobTrackerClient::EJobType::Partition;
        case NProto::EJobType::JT_SIMPLE_SORT:
            return NJobTrackerClient::EJobType::SimpleSort;
        case NProto::EJobType::JT_FINAL_SORT:
            return NJobTrackerClient::EJobType::FinalSort;
        case NProto::EJobType::JT_SORTED_REDUCE:
            return NJobTrackerClient::EJobType::SortedReduce;
        case NProto::EJobType::JT_PARTITION_REDUCE:
            return NJobTrackerClient::EJobType::PartitionReduce;
        case NProto::EJobType::JT_REDUCE_COMBINER:
            return NJobTrackerClient::EJobType::ReduceCombiner;
        case NProto::EJobType::JT_REMOTE_COPY:
            return NJobTrackerClient::EJobType::RemoteCopy;
        case NProto::EJobType::JT_INTERMEDIATE_SORT:
            return NJobTrackerClient::EJobType::IntermediateSort;
        case NProto::EJobType::JT_ORDERED_MAP:
            return NJobTrackerClient::EJobType::OrderedMap;
        case NProto::EJobType::JT_JOIN_REDUCE:
            return NJobTrackerClient::EJobType::JoinReduce;
        case NProto::EJobType::JT_VANILLA:
            return NJobTrackerClient::EJobType::Vanilla;
        case NProto::EJobType::JT_SCHEDULER_UNKNOWN:
            return NJobTrackerClient::EJobType::SchedulerUnknown;
        case NProto::EJobType::JT_REPLICATE_CHUNK:
            return NJobTrackerClient::EJobType::ReplicateChunk;
        case NProto::EJobType::JT_REMOVE_CHUNK:
            return NJobTrackerClient::EJobType::RemoveChunk;
        case NProto::EJobType::JT_REPAIR_CHUNK:
            return NJobTrackerClient::EJobType::RepairChunk;
        case NProto::EJobType::JT_SEAL_CHUNK:
            return NJobTrackerClient::EJobType::SealChunk;
        case NProto::EJobType::JT_MERGE_CHUNKS:
            return NJobTrackerClient::EJobType::MergeChunks;
        case NProto::EJobType::JT_AUTOTOMIZE_CHUNK:
            return NJobTrackerClient::EJobType::AutotomizeChunk;
        case NProto::EJobType::JT_SHALLOW_MERGE:
            return NJobTrackerClient::EJobType::ShallowMerge;
        case NProto::EJobType::JT_REINCARNATE_CHUNK:
            return NJobTrackerClient::EJobType::ReincarnateChunk;
        case NProto::EJobType::JT_UNKNOWN:
            THROW_ERROR_EXCEPTION("Protobuf contains unknown value for job type");
    }
    YT_ABORT();
}

NProto::EJobState ConvertJobStateToProto(
    NJobTrackerClient::EJobState jobState)
{
    switch (jobState) {
        case NJobTrackerClient::EJobState::Waiting:
            return NProto::EJobState::JS_WAITING;
        case NJobTrackerClient::EJobState::Running:
            return NProto::EJobState::JS_RUNNING;
        case NJobTrackerClient::EJobState::Aborting:
            return NProto::EJobState::JS_ABORTING;
        case NJobTrackerClient::EJobState::Completed:
            return NProto::EJobState::JS_COMPLETED;
        case NJobTrackerClient::EJobState::Failed:
            return NProto::EJobState::JS_FAILED;
        case NJobTrackerClient::EJobState::Aborted:
            return NProto::EJobState::JS_ABORTED;
        case NJobTrackerClient::EJobState::Lost:
            return NProto::EJobState::JS_LOST;
        case NJobTrackerClient::EJobState::None:
            return NProto::EJobState::JS_NONE;
    }
    YT_ABORT();
}

NJobTrackerClient::EJobState ConvertJobStateFromProto(
    NProto::EJobState proto)
{
    switch (proto) {
        case NProto::EJobState::JS_WAITING:
            return NJobTrackerClient::EJobState::Waiting;
        case NProto::EJobState::JS_RUNNING:
            return NJobTrackerClient::EJobState::Running;
        case NProto::EJobState::JS_ABORTING:
            return NJobTrackerClient::EJobState::Aborting;
        case NProto::EJobState::JS_COMPLETED:
            return NJobTrackerClient::EJobState::Completed;
        case NProto::EJobState::JS_FAILED:
            return NJobTrackerClient::EJobState::Failed;
        case NProto::EJobState::JS_ABORTED:
            return NJobTrackerClient::EJobState::Aborted;
        case NProto::EJobState::JS_LOST:
            return NJobTrackerClient::EJobState::Lost;
        case NProto::EJobState::JS_NONE:
            return NJobTrackerClient::EJobState::None;
        case NProto::EJobState::JS_UNKNOWN:
            THROW_ERROR_EXCEPTION("Protobuf contains unknown value for job state");
    }
    YT_ABORT();
}

NProto::EQueryEngine ConvertQueryEngineToProto(
    NQueryTrackerClient::EQueryEngine queryEngine)
{
    switch (queryEngine) {
        case NQueryTrackerClient::EQueryEngine::Ql:
            return NProto::EQueryEngine::QE_QL;
        case NQueryTrackerClient::EQueryEngine::Yql:
            return NProto::EQueryEngine::QE_YQL;
        case NQueryTrackerClient::EQueryEngine::Chyt:
            return NProto::EQueryEngine::QE_CHYT;
        case NQueryTrackerClient::EQueryEngine::Mock:
            return NProto::EQueryEngine::QE_MOCK;
        case NQueryTrackerClient::EQueryEngine::Spyt:
            return NProto::EQueryEngine::QE_SPYT;
    }
    YT_ABORT();
}

NQueryTrackerClient::EQueryEngine ConvertQueryEngineFromProto(
    NProto::EQueryEngine proto)
{
    switch (proto) {
        case NProto::EQueryEngine::QE_QL:
            return NQueryTrackerClient::EQueryEngine::Ql;
        case NProto::EQueryEngine::QE_YQL:
            return NQueryTrackerClient::EQueryEngine::Yql;
        case NProto::EQueryEngine::QE_CHYT:
            return NQueryTrackerClient::EQueryEngine::Chyt;
        case NProto::EQueryEngine::QE_MOCK:
            return NQueryTrackerClient::EQueryEngine::Mock;
        case NProto::EQueryEngine::QE_SPYT:
            return NQueryTrackerClient::EQueryEngine::Spyt;
        case NProto::EQueryEngine::QE_UNKNOWN:
            THROW_ERROR_EXCEPTION("Protobuf contains unknown value for query engine");
    }
    YT_ABORT();
}

NProto::EQueryState ConvertQueryStateToProto(
    NQueryTrackerClient::EQueryState queryState)
{
    switch (queryState) {
        case NQueryTrackerClient::EQueryState::Draft:
            return NProto::EQueryState::QS_DRAFT;
        case NQueryTrackerClient::EQueryState::Pending:
            return NProto::EQueryState::QS_PENDING;
        case NQueryTrackerClient::EQueryState::Running:
            return NProto::EQueryState::QS_RUNNING;
        case NQueryTrackerClient::EQueryState::Aborting:
            return NProto::EQueryState::QS_ABORTING;
        case NQueryTrackerClient::EQueryState::Aborted:
            return NProto::EQueryState::QS_ABORTED;
        case NQueryTrackerClient::EQueryState::Completing:
            return NProto::EQueryState::QS_COMPLETING;
        case NQueryTrackerClient::EQueryState::Completed:
            return NProto::EQueryState::QS_COMPLETED;
        case NQueryTrackerClient::EQueryState::Failing:
            return NProto::EQueryState::QS_FAILING;
        case NQueryTrackerClient::EQueryState::Failed:
            return NProto::EQueryState::QS_FAILED;
    }
    YT_ABORT();
}

NQueryTrackerClient::EQueryState ConvertQueryStateFromProto(
    NProto::EQueryState proto)
{
    switch (proto) {
        case NProto::EQueryState::QS_DRAFT:
            return NQueryTrackerClient::EQueryState::Draft;
        case NProto::EQueryState::QS_PENDING:
            return NQueryTrackerClient::EQueryState::Pending;
        case NProto::EQueryState::QS_RUNNING:
            return NQueryTrackerClient::EQueryState::Running;
        case NProto::EQueryState::QS_ABORTING:
            return NQueryTrackerClient::EQueryState::Aborting;
        case NProto::EQueryState::QS_ABORTED:
            return NQueryTrackerClient::EQueryState::Aborted;
        case NProto::EQueryState::QS_COMPLETING:
            return NQueryTrackerClient::EQueryState::Completing;
        case NProto::EQueryState::QS_COMPLETED:
            return NQueryTrackerClient::EQueryState::Completed;
        case NProto::EQueryState::QS_FAILING:
            return NQueryTrackerClient::EQueryState::Failing;
        case NProto::EQueryState::QS_FAILED:
            return NQueryTrackerClient::EQueryState::Failed;
        case NProto::EQueryState::QS_UNKNOWN:
            THROW_ERROR_EXCEPTION("Protobuf contains unknown value for query state");
    }
    YT_ABORT();
}

NApi::EJobStderrType ConvertJobStderrTypeFromProto(
    NProto::EJobStderrType proto)
{
    switch (proto) {
        case NProto::EJobStderrType::JST_USER_JOB_STDERR:
            return NApi::EJobStderrType::UserJobStderr;
        case NProto::EJobStderrType::JST_GPU_CHECK_STDERR:
            return NApi::EJobStderrType::GpuCheckStderr;
    }
}

NProto::EJobStderrType ConvertJobStderrTypeToProto(
    NApi::EJobStderrType jobStderrType)
{
    switch (jobStderrType) {
        case NApi::EJobStderrType::UserJobStderr:
            return NProto::EJobStderrType::JST_USER_JOB_STDERR;
        case NApi::EJobStderrType::GpuCheckStderr:
            return NProto::EJobStderrType::JST_GPU_CHECK_STDERR;
    }
}

////////////////////////////////////////////////////////////////////////////////

void FillRequest(
    TReqStartDistributedWriteSession* req,
    const NYPath::TRichYPath& path,
    const TDistributedWriteSessionStartOptions& options)
{
    ToProto(req->mutable_path(), path);
    req->set_cookie_count(options.CookieCount);
    if (options.Timeout) {
        req->set_timeout(options.Timeout->GetValue());
    }

    if (options.TransactionId) {
        ToProto(req->mutable_transactional_options(), options);
    }
}

void ParseRequest(
    NYPath::TRichYPath* mutablePath,
    TDistributedWriteSessionStartOptions* mutableOptions,
    const TReqStartDistributedWriteSession& req)
{
    *mutablePath = FromProto<NYPath::TRichYPath>(req.path());
    mutableOptions->CookieCount = req.cookie_count();
    if (req.has_timeout()) {
        mutableOptions->Timeout = TDuration::FromValue(req.timeout());
    }
    if (req.has_transactional_options()) {
        FromProto(mutableOptions, req.transactional_options());
    }
}

////////////////////////////////////////////////////////////////////////////////

void FillRequest(
    TReqFinishDistributedWriteSession* req,
    const TDistributedWriteSessionWithResults& sessionWithResults,
    const TDistributedWriteSessionFinishOptions& options)
{
    YT_VERIFY(sessionWithResults.Session);

    req->set_signed_session(ToProto(ConvertToYsonString(sessionWithResults.Session)));
    for (const auto& writeResult : sessionWithResults.Results) {
        YT_VERIFY(writeResult);
        req->add_signed_write_results(ConvertToYsonString(writeResult).ToString());
    }
    req->set_max_children_per_attach_request(options.MaxChildrenPerAttachRequest);
}

void ParseRequest(
    TDistributedWriteSessionWithResults* mutableSessionWithResults,
    TDistributedWriteSessionFinishOptions* mutableOptions,
    const TReqFinishDistributedWriteSession& req)
{
    mutableSessionWithResults->Results.reserve(req.signed_write_results().size());
    for (const auto& writeResult : req.signed_write_results()) {
        mutableSessionWithResults->Results.push_back(ConvertTo<TSignedWriteFragmentResultPtr>(TYsonString(writeResult)));
    }

    mutableSessionWithResults->Session = ConvertTo<TSignedDistributedWriteSessionPtr>(TYsonString(req.signed_session()));

    mutableOptions->MaxChildrenPerAttachRequest = req.max_children_per_attach_request();
}

////////////////////////////////////////////////////////////////////////////////

void FillRequest(
    TReqWriteTableFragment* req,
    const TSignedWriteFragmentCookiePtr& cookie,
    const TTableFragmentWriterOptions& options)
{
    req->set_signed_cookie(ToProto(ConvertToYsonString(cookie)));

    if (options.Config) {
        req->set_config(ToProto(ConvertToYsonString(*options.Config)));
    }
}

void ParseRequest(
    TSignedWriteFragmentCookiePtr* mutableCookie,
    TTableFragmentWriterOptions* mutableOptions,
    const TReqWriteTableFragment& req)
{
    *mutableCookie = ConvertTo<TSignedWriteFragmentCookiePtr>(TYsonString(req.signed_cookie()));
    if (req.has_config()) {
        mutableOptions->Config = ConvertTo<TTableWriterConfigPtr>(TYsonString(req.config()));
    } else {
        mutableOptions->Config = ConvertTo<TTableWriterConfigPtr>(TYsonString(TStringBuf("{}")));
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

bool IsDynamicTableRetriableError(const TError& error)
{
    return
        error.FindMatching(NTabletClient::EErrorCode::RowIsBlocked) ||
        error.FindMatching(NTabletClient::EErrorCode::BlockedRowWaitTimeout) ||
        error.FindMatching(NTabletClient::EErrorCode::NoSuchCell) ||
        error.FindMatching(NTabletClient::EErrorCode::ChunkIsNotPreloaded) ||
        error.FindMatching(NTabletClient::EErrorCode::NoInSyncReplicas) ||
        error.FindMatching(NTabletClient::EErrorCode::TabletNotMounted) ||
        error.FindMatching(NTabletClient::EErrorCode::NoSuchTablet) ||
        error.FindMatching(NTabletClient::EErrorCode::TabletReplicationEraMismatch) ||
        error.FindMatching(NTableClient::EErrorCode::UnableToSynchronizeReplicationCard);
}

bool IsRetriableError(const TError& error, bool retryProxyBanned)
{
    if (error.FindMatching(NRpcProxy::EErrorCode::ProxyBanned) ||
        error.FindMatching(NRpc::EErrorCode::PeerBanned))
    {
        return retryProxyBanned;
    }

    return
        NRpc::IsRetriableError(error) ||
        error.FindMatching(NRpc::EErrorCode::RequestQueueSizeLimitExceeded) ||
        error.FindMatching(NRpc::EErrorCode::TransportError) ||
        error.FindMatching(NRpc::EErrorCode::Unavailable) ||
        error.FindMatching(NRpc::EErrorCode::TransientFailure) ||
        error.FindMatching(NSecurityClient::EErrorCode::RequestQueueSizeLimitExceeded) ||
        IsDynamicTableRetriableError(error);
}


////////////////////////////////////////////////////////////////////////////////
// ROWSETS
////////////////////////////////////////////////////////////////////////////////

template <class TRow>
struct TRowsetTraits;

template <>
struct TRowsetTraits<TUnversionedRow>
{
    static constexpr NProto::ERowsetKind Kind = NProto::RK_UNVERSIONED;
};

template <>
struct TRowsetTraits<TVersionedRow>
{
    static constexpr NProto::ERowsetKind Kind = NProto::RK_VERSIONED;
};

void ValidateRowsetDescriptor(
    const NProto::TRowsetDescriptor& descriptor,
    int expectedVersion,
    NProto::ERowsetKind expectedKind,
    NProto::ERowsetFormat expectedFormat)
{
    if (descriptor.wire_format_version() != expectedVersion) {
        THROW_ERROR_EXCEPTION(
            "Incompatible rowset wire format version: expected %v, got %v",
            expectedVersion,
            descriptor.wire_format_version());
    }

    if (descriptor.rowset_kind() != expectedKind) {
        THROW_ERROR_EXCEPTION(
            "Incompatible rowset kind: expected %Qv, got %Qv",
            NProto::ERowsetKind_Name(expectedKind),
            NProto::ERowsetKind_Name(descriptor.rowset_kind()));
    }
    if (descriptor.rowset_format() != expectedFormat) {
        THROW_ERROR_EXCEPTION(
            "Incompatible rowset format: expected %Qv, got %Qv",
            NProto::ERowsetFormat_Name(expectedFormat),
            NProto::ERowsetFormat_Name(descriptor.rowset_format()));
    }
}

std::vector<TSharedRef> SerializeRowset(
    const NTableClient::TNameTablePtr& nameTable,
    TRange<NTableClient::TUnversionedRow> rows,
    NProto::TRowsetDescriptor* descriptor)
{
    descriptor->Clear();
    descriptor->set_wire_format_version(NApi::NRpcProxy::CurrentWireFormatVersion);
    descriptor->set_rowset_kind(NProto::RK_UNVERSIONED);
    for (int id = 0; id < nameTable->GetSize(); ++id) {
        auto* entry = descriptor->add_name_table_entries();
        entry->set_name(TString(nameTable->GetName(id)));
    }

    auto writer = CreateWireProtocolWriter();
    writer->WriteUnversionedRowset(rows);
    return writer->Finish();
}

template <class TRow>
std::vector<TSharedRef> SerializeRowset(
    const TTableSchema& schema,
    TRange<TRow> rows,
    NProto::TRowsetDescriptor* descriptor)
{
    descriptor->Clear();
    descriptor->set_wire_format_version(NApi::NRpcProxy::CurrentWireFormatVersion);
    descriptor->set_rowset_kind(TRowsetTraits<TRow>::Kind);
    ToProto(descriptor->mutable_schema(), schema);

    // COMPAT(babenko)
    for (const auto& column : schema.Columns()) {
        auto* entry = descriptor->add_name_table_entries();
        entry->set_name(ToProto(column.Name()));
        // we save physical type for backward compatibility
        // COMPAT(babenko)
        entry->set_type(ToProto(column.GetWireType()));
        // COMPAT(babenko)
        entry->set_logical_type(ToProto(column.CastToV1Type()));
    }

    auto writer = CreateWireProtocolWriter();
    writer->WriteRowset(rows);
    return writer->Finish();
}

// Instantiate templates.
template std::vector<TSharedRef> SerializeRowset(
    const TTableSchema& schema,
    TRange<TUnversionedRow> rows,
    NProto::TRowsetDescriptor* descriptor);
template std::vector<TSharedRef> SerializeRowset(
    const TTableSchema& schema,
    TRange<TVersionedRow> rows,
    NProto::TRowsetDescriptor* descriptor);

TTableSchemaPtr DeserializeRowsetSchema(
    const NProto::TRowsetDescriptor& descriptor)
{
    if (descriptor.has_schema()) {
        return FromProto<TTableSchemaPtr>(descriptor.schema());
    }

    // COMPAT(babenko)
    std::vector<TColumnSchema> columns;
    columns.resize(descriptor.name_table_entries_size());
    for (int i = 0; i < descriptor.name_table_entries_size(); ++i) {
        const auto& entry = descriptor.name_table_entries(i);
        if (entry.has_name()) {
            columns[i].SetName(entry.name());
            columns[i].SetStableName(TColumnStableName(entry.name()));
        }
        if (entry.has_logical_type()) {
            auto simpleLogicalType = FromProto<NTableClient::ESimpleLogicalValueType>(entry.logical_type());
            columns[i].SetLogicalType(OptionalLogicalType(SimpleLogicalType(simpleLogicalType)));
        } else if (entry.has_type()) {
            auto simpleLogicalType = FromProto<NTableClient::ESimpleLogicalValueType>(entry.type());
            columns[i].SetLogicalType(OptionalLogicalType(SimpleLogicalType(simpleLogicalType)));
        }
    }

    auto schema = New<TTableSchema>(std::move(columns));
    ValidateColumnUniqueness(*schema);
    return schema;
}

namespace {

template <class TRow>
auto ReadRows(IWireProtocolReader* reader, const TTableSchema& schema);

template <>
auto ReadRows<TUnversionedRow>(IWireProtocolReader* reader, const TTableSchema& /*schema*/)
{
    return reader->ReadUnversionedRowset(true);
}

template <>
auto ReadRows<TVersionedRow>(IWireProtocolReader* reader, const TTableSchema& schema)
{
    auto schemaData = IWireProtocolReader::GetSchemaData(schema, TColumnFilter());
    return reader->ReadVersionedRowset(schemaData, true);
}

} // namespace

template <class TRow>
TIntrusivePtr<NApi::IRowset<TRow>> DeserializeRowset(
    const NProto::TRowsetDescriptor& descriptor,
    const TSharedRef& data,
    NTableClient::TRowBufferPtr rowBuffer)
{
    if (descriptor.rowset_format() != NApi::NRpcProxy::NProto::RF_YT_WIRE) {
        THROW_ERROR_EXCEPTION("Unsupported rowset format %Qv",
            NApi::NRpcProxy::NProto::ERowsetFormat_Name(descriptor.rowset_format()));
    }

    ValidateRowsetDescriptor(
        descriptor,
        NApi::NRpcProxy::CurrentWireFormatVersion,
        TRowsetTraits<TRow>::Kind,
        NApi::NRpcProxy::NProto::RF_YT_WIRE);

    if (!rowBuffer) {
        struct TDeserializedRowsetTag { };
        rowBuffer = New<TRowBuffer>(TDeserializedRowsetTag());
    }

    auto reader = CreateWireProtocolReader(data, std::move(rowBuffer));

    auto schema = DeserializeRowsetSchema(descriptor);
    auto rows = ReadRows<TRow>(reader.get(), *schema);
    return NApi::CreateRowset(std::move(schema), std::move(rows));
}

// Instantiate templates.
template NApi::IUnversionedRowsetPtr DeserializeRowset(
    const NProto::TRowsetDescriptor& descriptor,
    const TSharedRef& data,
    NTableClient::TRowBufferPtr buffer = nullptr);

template NApi::IVersionedRowsetPtr DeserializeRowset(
    const NProto::TRowsetDescriptor& descriptor,
    const TSharedRef& data,
    NTableClient::TRowBufferPtr buffer = nullptr);

////////////////////////////////////////////////////////////////////////////////

std::vector<TSharedRef> SerializeRowset(
    const NTableClient::TTableSchema& schema,
    TRange<TTypeErasedRow> rows,
    NProto::TRowsetDescriptor* descriptor,
    bool versioned)
{
    if (versioned) {
        return SerializeRowset(
            schema,
            ReinterpretCastRange<TVersionedRow>(rows),
            descriptor);
    } else {
        return SerializeRowset(
            schema,
            ReinterpretCastRange<TUnversionedRow>(rows),
            descriptor);
    }
}

TIntrusivePtr<NApi::IRowset<TTypeErasedRow>> DeserializeRowset(
    const NProto::TRowsetDescriptor& descriptor,
    const TSharedRef& data,
    bool versioned)
{
    if (versioned) {
        auto rowset = DeserializeRowset<TVersionedRow>(descriptor, data);
        // TODO(savrus): Get refcounted schema from rowset.
        auto schema = DeserializeRowsetSchema(descriptor);
        return CreateRowset(
            std::move(schema),
            ReinterpretCastRange<TTypeErasedRow>(rowset->GetRows()));
    } else {
        auto rowset = DeserializeRowset<TUnversionedRow>(descriptor, data);
        // TODO(savrus): Get refcounted schema from rowset.
        auto schema = DeserializeRowsetSchema(descriptor);
        return CreateRowset(
            std::move(schema),
            ReinterpretCastRange<TTypeErasedRow>(rowset->GetRows()));
    }
}

////////////////////////////////////////////////////////////////////////////////

void SortByRegexes(std::vector<TString>& values, const std::vector<NRe2::TRe2Ptr>& regexes)
{
    auto valueToRank = [&] (const TString& value) -> size_t {
        for (size_t index = 0; index < regexes.size(); ++index) {
            if (NRe2::TRe2::FullMatch(NRe2::StringPiece(value), *regexes[index])) {
                return index;
            }
        }
        return regexes.size();
    };
    std::stable_sort(values.begin(), values.end(), [&] (const auto& lhs, const auto& rhs) {
        return valueToRank(lhs) < valueToRank(rhs);
    });
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NRpcProxy
