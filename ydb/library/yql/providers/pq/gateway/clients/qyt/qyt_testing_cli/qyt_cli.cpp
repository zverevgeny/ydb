#include "../yql_qyt_topic_client.h"

#include <yt/yt/client/api/client.h>
#include <yt/yt/client/api/rpc_proxy/config.h>
#include <yt/yt/client/api/rpc_proxy/connection.h>
#include <yt/yt/client/api/transaction.h>

#include <yt/yt/core/actions/future.h>

#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace NYql;

static void PrintUsage() {
    std::cerr << "Usage: qyt_cli <proxy> <command> <args...>\n"
              << "\nProxy format: host:port (without http:// prefix)\n"
              << "  Example: qyt_cli localhost:8443 describe-topic //tmp/my_queue\n"
              << "\nCommands:\n"
              << "  describe-topic <path>                  - Describe topic (check exists + partitions)\n"
              << "  describe-consumer <path> <consumer>    - Describe consumer\n"
              << "  describe-partition <path> <partition>  - Describe partition\n"
              << "  write <path> <consumer> <msg1> [msg2]  - Write messages to topic\n"
              << "  read <path> <consumer> [max_events]    - Read messages from topic\n"
              << "  commit <path> <consumer> <offset>      - Commit offset for consumer\n"
              << "  get <path>                             - Check if node exists\n";
}

static NYT::NApi::IClientPtr CreateYtClient(const std::string& proxy) {
    auto connectionConfig = NYT::New<NYT::NApi::NRpcProxy::TConnectionConfig>();

    // Strip http:// or https:// prefix if present — the RPC transport needs a
    // bare host:port address, not an HTTP URL.
    std::string rpcProxy = proxy;
    if (rpcProxy.size() > 7 && rpcProxy.substr(0, 7) == "http://") {
        rpcProxy = rpcProxy.substr(7);
    } else if (rpcProxy.size() > 8 && rpcProxy.substr(0, 8) == "https://") {
        rpcProxy = rpcProxy.substr(8);
    }

    // Set the proxy address directly and disable HTTP-based proxy list
    // discovery. In test environments the proxy list endpoint may return
    // internal container addresses that are unreachable from the host.
    connectionConfig->ProxyAddresses = std::vector<std::string>{rpcProxy};
    connectionConfig->EnableProxyDiscovery = false;

    auto connection = NYT::NApi::NRpcProxy::CreateConnection(connectionConfig);
    return connection->CreateClient(NYT::NApi::TClientOptions());
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        PrintUsage();
        return 1;
    }

    const std::string proxy = argv[1];
    const std::string command = argv[2];

    // Create YT client
    auto ytClient = CreateYtClient(proxy);

    // Create QYT topic client
    TQytTopicClientSettings settings;
    settings.Client = ytClient;

    auto topicClient = CreateQytTopicClient(settings);

    if (command == "describe-topic") {
        if (argc < 4) {
            std::cerr << "describe-topic requires <path>\n";
            return 1;
        }
        const TString path(argv[3]);
        auto future = topicClient->DescribeTopic(path, {});
        auto result = future.ExtractValue();
        if (result.IsSuccess()) {
            std::cout << "OK: Topic " << path << " exists\n";
            return 0;
        } else {
            std::cerr << "DescribeTopic failed: " << result.GetIssues().ToString() << "\n";
            return 1;
        }

    } else if (command == "write") {
        if (argc < 6) {
            std::cerr << "write requires <path> <consumer> <msg1> [msg2...]\n";
            return 1;
        }
        const std::string path = argv[3];
        const std::string consumer = argv[4];

        std::vector<std::string> messages;
        for (int i = 5; i < argc; i++) {
            messages.push_back(argv[i]);
        }
        if (messages.empty()) {
            std::cerr << "write requires at least one message\n";
            return 1;
        }

        NYdb::NTopic::TWriteSessionSettings writeSettings;
        writeSettings.Path(path);
        writeSettings.ProducerId(path + ".producer");

        auto writeSession = topicClient->CreateWriteSession(writeSettings);
        if (!writeSession) {
            std::cerr << "Failed to create write session\n";
            return 1;
        }

        // Wait for ReadyToAccept event
        std::optional<NYdb::NTopic::TContinuationToken> token;
        bool gotToken = false;
        while (!gotToken) {
            auto event = writeSession->GetEvent(/* block */ true);
            if (!event) {
                std::cerr << "Write session closed unexpectedly\n";
                return 1;
            }
            if (const auto* readyEvent = std::get_if<NYdb::NTopic::TWriteSessionEvent::TReadyToAcceptEvent>(&*event)) {
                token = std::move(readyEvent->ContinuationToken);
                gotToken = true;
            } else if (std::get_if<NYdb::NTopic::TSessionClosedEvent>(&*event)) {
                std::cerr << "Write session closed\n";
                return 1;
            }
        }

        for (const auto& msg : messages) {
            writeSession->Write(std::move(*token), NYdb::NTopic::TWriteMessage(msg), nullptr);

            // Wait for ack
            while (true) {
                auto event = writeSession->GetEvent(/* block */ true);
                if (!event) {
                    std::cerr << "Failed to get write ack\n";
                    return 1;
                }
                if (const auto* acks = std::get_if<NYdb::NTopic::TWriteSessionEvent::TAcksEvent>(&*event)) {
                    for (const auto& ack : acks->Acks) {
                        if (ack.State != NYdb::NTopic::TWriteSessionEvent::TWriteAck::EES_WRITTEN) {
                            std::cerr << "Message not written successfully, state=" << static_cast<int>(ack.State) << "\n";
                            return 1;
                        }
                    }
                    break;
                } else if (std::get_if<NYdb::NTopic::TSessionClosedEvent>(&*event)) {
                    std::cerr << "Write session closed during write\n";
                    return 1;
                }
            }

            // Wait for next ReadyToAccept
            while (true) {
                auto event = writeSession->GetEvent(/* block */ true);
                if (!event) {
                    break;
                }
                if (const auto* readyEvent = std::get_if<NYdb::NTopic::TWriteSessionEvent::TReadyToAcceptEvent>(&*event)) {
                    token = std::move(readyEvent->ContinuationToken);
                    break;
                } else if (std::get_if<NYdb::NTopic::TSessionClosedEvent>(&*event)) {
                    std::cerr << "Write session closed\n";
                    return 1;
                }
            }
        }

        bool ok = writeSession->Close(TDuration::Seconds(5));
        if (ok) {
            std::cout << "OK: Wrote " << messages.size() << " messages\n";
            return 0;
        } else {
            std::cerr << "Write session close failed\n";
            return 1;
        }

    } else if (command == "read") {
        if (argc < 5) {
            std::cerr << "read requires <path> <consumer> [max_events]\n";
            return 1;
        }
        const std::string path = argv[3];
        const std::string consumer = argv[4];
        size_t maxEvents = 10;
        if (argc >= 6) {
            maxEvents = std::stoul(argv[5]);
        }

        NYdb::NTopic::TReadSessionSettings readSettings;
        NYdb::NTopic::TTopicReadSettings topic;
        topic.Path(path);
        readSettings.Topics_.push_back(topic);
        readSettings.ConsumerName(consumer);

        auto readSession = topicClient->CreateReadSession(readSettings);
        if (!readSession) {
            std::cerr << "Failed to create read session\n";
            return 1;
        }

        size_t totalRead = 0;
        for (size_t i = 0; i < maxEvents; i++) {
            auto events = readSession->GetEvents(/* block */ true, std::nullopt, 0);
            if (events.empty()) {
                break;
            }
            for (auto& event : events) {
                if (auto* dataEvent = std::get_if<NYdb::NTopic::TReadSessionEvent::TDataReceivedEvent>(&event)) {
                    for (auto& msg : dataEvent->GetMessages()) {
                        std::cout << msg.GetData() << "\n";
                        msg.Commit();
                        totalRead++;
                    }
                } else if (std::get_if<NYdb::NTopic::TSessionClosedEvent>(&event)) {
                    break;
                }
            }
            if (totalRead >= maxEvents) {
                break;
            }
        }

        readSession->Close(TDuration::Seconds(5));
        std::cout << "OK: Read " << totalRead << " messages\n";
        return 0;

    } else if (command == "commit") {
        if (argc < 6) {
            std::cerr << "commit requires <path> <consumer> <offset>\n";
            return 1;
        }
        TString path(argv[3]);
        TString consumer(argv[4]);
        ui64 offset = std::stoull(argv[5]);

        auto future = topicClient->CommitOffset(path, 0, consumer, offset, {});
        auto result = future.ExtractValue();
        if (result.IsSuccess()) {
            std::cout << "OK: Committed offset " << offset << "\n";
            return 0;
        } else {
            std::cerr << "CommitOffset failed\n";
            return 1;
        }

    } else if (command == "describe-consumer") {
        if (argc < 6) {
            std::cerr << "describe-consumer requires <path> <consumer>\n";
            return 1;
        }
        TString path(argv[3]);
        TString consumer(argv[4]);
        auto future = topicClient->DescribeConsumer(path, consumer, {});
        auto result = future.ExtractValue();
        if (result.IsSuccess()) {
            std::cout << "OK: Consumer " << consumer << " on topic " << path << "\n";
            return 0;
        } else {
            std::cerr << "DescribeConsumer failed: " << result.GetIssues().ToString() << "\n";
            return 1;
        }

    } else if (command == "describe-partition") {
        if (argc < 6) {
            std::cerr << "describe-partition requires <path> <partition>\n";
            return 1;
        }
        TString path(argv[3]);
        i64 partitionId = std::stoll(argv[4]);
        auto future = topicClient->DescribePartition(path, partitionId, {});
        auto result = future.ExtractValue();
        if (result.IsSuccess()) {
            std::cout << "OK: Partition " << partitionId << " on topic " << path << "\n";
            return 0;
        } else {
            std::cerr << "DescribePartition failed: " << result.GetIssues().ToString() << "\n";
            return 1;
        }

    } else if (command == "get") {
        if (argc < 4) {
            std::cerr << "get requires <path>\n";
            return 1;
        }
        const TString path(argv[3]);

        try {
            auto node = NYT::NConcurrency::WaitFor(
                ytClient->GetNode(path)).ValueOrThrow();

            std::cout << "OK: " << path << " exists\n";
            return 0;
        } catch (const std::exception& ex) {
            std::cerr << "Get failed: " << ex.what() << "\n";
            return 1;
        }

    } else {
        std::cerr << "Unknown command: " << command << "\n";
        PrintUsage();
        return 1;
    }
}
