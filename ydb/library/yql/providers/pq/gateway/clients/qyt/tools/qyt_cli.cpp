#include "../yql_qyt_topic_client.h"

#include <yt/yt/client/api/rpc_proxy/config.h>
#include <yt/yt/client/api/rpc_proxy/connection.h>

#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace NYql;

static void PrintUsage() {
    std::cerr << "Usage: qyt_cli <proxy> <command> <args...>\n"
              << "Commands:\n"
              << "  describe-topic <path>                  - Describe topic (check exists + partitions)\n"
              << "  write <path> <consumer> <msg1> [msg2]  - Write messages to topic\n"
              << "  read <path> <consumer> [max_events]    - Read messages from topic\n"
              << "  commit <path> <consumer> <offset>      - Commit offset for consumer\n";
}

static NYT::NApi::IClientPtr CreateYtClient(const std::string& proxy) {
    auto connectionConfig = NYT::New<NYT::NApi::NRpcProxy::TConnectionConfig>();
    connectionConfig->ClusterUrl = proxy;
    connectionConfig->ConnectionType = NYT::NApi::EConnectionType::Rpc;

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
            std::cerr << "DescribeTopic failed\n";
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

    } else {
        std::cerr << "Unknown command: " << command << "\n";
        PrintUsage();
        return 1;
    }
}
