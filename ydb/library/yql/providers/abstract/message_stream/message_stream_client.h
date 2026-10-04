#pragma once

#include "message_stream_session.h"

#include <library/cpp/threading/future/core/future.h>

#include <util/datetime/base.h>
#include <util/generic/string.h>

#include <memory>
#include <optional>
#include <vector>

namespace NFq {

// Backend-neutral message stream: YDB and Logbroker topics, YT queues, Kafka topics.
// Applicability: basic reads and offset storage fit all three backends; consumer
// discovery, timestamps, generations and transactional commits are not equivalent.
// Backend adapters must document unsupported capabilities and consistency guarantees.
// YDB below also denotes the existing Logbroker topic adapter.
// References for the YT and Kafka mappings below:
// https://ytsaurus.tech/docs/en/user-guide/dynamic-tables/queues
// https://kafka.apache.org/41/javadoc/org/apache/kafka/clients/consumer/KafkaConsumer.html
// https://kafka.apache.org/41/javadoc/org/apache/kafka/clients/admin/Admin.html

// Applicable to all three backends; this envelope has no per-partition error channel.
// Adapters must not silently turn partial failures into a complete successful result.
template <class TValue>
struct TMessageStreamResult {
    // All backends: normalize backend errors; the enum cannot preserve every native category.
    EMessageStreamStatus Status = EMessageStreamStatus::Success;
    // All backends: retain native error codes/context in diagnostics where normalization loses them.
    NYql::TIssues Issues;
    // All backends: inspect only on success; optional fields can still be unavailable.
    TValue Value;

    // All backends: operation success, not a guarantee that all optional metadata is present.
    bool IsSuccess() const {
        return Status == EMessageStreamStatus::Success;
    }
};

// Logical offset owner in all three backends, not an individual reader process.
struct TMessageStreamConsumer {
    // YDB: topic consumer name. YT: consumer table path, potentially cluster-qualified.
    // Kafka: group.id.
    TString Name;
};

// YDB: topic description. YT: queue metadata. Kafka: topic metadata; consumer
// discovery needs extra work.
struct TMessageStreamTopicDescription {
    // YDB/Kafka: partition count. YT: tablet count. A count alone does not enumerate IDs.
    ui64 PartitionsCount = 0;
    // YDB: configured topic consumers. YT: queue registrations.
    // Kafka: no equivalent topic-owned consumer registry;
    // enumerating groups and their offsets/assignments is permission-dependent and
    // not an authoritative list. This field lacks an unavailable/incomplete marker.
    std::vector<TMessageStreamConsumer> Consumers;
};

// Per-stream consumer progress; metadata and offsets need not form an atomic snapshot.
struct TMessageStreamConsumerPartition {
    // YDB: partition ID. YT: tablet_index. Kafka: nonnegative partition number.
    // Range-check conversions to backend-specific integer types.
    ui64 PartitionId = 0;
    // YDB: partition statistics start offset. YT: lower_row_index after trimming.
    // Kafka: beginning offset; compaction may
    // leave gaps, so this is a lower bound, not necessarily an existing record.
    std::optional<ui64> StartOffset;
    // All backends: next unprocessed position, not the last processed record's offset.
    // Missing progress must remain distinguishable from a stored zero.
    std::optional<ui64> CommittedOffset;
    // Exclusive upper bound: YDB partition statistics end offset; YT upper_row_index;
    // Kafka high watermark or last stable
    // offset for read_committed. Adapter isolation must match the read session.
    std::optional<ui64> EndOffset;
    // YDB: partition statistics last write time. YT: last_row_commit_time where available.
    // Kafka: no direct equivalent in topic
    // metadata; record timestamps may be producer times. Leave unset without a
    // documented equivalent; a maximum record timestamp is not the last write time.
    std::optional<TInstant> LastWriteTime;
    // YDB partition generation has no portable equivalent. Neither YT tablet
    // relocation nor Kafka leader/group epochs guarantee the same restart semantics.
    // Leave unset unless the adapter explicitly defines an equivalent generation.
    std::optional<i64> Generation;
};

// YDB: DescribeConsumer result. YT: consumer state for one queue.
// Kafka: group offsets filtered to one topic.
struct TMessageStreamConsumerDescription {
    // All backends: distinguish missing commits from missing partitions. Kafka group
    // assignments alone do not enumerate the stream. Completeness requires an adapter policy.
    std::vector<TMessageStreamConsumerPartition> Partitions;
};

// Retained/readable bounds for one partition in all three backends, independent of commits.
struct TMessageStreamPartitionDescription {
    // YDB: partition ID. YT: tablet_index. Kafka: nonnegative partition number.
    // Range-check conversions to backend-specific integer types.
    ui64 PartitionId = 0;
    // Unset when the describe response did not include partition statistics.
    // Zero is a valid offset; it does not by itself imply an empty partition.
    // All backends: same StartOffset/EndOffset semantics as TMessageStreamConsumerPartition;
    // missing bounds are not evidence of an empty partition.
    std::optional<ui64> StartOffset;
    std::optional<ui64> EndOffset;
};

// Metadata requests, not a portable capability guarantee across backends.
struct TMessageStreamDescribeConsumerSettings {
    // YDB: DescribeConsumer IncludeStats. YT: may require separate reads.
    // Kafka: combine group offsets and partition bounds.
    // Unsupported optional statistics remain unset; snapshots may differ in time.
    bool IncludeStats = false;
    // YDB: request partition location to obtain Generation; the result omits location.
    // YT/Kafka
    // placement metadata cannot be represented faithfully by this flag and field.
    bool IncludeLocation = false;
};

// Data plane: read sessions and offset commit. Writes stay on the federated topic client.
// All backends: sufficient for nontransactional progress; no external transaction/fencing token.
class IMessageStreamDataClient {
public:
    // All backends: polymorphic lifetime only; use the session API for explicit shutdown.
    virtual ~IMessageStreamDataClient() = default;

    // YDB: wrap the SDK read session and partition controls.
    // YT: adapt pull_queue[_consumer] into sessions; define row encoding and ownership.
    // Kafka: adapt poll and assignment/rebalance events; define timestamp/isolation
    // policy. YT and Kafka require adapter-defined partition confirmation semantics.
    // Unsupported settings must be rejected rather than silently changing read semantics.
    virtual std::shared_ptr<IMessageStreamReadSession> CreateReadSession(const TMessageStreamReadSettings& settings) = 0;
    // All backends: offset is the next unprocessed position (exclusive), unlike a message ID.
    // YDB: out-of-session CommitOffset for the named topic consumer.
    // YT: advance_queue_consumer; this signature lacks old_offset CAS and transaction
    // context, so it cannot expose atomic commit with application table updates.
    // Kafka: ordinary commits need suitable group/session ownership; administrative
    // alterConsumerGroupOffsets requires an empty group. Arbitrary live-group rewind
    // is not portable. Success should follow backend acknowledgement, not submission.
    virtual NThreading::TFuture<TMessageStreamResult<TMessageStreamOffset>> CommitOffset(
        const TString& stream, ui64 partitionId, const TString& consumer, ui64 offset) = 0;
};

// All backends: combines data and metadata access, but cannot provision queues/topics or consumers.
class IMessageStreamClient : public IMessageStreamDataClient {
public:
    // YDB: DescribeTopic. YT: resolve queue path/cluster.
    // Kafka: describeTopics plus optional group discovery;
    // the Consumers completeness limitation above prevents full semantic equivalence.
    virtual NThreading::TFuture<TMessageStreamResult<TMessageStreamTopicDescription>> DescribeStream(const TString& stream) = 0;
    // YDB: DescribeConsumer with the requested statistics/location options.
    // YT: combine consumer rows with queue bounds. Kafka: listConsumerGroupOffsets
    // plus topic/boundary metadata. Define absent consumer/progress behavior explicitly.
    virtual NThreading::TFuture<TMessageStreamResult<TMessageStreamConsumerDescription>> DescribeConsumer(
        const TString& stream, const TString& consumer, const TMessageStreamDescribeConsumerSettings& settings = {}) = 0;
    // All backends: fetch retained bounds under the same visibility policy as reading.
    // YDB: DescribePartition with IncludeStats.
    // YT Queue Agent attributes are introspection snapshots, not a high-load API.
    // Kafka needs an isolation policy configured outside this signature.
    virtual NThreading::TFuture<TMessageStreamResult<TMessageStreamPartitionDescription>> DescribePartition(
        const TString& stream, ui64 partitionId) = 0;
};

} // namespace NFq
