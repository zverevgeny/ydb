#pragma once

#include <ydb/library/yql/providers/pq/gateway/abstract/yql_pq_topic_client.h>

#include <yt/yt/client/api/public.h>

#include <util/generic/string.h>

namespace NYql {

// Settings for a topic client backed by a YTsaurus queue.
//
// The client emulates the YDB topic read/write session (push) model on top of
// the YTsaurus queue (pull) API:
//   * read  session  -> pull_queue_consumer polling loop
//   * write session  -> queue producer session
//   * CommitOffset    -> advance_queue_consumer (with CAS on the previous offset)
struct TYtTopicClientSettings {
    // Authenticated client for the YT cluster that hosts the queue.
    NYT::NApi::IClientPtr Client;

    // Optional path prefix (YT directory) that is prepended to relative topic paths.
    TString PathPrefix;

    // Name of the column that carries the message payload inside a queue row.
    TString DataColumn = "data";

    // Batch limits for a single pull_queue_consumer request.
    i64 MaxRowCount = 1000;
    i64 MaxDataWeight = 16 << 20; // 16 MB

    // Poll period used when the queue has no new rows to return.
    ui64 PollPeriodMs = 50;
};

ITopicClient::TPtr CreateYtTopicClient(const TYtTopicClientSettings& settings);

} // namespace NYql
