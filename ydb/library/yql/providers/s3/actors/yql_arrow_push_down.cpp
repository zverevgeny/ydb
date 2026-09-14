#include "yql_arrow_push_down.h"

#include <util/generic/vector.h>
#include <ydb/library/yql/providers/generic/pushdown/yql_generic_match_predicate.h>
#include <yql/essentials/utils/log/log.h>
#include <parquet/schema.h>
#include <parquet/statistics.h>
#include <parquet/types.h>

#include <cstring>

namespace {

bool HasMinMax(const std::shared_ptr<parquet::Statistics>& statistics) {
    return statistics && statistics->HasMinMax();
}

TMaybe<NYql::NGenericPushDown::TTimestampColumnStatsData> GetDateStatistics(parquet::Type::type physicalType, std::shared_ptr<parquet::Statistics> statistics) {
    if (!HasMinMax(statistics)) {
        return {};
    }
    switch (physicalType) {
        case parquet::Type::type::INT32: {
            const parquet::TypedStatistics<arrow::Int32Type>* typedStatistics = static_cast<const parquet::TypedStatistics<arrow::Int32Type>*>(statistics.get());
            int64_t minValue = typedStatistics->min();
            int64_t maxValue = typedStatistics->max();
            NYql::NGenericPushDown::TTimestampColumnStatsData stats;
            stats.lowValue = TInstant::Days(minValue);
            stats.highValue = TInstant::Days(maxValue);
            return stats;
        }
        case parquet::Type::type::INT64: {
            const parquet::TypedStatistics<arrow::Int64Type>* typedStatistics = static_cast<const parquet::TypedStatistics<arrow::Int64Type>*>(statistics.get());
            int64_t minValue = typedStatistics->min();
            int64_t maxValue = typedStatistics->max();
            NYql::NGenericPushDown::TTimestampColumnStatsData stats;
            stats.lowValue = TInstant::Days(minValue);
            stats.highValue = TInstant::Days(maxValue);
            return stats;
        }
        case parquet::Type::type::BOOLEAN:
        case parquet::Type::type::INT96:
        case parquet::Type::type::FLOAT:
        case parquet::Type::type::DOUBLE:
        case parquet::Type::type::BYTE_ARRAY:
        case parquet::Type::type::FIXED_LEN_BYTE_ARRAY:
        case parquet::Type::type::UNDEFINED:
        return {};
    }
    return {};
}

TMaybe<NYql::NGenericPushDown::TTimestampColumnStatsData> GetTimestampStatistics(const parquet::TimestampLogicalType* typestampLogicalType, parquet::Type::type physicalType, std::shared_ptr<parquet::Statistics> statistics) {
    if (!HasMinMax(statistics)) {
        return {};
    }
    int64_t multiplier = 1;
    switch (typestampLogicalType->time_unit()) {
        case parquet::LogicalType::TimeUnit::unit::UNKNOWN:
        case parquet::LogicalType::TimeUnit::unit::NANOS:
            return {};
        case parquet::LogicalType::TimeUnit::unit::MILLIS:
            multiplier *= 1000;
        break;
        case parquet::LogicalType::TimeUnit::unit::MICROS:
        break;
    }
    switch (physicalType) {
        case parquet::Type::type::INT32: {
            const parquet::TypedStatistics<arrow::Int32Type>* typedStatistics = static_cast<const parquet::TypedStatistics<arrow::Int32Type>*>(statistics.get());
            int64_t minValue = typedStatistics->min();
            int64_t maxValue = typedStatistics->max();
            NYql::NGenericPushDown::TTimestampColumnStatsData stats;
            stats.lowValue = TInstant::FromValue(minValue * multiplier);
            stats.highValue = TInstant::FromValue(maxValue * multiplier);
            return stats;
        }
        case parquet::Type::type::INT64: {
            const parquet::TypedStatistics<arrow::Int64Type>* typedStatistics = static_cast<const parquet::TypedStatistics<arrow::Int64Type>*>(statistics.get());
            int64_t minValue = typedStatistics->min();
            int64_t maxValue = typedStatistics->max();
            NYql::NGenericPushDown::TTimestampColumnStatsData stats;
            stats.lowValue = TInstant::FromValue(minValue * multiplier);
            stats.highValue = TInstant::FromValue(maxValue * multiplier);
            return stats;
        }
        case parquet::Type::type::BOOLEAN:
        case parquet::Type::type::INT96:
        case parquet::Type::type::FLOAT:
        case parquet::Type::type::DOUBLE:
        case parquet::Type::type::BYTE_ARRAY:
        case parquet::Type::type::FIXED_LEN_BYTE_ARRAY:
        case parquet::Type::type::UNDEFINED:
        return {};
    }
    return {};
}

NYql::NGenericPushDown::TColumnStatistics MakeTimestampStatistics(const TString& name, ::Ydb::Type::PrimitiveTypeId type, const TMaybe<NYql::NGenericPushDown::TTimestampColumnStatsData>& statistics) {
    NYql::NGenericPushDown::TColumnStatistics columnStatistics;
    columnStatistics.ColumnName = name;
    columnStatistics.ColumnType.set_type_id(type);
    columnStatistics.Timestamp = statistics;
    return columnStatistics;
}

TMaybe<NYql::NGenericPushDown::TColumnStatistics> MakeLongStatistics(const TString& name, ::Ydb::Type::PrimitiveTypeId type, i64 minValue, i64 maxValue) {
    NYql::NGenericPushDown::TColumnStatistics columnStatistics;
    columnStatistics.ColumnName = name;
    columnStatistics.ColumnType.set_type_id(type);
    columnStatistics.LongStats.ConstructInPlace();
    columnStatistics.LongStats->lowValue = minValue;
    columnStatistics.LongStats->highValue = maxValue;
    return columnStatistics;
}

TMaybe<NYql::NGenericPushDown::TColumnStatistics> MakeStatistics(const parquet::ColumnDescriptor* column, const parquet::ColumnChunkMetaData* chunk) {
    if (!chunk->is_stats_set()) {
        return {};
    }
    auto statistics = chunk->statistics();
    if (!HasMinMax(statistics)) {
        return {};
    }

    const TString columnName{column->name()};
    std::shared_ptr<const parquet::LogicalType> logicalType = column->logical_type();
    parquet::Type::type physicalType = column->physical_type();

    switch (logicalType->type()) {
        case parquet::LogicalType::Type::type::DATE: {
            auto dateStatistics = GetDateStatistics(physicalType, statistics);
            if (dateStatistics) {
                return MakeTimestampStatistics(columnName, ::Ydb::Type::DATE, dateStatistics);
            }
            return {};
        }
        case parquet::LogicalType::Type::type::TIMESTAMP: {
            const parquet::TimestampLogicalType* typestampLogicalType = static_cast<const parquet::TimestampLogicalType*>(logicalType.get());
            auto timestampStatistics = GetTimestampStatistics(typestampLogicalType, physicalType, statistics);
            if (timestampStatistics) {
                return MakeTimestampStatistics(columnName, ::Ydb::Type::TIMESTAMP, timestampStatistics);
            }
            return {};
        }
        case parquet::LogicalType::Type::type::UUID: {
            if (physicalType != parquet::Type::type::FIXED_LEN_BYTE_ARRAY || column->type_length() != 16) {
                return {};
            }
            const auto* typedStatistics = static_cast<const parquet::FLBAStatistics*>(statistics.get());
            NYql::NGenericPushDown::TColumnStatistics columnStatistics;
            columnStatistics.ColumnName = columnName;
            columnStatistics.ColumnType.set_type_id(::Ydb::Type::UUID);
            columnStatistics.UuidStats.ConstructInPlace();
            columnStatistics.UuidStats->lowValue = TString(reinterpret_cast<const char*>(typedStatistics->min().ptr), 16);
            columnStatistics.UuidStats->highValue = TString(reinterpret_cast<const char*>(typedStatistics->max().ptr), 16);
            return columnStatistics;
        }
        default:
            break;
    }

    if (column->sort_order() == parquet::SortOrder::SIGNED) {
        switch (physicalType) {
            case parquet::Type::type::INT32: {
                const auto* typedStatistics = static_cast<const parquet::Int32Statistics*>(statistics.get());
                return MakeLongStatistics(columnName, ::Ydb::Type::INT32, typedStatistics->min(), typedStatistics->max());
            }
            case parquet::Type::type::INT64: {
                const auto* typedStatistics = static_cast<const parquet::Int64Statistics*>(statistics.get());
                return MakeLongStatistics(columnName, ::Ydb::Type::INT64, typedStatistics->min(), typedStatistics->max());
            }
            default:
                break;
        }
    }

    // FLBA(16) without UUID logical type: treat as UUID (pyarrow 5 compatibility).
    // FLBA has SortOrder::UNSIGNED by default, so this check is independent of sort_order.
    if (physicalType == parquet::Type::type::FIXED_LEN_BYTE_ARRAY
        && logicalType->type() == parquet::LogicalType::Type::type::NONE
        && column->type_length() == 16) {
        YQL_LOG(DEBUG) << "Treating FLBA(16) without UUID logical type as UUID: column=" << columnName;
        const auto* typedStatistics = static_cast<const parquet::FLBAStatistics*>(statistics.get());
        NYql::NGenericPushDown::TColumnStatistics columnStatistics;
        columnStatistics.ColumnName = columnName;
        columnStatistics.ColumnType.set_type_id(::Ydb::Type::UUID);
        columnStatistics.UuidStats.ConstructInPlace();
        columnStatistics.UuidStats->lowValue = TString(reinterpret_cast<const char*>(typedStatistics->min().ptr), 16);
        columnStatistics.UuidStats->highValue = TString(reinterpret_cast<const char*>(typedStatistics->max().ptr), 16);
        return columnStatistics;
    }

    // FLOAT/DOUBLE/BOOL: sort_order does not affect min/max semantics,
    // but we still require SIGNED for consistency with INT handling.
    if (column->sort_order() == parquet::SortOrder::SIGNED) {
        switch (physicalType) {
            case parquet::Type::type::FLOAT: {
                const auto* typedStatistics = static_cast<const parquet::FloatStatistics*>(statistics.get());
                NYql::NGenericPushDown::TColumnStatistics columnStatistics;
                columnStatistics.ColumnName = columnName;
                columnStatistics.ColumnType.set_type_id(::Ydb::Type::FLOAT);
                columnStatistics.DoubleStats.ConstructInPlace();
                columnStatistics.DoubleStats->lowValue = typedStatistics->min();
                columnStatistics.DoubleStats->highValue = typedStatistics->max();
                return columnStatistics;
            }
            case parquet::Type::type::DOUBLE: {
                const auto* typedStatistics = static_cast<const parquet::DoubleStatistics*>(statistics.get());
                NYql::NGenericPushDown::TColumnStatistics columnStatistics;
                columnStatistics.ColumnName = columnName;
                columnStatistics.ColumnType.set_type_id(::Ydb::Type::DOUBLE);
                columnStatistics.DoubleStats.ConstructInPlace();
                columnStatistics.DoubleStats->lowValue = typedStatistics->min();
                columnStatistics.DoubleStats->highValue = typedStatistics->max();
                return columnStatistics;
            }
            case parquet::Type::type::BOOLEAN: {
                const auto* typedStatistics = static_cast<const parquet::BoolStatistics*>(statistics.get());
                const bool minValue = typedStatistics->min();
                const bool maxValue = typedStatistics->max();
                NYql::NGenericPushDown::TColumnStatistics columnStatistics;
                columnStatistics.ColumnName = columnName;
                columnStatistics.ColumnType.set_type_id(::Ydb::Type::BOOL);
                columnStatistics.BooleanStats.ConstructInPlace();
                columnStatistics.BooleanStats->numTrues = (minValue || maxValue) ? 1 : 0;
                columnStatistics.BooleanStats->numFalses = (!minValue || !maxValue) ? 1 : 0;
                return columnStatistics;
            }
            default:
                return {};
        }
    }
    return {};
}

bool MatchRowGroup(std::unique_ptr<parquet::RowGroupMetaData> rowGroupMetadata, const NYql::NConnector::NApi::TPredicate& predicate) {
    TMap<TString, NYql::NGenericPushDown::TColumnStatistics> columns;
    for (int i = 0; i < rowGroupMetadata->schema()->num_columns(); i++) {
        auto columnChunkMetadata = rowGroupMetadata->ColumnChunk(i);
        auto column = rowGroupMetadata->schema()->Column(i);
        if (auto statistics = MakeStatistics(column, columnChunkMetadata.get())) {
            columns[statistics->ColumnName] = std::move(*statistics);
        }
    }
    return NYql::NGenericPushDown::MatchPredicate(columns, predicate);
}

TVector<ui64> MatchedRowGroupsImpl(parquet::FileMetaData* fileMetadata, const NYql::NConnector::NApi::TPredicate& predicate) {
    TVector<ui64> matchedRowGroups;
    matchedRowGroups.reserve(fileMetadata->num_row_groups());
    for (int i = 0; i < fileMetadata->num_row_groups(); i++) {
        if (MatchRowGroup(fileMetadata->RowGroup(i), predicate)) {
            matchedRowGroups.push_back(i);
        }
    }
    return matchedRowGroups;
}

}

namespace NYql::NDq {

TVector<ui64> MatchedRowGroups(std::shared_ptr<parquet::FileMetaData> fileMetadata, const NYql::NConnector::NApi::TPredicate& predicate) {
    return MatchedRowGroupsImpl(fileMetadata.get(), predicate);
}

TVector<ui64> MatchedRowGroups(const std::unique_ptr<parquet::FileMetaData>& fileMetadata, const NYql::NConnector::NApi::TPredicate& predicate) {
    return MatchedRowGroupsImpl(fileMetadata.get(), predicate);
}


} // namespace NYql::NDq
