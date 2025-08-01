#pragma once

#include "sys_params.h"

#include <ydb/core/base/defs.h>
#include <ydb/core/scheme/scheme_pathid.h>

#include <util/datetime/base.h>
#include <util/generic/hash_set.h>
#include <util/generic/maybe.h>
#include <util/generic/ptr.h>

#include <memory>
#include <optional>

namespace NKikimrReplication {
    class TReplicationConfig;
}

namespace NKikimr::NReplication::NController {

class TReplication: public TSimpleRefCount<TReplication> {
public:
    using TPtr = TIntrusivePtr<TReplication>;

    enum class EState: ui8 {
        Ready,
        Done,
        Removing,
        Paused,
        Error = 255
    };

    enum class ETargetKind: ui8 {
        Table,
        IndexTable,
        Transfer,
    };

    enum class EDstState: ui8 {
        Creating,
        Ready,
        Alter,
        Done,
        Removing,
        Paused,
        Error = 255
    };

    enum class EStreamState: ui8 {
        Creating,
        Ready,
        Removing,
        Removed,
        Error = 255
    };

    class ITarget {
    public:
        struct IConfig {
            using TPtr = std::shared_ptr<const IConfig>;

            virtual ~IConfig() = default;

            virtual ETargetKind GetKind() const = 0;
            virtual const TString& GetSrcPath() const = 0;
            virtual const TString& GetDstPath() const = 0;
        };

        virtual ~ITarget() = default;

        virtual ui64 GetId() const = 0;
        virtual ETargetKind GetKind() const = 0;

        virtual const IConfig::TPtr& GetConfig() const = 0;
        virtual const TString& GetSrcPath() const = 0;
        virtual const TString& GetDstPath() const = 0;

        virtual EDstState GetDstState() const = 0;
        virtual void SetDstState(const EDstState value) = 0;

        virtual const TPathId& GetDstPathId() const = 0;
        virtual void SetDstPathId(const TPathId& value) = 0;

        virtual const TString& GetStreamName() const = 0;
        virtual void SetStreamName(const TString& value) = 0;
        virtual const TString& GetStreamConsumerName() const = 0;
        virtual void SetStreamConsumerName(const TString& value) = 0;
        virtual TString GetStreamPath() const = 0;

        virtual EStreamState GetStreamState() const = 0;
        virtual void SetStreamState(EStreamState value) = 0;

        virtual const TString& GetIssue() const = 0;
        virtual void SetIssue(const TString& value) = 0;

        virtual void AddWorker(ui64 id) = 0;
        virtual void RemoveWorker(ui64 id) = 0;
        virtual TVector<ui64> GetWorkers() const = 0;
        virtual void UpdateLag(ui64 workerId, TDuration lag) = 0;
        virtual const TMaybe<TDuration> GetLag() const = 0;

        virtual void Progress(const TActorContext& ctx) = 0;
        virtual void Shutdown(const TActorContext& ctx) = 0;

        virtual void UpdateConfig(const NKikimrReplication::TReplicationConfig&) = 0;

    protected:
        virtual IActor* CreateWorkerRegistar(const TActorContext& ctx) const = 0;
    };

    friend class TTargetBase;
    void AddPendingAlterTarget(ui64 id);
    void RemovePendingAlterTarget(ui64 id);
    void UpdateLag(ui64 targetId, TDuration lag);

    struct TDropOp {
        TActorId Sender;
        std::pair<ui64, ui32> OperationId; // txId, partId
    };

public:
    explicit TReplication(ui64 id, const TPathId& pathId, const NKikimrReplication::TReplicationConfig& config, const TString& database);
    explicit TReplication(ui64 id, const TPathId& pathId, NKikimrReplication::TReplicationConfig&& config, TString&& database);
    explicit TReplication(ui64 id, const TPathId& pathId, const TString& config, const TString& database);

    ui64 AddTarget(ETargetKind kind, const ITarget::IConfig::TPtr& config);
    ITarget* AddTarget(ui64 id, ETargetKind kind, const ITarget::IConfig::TPtr& config);
    const ITarget* FindTarget(ui64 id) const;
    ITarget* FindTarget(ui64 id);
    void RemoveTarget(ui64 id);
    const TVector<TString>& GetTargetTablePaths() const;

    void Progress(const TActorContext& ctx);
    void Shutdown(const TActorContext& ctx);

    ui64 GetId() const;
    const TPathId& GetPathId() const;
    const TActorId& GetYdbProxy() const;
    ui64 GetSchemeShardId() const;
    void SetConfig(NKikimrReplication::TReplicationConfig&& config);
    const NKikimrReplication::TReplicationConfig& GetConfig() const;
    const TString& GetDatabase() const;
    void SetState(EState state, TString issue = {});
    EState GetState() const;
    EState GetDesiredState() const;
    void SetDesiredState(EState state);
    const TString& GetIssue() const;
    const TMaybe<TDuration> GetLag() const;

    void SetNextTargetId(ui64 value);
    ui64 GetNextTargetId() const;

    void UpdateSecret(const TString& secretValue);

    void SetTenant(const TString& value);
    const TString& GetTenant() const;

    bool CheckAlterDone() const;

    void SetDropOp(const TActorId& sender, const std::pair<ui64, ui32>& opId);
    const std::optional<TDropOp>& GetDropOp() const;

private:
    class TImpl;
    std::shared_ptr<TImpl> Impl;
    std::optional<TDropOp> DropOp;

}; // TReplication

}
