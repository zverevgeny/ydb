#pragma once

#include <library/cpp/lwtrace/shuttle.h>
#include <ydb/core/kqp/common/kqp_batch_operations.h>
#include <ydb/core/kqp/common/kqp_tx.h>
#include <ydb/core/kqp/common/kqp_event_ids.h>
#include <ydb/core/kqp/common/kqp_user_request_context.h>
#include <ydb/core/kqp/executer_actor/kqp_partition_helper.h>
#include <ydb/core/kqp/executer_actor/shards_resolver/kqp_shards_resolver_events.h>
#include <ydb/core/kqp/query_data/kqp_query_data.h>
#include <ydb/core/kqp/gateway/kqp_gateway.h>
#include <ydb/core/kqp/counters/kqp_counters.h>
#include <ydb/core/tx/long_tx_service/public/lock_handle.h>
#include <ydb/library/yql/dq/actors/compute/dq_compute_actor_async_io_factory.h>
#include <ydb/core/protos/table_service_config.pb.h>

namespace NKikimr {
namespace NKqp {

struct TEvKqpExecuter {
    struct TEvTxRequest : public TEventPB<TEvTxRequest, NKikimrKqp::TEvExecuterTxRequest,
        TKqpExecuterEvents::EvTxRequest> {};

    struct TEvTxResponse : public TEventLocal<TEvTxResponse, TKqpExecuterEvents::EvTxResponse> {
        NKikimrKqp::TEvExecuterTxResponse Record;
        TTxAllocatorState::TPtr AllocState;
        NLongTxService::TLockHandle LockHandle;
        TVector<TKqpPhyTxHolder::TConstPtr> TxHolders;
        TVector<TKqpExecuterTxResult> TxResults;

        NLWTrace::TOrbit Orbit;
        IKqpGateway::TKqpSnapshot Snapshot;
        std::optional<NYql::TKikimrPathId> BrokenLockPathId;
        std::optional<ui64> BrokenLockShardId;

        ui64 ResultRowsCount = 0;
        ui64 ResultRowsBytes = 0;

        THashSet<ui32> ParticipantNodes;

        // For BATCH operations only
        TVector<TSerializedCellVec> BatchOperationMaxKeys;
        TVector<ui32> BatchOperationKeyIds;

        enum class EExecutionType {
            Data,
            Scan,
            Scheme,
            Literal,
        } ExecutionType;

        TEvTxResponse(TTxAllocatorState::TPtr allocState, EExecutionType type)
            : AllocState(std::move(allocState))
            , ExecutionType(type)
        {}

        ~TEvTxResponse();

        TVector<TKqpPhyTxHolder::TConstPtr>& GetTxHolders() { return TxHolders; }
        TVector<TKqpExecuterTxResult>& GetTxResults() { return TxResults; }
        void InitTxResult(const TKqpPhyTxHolder::TConstPtr& tx);
        void TakeResult(ui32 idx, NKikimr::NMiniKQL::TUnboxedValueVector&& rows);
        void TakeResult(ui32 idx, NYql::NDq::TDqSerializedBatch&& rows);

        ui64 GetResultRowsCount() const {
            return ResultRowsCount;
        }

        ui64 GetByteSize() {
            return Record.MutableResponse()->ByteSizeLong() + ResultRowsBytes;
        }

        size_t ResultsSize() const {
            return TxResults.size();
        }
    };

    struct TEvStreamData : public TEventPBWithArena<TEvStreamData, NKikimrKqp::TEvExecuterStreamData, TKqpExecuterEvents::EvStreamData> {
        using TBaseEv = TEventPBWithArena<TEvStreamData, NKikimrKqp::TEvExecuterStreamData, TKqpExecuterEvents::EvStreamData>;
        using TBaseEv::TEventPBBase;

        TEvStreamData() = default;
        explicit TEvStreamData(TIntrusivePtr<NActors::TProtoArenaHolder> arena)
            : TEventPBBase(std::move(arena))
        {}
    };

    struct TEvStreamDataAck : public TEventPB<TEvStreamDataAck, NKikimrKqp::TEvExecuterStreamDataAck,
        TKqpExecuterEvents::EvStreamDataAck>
    {
        friend class TEventPBBase;
        explicit TEvStreamDataAck(ui64 seqno, ui64 channelId)
        {
            Record.SetSeqNo(seqno);
            Record.SetChannelId(channelId);
        }

    private:
        // using a little hack to hide default empty constructor
        TEvStreamDataAck() = default;
    };

    // deprecated event, remove in the future releases.
    struct TEvExecuterProgress : public TEventPB<TEvExecuterProgress, NKikimrKqp::TEvExecuterProgress,
            TKqpExecuterEvents::EvProgress> {};

    struct TEvTableResolveStatus : public TEventLocal<TEvTableResolveStatus,
        TKqpExecuterEvents::EvTableResolveStatus>
    {
        Ydb::StatusIds::StatusCode Status = Ydb::StatusIds::SUCCESS;
        NYql::TIssues Issues;
        TDuration CpuTime;
    };

    struct TEvTxDelayedExecution : public TEventLocal<TEvTxDelayedExecution,
        TKqpExecuterEvents::EvDelayedExecution>
    {
        TEvTxDelayedExecution(size_t partitionIdx)
            : PartitionIdx(partitionIdx)
        {}

        size_t PartitionIdx;
    };
};

struct TKqpFederatedQuerySetup;

struct TExecuterMutableConfig : public TAtomicRefCount<TExecuterMutableConfig>{
    std::atomic<bool> EnableRowsDuplicationCheck = false;
    std::atomic<bool> EnableParallelPointReadConsolidation = false;
    std::atomic<bool> VerboseMemoryLimitException = false;

    void ApplyFromTableServiceConfig(const NKikimrConfig::TTableServiceConfig& tableServiceConfig) {
        EnableRowsDuplicationCheck.store(tableServiceConfig.GetEnableRowsDuplicationCheck());
        EnableParallelPointReadConsolidation.store(tableServiceConfig.GetEnableParallelPointReadConsolidation());
        VerboseMemoryLimitException.store(tableServiceConfig.GetResourceManager().GetVerboseMemoryLimitException());
    }
};

struct TExecuterConfig : TNonCopyable {
    TIntrusivePtr<TExecuterMutableConfig> MutableConfig;
    const NKikimrConfig::TTableServiceConfig& TableServiceConfig;

    TExecuterConfig( TIntrusivePtr<TExecuterMutableConfig> mutableConfig, const NKikimrConfig::TTableServiceConfig& tableServiceConfig)
        : MutableConfig(mutableConfig)
        , TableServiceConfig(tableServiceConfig)
    {}
};

IActor* CreateKqpExecuter(IKqpGateway::TExecPhysicalRequest&& request, const TString& database,
    const TIntrusiveConstPtr<NACLib::TUserToken>& userToken, TKqpRequestCounters::TPtr counters,
    const TExecuterConfig& executerConfig,
    NYql::NDq::IDqAsyncIoFactory::TPtr asyncIoFactory, TPreparedQueryHolder::TConstPtr preparedQuery, const TActorId& creator,
    const TIntrusivePtr<TUserRequestContext>& userRequestContext, ui32 statementResultIndex,
    const std::optional<TKqpFederatedQuerySetup>& federatedQuerySetup, const TGUCSettings::TPtr& GUCSettings,
    TPartitionPruner::TConfig partitionPrunerConfig, const TShardIdToTableInfoPtr& shardIdToTableInfo,
    const IKqpTransactionManagerPtr& txManager, const TActorId bufferActorId,
    TMaybe<NBatchOperations::TSettings> batchOperationSettings = Nothing());

IActor* CreateKqpSchemeExecuter(
    TKqpPhyTxHolder::TConstPtr phyTx, NKikimrKqp::EQueryType queryType, const TActorId& target,
    const TMaybe<TString>& requestType, const TString& database,
    TIntrusiveConstPtr<NACLib::TUserToken> userToken, const TString& clientAddress,
    bool temporary, TString SessionId, TIntrusivePtr<TUserRequestContext> ctx,
    const TActorId& kqpTempTablesAgentActor = TActorId());

std::unique_ptr<TEvKqpExecuter::TEvTxResponse> ExecuteLiteral(
    IKqpGateway::TExecPhysicalRequest&& request, TKqpRequestCounters::TPtr counters, TActorId owner, const TIntrusivePtr<TUserRequestContext>& userRequestContext);

} // namespace NKqp
} // namespace NKikimr
