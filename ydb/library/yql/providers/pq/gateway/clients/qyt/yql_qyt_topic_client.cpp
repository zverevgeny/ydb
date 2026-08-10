#include "yql_qyt_topic_client.h"
#include "yql_qyt_blocking_queue.h"

#include <library/cpp/threading/future/async.h>

#include <yt/yt/client/api/client.h>
#include <yt/yt/client/api/queue_client.h>
#include <yt/yt/client/api/transaction.h>
#include <yt/yt/client/queue_client/consumer_client.h>
#include <yt/yt/client/queue_client/producer_client.h>
#include <yt/yt/client/queue_client/queue_rowset.h>
#include <yt/yt/client/table_client/name_table.h>
#include <yt/yt/client/table_client/row_buffer.h>
#include <yt/yt/client/table_client/unversioned_row.h>
#include <yt/yt/client/ypath/rich.h>

#include <yt/yt/core/actions/future.h>

#include <util/generic/yexception.h>
#include <util/string/builder.h>

#include <thread>

namespace NYql {

namespace {

using namespace NYdb;
using namespace NYdb::NTopic;

using NYT::NApi::IClientPtr;
using NYT::NApi::ITransactionPtr;
using NYT::NQueueClient::IQueueRowsetPtr;
using NYT::NQueueClient::TQueueRowBatchReadOptions;
using NYT::NTableClient::TUnversionedRow;
using NYT::NTableClient::TUnversionedValue;

////////////////////////////////////////////////////////////////////////////////

TString JoinYtPath(const TString& prefix, const TString& path) {
    if (prefix.empty()) {
        return path;
    }
    if (path.StartsWith('/')) {
        return path;
    }
    TStringBuilder result;
    result << prefix;
    if (!prefix.EndsWith('/')) {
        result << '/';
    }
    result << path;
    return result;
}

////////////////////////////////////////////////////////////////////////////////

// A minimal partition session used to tag read messages with topic/partition
// metadata. It does not talk to any server: offset management for QYT queues is
// done via advance_queue_consumer in TQytTopicClient::CommitOffset.
struct TQytPartitionSession final : public TPartitionSessionControl {
    TQytPartitionSession(ui64 sessionId, const TString& topicPath, ui64 partitionId) {
        PartitionSessionId = sessionId;
        TopicPath = topicPath;
        PartitionId = partitionId;
    }

    void RequestStatus() override {
    }

    void Commit(uint64_t /*startOffset*/, uint64_t /*endOffset*/) override {
    }

    void ConfirmCreate(std::optional<uint64_t> /*readOffset*/, std::optional<uint64_t> /*commitOffset*/, std::optional<uint64_t> /*maxOffset*/) override {
    }

    void ConfirmDestroy() override {
    }

    void ConfirmEnd(std::span<const uint32_t> /*childIds*/) override {
    }
};

////////////////////////////////////////////////////////////////////////////////

// Emulates a YDB topic read session on top of the YTsaurus queue pull API.
//
// A background thread polls pull_queue_consumer starting from the current
// offset, converts each queue row into a TReadSessionEvent::TDataReceivedEvent
// message and pushes it into a bounded blocking queue. WaitEvent()/GetEvent()
// drain that queue, mirroring the SDK's push interface so that the existing
// TDqPqReadActor works unchanged.
class TQytTopicReadSession final : public IReadSession {
    using TEQueue = TBlockingEQueue<TReadSessionEvent::TEvent>;
    using TMessageInformation = TReadSessionEvent::TDataReceivedEvent::TMessageInformation;
    using TMessage = TReadSessionEvent::TDataReceivedEvent::TMessage;

public:
    TQytTopicReadSession(
        IClientPtr client,
        NYT::NYPath::TRichYPath queuePath,
        NYT::NYPath::TRichYPath consumerPath,
        TPartitionSession::TPtr session,
        int partitionIndex,
        i64 startOffset,
        TString dataColumn,
        TQueueRowBatchReadOptions readOptions,
        TDuration pollPeriod,
        size_t maxMemoryBytes)
        : Client(std::move(client))
        , QueuePath(std::move(queuePath))
        , ConsumerPath(std::move(consumerPath))
        , Session(std::move(session))
        , PartitionIndex(partitionIndex)
        , Offset(startOffset)
        , DataColumn(std::move(dataColumn))
        , ReadOptions(readOptions)
        , PollPeriod(pollPeriod)
        , EventsQ(maxMemoryBytes)
        , Poller([this]() { PollLoop(); })
    {
        Pool.Start(1);
    }

    ~TQytTopicReadSession() override {
        try {
            Cleanup();
        } catch (...) {
        }
    }

    NThreading::TFuture<void> WaitEvent() final {
        return NThreading::Async([this]() {
            EventsQ.BlockUntilEvent();
            return NThreading::MakeFuture();
        }, Pool);
    }

    std::vector<TReadSessionEvent::TEvent> GetEvents(bool block, std::optional<size_t> maxEventsCount, size_t maxByteSize) final {
        Y_UNUSED(maxByteSize);

        std::vector<TReadSessionEvent::TEvent> res;
        for (auto event = EventsQ.Pop(block);
             event.has_value() && res.size() < maxEventsCount.value_or(std::numeric_limits<size_t>::max());
             event = EventsQ.Pop(/* block */ false))
        {
            res.push_back(std::move(*event));
        }
        return res;
    }

    std::vector<TReadSessionEvent::TEvent> GetEvents(const TReadSessionGetEventSettings& settings) final {
        return GetEvents(settings.Block_, settings.MaxEventsCount_, settings.MaxByteSize_);
    }

    std::optional<TReadSessionEvent::TEvent> GetEvent(bool block, size_t maxByteSize) final {
        Y_UNUSED(maxByteSize);
        return EventsQ.Pop(block);
    }

    std::optional<TReadSessionEvent::TEvent> GetEvent(const TReadSessionGetEventSettings& settings) final {
        return GetEvent(settings.Block_, settings.MaxByteSize_);
    }

    bool Close(TDuration timeout) final {
        Y_UNUSED(timeout);
        Cleanup();
        return true;
    }

    TReaderCounters::TPtr GetCounters() const final {
        return nullptr;
    }

    std::string GetSessionId() const final {
        return ToString(Session->GetPartitionSessionId());
    }

private:
    TMessage MakeMessage(const TString& data, i64 offset) {
        const auto now = TInstant::Now();
        TMessageInformation info(
            offset,
            /* producerId */ "",
            /* seqNo */ static_cast<ui64>(offset),
            /* createTime */ now,
            /* writeTime */ now,
            MakeIntrusive<TWriteSessionMeta>(),
            MakeIntrusive<TMessageMeta>(),
            data.size(),
            /* messageGroupId */ ""
        );
        return TMessage(data, nullptr, info, Session);
    }

    // Extracts the payload of a single queue row as a string. If a DataColumn is
    // configured it returns that column's value, otherwise it concatenates the
    // string-like values (which covers the common single-column queue layout).
    TString ExtractRowData(const NYT::NTableClient::TNameTablePtr& nameTable, TUnversionedRow row) {
        std::optional<int> dataId;
        if (!DataColumn.empty()) {
            dataId = nameTable->FindId(DataColumn);
        }

        TStringBuilder data;
        for (const auto& value : row) {
            if (dataId && value.Id != static_cast<ui16>(*dataId)) {
                continue;
            }
            if (value.Type == NYT::NTableClient::EValueType::String) {
                data << TStringBuf(value.Data.String, value.Length);
            }
        }
        return data;
    }

    void PollLoop() {
        while (!EventsQ.IsStopped()) {
            IQueueRowsetPtr rowset;
            try {
                auto future = Client->PullQueueConsumer(
                    ConsumerPath,
                    QueuePath,
                    Offset,
                    PartitionIndex,
                    ReadOptions);
                rowset = NYT::NConcurrency::WaitFor(future).ValueOrThrow();
            } catch (const std::exception& ex) {
                if (EventsQ.IsStopped()) {
                    break;
                }
                EventsQ.Push(TSessionClosedEvent(EStatus::INTERNAL_ERROR,
                    {NIssue::TIssue(TStringBuilder() << "QYT queue pull failed: " << ex.what())}), 0);
                break;
            }

            const auto rows = rowset->GetRows();
            if (rows.empty()) {
                Sleep(PollPeriod);
                continue;
            }

            const auto& nameTable = rowset->GetNameTable();
            i64 rowOffset = rowset->GetStartOffset();

            TVector<TMessage> msgs;
            msgs.reserve(rows.size());
            size_t batchSize = 0;
            for (auto row : rows) {
                TString data = ExtractRowData(nameTable, row);
                batchSize += data.size();
                msgs.emplace_back(MakeMessage(data, rowOffset));
                ++rowOffset;
            }

            Offset = rowset->GetFinishOffset();

            EventsQ.Push(TReadSessionEvent::TDataReceivedEvent(msgs, {}, Session), batchSize);
        }
    }

    void Cleanup() {
        EventsQ.Stop();
        Pool.Stop();
        if (Poller.joinable()) {
            Poller.join();
        }
    }

    const IClientPtr Client;
    const NYT::NYPath::TRichYPath QueuePath;
    const NYT::NYPath::TRichYPath ConsumerPath;
    const TPartitionSession::TPtr Session;
    const int PartitionIndex;
    std::atomic<i64> Offset;
    const TString DataColumn;
    const TQueueRowBatchReadOptions ReadOptions;
    const TDuration PollPeriod;
    TEQueue EventsQ;
    TThreadPool Pool;
    std::thread Poller;
};

////////////////////////////////////////////////////////////////////////////////

// Emulates a YDB topic write session on top of a YTsaurus queue producer
// session. Incoming write messages are queued and flushed by a background
// thread that appends unversioned rows via the producer session writer,
// issuing TAcksEvent/TReadyToAcceptEvent to preserve the SDK contract.
class TQytTopicWriteSession final : public IWriteSession, private TContinuationTokenIssuer {
    struct TOwningWriteMessage {
        explicit TOwningWriteMessage(TWriteMessage&& msg)
            : Content(msg.Data)
            , Msg(std::move(msg))
        {
            Msg.Data = Content;
        }

        TString Content;
        TWriteMessage Msg;
    };

    using TMsgQueue = TBlockingEQueue<TOwningWriteMessage>;
    using TEQueue = TBlockingEQueue<TWriteSessionEvent::TEvent>;

public:
    TQytTopicWriteSession(
        NYT::NQueueClient::IProducerSessionPtr producerSession,
        NYT::NTableClient::TNameTablePtr nameTable,
        int dataColumnId)
        : ProducerSession(std::move(producerSession))
        , NameTable(std::move(nameTable))
        , DataColumnId(dataColumnId)
        , RowBuffer(NYT::New<NYT::NTableClient::TRowBuffer>())
        , Writer([this]() { WriteLoop(); })
    {
        Pool.Start(1);
        SeqNo = static_cast<uint64_t>(ProducerSession->GetLastSequenceNumber().Underlying());
        EventsQ.Push(TWriteSessionEvent::TReadyToAcceptEvent(IssueContinuationToken()));
    }

    ~TQytTopicWriteSession() override {
        try {
            Cleanup();
        } catch (...) {
        }
    }

    NThreading::TFuture<void> WaitEvent() final {
        return NThreading::Async([this]() {
            EventsQ.BlockUntilEvent();
            return NThreading::MakeFuture();
        }, Pool);
    }

    std::optional<TWriteSessionEvent::TEvent> GetEvent(bool block) final {
        return EventsQ.Pop(block);
    }

    std::vector<TWriteSessionEvent::TEvent> GetEvents(bool block, std::optional<size_t> maxEventsCount) final {
        std::vector<TWriteSessionEvent::TEvent> res;
        for (auto event = EventsQ.Pop(block);
             event.has_value() && res.size() < maxEventsCount.value_or(std::numeric_limits<size_t>::max());
             event = EventsQ.Pop(/* block */ false))
        {
            res.push_back(std::move(*event));
        }
        return res;
    }

    NThreading::TFuture<uint64_t> GetInitSeqNo() final {
        return NThreading::MakeFuture(SeqNo);
    }

    void Write(TContinuationToken&&, TWriteMessage&& message, TTransactionBase* tx) final {
        Y_UNUSED(tx);
        const auto size = message.Data.size();
        EventsMsgQ.Push(TOwningWriteMessage(std::move(message)), size);
    }

    void Write(TContinuationToken&& token, std::string_view data, std::optional<uint64_t> seqNo, std::optional<TInstant> createTimestamp) final {
        TWriteMessage message(data);
        if (seqNo.has_value()) {
            message.SeqNo(*seqNo);
        }
        if (createTimestamp.has_value()) {
            message.CreateTimestamp(*createTimestamp);
        }
        Write(std::move(token), std::move(message), nullptr);
    }

    void WriteEncoded(TContinuationToken&& token, TWriteMessage&& params, TTransactionBase* tx) final {
        Y_UNUSED(tx);
        TWriteMessage message(params.Data);
        if (params.CreateTimestamp_.has_value()) {
            message.CreateTimestamp(*params.CreateTimestamp_);
        }
        if (params.SeqNo_) {
            message.SeqNo(*params.SeqNo_);
        }
        message.MessageMeta(params.MessageMeta_);
        Write(std::move(token), std::move(message), nullptr);
    }

    void WriteEncoded(TContinuationToken&& token, std::string_view data, ECodec codec, uint32_t originalSize, std::optional<uint64_t> seqNo, std::optional<TInstant> createTimestamp) final {
        Y_UNUSED(codec, originalSize);
        TWriteMessage message(data);
        if (seqNo.has_value()) {
            message.SeqNo(*seqNo);
        }
        if (createTimestamp.has_value()) {
            message.CreateTimestamp(*createTimestamp);
        }
        Write(std::move(token), std::move(message), nullptr);
    }

    bool Close(TDuration timeout) final {
        Y_UNUSED(timeout);
        Cleanup();
        return true;
    }

    TWriterCounters::TPtr GetCounters() final {
        return nullptr;
    }

private:
    void WriteLoop() {
        while (auto maybeMsg = EventsMsgQ.Pop(true)) {
            TWriteSessionEvent::TAcksEvent acks;
            std::vector<TUnversionedRow> rows;

            do {
                auto& [content, msg] = *maybeMsg;

                NYT::NTableClient::TUnversionedRowBuilder builder;
                builder.AddValue(NYT::NTableClient::MakeUnversionedStringValue(
                    TStringBuf(content.data(), content.size()), DataColumnId));
                rows.push_back(RowBuffer->CaptureRow(builder.GetRow()));

                TWriteSessionEvent::TWriteAck ack;
                ack.SeqNo = msg.SeqNo_.value_or(++SeqNo);
                ack.State = TWriteSessionEvent::TWriteAck::EES_WRITTEN;
                ack.Details.emplace(static_cast<ui64>(rows.size() - 1), 0);
                acks.Acks.emplace_back(std::move(ack));
            } while ((maybeMsg = EventsMsgQ.Pop(false)));

            try {
                Y_UNUSED(ProducerSession->Write(NYT::TRange<TUnversionedRow>(rows.data(), rows.size())));
                NYT::NConcurrency::WaitFor(ProducerSession->Flush()).ThrowOnError();
            } catch (const std::exception& ex) {
                EventsQ.Push(TSessionClosedEvent(EStatus::INTERNAL_ERROR,
                    {NIssue::TIssue(TStringBuilder() << "QYT queue push failed: " << ex.what())}), 0);
                break;
            }

            RowBuffer->Clear();

            const auto acksSize = acks.Acks.size();
            EventsQ.Push(std::move(acks), 1 + acksSize);
            EventsQ.Push(TWriteSessionEvent::TReadyToAcceptEvent(IssueContinuationToken()), 1);

            if (EventsQ.IsStopped()) {
                break;
            }
        }
    }

    void Cleanup() {
        EventsQ.Stop();
        EventsMsgQ.Stop();
        Pool.Stop();
        if (Writer.joinable()) {
            Writer.join();
        }
        try {
            ProducerSession->Cancel();
        } catch (...) {
        }
    }

    const NYT::NQueueClient::IProducerSessionPtr ProducerSession;
    const NYT::NTableClient::TNameTablePtr NameTable;
    const int DataColumnId;
    const NYT::NTableClient::TRowBufferPtr RowBuffer;
    TMsgQueue EventsMsgQ = TMsgQueue(4 << 20);
    TEQueue EventsQ = TEQueue(128 << 10);
    TThreadPool Pool;
    std::thread Writer;
    uint64_t SeqNo = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TQytTopicClient final : public ITopicClient {
    public:
    explicit TQytTopicClient(const TQytTopicClientSettings& settings)
        : Settings(settings)
    {
        Y_ENSURE(Settings.Client, "YT client must be provided for YT topic client");
    }


    TAsyncDescribeTopicResult DescribeTopic(const TString& path, const TDescribeTopicSettings& settings) final {
        Y_UNUSED(settings);
        try {
            const auto tabletCount = GetTabletCount(path);
            Ydb::Topic::DescribeTopicResult describeResult;
            for (i64 partition = 0; partition < tabletCount; ++partition) {
                auto& partitionInfo = *describeResult.add_partitions();
                partitionInfo.set_partition_id(partition);
                partitionInfo.set_active(true);
            }
            return NThreading::MakeFuture(TDescribeTopicResult(
                TStatus(EStatus::SUCCESS, {}), std::move(describeResult)));
        } catch (const std::exception& ex) {
            NYdb::NIssue::TIssues issues;
            issues.AddIssue(TStringBuilder() << "Failed to describe YT queue " << path << ": " << ex.what());
            return NThreading::MakeFuture(TDescribeTopicResult(
                TStatus(EStatus::INTERNAL_ERROR, std::move(issues)), {}));
        }
    }

    TAsyncDescribeConsumerResult DescribeConsumer(const TString& path, const TString& consumer, const TDescribeConsumerSettings& settings) final {
        Y_UNUSED(path, consumer, settings);
        return NThreading::MakeFuture(TDescribeConsumerResult(TStatus(EStatus::SUCCESS, {}), {}));
    }

    TAsyncDescribePartitionResult DescribePartition(const TString& path, i64 partitionId, const TDescribePartitionSettings& settings) final {
        Y_UNUSED(path, partitionId, settings);
        return NThreading::MakeFuture(TDescribePartitionResult(TStatus(EStatus::SUCCESS, {}), {}));
    }

    std::shared_ptr<IReadSession> CreateReadSession(const TReadSessionSettings& settings) final {
        Y_ENSURE(!settings.Topics_.empty(), "YT topic read session requires a topic");
        const auto& topic = settings.Topics_.front();
        const TString topicPath(topic.Path_);
        Y_ENSURE(!settings.WithoutConsumer_ && !settings.ConsumerName_.empty(),
            "YT topic read session requires a consumer");

        const int partitionIndex = topic.PartitionIds_.empty()
            ? 0
            : static_cast<int>(topic.PartitionIds_.front());

        NYT::NYPath::TRichYPath queuePath(ResolvePath(topicPath));
        NYT::NYPath::TRichYPath consumerPath(ResolvePath(TString(settings.ConsumerName_)));

        const i64 startOffset = GetConsumerOffset(consumerPath, queuePath, partitionIndex);

        TQueueRowBatchReadOptions readOptions;
        readOptions.MaxRowCount = Settings.MaxRowCount;
        readOptions.MaxDataWeight = Settings.MaxDataWeight;

        auto partitionSession = MakeIntrusive<TQytPartitionSession>(
            static_cast<ui64>(partitionIndex), topicPath, static_cast<ui64>(partitionIndex));

        return std::make_shared<TQytTopicReadSession>(
            Settings.Client,
            queuePath,
            consumerPath,
            partitionSession,
            partitionIndex,
            startOffset,
            Settings.DataColumn,
            readOptions,
            TDuration::MilliSeconds(Settings.PollPeriodMs),
            settings.MaxMemoryUsageBytes_);
    }

    std::shared_ptr<ISimpleBlockingWriteSession> CreateSimpleBlockingWriteSession(const TWriteSessionSettings& settings) final {
        Y_UNUSED(settings);
        return nullptr;
    }

    std::shared_ptr<IWriteSession> CreateWriteSession(const TWriteSessionSettings& settings) final {
        const TString queuePathStr(settings.Path_);
        NYT::NYPath::TRichYPath queuePath(ResolvePath(queuePathStr));

        // The producer table is derived from the queue path unless the producer id
        // encodes an explicit producer path. We use "<queue>.producer" by default.
        const TString producerId = settings.ProducerId_.empty()
            ? (queuePathStr + ".producer")
            : TString(settings.ProducerId_);
        NYT::NYPath::TRichYPath producerPath(ResolvePath(producerId));

        auto nameTable = NYT::New<NYT::NTableClient::TNameTable>();
        const int dataColumnId = nameTable->GetIdOrRegisterName(Settings.DataColumn);

        auto producerClient = NYT::NQueueClient::CreateProducerClient(Settings.Client, producerPath);

        NYT::NQueueClient::TProducerSessionOptions producerOptions;
        producerOptions.AutoSequenceNumber = true;

        const NYT::NQueueClient::TQueueProducerSessionId sessionId(
            settings.MessageGroupId_.empty()
                ? std::string(producerId)
                : std::string(settings.MessageGroupId_));

        auto session = NYT::NConcurrency::WaitFor(producerClient->CreateSession(
            queuePath, nameTable, sessionId, producerOptions)).ValueOrThrow();

        return std::make_shared<TQytTopicWriteSession>(session, nameTable, dataColumnId);
    }

    TAsyncStatus CommitOffset(const TString& path, ui64 partitionId, const TString& consumerName, ui64 offset, const TCommitOffsetSettings& settings) final {
        Y_UNUSED(settings);
        try {
            NYT::NYPath::TRichYPath queuePath(ResolvePath(path));
            NYT::NYPath::TRichYPath consumerPath(ResolvePath(consumerName));

            auto txn = NYT::NConcurrency::WaitFor(
                Settings.Client->StartTransaction(
                    NYT::NTransactionClient::ETransactionType::Tablet)).ValueOrThrow();

            NYT::NConcurrency::WaitFor(txn->AdvanceQueueConsumer(
                consumerPath,
                queuePath,
                static_cast<int>(partitionId),
                /* oldOffset */ std::nullopt,
                static_cast<i64>(offset))).ThrowOnError();

            NYT::NConcurrency::WaitFor(txn->Commit()).ThrowOnError();
            return MakeSuccess();
        } catch (const std::exception& ex) {
            return MakeError(EStatus::INTERNAL_ERROR,
                TStringBuilder() << "YT advance consumer failed: " << ex.what());
        }
    }

private:
    TString ResolvePath(const TString& path) const {
        return JoinYtPath(Settings.PathPrefix, path);
    }

    i64 GetConsumerOffset(const NYT::NYPath::TRichYPath& consumerPath, const NYT::NYPath::TRichYPath& queuePath, int partitionIndex) {
        auto subConsumer = NYT::NQueueClient::CreateSubConsumerClient(
            Settings.Client, Settings.Client, consumerPath, queuePath);
        auto partitions = NYT::NConcurrency::WaitFor(
            subConsumer->CollectPartitions(std::vector<int>{partitionIndex})).ValueOrThrow();
        for (const auto& partition : partitions) {
            if (partition.PartitionIndex == partitionIndex) {
                return partition.NextRowIndex < 0 ? 0 : partition.NextRowIndex;
            }
        }
        return 0;
    }

    i64 GetTabletCount(const TString& path) {
        const auto attrPath = ResolvePath(path) + "/@tablet_count";
        auto node = NYT::NConcurrency::WaitFor(
            Settings.Client->GetNode(attrPath)).ValueOrThrow();
        return NYT::NYTree::ConvertTo<i64>(node);
    }

    static TAsyncStatus MakeSuccess() {
        return NThreading::MakeFuture(TStatus(EStatus::SUCCESS, {}));
    }

    static TAsyncStatus MakeError(EStatus status, const TString& message) {
        NYdb::NIssue::TIssues issues;
        issues.AddIssue(message);
        return NThreading::MakeFuture(TStatus(status, std::move(issues)));
    }

    const TQytTopicClientSettings Settings;
};

} // anonymous namespace

ITopicClient::TPtr CreateQytTopicClient(const TQytTopicClientSettings& settings) {
    return MakeIntrusive<TQytTopicClient>(settings);
}

} // namespace NYql
