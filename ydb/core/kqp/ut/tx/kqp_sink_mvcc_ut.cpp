#include "kqp_sink_common.h"

#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/testlib/common_helper.h>
#include <ydb/core/tx/columnshard/hooks/abstract/abstract.h>
#include <ydb/core/tx/columnshard/hooks/testing/controller.h>

namespace NKikimr {
namespace NKqp {

using namespace NYdb;
using namespace NYdb::NQuery;

Y_UNIT_TEST_SUITE(KqpSinkMvcc) {
    class TSnapshotExpiration : public TTableDataModificationTester {
    protected:
        void DoExecute() override {
            auto client = Kikimr->GetQueryClient();

            auto session1 = client.GetSession().GetValueSync().GetSession();
            auto session2 = client.GetSession().GetValueSync().GetSession();

            auto result = session1.ExecuteQuery(Q_(R"(
                SELECT * FROM `/Root/KV` WHERE Key = 1u OR Key = 4000000001u ORDER BY Key;
            )"), TTxControl::BeginTx(TTxSettings::SerializableRW())).ExtractValueSync();

            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [1u;["One"]];
                [4000000001u;["BigOne"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));

            auto tx = result.GetTransaction();

            result = session2.ExecuteQuery(Q_(R"(
                UPSERT INTO `/Root/KV` (Key, Value) VALUES (1u, "ChangedOne");
            )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();

            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

            auto deadline = TInstant::Now() + TDuration::Seconds(30);
            auto caught = false;
            do {
                Sleep(TDuration::Seconds(1));
                auto result = session1.ExecuteQuery(Q_(R"(
                    SELECT * FROM `/Root/KV` WHERE Key = 1u OR Key = 4000000001u;
                )"), TTxControl::Tx(*tx)).ExtractValueSync();
                if (result.GetStatus() == EStatus::SUCCESS)
                    continue;

                UNIT_ASSERT_C(HasIssue(result.GetIssues(), NYql::TIssuesIds::DEFAULT_ERROR,
                    [](const auto& issue){
                        return issue.GetMessage().contains("has no snapshot at");
                    }), result.GetIssues().ToString());

                UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::ABORTED);

                caught = true;
                break;
            } while (TInstant::Now() < deadline);
            UNIT_ASSERT_C(caught, "Failed to wait for snapshot expiration.");
        }
    };

    Y_UNIT_TEST(SnapshotExpiration) {
        TSnapshotExpiration tester;
        tester.SetFastSnapshotExpiration(true);
        tester.SetIsOlap(false);
        tester.Execute();
    }

//     Y_UNIT_TEST(OlapSnapshotExpiration) {
//         TSnapshotExpiration tester;
//         tester.SetFastSnapshotExpiration(true);
//         tester.SetIsOlap(true);
//         tester.Execute();
//     }

    class TReadOnlyTxCommitsOnConcurrentWrite : public TTableDataModificationTester {
    protected:
        void DoExecute() override {
            auto client = Kikimr->GetQueryClient();

            auto session1 = client.GetSession().GetValueSync().GetSession();
            auto session2 = client.GetSession().GetValueSync().GetSession();

            auto result = session1.ExecuteQuery(Q_(R"(
                SELECT * FROM `/Root/KV` WHERE Key = 1u OR Key = 4000000001u ORDER BY Key;
            )"), TTxControl::BeginTx(TTxSettings::SerializableRW())).ExtractValueSync();

            auto tx = result.GetTransaction();

            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [1u;["One"]];
                [4000000001u;["BigOne"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));

            result = session2.ExecuteQuery(Q_(R"(
                UPSERT INTO `/Root/KV` (Key, Value) VALUES (1u, "ChangedOne");
            )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();

            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

            result = session2.ExecuteQuery(Q_(R"(
                SELECT * FROM `/Root/KV` WHERE Key = 1u;
            )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();

            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [1u;["ChangedOne"]];
            ])", FormatResultSetYson(result.GetResultSet(0)));

            result = session1.ExecuteQuery(Q_(R"(
                SELECT * FROM `/Root/KV` WHERE Key = 1u;
            )"), TTxControl::Tx(*tx)).ExtractValueSync();

            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [1u;["One"]];
            ])", FormatResultSetYson(result.GetResultSet(0)));

            result = session1.ExecuteQuery(Q_(R"(
                SELECT * FROM `/Root/KV` WHERE Key = 2u OR Key = 4000000002u ORDER BY Key;
            )"), TTxControl::Tx(*tx).CommitTx()).ExtractValueSync();

            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
                CompareYson(R"([
                [2u;["Two"]];
                [4000000002u;["BigTwo"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));
        }
    };

    Y_UNIT_TEST(ReadOnlyTxCommitsOnConcurrentWrite) {
        TReadOnlyTxCommitsOnConcurrentWrite tester;
        tester.SetIsOlap(false);
        tester.Execute();
    }

    Y_UNIT_TEST(OlapReadOnlyTxCommitsOnConcurrentWrite) {
        TReadOnlyTxCommitsOnConcurrentWrite tester;
        tester.SetIsOlap(true);
        tester.Execute();
    }

    class TReadWriteTxFailsOnConcurrentWrite1 : public TTableDataModificationTester {
    protected:
        void DoExecute() override {
            auto client = Kikimr->GetQueryClient();

            auto session1 = client.GetSession().GetValueSync().GetSession();
            auto session2 = client.GetSession().GetValueSync().GetSession();

            auto result = session1.ExecuteQuery(Q_(R"(
                SELECT * FROM `/Root/KV` WHERE Key = 1u OR Key = 4000000001u ORDER BY Key;
            )"), TTxControl::BeginTx(TTxSettings::SerializableRW())).ExtractValueSync();

            auto tx = result.GetTransaction();

            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [1u;["One"]];
                [4000000001u;["BigOne"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));

            result = session2.ExecuteQuery(Q_(R"(
                UPSERT INTO `/Root/KV` (Key, Value) VALUES (1u, "ChangedOne");
            )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();

            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

            result = session1.ExecuteQuery(Q_(R"(
                UPSERT INTO `/Root/KV` (Key, Value) VALUES (1u, "TwiceChangedOne");
            )"), TTxControl::Tx(*tx).CommitTx()).ExtractValueSync();

            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::ABORTED, result.GetIssues().ToString());
            UNIT_ASSERT_C(HasIssue(result.GetIssues(), NYql::TIssuesIds::KIKIMR_LOCKS_INVALIDATED), result.GetIssues().ToString());
        }
    };

    Y_UNIT_TEST(ReadWriteTxFailsOnConcurrentWrite1) {
        TReadWriteTxFailsOnConcurrentWrite1 tester;
        tester.SetIsOlap(false);
        tester.Execute();
    }

    Y_UNIT_TEST(OlapReadWriteTxFailsOnConcurrentWrite1) {
        TReadWriteTxFailsOnConcurrentWrite1 tester;
        tester.SetIsOlap(true);
        tester.Execute();
    }

    class TReadWriteTxFailsOnConcurrentWrite2 : public TTableDataModificationTester {
    protected:
        void DoExecute() override {
            auto client = Kikimr->GetQueryClient();

            auto session1 = client.GetSession().GetValueSync().GetSession();
            auto session2 = client.GetSession().GetValueSync().GetSession();

            auto result = session1.ExecuteQuery(Q_(R"(
                SELECT * FROM `/Root/KV` WHERE Key = 1u OR Key = 4000000001u ORDER BY Key;
            )"), TTxControl::BeginTx(TTxSettings::SerializableRW())).ExtractValueSync();

            auto tx = result.GetTransaction();

            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [1u;["One"]];
                [4000000001u;["BigOne"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));

            // We need to sleep before the upsert below, otherwise writes
            // might happen in the same step as the snapshot, which would be
            // treated as happening before snapshot and will not break any locks.
            Sleep(TDuration::Seconds(2));

            result = session2.ExecuteQuery(Q_(R"(
                UPSERT INTO `/Root/KV2` (Key, Value) VALUES (101u, "SomeText");
            )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

            result = session1.ExecuteQuery(Q_(R"(
                UPDATE `/Root/KV` SET Value = "Something" WHERE Key = 1u;
                UPDATE `/Root/KV2` SET Value = "AnotherString" WHERE Key = 101u;
            )"), TTxControl::Tx(*tx).CommitTx()).ExtractValueSync();

            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::ABORTED, result.GetIssues().ToString());
            UNIT_ASSERT_C(HasIssue(result.GetIssues(), NYql::TIssuesIds::KIKIMR_LOCKS_INVALIDATED), result.GetIssues().ToString());
        }
    };

    Y_UNIT_TEST(ReadWriteTxFailsOnConcurrentWrite2) {
        TReadWriteTxFailsOnConcurrentWrite2 tester;
        tester.SetIsOlap(false);
        tester.Execute();
    }

    Y_UNIT_TEST(OlapReadWriteTxFailsOnConcurrentWrite2) {
        TReadWriteTxFailsOnConcurrentWrite2 tester;
        tester.SetIsOlap(true);
        tester.Execute();
    }

    class TReadWriteTxFailsOnConcurrentWrite3 : public TTableDataModificationTester {
    protected:
        void DoExecute() override {
            auto client = Kikimr->GetQueryClient();

            auto session1 = client.GetSession().GetValueSync().GetSession();
            auto session2 = client.GetSession().GetValueSync().GetSession();

            auto result = session1.ExecuteQuery(Q_(R"(
                SELECT * FROM `/Root/KV` WHERE Key = 1u OR Key = 4000000001u ORDER BY Key;
            )"), TTxControl::BeginTx(TTxSettings::SerializableRW())).ExtractValueSync();

            auto tx = result.GetTransaction();

            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            CompareYson(R"([
                [1u;["One"]];
                [4000000001u;["BigOne"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));

            result = session2.ExecuteQuery(Q_(R"(
                UPSERT INTO `/Root/KV` (Key, Value) VALUES (2u, "ChangedTwo");
            )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();

            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

            result = session1.ExecuteQuery(Q_(R"(
                SELECT * FROM `/Root/KV` WHERE Key = 2u OR Key = 4000000002u ORDER BY Key;
            )"), TTxControl::Tx(*tx)).ExtractValueSync();

            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
                CompareYson(R"([
                [2u;["Two"]];
                [4000000002u;["BigTwo"]]
            ])", FormatResultSetYson(result.GetResultSet(0)));

            result = session1.ExecuteQuery(Q_(R"(
                UPSERT INTO `/Root/KV` (Key, Value) VALUES (2u, "TwiceChangedTwo");
            )"), TTxControl::Tx(*tx).CommitTx()).ExtractValueSync();

            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::ABORTED, result.GetIssues().ToString());
            UNIT_ASSERT_C(HasIssue(result.GetIssues(), NYql::TIssuesIds::KIKIMR_LOCKS_INVALIDATED), result.GetIssues().ToString());
        }
    };

    Y_UNIT_TEST(ReadWriteTxFailsOnConcurrentWrite3) {
        TReadWriteTxFailsOnConcurrentWrite3 tester;
        tester.SetIsOlap(false);
        tester.Execute();
    }

    Y_UNIT_TEST(OlapReadWriteTxFailsOnConcurrentWrite3) {
        TReadWriteTxFailsOnConcurrentWrite3 tester;
        tester.SetIsOlap(true);
        tester.Execute();
    }

    class TNamedStatement : public TTableDataModificationTester {
    protected:
        void DoExecute() override {
            auto client = Kikimr->GetQueryClient();

            auto session1 = client.GetSession().GetValueSync().GetSession();

            {
                auto result = session1.ExecuteQuery(Q_(R"(
                    $data = SELECT * FROM `/Root/KV`;
                    DELETE FROM `/Root/KV` WHERE 1=1;
                    SELECT COUNT(*) FROM `/Root/KV`;
                    SELECT COUNT(*) FROM $data;
                    DELETE FROM `/Root/KV` ON SELECT 424242u AS Key, "One" As Value;
                    UPSERT INTO `/Root/KV` (Key, Value) VALUES (424242u, "One");
                    SELECT COUNT(*) FROM `/Root/KV`;
                    SELECT COUNT(*) FROM $data;
                )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();
                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
                CompareYson(R"([[0u]])", FormatResultSetYson(result.GetResultSet(0)));
                CompareYson(R"([[0u]])", FormatResultSetYson(result.GetResultSet(1)));
                CompareYson(R"([[1u]])", FormatResultSetYson(result.GetResultSet(2)));
                CompareYson(R"([[1u]])", FormatResultSetYson(result.GetResultSet(3)));
            }
        }
    };

    Y_UNIT_TEST(OltpNamedStatementNoSink) {
        TNamedStatement tester;
        tester.SetDisableSinks(true);
        tester.SetIsOlap(false);
        tester.Execute();
    }

    Y_UNIT_TEST(OltpNamedStatement) {
        TNamedStatement tester;
        tester.SetIsOlap(false);
        tester.Execute();
    }

    Y_UNIT_TEST(OlapNamedStatement) {
        TNamedStatement tester;
        tester.SetIsOlap(true);
        tester.Execute();
    }

    class TMultiSinks: public TTableDataModificationTester {
    protected:
        void DoExecute() override {
            auto client = Kikimr->GetQueryClient();

            auto session1 = client.GetSession().GetValueSync().GetSession();

            {
                auto result = session1.ExecuteQuery(Q_(R"(
                    UPSERT INTO `/Root/KV` (Key, Value) VALUES (1u, "1");
                    UPSERT INTO `/Root/KV` (Key, Value) VALUES (1u, "2");
                )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();
                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
            }
            {
                auto result = session1.ExecuteQuery(Q_(R"(
                    SELECT Value FROM `/Root/KV` WHERE Key = 1u;
                )"), TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).ExtractValueSync();
                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
                CompareYson(R"([[["2"]]])", FormatResultSetYson(result.GetResultSet(0)));
            }
        }
    };

    Y_UNIT_TEST(OltpMultiSinksNoSinks) {
        TMultiSinks tester;
        tester.SetDisableSinks(true);
        tester.SetIsOlap(false);
        tester.Execute();
    }

    Y_UNIT_TEST(OltpMultiSinks) {
        TMultiSinks tester;
        tester.SetIsOlap(false);
        tester.Execute();
    }

    Y_UNIT_TEST(OlapMultiSinks) {
        TMultiSinks tester;
        tester.SetIsOlap(true);
        tester.Execute();
    }
}

} // namespace NKqp
} // namespace NKikimr
