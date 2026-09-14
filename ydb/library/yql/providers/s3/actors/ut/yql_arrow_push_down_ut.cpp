#include <ydb/library/yql/providers/s3/actors/yql_arrow_push_down.h>

#include <library/cpp/testing/unittest/registar.h>

#include <arrow/array.h>
#include <arrow/array/array_primitive.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/io/memory.h>
#include <arrow/table.h>
#include <contrib/libs/apache/arrow/cpp/src/parquet/arrow/reader.h>
#include <contrib/libs/apache/arrow/cpp/src/parquet/arrow/schema.h>
#include <contrib/libs/apache/arrow/cpp/src/parquet/arrow/writer.h>
#include <contrib/libs/apache/arrow/cpp/src/parquet/schema.h>
#include <contrib/libs/apache/arrow/cpp/src/parquet/statistics.h>

#include <google/protobuf/text_format.h>

#include <util/string/builder.h>

#include <cstring>
#include <cstdint>

namespace NYql::NPathGenerator {

struct TFileMetaDataBuilder {
    struct TRowGroupBuilder {
        TRowGroupBuilder(TFileMetaDataBuilder* parent,
                         std::shared_ptr<parquet::SchemaDescriptor> schema,
                         parquet::RowGroupMetaDataBuilder* rowGroup)
            : Parent(parent)
            , Schema(schema)
            , RowGroup(rowGroup)
        {}

        TRowGroupBuilder& AddColumnTimestampStatistics(int64_t columnId, const int64_t min, const int64_t max) {
            auto columnChunk = RowGroup->NextColumnChunk();
            auto stat = parquet::MakeStatistics<parquet::Int64Type>(Schema->Column(columnId));
            stat->SetMinMax(min, max);
            columnChunk->SetStatistics(stat->Encode());
            return *this;
        }

        TRowGroupBuilder& AddColumnWithoutMinMax(int64_t columnId) {
            auto columnChunk = RowGroup->NextColumnChunk();
            auto stat = parquet::MakeStatistics<parquet::Int64Type>(Schema->Column(columnId));
            columnChunk->SetStatistics(stat->Encode());
            return *this;
        }

        TRowGroupBuilder& AddColumnInt64Statistics(int64_t columnId, int64_t min, int64_t max) {
            auto columnChunk = RowGroup->NextColumnChunk();
            auto stat = parquet::MakeStatistics<parquet::Int64Type>(Schema->Column(columnId));
            stat->SetMinMax(min, max);
            columnChunk->SetStatistics(stat->Encode());
            return *this;
        }

        TRowGroupBuilder& AddColumnInt32Statistics(int64_t columnId, int32_t min, int32_t max) {
            auto columnChunk = RowGroup->NextColumnChunk();
            auto stat = parquet::MakeStatistics<parquet::Int32Type>(Schema->Column(columnId));
            stat->SetMinMax(min, max);
            columnChunk->SetStatistics(stat->Encode());
            return *this;
        }

        TRowGroupBuilder& AddColumnDoubleStatistics(int64_t columnId, double min, double max) {
            auto columnChunk = RowGroup->NextColumnChunk();
            auto stat = parquet::MakeStatistics<parquet::DoubleType>(Schema->Column(columnId));
            stat->SetMinMax(min, max);
            columnChunk->SetStatistics(stat->Encode());
            return *this;
        }

        TRowGroupBuilder& AddColumnFloatStatistics(int64_t columnId, float min, float max) {
            auto columnChunk = RowGroup->NextColumnChunk();
            auto stat = parquet::MakeStatistics<parquet::FloatType>(Schema->Column(columnId));
            stat->SetMinMax(min, max);
            columnChunk->SetStatistics(stat->Encode());
            return *this;
        }

        TRowGroupBuilder& AddColumnBoolStatistics(int64_t columnId, bool min, bool max) {
            auto columnChunk = RowGroup->NextColumnChunk();
            auto stat = parquet::MakeStatistics<parquet::BooleanType>(Schema->Column(columnId));
            stat->SetMinMax(min, max);
            columnChunk->SetStatistics(stat->Encode());
            return *this;
        }

        TRowGroupBuilder& AddColumnFlbaStatistics(int64_t columnId, TString min, TString max) {
            auto columnChunk = RowGroup->NextColumnChunk();
            auto stat = parquet::MakeStatistics<parquet::FLBAType>(Schema->Column(columnId));
            parquet::FixedLenByteArray minFlba(reinterpret_cast<const uint8_t*>(min.data()));
            parquet::FixedLenByteArray maxFlba(reinterpret_cast<const uint8_t*>(max.data()));
            stat->SetMinMax(minFlba, maxFlba);
            columnChunk->SetStatistics(stat->Encode());
            return *this;
        }

        TFileMetaDataBuilder& Build() {
            return *Parent;
        }

    private:
        TFileMetaDataBuilder* Parent;
        std::shared_ptr<parquet::SchemaDescriptor> Schema;
        parquet::RowGroupMetaDataBuilder* RowGroup;
    };

    TFileMetaDataBuilder(const TVector<std::shared_ptr<arrow::Field>>& columns) {
        auto schema = arrow::schema(columns);
        parquet::WriterProperties::Builder builder;
        auto properties = builder.build();

        UNIT_ASSERT(parquet::arrow::ToParquetSchema(schema.get(), *properties, &Schema) == ::arrow::Status::OK());

       FileMetadata = parquet::FileMetaDataBuilder::Make(Schema.get(), properties);
    }

    explicit TFileMetaDataBuilder(std::shared_ptr<parquet::SchemaDescriptor> schema)
        : Schema(std::move(schema))
    {
        parquet::WriterProperties::Builder builder;
        auto properties = builder.build();
        FileMetadata = parquet::FileMetaDataBuilder::Make(Schema.get(), properties);
    }

    TRowGroupBuilder AddRowGroup() {
        return TRowGroupBuilder(this, Schema, FileMetadata->AppendRowGroup());
    }

    std::unique_ptr<parquet::FileMetaData> Build() {
        return FileMetadata->Finish();
    }

private:
    std::unique_ptr<parquet::FileMetaDataBuilder> FileMetadata;
    std::shared_ptr<parquet::SchemaDescriptor> Schema;
};

std::shared_ptr<parquet::SchemaDescriptor> MakeLogicalUuidSchema(const TString& name) {
    auto node = parquet::schema::PrimitiveNode::Make(
        name,
        parquet::Repetition::REQUIRED,
        parquet::LogicalType::UUID(),
        parquet::Type::FIXED_LEN_BYTE_ARRAY,
        16);
    auto group = parquet::schema::GroupNode::Make(
        "schema", parquet::Repetition::REQUIRED, {node});
    auto schema = std::make_shared<parquet::SchemaDescriptor>();
    schema->Init(group);
    return schema;
}

NYql::NConnector::NApi::TPredicate BuildPredicate(const TString& text) {
    NYql::NConnector::NApi::TPredicate predicate;
    UNIT_ASSERT(google::protobuf::TextFormat::ParseFromString(text, &predicate));
    return predicate;
}

TString UuidValueField(const TString& bytes) {
    ui64 low = 0;
    ui64 high = 0;
    memcpy(&low, bytes.data(), sizeof(ui64));
    memcpy(&high, bytes.data() + sizeof(ui64), sizeof(ui64));
    return TStringBuilder() << "low_128: " << low << " high_128: " << high;
}

Y_UNIT_TEST_SUITE(TArrowPushDown) {
    Y_UNIT_TEST(SimplePushDown) {
        TFileMetaDataBuilder builder{{
            arrow::field("field1", arrow::timestamp(arrow::TimeUnit::type::MILLI)),
            arrow::field("field2", arrow::int64()),
            arrow::field("field3", arrow::float64())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnTimestampStatistics(0, TInstant::ParseIso8601("2024-03-01T00:00:00Z").MilliSeconds(), TInstant::ParseIso8601("2024-04-01T00:00:00Z").MilliSeconds())
                                   .Build()
                            .Build();

        auto predicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value {
                            column: "field1"
                        }
                        right_value {
                            typed_value {
                                type {
                                    type_id: TIMESTAMP
                                }
                                value {
                                    int64_value: 1709290801000000 # 2024-03-01T11:00:01.000Z
                                }
                            }
                        }
                    }
                )proto");

        auto rowGroups = NDq::MatchedRowGroups(fileMetadata, predicate);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups[0], 0);
    }

    Y_UNIT_TEST(FilterEverything) {
        TFileMetaDataBuilder builder{{
            arrow::field("field1", arrow::timestamp(arrow::TimeUnit::type::MILLI)),
            arrow::field("field2", arrow::int64()),
            arrow::field("field3", arrow::float64())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnTimestampStatistics(0, TInstant::ParseIso8601("2024-04-01T00:00:00Z").MilliSeconds(), TInstant::ParseIso8601("2024-04-13T00:00:00Z").MilliSeconds())
                                   .Build()
                            .Build();

        auto predicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value {
                            column: "field1"
                        }
                        right_value {
                            typed_value {
                                type {
                                    type_id: TIMESTAMP
                                }
                                value {
                                    int64_value: 1709290801000000 # 2024-03-01T11:00:01.000Z
                                }
                            }
                        }
                    }
                )proto");

        auto rowGroups = NDq::MatchedRowGroups(fileMetadata, predicate);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups.size(), 0);
    }

    Y_UNIT_TEST(MatchSeveralRowGroups) {
        TFileMetaDataBuilder builder{{
            arrow::field("field1", arrow::timestamp(arrow::TimeUnit::type::MILLI)),
            arrow::field("field2", arrow::int64()),
            arrow::field("field3", arrow::float64())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnTimestampStatistics(0, TInstant::ParseIso8601("2024-03-01T00:00:00Z").MilliSeconds(), TInstant::ParseIso8601("2024-04-01T00:00:00Z").MilliSeconds())
                                   .Build()
                                   .AddRowGroup()
                                   .AddColumnTimestampStatistics(0, TInstant::ParseIso8601("2024-02-01T00:00:00Z").MilliSeconds(), TInstant::ParseIso8601("2024-04-01T00:00:00Z").MilliSeconds())
                                   .Build()
                            .Build();

        auto predicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value {
                            column: "field1"
                        }
                        right_value {
                            typed_value {
                                type {
                                    type_id: TIMESTAMP
                                }
                                value {
                                    int64_value: 1709290801000000 # 2024-03-01T11:00:01.000Z
                                }
                            }
                        }
                    }
                )proto");

        auto rowGroups = NDq::MatchedRowGroups(fileMetadata, predicate);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups[0], 0);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups[1], 1);
    }

    Y_UNIT_TEST(TimestampWithoutMinMaxKeepsGroup) {
        TFileMetaDataBuilder builder{{
            arrow::field("field1", arrow::timestamp(arrow::TimeUnit::type::MILLI))
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnWithoutMinMax(0)
                                   .Build()
                            .Build();

        auto predicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value { column: "field1" }
                        right_value {
                            typed_value {
                                type { type_id: TIMESTAMP }
                                value { int64_value: 1 }
                            }
                        }
                    }
                )proto");

        auto rowGroups = NDq::MatchedRowGroups(fileMetadata, predicate);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups[0], 0);
    }

    Y_UNIT_TEST(Int64PushDown) {
        TFileMetaDataBuilder builder{{
            arrow::field("price", arrow::int64())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnInt64Statistics(0, 100, 200)
                                   .Build()
                            .Build();

        auto skipPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value { column: "price" }
                        right_value { typed_value { type { type_id: INT64 } value { int64_value: 50 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, skipPredicate).size(), 0);

        auto keepPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: G
                        left_value { column: "price" }
                        right_value { typed_value { type { type_id: INT64 } value { int64_value: 50 } } }
                    }
                )proto");
        auto kept = NDq::MatchedRowGroups(fileMetadata, keepPredicate);
        UNIT_ASSERT_VALUES_EQUAL(kept.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(kept[0], 0);
    }

    Y_UNIT_TEST(Int64MatchSecondGroup) {
        TFileMetaDataBuilder builder{{
            arrow::field("price", arrow::int64())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnInt64Statistics(0, 0, 10)
                                   .Build()
                                   .AddRowGroup()
                                   .AddColumnInt64Statistics(0, 100, 200)
                                   .Build()
                            .Build();

        auto predicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: GE
                        left_value { column: "price" }
                        right_value { typed_value { type { type_id: INT64 } value { int64_value: 150 } } }
                    }
                )proto");
        auto rowGroups = NDq::MatchedRowGroups(fileMetadata, predicate);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups[0], 1);
    }

    Y_UNIT_TEST(Int32PushDown) {
        TFileMetaDataBuilder builder{{
            arrow::field("price", arrow::int32())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnInt32Statistics(0, 100, 200)
                                   .Build()
                            .Build();

        auto skipPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value { column: "price" }
                        right_value { typed_value { type { type_id: INT32 } value { int32_value: 50 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, skipPredicate).size(), 0);

        auto keepPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: G
                        left_value { column: "price" }
                        right_value { typed_value { type { type_id: INT32 } value { int32_value: 50 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, keepPredicate).size(), 1);
    }

    Y_UNIT_TEST(IntWithoutStatsKeepsGroup) {
        TFileMetaDataBuilder builder{{
            arrow::field("price", arrow::int64())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnWithoutMinMax(0)
                                   .Build()
                            .Build();

        auto predicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value { column: "price" }
                        right_value { typed_value { type { type_id: INT64 } value { int64_value: 50 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, predicate).size(), 1);
    }

    Y_UNIT_TEST(DoublePushDown) {
        TFileMetaDataBuilder builder{{
            arrow::field("price", arrow::float64())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnDoubleStatistics(0, 1.0, 3.0)
                                   .Build()
                            .Build();

        auto skipPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: EQ
                        left_value { column: "price" }
                        right_value { typed_value { type { type_id: DOUBLE } value { double_value: 0.0 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, skipPredicate).size(), 0);

        auto keepPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: EQ
                        left_value { column: "price" }
                        right_value { typed_value { type { type_id: DOUBLE } value { double_value: 2.0 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, keepPredicate).size(), 1);
    }

    Y_UNIT_TEST(BoolPushDown) {
        TFileMetaDataBuilder builder{{
            arrow::field("flag", arrow::boolean())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnBoolStatistics(0, false, false)
                                   .Build()
                            .Build();

        auto skipPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: EQ
                        left_value { column: "flag" }
                        right_value { typed_value { type { type_id: BOOL } value { bool_value: true } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, skipPredicate).size(), 0);
    }

    Y_UNIT_TEST(UuidLogicalTypePushDown) {
        const TString lo(16, '\x10');
        const TString hi(16, '\x20');
        const TString inside(16, '\x15');
        const TString outside(16, '\x30');

        TFileMetaDataBuilder builder{MakeLogicalUuidSchema("id")};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnFlbaStatistics(0, lo, hi)
                                   .Build()
                            .Build();

        auto skipPredicate = BuildPredicate(
            TStringBuilder() << R"proto(
                comparison {
                    operation: EQ
                    left_value { column: "id" }
                    right_value { typed_value { type { type_id: UUID } value { )proto"
                             << UuidValueField(outside) << R"proto( } } }
                }
            )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, skipPredicate).size(), 0);

        auto keepPredicate = BuildPredicate(
            TStringBuilder() << R"proto(
                comparison {
                    operation: EQ
                    left_value { column: "id" }
                    right_value { typed_value { type { type_id: UUID } value { )proto"
                             << UuidValueField(inside) << R"proto( } } }
                }
            )proto");
        auto kept = NDq::MatchedRowGroups(fileMetadata, keepPredicate);
        UNIT_ASSERT_VALUES_EQUAL(kept.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(kept[0], 0);
    }

    Y_UNIT_TEST(UuidMatchSecondGroup) {
        const TString firstLo(16, '\x10');
        const TString firstHi(16, '\x11');
        const TString secondLo(16, '\x20');
        const TString secondHi(16, '\x21');
        const TString fromSecond(16, '\x20');

        TFileMetaDataBuilder builder{MakeLogicalUuidSchema("id")};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnFlbaStatistics(0, firstLo, firstHi)
                                   .Build()
                                   .AddRowGroup()
                                   .AddColumnFlbaStatistics(0, secondLo, secondHi)
                                   .Build()
                            .Build();

        auto predicate = BuildPredicate(
            TStringBuilder() << R"proto(
                comparison {
                    operation: EQ
                    left_value { column: "id" }
                    right_value { typed_value { type { type_id: UUID } value { )proto"
                             << UuidValueField(fromSecond) << R"proto( } } }
                }
            )proto");
        auto rowGroups = NDq::MatchedRowGroups(fileMetadata, predicate);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups[0], 1);
    }

    Y_UNIT_TEST(FixedSizeBinaryWithoutUuidLogicalType) {
        const TString lo(16, '\x10');
        const TString hi(16, '\x20');
        const TString outside(16, '\x30');

        TFileMetaDataBuilder builder{{
            arrow::field("id", arrow::fixed_size_binary(16))
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnFlbaStatistics(0, lo, hi)
                                   .Build()
                            .Build();

        auto predicate = BuildPredicate(
            TStringBuilder() << R"proto(
                comparison {
                    operation: EQ
                    left_value { column: "id" }
                    right_value { typed_value { type { type_id: UUID } value { )proto"
                             << UuidValueField(outside) << R"proto( } } }
                }
            )proto");
        // pyarrow 5 writes Uuid as FLBA(16) without UUID logical type; skip using raw bytes.
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, predicate).size(), 0);
    }

    Y_UNIT_TEST(FilePathReaderSkipsUnmatchedRowGroup) {
        arrow::Int64Builder values;
        UNIT_ASSERT(values.Append(1).ok());
        UNIT_ASSERT(values.Append(2).ok());
        UNIT_ASSERT(values.Append(100).ok());
        UNIT_ASSERT(values.Append(200).ok());
        std::shared_ptr<arrow::Int64Array> array;
        UNIT_ASSERT(values.Finish(&array).ok());

        auto table = arrow::Table::Make(
            arrow::schema({arrow::field("price", arrow::int64())}),
            {array});

        auto sinkResult = arrow::io::BufferOutputStream::Create();
        UNIT_ASSERT(sinkResult.ok());
        auto sink = *sinkResult;
        UNIT_ASSERT(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), sink, /*chunk_size=*/2).ok());
        auto bufferResult = sink->Finish();
        UNIT_ASSERT(bufferResult.ok());

        std::unique_ptr<parquet::arrow::FileReader> fileReader;
        parquet::arrow::FileReaderBuilder readerBuilder;
        UNIT_ASSERT(readerBuilder.Open(std::make_shared<arrow::io::BufferReader>(*bufferResult)).ok());
        UNIT_ASSERT(readerBuilder.Build(&fileReader).ok());
        UNIT_ASSERT_VALUES_EQUAL(fileReader->num_row_groups(), 2);

        auto predicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: GE
                        left_value { column: "price" }
                        right_value { typed_value { type { type_id: INT64 } value { int64_value: 50 } } }
                    }
                )proto");
        const auto matched = NDq::MatchedRowGroups(fileReader->parquet_reader()->metadata(), predicate);
        UNIT_ASSERT_VALUES_EQUAL(matched.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(matched[0], 1);

        int readCount = 0;
        i64 rowsRead = 0;
        for (auto group : matched) {
            std::shared_ptr<arrow::Table> groupTable;
            UNIT_ASSERT(fileReader->ReadRowGroup(static_cast<int>(group), &groupTable).ok());
            ++readCount;
            rowsRead += groupTable->num_rows();
        }
        UNIT_ASSERT_VALUES_EQUAL(readCount, 1);
        UNIT_ASSERT_VALUES_EQUAL(rowsRead, 2);
    }

    Y_UNIT_TEST(Flba16WithoutUuidLogicalTypeIsTreatedAsUuid) {
        // FLBA(16) with NONE logical type is treated as UUID (pyarrow 5 compatibility).
        // This test verifies that a non-UUID FLBA(16) column with a UUID predicate
        // will be skipped if the constant is outside the min/max range.
        // This is a known risk: if the column is not actually UUID, this is a false skip.
        const TString lo(16, '\x10');
        const TString hi(16, '\x20');
        const TString outside(16, '\x30');

        TFileMetaDataBuilder builder{{
            arrow::field("data", arrow::fixed_size_binary(16))
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnFlbaStatistics(0, lo, hi)
                                   .Build()
                            .Build();

        auto predicate = BuildPredicate(
            TStringBuilder() << R"proto(
                comparison {
                    operation: EQ
                    left_value { column: "data" }
                    right_value { typed_value { type { type_id: UUID } value { )proto"
                             << UuidValueField(outside) << R"proto( } } }
                }
            )proto");
        // The column is FLBA(16) without UUID logical type, but MakeStatistics treats it as UUID.
        // The predicate is UUID, so the group is skipped.
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, predicate).size(), 0);
    }

    Y_UNIT_TEST(Flba16WithoutUuidLogicalTypeKeepGroup) {
        // Same as above, but constant is inside the range -> keep group.
        const TString lo(16, '\x10');
        const TString hi(16, '\x20');
        const TString inside(16, '\x15');

        TFileMetaDataBuilder builder{{
            arrow::field("data", arrow::fixed_size_binary(16))
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnFlbaStatistics(0, lo, hi)
                                   .Build()
                            .Build();

        auto predicate = BuildPredicate(
            TStringBuilder() << R"proto(
                comparison {
                    operation: EQ
                    left_value { column: "data" }
                    right_value { typed_value { type { type_id: UUID } value { )proto"
                             << UuidValueField(inside) << R"proto( } } }
                }
            )proto");
        auto kept = NDq::MatchedRowGroups(fileMetadata, predicate);
        UNIT_ASSERT_VALUES_EQUAL(kept.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(kept[0], 0);
    }

    Y_UNIT_TEST(Flba32WithoutUuidLogicalTypeNotTreatedAsUuid) {
        // FLBA(32) with NONE logical type should NOT be treated as UUID.
        // MakeStatistics returns {} for FLBA with type_length != 16.
        const TString lo(32, '\x10');
        const TString hi(32, '\x20');
        const TString outside(16, '\x30');

        TFileMetaDataBuilder builder{{
            arrow::field("data", arrow::fixed_size_binary(32))
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnFlbaStatistics(0, lo, hi)
                                   .Build()
                            .Build();

        auto predicate = BuildPredicate(
            TStringBuilder() << R"proto(
                comparison {
                    operation: EQ
                    left_value { column: "data" }
                    right_value { typed_value { type { type_id: UUID } value { )proto"
                             << UuidValueField(outside) << R"proto( } } }
                }
            )proto");
        // FLBA(32) is not treated as UUID, so no statistics are extracted.
        // MatchPredicate returns Unknown -> keep group.
        auto kept = NDq::MatchedRowGroups(fileMetadata, predicate);
        UNIT_ASSERT_VALUES_EQUAL(kept.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(kept[0], 0);
    }

    Y_UNIT_TEST(BoolStatsAllNullKeepsGroup) {
        // Bool column with no min/max (all NULL) -> HasMinMax() returns false.
        // MakeStatistics returns {} -> no statistics -> Unknown -> keep group.
        TFileMetaDataBuilder builder{{
            arrow::field("flag", arrow::boolean())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnWithoutMinMax(0)
                                   .Build()
                            .Build();

        auto predicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: EQ
                        left_value { column: "flag" }
                        right_value { typed_value { type { type_id: BOOL } value { bool_value: true } } }
                    }
                )proto");
        auto kept = NDq::MatchedRowGroups(fileMetadata, predicate);
        UNIT_ASSERT_VALUES_EQUAL(kept.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(kept[0], 0);
    }

    Y_UNIT_TEST(DoubleStatsAllNullKeepsGroup) {
        // Double column with no min/max (all NULL) -> HasMinMax() returns false.
        // MakeStatistics returns {} -> no statistics -> Unknown -> keep group.
        TFileMetaDataBuilder builder{{
            arrow::field("value", arrow::float64())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnWithoutMinMax(0)
                                   .Build()
                            .Build();

        auto predicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: EQ
                        left_value { column: "value" }
                        right_value { typed_value { type { type_id: DOUBLE } value { double_value: 2.0 } } }
                    }
                )proto");
        auto kept = NDq::MatchedRowGroups(fileMetadata, predicate);
        UNIT_ASSERT_VALUES_EQUAL(kept.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(kept[0], 0);
    }

    Y_UNIT_TEST(UuidStatsAllNullKeepsGroup) {
        // UUID column with no min/max (all NULL) -> HasMinMax() returns false.
        // MakeStatistics returns {} -> no statistics -> Unknown -> keep group.
        const TString outside(16, '\x30');

        TFileMetaDataBuilder builder{MakeLogicalUuidSchema("id")};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnWithoutMinMax(0)
                                   .Build()
                             .Build();

        auto predicate = BuildPredicate(
            TStringBuilder() << R"proto(
                comparison {
                    operation: EQ
                    left_value { column: "id" }
                    right_value { typed_value { type { type_id: UUID } value { )proto"
                             << UuidValueField(outside) << R"proto( } } }
                }
            )proto");
        auto kept = NDq::MatchedRowGroups(fileMetadata, predicate);
        UNIT_ASSERT_VALUES_EQUAL(kept.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(kept[0], 0);
    }

    // --- Item 5: INT8/INT16/UINT8/UINT16/UINT32 ---
    // Arrow maps all of these to INT32 physical type in parquet.
    // MakeStatistics handles INT32 physical type, so all should work.

    Y_UNIT_TEST(Int8PushDown) {
        TFileMetaDataBuilder builder{{
            arrow::field("val", arrow::int8())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnInt32Statistics(0, 10, 20)
                                   .Build()
                             .Build();

        auto skipPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value { column: "val" }
                        right_value { typed_value { type { type_id: INT8 } value { int32_value: 5 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, skipPredicate).size(), 0);

        auto keepPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: G
                        left_value { column: "val" }
                        right_value { typed_value { type { type_id: INT8 } value { int32_value: 5 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, keepPredicate).size(), 1);
    }

    Y_UNIT_TEST(Int16PushDown) {
        TFileMetaDataBuilder builder{{
            arrow::field("val", arrow::int16())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnInt32Statistics(0, 100, 200)
                                   .Build()
                             .Build();

        auto skipPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value { column: "val" }
                        right_value { typed_value { type { type_id: INT16 } value { int32_value: 50 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, skipPredicate).size(), 0);

        auto keepPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: G
                        left_value { column: "val" }
                        right_value { typed_value { type { type_id: INT16 } value { int32_value: 50 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, keepPredicate).size(), 1);
    }

    Y_UNIT_TEST(Uint8PushDown) {
        TFileMetaDataBuilder builder{{
            arrow::field("val", arrow::uint8())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnInt32Statistics(0, 10, 20)
                                   .Build()
                             .Build();

        // UINT8 maps to INT32 physical type with UNSIGNED sort order.
        // MakeStatistics requires SIGNED sort order for INT types, so stats are not extracted.
        // MatchPredicate returns Unknown -> keep group for both predicates.
        auto skipPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value { column: "val" }
                        right_value { typed_value { type { type_id: UINT8 } value { int32_value: 5 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, skipPredicate).size(), 1);

        auto keepPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: G
                        left_value { column: "val" }
                        right_value { typed_value { type { type_id: UINT8 } value { int32_value: 5 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, keepPredicate).size(), 1);
    }

    Y_UNIT_TEST(Uint16PushDown) {
        TFileMetaDataBuilder builder{{
            arrow::field("val", arrow::uint16())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnInt32Statistics(0, 100, 200)
                                   .Build()
                             .Build();

        // UINT16 maps to INT32 physical type with UNSIGNED sort order.
        // MakeStatistics requires SIGNED sort order for INT types, so stats are not extracted.
        // MatchPredicate returns Unknown -> keep group for both predicates.
        auto skipPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value { column: "val" }
                        right_value { typed_value { type { type_id: UINT16 } value { int32_value: 50 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, skipPredicate).size(), 1);

        auto keepPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: G
                        left_value { column: "val" }
                        right_value { typed_value { type { type_id: UINT16 } value { int32_value: 50 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, keepPredicate).size(), 1);
    }

    Y_UNIT_TEST(Uint32PushDown) {
        TFileMetaDataBuilder builder{{
            arrow::field("val", arrow::uint32())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnInt32Statistics(0, 100, 200)
                                   .Build()
                             .Build();

        // UINT32 maps to INT32 physical type. In this test environment, ToParquetSchema
        // does NOT set ConvertedType::UINT_32, so sort_order remains SIGNED and
        // MakeStatistics returns valid stats. Pushdown works for both predicates.
        // Note: must use uint32_value (not int32_value) for UINT32 type in proto.
        auto skipPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value { column: "val" }
                        right_value { typed_value { type { type_id: UINT32 } value { uint32_value: 50 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, skipPredicate).size(), 0);

        auto keepPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: G
                        left_value { column: "val" }
                        right_value { typed_value { type { type_id: UINT32 } value { uint32_value: 50 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, keepPredicate).size(), 1);
    }

    // --- Item 6: FLOAT (not just DOUBLE) ---

    Y_UNIT_TEST(FloatPushDown) {
        TFileMetaDataBuilder builder{{
            arrow::field("val", arrow::float32())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnFloatStatistics(0, 1.0f, 3.0f)
                                   .Build()
                             .Build();

        auto skipPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: EQ
                        left_value { column: "val" }
                        right_value { typed_value { type { type_id: FLOAT } value { float_value: 0.0 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, skipPredicate).size(), 0);

        auto keepPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: EQ
                        left_value { column: "val" }
                        right_value { typed_value { type { type_id: FLOAT } value { float_value: 2.0 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, keepPredicate).size(), 1);
    }

    Y_UNIT_TEST(FloatMatchSecondGroup) {
        TFileMetaDataBuilder builder{{
            arrow::field("val", arrow::float32())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnFloatStatistics(0, 0.0f, 1.0f)
                                   .Build()
                                   .AddRowGroup()
                                   .AddColumnFloatStatistics(0, 10.0f, 20.0f)
                                   .Build()
                             .Build();

        auto predicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: GE
                        left_value { column: "val" }
                        right_value { typed_value { type { type_id: FLOAT } value { float_value: 15.0 } } }
                    }
                )proto");
        auto rowGroups = NDq::MatchedRowGroups(fileMetadata, predicate);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups[0], 1);
    }

    // --- Item 7: DATE ---

    Y_UNIT_TEST(DatePushDown) {
        // DATE is stored as INT32 physical type with DATE logical type.
        // MakeStatistics handles DATE logical type via GetDateStatistics.
        TFileMetaDataBuilder builder{{
            arrow::field("d", arrow::date32())
        }};
        // 2024-01-01 = day 19723, 2024-03-01 = day 19783
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnInt32Statistics(0, 19723, 19783)
                                   .Build()
                             .Build();

        // Predicate: d > 2024-06-01 (day 19923) -> skip
        auto skipPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value { column: "d" }
                        right_value { typed_value { type { type_id: DATE } value { int32_value: 19923 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, skipPredicate).size(), 0);

        // Predicate: d > 2024-02-01 (day 19753) -> keep
        auto keepPredicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: G
                        left_value { column: "d" }
                        right_value { typed_value { type { type_id: DATE } value { int32_value: 19753 } } }
                    }
                )proto");
        UNIT_ASSERT_VALUES_EQUAL(NDq::MatchedRowGroups(fileMetadata, keepPredicate).size(), 1);
    }

    // --- Item 9: sort_order != SIGNED for INT ---
    // When sort_order is not SIGNED, MakeStatistics should return {} for INT columns.
    // This means no statistics are extracted, so MatchPredicate returns Unknown -> keep group.

    Y_UNIT_TEST(Int32NonSignedSortOrderKeepsGroup) {
        // Build a schema with INT32 column but non-SIGNED sort order.
        // parquet::schema::PrimitiveNode::Make with sort_order = UNSIGNED.
        auto node = parquet::schema::PrimitiveNode::Make(
            "val",
            parquet::Repetition::REQUIRED,
            parquet::Type::INT32,
            parquet::ConvertedType::UINT_32);
        auto group = parquet::schema::GroupNode::Make(
            "schema", parquet::Repetition::REQUIRED, {node});
        auto schema = std::make_shared<parquet::SchemaDescriptor>();
        schema->Init(group);

        TFileMetaDataBuilder builder{schema};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnInt32Statistics(0, 10, 20)
                                   .Build()
                             .Build();

        // Even though stats are present, sort_order != SIGNED means MakeStatistics returns {}.
        // MatchPredicate returns Unknown -> keep group.
        auto predicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value { column: "val" }
                        right_value { typed_value { type { type_id: INT32 } value { int32_value: 5 } } }
                    }
                )proto");
        auto kept = NDq::MatchedRowGroups(fileMetadata, predicate);
        UNIT_ASSERT_VALUES_EQUAL(kept.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(kept[0], 0);
    }

    Y_UNIT_TEST(Int64NonSignedSortOrderKeepsGroup) {
        auto node = parquet::schema::PrimitiveNode::Make(
            "val",
            parquet::Repetition::REQUIRED,
            parquet::Type::INT64,
            parquet::ConvertedType::UINT_64);
        auto group = parquet::schema::GroupNode::Make(
            "schema", parquet::Repetition::REQUIRED, {node});
        auto schema = std::make_shared<parquet::SchemaDescriptor>();
        schema->Init(group);

        TFileMetaDataBuilder builder{schema};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnInt64Statistics(0, 10, 20)
                                   .Build()
                             .Build();

        auto predicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value { column: "val" }
                        right_value { typed_value { type { type_id: INT64 } value { int64_value: 5 } } }
                    }
                )proto");
        auto kept = NDq::MatchedRowGroups(fileMetadata, predicate);
        UNIT_ASSERT_VALUES_EQUAL(kept.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(kept[0], 0);
    }
}

}
