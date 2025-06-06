#pragma once
#include "chunks.h"
#include "column_info.h"
#include "blob_info.h"
#include <ydb/core/formats/arrow/splitter/scheme_info.h>
#include <ydb/library/formats/arrow/splitter/stats.h>
#include <ydb/library/formats/arrow/splitter/similar_packer.h>
#include <ydb/core/tx/columnshard/counters/indexation.h>
#include <ydb/core/tx/columnshard/engines/scheme/column_features.h>
#include <ydb/core/tx/columnshard/engines/scheme/abstract_scheme.h>
#include <ydb/core/tx/columnshard/engines/scheme/index_info.h>

#include <contrib/libs/apache/arrow/cpp/src/arrow/record_batch.h>

namespace NKikimr::NOlap {

class TDefaultSchemaDetails: public NArrow::NSplitter::ISchemaDetailInfo {
private:
    ISnapshotSchema::TPtr Schema;
    std::shared_ptr<NArrow::NSplitter::TSerializationStats> Stats;

protected:
    virtual TColumnSaver DoGetColumnSaver(const ui32 columnId) const override {
        return Schema->GetColumnSaver(columnId);
    }
public:
    TDefaultSchemaDetails(ISnapshotSchema::TPtr schema, const std::shared_ptr<NArrow::NSplitter::TSerializationStats>& stats)
        : Schema(schema)
        , Stats(stats)
    {
        AFL_VERIFY(Stats);
    }
    virtual std::shared_ptr<arrow::Field> GetField(const ui32 columnId) const override {
        return Schema->GetFieldByColumnIdOptional(columnId);
    }

    virtual std::optional<NArrow::NSplitter::TColumnSerializationStat> GetColumnSerializationStats(const ui32 columnId) const override {
        auto stats = Stats->GetColumnInfo(columnId);
        if (stats && stats->GetRecordsCount() != 0) {
            return stats;
        }
        return std::nullopt;
    }
    virtual std::optional<NArrow::NSplitter::TBatchSerializationStat> GetBatchSerializationStats(
        const std::shared_ptr<arrow::RecordBatch>& rb) const override {
        return Stats->GetStatsForRecordBatch(rb);
    }
    virtual ui32 GetColumnId(const std::string& fieldName) const override {
        return Schema->GetColumnId(fieldName);
    }
};

class TGeneralSerializedSlice {
private:
    YDB_READONLY(ui32, RecordsCount, 0);
    YDB_READONLY(ui32, InternalSplitsCount, 0);

protected:
    std::vector<TSplittedEntity> Data;
    ui64 Size = 0;
    NArrow::NSplitter::ISchemaDetailInfo::TPtr Schema;
    std::shared_ptr<NColumnShard::TSplitterCounters> Counters;
    TGeneralSerializedSlice() = default;

    const TSplittedEntity& GetEntityDataVerified(const ui32 entityId) const {
        for (auto&& i : Data) {
            if (i.GetEntityId() == entityId) {
                return i;
            }
        }
        AFL_VERIFY(false)("id", entityId);
        return Data.front();
    }
    bool GroupBlobsImpl(const NSplitter::TGroupFeatures& features, std::vector<TSplittedBlob>& blobs);

public:

    THashMap<ui32, std::vector<std::shared_ptr<IPortionDataChunk>>> GetPortionChunksToHash() const {
        THashMap<ui32, std::vector<std::shared_ptr<IPortionDataChunk>>> result;
        for (auto&& i : Data) {
            AFL_VERIFY(result.emplace(i.GetEntityId(), i.GetChunks()).second);
        }
        return result;
    }

    std::map<ui32, std::vector<std::shared_ptr<IPortionDataChunk>>> GetPortionChunksToMap() const {
        std::map<ui32, std::vector<std::shared_ptr<IPortionDataChunk>>> result;
        for (auto&& i : Data) {
            AFL_VERIFY(result.emplace(i.GetEntityId(), i.GetChunks()).second);
        }
        return result;
    }

    std::shared_ptr<arrow::RecordBatch> GetFirstLastPKBatch(const std::shared_ptr<arrow::Schema>& pkSchema) const {
        std::vector<std::shared_ptr<arrow::Array>> pkColumns;
        for (auto&& i : pkSchema->fields()) {
            auto aBuilder = NArrow::MakeBuilder(i);
            const TSplittedEntity& splittedEntity = GetEntityDataVerified(Schema->GetColumnId(i->name()));
            NArrow::TStatusValidator::Validate(aBuilder->AppendScalar(*splittedEntity.GetFirstScalar()));
            NArrow::TStatusValidator::Validate(aBuilder->AppendScalar(*splittedEntity.GetLastScalar()));
            pkColumns.emplace_back(NArrow::TStatusValidator::GetValid(aBuilder->Finish()));
        }
        return arrow::RecordBatch::Make(pkSchema, 2, pkColumns);
    }

    ui64 GetPackedSize() const {
        ui64 result = 0;
        for (auto&& i : Data) {
            result += i.GetPackedSize();
        }
        AFL_VERIFY(Size == result)("size", Size)("result", result);
        return result;
    }

    ui64 GetSize() const {
        return Size;
    }

    std::vector<TSplittedBlob> GroupChunksByBlobs(const NSplitter::TEntityGroups& groups) {
        std::vector<TSplittedBlob> blobs;
        AFL_VERIFY(GroupBlobs(blobs, groups));
        return blobs;
    }

    explicit TGeneralSerializedSlice(NArrow::NSplitter::TVectorView<TGeneralSerializedSlice>&& objects) {
        Y_ABORT_UNLESS(objects.size());
        std::swap(*this, objects.front());
        for (ui32 i = 1; i < objects.size(); ++i) {
            MergeSlice(std::move(objects[i]));
        }
    }
    TGeneralSerializedSlice(const THashMap<ui32, std::vector<std::shared_ptr<IPortionDataChunk>>>& data,
        NArrow::NSplitter::ISchemaDetailInfo::TPtr schema, std::shared_ptr<NColumnShard::TSplitterCounters> counters);
    TGeneralSerializedSlice(
        const ui32 recordsCount, NArrow::NSplitter::ISchemaDetailInfo::TPtr schema, std::shared_ptr<NColumnShard::TSplitterCounters> counters);

    void MergeSlice(TGeneralSerializedSlice&& slice);

    bool GroupBlobs(std::vector<TSplittedBlob>& blobs, const NSplitter::TEntityGroups& groups);
};

}
