#pragma once

#include <ydb/core/base/defs.h>
#include <ydb/core/base/events.h>

#include <util/datetime/base.h>
#include <util/generic/vector.h>

#include <functional>

namespace NKikimr::NReplication::NService {

struct TEvWorker {
    enum EEv {
        EvBegin = EventSpaceBegin(TKikimrEvents::ES_REPLICATION_WORKER),

        EvHandshake,
        EvPoll,
        EvData,
        EvGone,
        EvStatus,
        EvDataEnd,

        EvEnd,
    };

    static_assert(EvEnd < EventSpaceEnd(TKikimrEvents::ES_REPLICATION_WORKER));

    struct TEvHandshake: public TEventLocal<TEvHandshake, EvHandshake> {};
    struct TEvPoll: public TEventLocal<TEvPoll, EvPoll> {};

    struct TEvData: public TEventLocal<TEvData, EvData> {
        struct TRecord {
            ui64 Offset;
            TString Data;
            TInstant CreateTime;
            TString MessageGroupId;
            TString ProducerId;
            ui64 SeqNo;

            explicit TRecord(ui64 offset, const TString& data, TInstant createTime, const TString& messageGroupId, const TString& producerId, ui64 seqNo);
            explicit TRecord(ui64 offset, TString&& data, TInstant createTime, TString&& messageGroupId, TString&& producerId, ui64 seqNo);
            void Out(IOutputStream& out) const;
        };

        ui32 PartitionId;
        TString Source;
        TVector<TRecord> Records;

        explicit TEvData(ui32 partitionId, const TString& source, const TVector<TRecord>& records);
        explicit TEvData(ui32 partitionId, const TString& source, TVector<TRecord>&& records);
        TString ToString() const override;
    };

    struct TEvGone: public TEventLocal<TEvGone, EvGone> {
        enum EStatus {
            DONE,
            S3_ERROR,
            SCHEME_ERROR,
            UNAVAILABLE,
        };

        EStatus Status;
        TString ErrorDescription;

        explicit TEvGone(EStatus status, const TString& errorDescription = {});
        TString ToString() const override;
    };

    struct TEvStatus: public TEventLocal<TEvStatus, EvStatus> {
        TDuration Lag;

        explicit TEvStatus(TDuration lag);
        TString ToString() const override;
    };

    struct TEvDataEnd: public TEventLocal<TEvDataEnd, EvDataEnd> {
        ui64 PartitionId;
        TVector<ui64> AdjacentPartitionsIds;
        TVector<ui64> ChildPartitionsIds;

        TEvDataEnd(ui64 partitionId, TVector<ui64>&& adjacentPartitionsIds, TVector<ui64>&& childPartitionsIds);
        TString ToString() const override;
    };
};

IActor* CreateWorker(
    const TActorId& parent,
    std::function<IActor*(void)>&& createReaderFn,
    std::function<IActor*(void)>&& createWriterFn);

}

Y_DECLARE_OUT_SPEC(inline, NKikimr::NReplication::NService::TEvWorker::TEvData::TRecord, o, x) {
    return x.Out(o);
}
