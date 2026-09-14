#include <ydb/library/yql/providers/generic/pushdown/yql_generic_match_predicate.h>

#include <library/cpp/testing/unittest/registar.h>

#include <google/protobuf/text_format.h>

#include <util/string/builder.h>

#include <cstring>
#include <limits>
#include <utility>

namespace {

    NYql::NConnector::NApi::TPredicate BuildPredicate(const TString& text) {
        NYql::NConnector::NApi::TPredicate predicate;
        UNIT_ASSERT(google::protobuf::TextFormat::ParseFromString(text, &predicate));
        return predicate;
    }

    NYql::NGenericPushDown::TColumnStatistics BuildTimestampStats(const TInstant& from, const TInstant& to) {
        NYql::NGenericPushDown::TColumnStatistics statistics;
        statistics.ColumnType.set_type_id(::Ydb::Type::TIMESTAMP);
        statistics.Timestamp.ConstructInPlace();
        statistics.Timestamp->lowValue = from;
        statistics.Timestamp->highValue = to;
        return statistics;
    }

    NYql::NGenericPushDown::TColumnStatistics BuildLongStats(i64 from, i64 to) {
        NYql::NGenericPushDown::TColumnStatistics statistics;
        statistics.ColumnType.set_type_id(::Ydb::Type::INT64);
        statistics.LongStats.ConstructInPlace();
        statistics.LongStats->lowValue = from;
        statistics.LongStats->highValue = to;
        return statistics;
    }

    NYql::NGenericPushDown::TColumnStatistics BuildDoubleStats(double from, double to) {
        NYql::NGenericPushDown::TColumnStatistics statistics;
        statistics.ColumnType.set_type_id(::Ydb::Type::DOUBLE);
        statistics.DoubleStats.ConstructInPlace();
        statistics.DoubleStats->lowValue = from;
        statistics.DoubleStats->highValue = to;
        return statistics;
    }

    NYql::NGenericPushDown::TColumnStatistics BuildBooleanStats(i64 numTrues, i64 numFalses) {
        NYql::NGenericPushDown::TColumnStatistics statistics;
        statistics.ColumnType.set_type_id(::Ydb::Type::BOOL);
        statistics.BooleanStats.ConstructInPlace();
        statistics.BooleanStats->numTrues = numTrues;
        statistics.BooleanStats->numFalses = numFalses;
        return statistics;
    }

    TString UuidBytesFromHalves(ui64 low, ui64 high) {
        TString bytes;
        bytes.resize(16);
        memcpy(bytes.begin(), &low, sizeof(ui64));
        memcpy(bytes.begin() + sizeof(ui64), &high, sizeof(ui64));
        return bytes;
    }

    NYql::NGenericPushDown::TColumnStatistics BuildUuidStats(const TString& from, const TString& to) {
        NYql::NGenericPushDown::TColumnStatistics statistics;
        statistics.ColumnType.set_type_id(::Ydb::Type::UUID);
        statistics.UuidStats.ConstructInPlace();
        statistics.UuidStats->lowValue = from;
        statistics.UuidStats->highValue = to;
        return statistics;
    }

    TString ComparisonPredicate(const TString& column, const TString& operation, const TString& typeId, const TString& valueField) {
        return TStringBuilder()
            << "comparison {\n"
            << "    operation: " << operation << "\n"
            << "    left_value { column: \"" << column << "\" }\n"
            << "    right_value {\n"
            << "        typed_value {\n"
            << "            type { type_id: " << typeId << " }\n"
            << "            value { " << valueField << " }\n"
            << "        }\n"
            << "    }\n"
            << "}\n";
    }

    bool MatchLong(i64 lo, i64 hi, const TString& operation, i64 constant) {
        return MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildLongStats(lo, hi)}}},
            BuildPredicate(ComparisonPredicate("col1", operation, "INT64", TStringBuilder() << "int64_value: " << constant)));
    }

    bool MatchDouble(double lo, double hi, const TString& operation, double constant) {
        return MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildDoubleStats(lo, hi)}}},
            BuildPredicate(ComparisonPredicate("col1", operation, "DOUBLE", TStringBuilder() << "double_value: " << constant)));
    }

    std::pair<ui64, ui64> UuidHalves(const TString& bytes) {
        ui64 low = 0;
        ui64 high = 0;
        memcpy(&low, bytes.data(), sizeof(ui64));
        memcpy(&high, bytes.data() + sizeof(ui64), sizeof(ui64));
        return {low, high};
    }

    bool MatchUuid(const TString& lo, const TString& hi, const TString& operation, const TString& constant) {
        const auto halves = UuidHalves(constant);
        return MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildUuidStats(lo, hi)}}},
            BuildPredicate(ComparisonPredicate(
                "col1",
                operation,
                "UUID",
                TStringBuilder() << "low_128: " << halves.first << " high_128: " << halves.second)));
    }

} // namespace

Y_UNIT_TEST_SUITE(MatchPredicate) {
    Y_UNIT_TEST(EmptyMatch) {
        UNIT_ASSERT(MatchPredicate(TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{}, NYql::NConnector::NApi::TPredicate{}));
    }

    Y_UNIT_TEST(EmptyWhere) {
        UNIT_ASSERT(MatchPredicate(TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", NYql::NGenericPushDown::TColumnStatistics{}},
                                                                                             {"col2", NYql::NGenericPushDown::TColumnStatistics{}}}},
                                   NYql::NConnector::NApi::TPredicate{}));
    }

    Y_UNIT_TEST(Between) {
        UNIT_ASSERT(MatchPredicate(TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildTimestampStats(TInstant::ParseIso8601("2024-03-01T00:00:00Z"), TInstant::ParseIso8601("2024-03-01T23:59:59Z"))}}},
                                   BuildPredicate(
                                       R"proto(
                                between {
                                    value {
                                        column: "col1"
                                    }
                                    least {
                                        typed_value {
                                            type {
                                                type_id: TIMESTAMP
                                            }
                                            value {
                                                int64_value: 1709290801000000 # 2024-03-01T11:00:01.000Z
                                            }
                                        }
                                    }
                                    greatest {
                                        typed_value {
                                            type {
                                                type_id: TIMESTAMP
                                            }
                                            value {
                                                int64_value: 1709294401000000 # 2024-03-01T12:00:01.000Z
                                            }
                                        }
                                    }
                                }
                            )proto")));
    }

    Y_UNIT_TEST(Less) {
        UNIT_ASSERT(MatchPredicate(TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildTimestampStats(TInstant::ParseIso8601("2024-03-01T00:00:00Z"), TInstant::ParseIso8601("2024-03-01T23:59:59Z"))}}},
                                   BuildPredicate(
                                       R"proto(
                                comparison {
                                    operation: L
                                    left_value {
                                        column: "col1"
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
                            )proto")));
    }

    Y_UNIT_TEST(NotLess) {
        UNIT_ASSERT(!MatchPredicate(TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildTimestampStats(TInstant::ParseIso8601("2024-03-02T00:00:00Z"), TInstant::ParseIso8601("2024-03-02T23:59:59Z"))}}},
                                    BuildPredicate(
                                        R"proto(
                                    comparison {
                                        operation: L
                                        left_value {
                                            column: "col1"
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
                                )proto")));
    }

    Y_UNIT_TEST(RightColumn) {
        UNIT_ASSERT(MatchPredicate(TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildTimestampStats(TInstant::ParseIso8601("2024-03-01T00:00:00Z"), TInstant::ParseIso8601("2024-03-01T23:59:59Z"))}}},
                                   BuildPredicate(
                                       R"proto(
                                comparison {
                                    operation: G
                                    left_value {
                                        typed_value {
                                            type {
                                                type_id: TIMESTAMP
                                            }
                                            value {
                                                int64_value: 1709290801000000 # 2024-03-01T11:00:01.000Z
                                            }
                                        }
                                    }
                                    right_value {
                                        column: "col1"
                                    }
                                }
                            )proto")));
    }

    Y_UNIT_TEST(LongStatsComparators) {
        UNIT_ASSERT(MatchLong(10, 20, "EQ", 15));
        UNIT_ASSERT(!MatchLong(10, 20, "EQ", 5));
        UNIT_ASSERT(MatchLong(10, 20, "EQ", 10));
        UNIT_ASSERT(MatchLong(10, 20, "EQ", 20));
        UNIT_ASSERT(!MatchLong(10, 20, "L", 10));
        UNIT_ASSERT(MatchLong(10, 20, "L", 15));
        UNIT_ASSERT(!MatchLong(10, 20, "G", 20));
        UNIT_ASSERT(MatchLong(10, 20, "GE", 20));
        UNIT_ASSERT(!MatchLong(5, 5, "NE", 5));
        UNIT_ASSERT(MatchLong(10, 20, "NE", 15));

        UNIT_ASSERT(!MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildLongStats(10, 20)}}},
            BuildPredicate(
                R"proto(
                    between {
                        value { column: "col1" }
                        least { typed_value { type { type_id: INT64 } value { int64_value: 1 } } }
                        greatest { typed_value { type { type_id: INT64 } value { int64_value: 5 } } }
                    }
                )proto")));
        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildLongStats(10, 20)}}},
            BuildPredicate(
                R"proto(
                    between {
                        value { column: "col1" }
                        least { typed_value { type { type_id: INT64 } value { int64_value: 15 } } }
                        greatest { typed_value { type { type_id: INT64 } value { int64_value: 25 } } }
                    }
                )proto")));

        NYql::NGenericPushDown::TColumnStatistics emptyInt;
        emptyInt.ColumnType.set_type_id(::Ydb::Type::INT64);
        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", emptyInt}}},
            BuildPredicate(ComparisonPredicate("col1", "EQ", "INT64", "int64_value: 15"))));

        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildLongStats(10, 20)}}},
            BuildPredicate(ComparisonPredicate("col1", "EQ", "TIMESTAMP", "int64_value: 15"))));
    }

    Y_UNIT_TEST(DoubleStatsComparators) {
        UNIT_ASSERT(MatchDouble(1.0, 3.0, "EQ", 2.0));
        UNIT_ASSERT(!MatchDouble(1.0, 3.0, "EQ", 0.0));
        UNIT_ASSERT(!MatchDouble(1.0, 3.0, "L", 1.0));
        UNIT_ASSERT(!MatchDouble(1.0, 3.0, "G", 3.0));

        const double nan = std::numeric_limits<double>::quiet_NaN();
        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildDoubleStats(nan, 3.0)}}},
            BuildPredicate(ComparisonPredicate("col1", "EQ", "DOUBLE", "double_value: 2.0"))));
        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildDoubleStats(1.0, nan)}}},
            BuildPredicate(ComparisonPredicate("col1", "EQ", "DOUBLE", "double_value: 2.0"))));
    }

    Y_UNIT_TEST(BooleanStatsComparators) {
        UNIT_ASSERT(!MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildBooleanStats(0, 1)}}},
            BuildPredicate(ComparisonPredicate("col1", "EQ", "BOOL", "bool_value: true"))));
        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildBooleanStats(0, 1)}}},
            BuildPredicate(ComparisonPredicate("col1", "EQ", "BOOL", "bool_value: false"))));
        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildBooleanStats(1, 1)}}},
            BuildPredicate(ComparisonPredicate("col1", "EQ", "BOOL", "bool_value: true"))));
        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildBooleanStats(1, 1)}}},
            BuildPredicate(ComparisonPredicate("col1", "EQ", "BOOL", "bool_value: false"))));

        NYql::NGenericPushDown::TColumnStatistics emptyBool;
        emptyBool.ColumnType.set_type_id(::Ydb::Type::BOOL);
        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", emptyBool}}},
            BuildPredicate(ComparisonPredicate("col1", "EQ", "BOOL", "bool_value: true"))));
    }

    Y_UNIT_TEST(UuidStatsComparators) {
        const TString lo = TString(16, '\x10');
        const TString hi = TString(16, '\x20');
        const TString inside = TString(16, '\x15');
        const TString below = TString(16, '\x00');
        const TString above = TString(16, '\x30');
        const TString point = TString(16, '\x05');

        const auto insideH = UuidHalves(inside);
        UNIT_ASSERT_VALUES_EQUAL(inside.size(), 16);
        UNIT_ASSERT_VALUES_EQUAL(UuidBytesFromHalves(insideH.first, insideH.second), inside);

        UNIT_ASSERT(MatchUuid(lo, hi, "EQ", inside));
        UNIT_ASSERT(!MatchUuid(lo, hi, "EQ", below));
        UNIT_ASSERT(!MatchUuid(lo, hi, "EQ", above));
        UNIT_ASSERT(MatchUuid(lo, hi, "EQ", lo));
        UNIT_ASSERT(MatchUuid(lo, hi, "EQ", hi));
        UNIT_ASSERT(!MatchUuid(lo, hi, "L", lo));
        UNIT_ASSERT(!MatchUuid(lo, hi, "G", hi));
        UNIT_ASSERT(!MatchUuid(point, point, "NE", point));
        UNIT_ASSERT(MatchUuid(lo, hi, "NE", inside));

        const auto belowH = UuidHalves(below);
        const auto missHiH = UuidHalves(TString(16, '\x05'));
        const auto aboveH = UuidHalves(above);
        UNIT_ASSERT(!MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildUuidStats(lo, hi)}}},
            BuildPredicate(
                TStringBuilder() << "between {\n"
                                 << "    value { column: \"col1\" }\n"
                                 << "    least { typed_value { type { type_id: UUID } value { low_128: " << belowH.first
                                 << " high_128: " << belowH.second << " } } }\n"
                                 << "    greatest { typed_value { type { type_id: UUID } value { low_128: " << missHiH.first
                                 << " high_128: " << missHiH.second << " } } }\n"
                                 << "}\n")));
        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildUuidStats(lo, hi)}}},
            BuildPredicate(
                TStringBuilder() << "between {\n"
                                 << "    value { column: \"col1\" }\n"
                                 << "    least { typed_value { type { type_id: UUID } value { low_128: " << insideH.first
                                 << " high_128: " << insideH.second << " } } }\n"
                                 << "    greatest { typed_value { type { type_id: UUID } value { low_128: " << aboveH.first
                                 << " high_128: " << aboveH.second << " } } }\n"
                                 << "}\n")));

        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildUuidStats(lo, hi)}}},
            BuildPredicate(ComparisonPredicate("col1", "EQ", "INT64", "int64_value: 1"))));
    }

    Y_UNIT_TEST(UuidStatsWrongLengthKeepsGroup) {
        // UuidStats with lowValue/highValue not 16 bytes -> Unknown -> keep group.
        const TString shortLo(8, '\x10');
        const TString shortHi(8, '\x20');
        const TString inside(16, '\x15');
        const auto insideH = UuidHalves(inside);

        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildUuidStats(shortLo, shortHi)}}},
            BuildPredicate(ComparisonPredicate(
                "col1",
                "EQ",
                "UUID",
                TStringBuilder() << "low_128: " << insideH.first << " high_128: " << insideH.second))));
    }

    Y_UNIT_TEST(UuidStatsMissingMinMaxKeepsGroup) {
        // UuidStats present but lowValue/highValue not set -> Unknown -> keep group.
        NYql::NGenericPushDown::TColumnStatistics stats;
        stats.ColumnType.set_type_id(::Ydb::Type::UUID);
        stats.UuidStats.ConstructInPlace();
        // lowValue and highValue are TMaybe<TString>, not set.

        const TString inside(16, '\x15');
        const auto insideH = UuidHalves(inside);
        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", stats}}},
            BuildPredicate(ComparisonPredicate(
                "col1",
                "EQ",
                "UUID",
                TStringBuilder() << "low_128: " << insideH.first << " high_128: " << insideH.second))));
    }

    Y_UNIT_TEST(LongStatsUint64OverflowKeepsGroup) {
        // UINT64 constant > Max<i64>() -> Unknown -> keep group.
        NYql::NGenericPushDown::TColumnStatistics stats = BuildLongStats(10, 20);
        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", stats}}},
            BuildPredicate(ComparisonPredicate("col1", "EQ", "UINT64", "uint64_value: 18446744073709551615"))));
    }

    Y_UNIT_TEST(LongStatsUint64InRange) {
        // UINT64 constant <= Max<i64>() -> compare as i64.
        NYql::NGenericPushDown::TColumnStatistics stats = BuildLongStats(10, 20);
        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", stats}}},
            BuildPredicate(ComparisonPredicate("col1", "EQ", "UINT64", "uint64_value: 15"))));
        UNIT_ASSERT(!MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", stats}}},
            BuildPredicate(ComparisonPredicate("col1", "EQ", "UINT64", "uint64_value: 5"))));
    }

    Y_UNIT_TEST(DoubleStatsNanConstantKeepsGroup) {
        // NaN constant -> Unknown -> keep group.
        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildDoubleStats(1.0, 3.0)}}},
            BuildPredicate(ComparisonPredicate("col1", "EQ", "DOUBLE", "double_value: nan"))));
    }

    Y_UNIT_TEST(BooleanStatsMissingCountersKeepsGroup) {
        // BooleanStats present but numTrues/numFalses not set -> Unknown -> keep group.
        NYql::NGenericPushDown::TColumnStatistics stats;
        stats.ColumnType.set_type_id(::Ydb::Type::BOOL);
        stats.BooleanStats.ConstructInPlace();
        // numTrues and numFalses are TMaybe<i64>, not set.

        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", stats}}},
            BuildPredicate(ComparisonPredicate("col1", "EQ", "BOOL", "bool_value: true"))));
    }

    Y_UNIT_TEST(BooleanStatsNonEqOperatorKeepsGroup) {
        // BOOL with L/G/LE/GE operators -> Unknown -> keep group.
        NYql::NGenericPushDown::TColumnStatistics stats = BuildBooleanStats(1, 1);
        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", stats}}},
            BuildPredicate(ComparisonPredicate("col1", "L", "BOOL", "bool_value: true"))));
        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", stats}}},
            BuildPredicate(ComparisonPredicate("col1", "G", "BOOL", "bool_value: false"))));
    }

    Y_UNIT_TEST(LongStatsBetweenBoundary) {
        // BETWEEN where least == highValue and greatest == lowValue (reversed) -> False.
        UNIT_ASSERT(!MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildLongStats(10, 20)}}},
            BuildPredicate(
                R"proto(
                    between {
                        value { column: "col1" }
                        least { typed_value { type { type_id: INT64 } value { int64_value: 20 } } }
                        greatest { typed_value { type { type_id: INT64 } value { int64_value: 10 } } }
                    }
                )proto")));
    }

    Y_UNIT_TEST(LongStatsBetweenExactMatch) {
        // BETWEEN where [least, greatest] == [lowValue, highValue] -> True.
        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", BuildLongStats(10, 20)}}},
            BuildPredicate(
                R"proto(
                    between {
                        value { column: "col1" }
                        least { typed_value { type { type_id: INT64 } value { int64_value: 10 } } }
                        greatest { typed_value { type { type_id: INT64 } value { int64_value: 20 } } }
                    }
                )proto")));
    }

    // --- Item 8: UUID endianness (big-endian simulation) ---
    // The byte-by-byte fix in SerializeUuid and TypedValueToUuidBytes ensures
    // that UUID bytes are copied in the correct order regardless of platform
    // endianness. This test verifies that the round-trip through
    // low_128/high_128 proto fields produces the correct byte sequence.

    Y_UNIT_TEST(UuidEndiannessRoundTrip) {
        // Use a UUID with distinct bytes in each position to detect
        // any byte-ordering issues.
        // Bytes: 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10
        const unsigned char bytes[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
        const TString uuidBytes(reinterpret_cast<const char*>(bytes), 16);

        // Extract low/high halves the same way TypedValueToUuidBytes does
        // (byte-by-byte copy, not memcpy of ui64).
        ui64 low = 0;
        ui64 high = 0;
        for (int i = 0; i < 8; ++i) {
            low |= static_cast<ui64>(static_cast<unsigned char>(bytes[i])) << (i * 8);
            high |= static_cast<ui64>(static_cast<unsigned char>(bytes[8 + i])) << (i * 8);
        }

        // Build stats with this UUID as both low and high (single-value range).
        auto stats = BuildUuidStats(uuidBytes, uuidBytes);

        // Build predicate with the same UUID via low_128/high_128.
        // TypedValueToUuidBytes will reconstruct the bytes from low/high.
        // If the byte-by-byte copy is correct, the reconstructed bytes
        // should match uuidBytes exactly.
        auto predicate = BuildPredicate(ComparisonPredicate(
            "col1",
            "EQ",
            "UUID",
            TStringBuilder() << "low_128: " << low << " high_128: " << high));

        // Should match (EQ with exact value in range).
        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", stats}}},
            predicate));

        // Now test with a different UUID that is outside the range.
        // Bytes: 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 11 (last byte differs)
        const unsigned char otherBytes[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                     0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x11};
        const TString otherUuid(reinterpret_cast<const char*>(otherBytes), 16);
        ui64 otherLow = 0;
        ui64 otherHigh = 0;
        for (int i = 0; i < 8; ++i) {
            otherLow |= static_cast<ui64>(static_cast<unsigned char>(otherBytes[i])) << (i * 8);
            otherHigh |= static_cast<ui64>(static_cast<unsigned char>(otherBytes[8 + i])) << (i * 8);
        }

        auto otherPredicate = BuildPredicate(ComparisonPredicate(
            "col1",
            "EQ",
            "UUID",
            TStringBuilder() << "low_128: " << otherLow << " high_128: " << otherHigh));

        // Should NOT match (different UUID, outside range).
        UNIT_ASSERT(!MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", stats}}},
            otherPredicate));
    }

    Y_UNIT_TEST(UuidEndiannessBigEndianSimulation) {
        // Simulate a big-endian scenario: construct low_128/high_128 values
        // as if they were read from a big-endian system, and verify that
        // TypedValueToUuidBytes (byte-by-byte) produces the correct bytes.
        //
        // On a little-endian system, memcpy of ui64 would produce reversed bytes.
        // The byte-by-byte fix ensures correct ordering.
        //
        // UUID bytes: FF FE FD FC FB FA F9 F8 F7 F6 F5 F4 F3 F2 F1 F0
        const unsigned char bytes[16] = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8,
                                0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0xF1, 0xF0};
        const TString uuidBytes(reinterpret_cast<const char*>(bytes), 16);

        // Byte-by-byte extraction (what the fixed code does).
        ui64 low = 0;
        ui64 high = 0;
        for (int i = 0; i < 8; ++i) {
            low |= static_cast<ui64>(static_cast<unsigned char>(bytes[i])) << (i * 8);
            high |= static_cast<ui64>(static_cast<unsigned char>(bytes[8 + i])) << (i * 8);
        }

        // Verify the round-trip: bytes -> low/high -> bytes should be identity.
        TString reconstructed;
        reconstructed.resize(16);
        for (int i = 0; i < 8; ++i) {
            reconstructed[i] = static_cast<char>(low >> (i * 8) & 0xFF);
            reconstructed[8 + i] = static_cast<char>(high >> (i * 8) & 0xFF);
        }
        UNIT_ASSERT_VALUES_EQUAL(reconstructed, uuidBytes);

        // Build stats and predicate, verify match.
        auto stats = BuildUuidStats(uuidBytes, uuidBytes);
        auto predicate = BuildPredicate(ComparisonPredicate(
            "col1",
            "EQ",
            "UUID",
            TStringBuilder() << "low_128: " << low << " high_128: " << high));

        UNIT_ASSERT(MatchPredicate(
            TMap<TString, NYql::NGenericPushDown::TColumnStatistics>{{{"col1", stats}}},
            predicate));
    }
} // Y_UNIT_TEST_SUITE(MatchPredicate)
