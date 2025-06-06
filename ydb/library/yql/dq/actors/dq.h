#pragma once

#include <ydb/library/yql/dq/actors/protos/dq_events.pb.h>
#include <ydb/library/yql/dq/actors/dq_events_ids.h>
#include <yql/essentials/public/issue/yql_issue.h>
#include <yql/essentials/public/issue/yql_issue_message.h>
#include <yql/essentials/utils/chunked_buffer.h>

#include <ydb/public/api/protos/ydb_status_codes.pb.h>
#include <ydb/library/yql/dq/actors/protos/dq_status_codes.pb.h>

namespace NYql::NDq {

enum class EStatusCompatibilityLevel {
    Basic,
    WithUnauthorized
};

Ydb::StatusIds::StatusCode DqStatusToYdbStatus(NYql::NDqProto::StatusIds::StatusCode statusCode);
NYql::NDqProto::StatusIds::StatusCode YdbStatusToDqStatus(Ydb::StatusIds::StatusCode statusCode, EStatusCompatibilityLevel compatibility = EStatusCompatibilityLevel::Basic);
TMaybe<NYql::NDqProto::StatusIds::StatusCode> GetDqStatus(const TIssue& issue);

struct TEvDq {

    struct TEvAbortExecution : public NActors::TEventPB<TEvAbortExecution, NDqProto::TEvAbortExecution, TDqEvents::EvAbortExecution> {
        static THolder<TEvAbortExecution> Unavailable(const TString& s, const TIssues& subIssues = {}) {
            return MakeHolder<TEvAbortExecution>(NYql::NDqProto::StatusIds::UNAVAILABLE, s, subIssues);
        }

        static THolder<TEvAbortExecution> InternalError(const TString& s, const TIssues& subIssues = {}) {
            return MakeHolder<TEvAbortExecution>(NYql::NDqProto::StatusIds::INTERNAL_ERROR, s, subIssues);
        }

        static THolder<TEvAbortExecution> Aborted(const TString& s, const TIssues& subIssues = {}) {
            return MakeHolder<TEvAbortExecution>(NYql::NDqProto::StatusIds::ABORTED, s, subIssues);
        }

        TEvAbortExecution() = default;

        TEvAbortExecution(TEvAbortExecution&&) = default;

        TEvAbortExecution(const TEvAbortExecution&) = default;

        TEvAbortExecution(NYql::NDqProto::StatusIds::StatusCode code, const TIssues& issues) {
            Record.SetStatusCode(code);
            Record.SetYdbStatusCode(DqStatusToYdbStatus(code));
            IssuesToMessage(issues, Record.MutableIssues());
        }

        TEvAbortExecution(NYql::NDqProto::StatusIds::StatusCode code, const TString& message, const TIssues& subIssues = {}) {
            Record.SetStatusCode(code);
            Record.SetYdbStatusCode(DqStatusToYdbStatus(code));
            Record.SetLegacyMessage(message);
            TIssue issue(message);
            for (const TIssue& i : subIssues) {
                issue.AddSubIssue(MakeIntrusive<TIssue>(i));
            }
            TIssues issues;
            issues.AddIssue(std::move(issue));
            IssuesToMessage(issues, Record.MutableIssues());
        }

        TIssues GetIssues() const {
            TIssues issues;
            if (Record.IssuesSize()) {
                IssuesFromMessage(Record.GetIssues(), issues);
            } else if (const TString& msg = Record.GetLegacyMessage()) {
                issues.AddIssue(msg);
            }
            return issues;
        }

        static TEvAbortExecution* Load(const NActors::TEventSerializedData *input) {
            auto result = NActors::TEventPB<TEvAbortExecution, NDqProto::TEvAbortExecution, TDqEvents::EvAbortExecution>::Load(input);
            if (result) {
                auto dqStatus = result->Record.GetStatusCode();
                auto ydbStatus = result->Record.GetYdbStatusCode();
                if (dqStatus == NYql::NDqProto::StatusIds::UNSPECIFIED && ydbStatus != Ydb::StatusIds::STATUS_CODE_UNSPECIFIED) {
                    result->Record.SetStatusCode(YdbStatusToDqStatus(ydbStatus));
                }
            }
            return result;
        }
    };

};

struct TChannelDataOOB {
    NDqProto::TChannelData Proto;
    TChunkedBuffer Payload;

    size_t PayloadSize() const {
        return Proto.GetData().GetRaw().size() + Payload.Size();
    }

    ui32 ChunkCount() const {
        return Proto.GetData().GetChunks();
    }
};

} // namespace NYql::NDq
