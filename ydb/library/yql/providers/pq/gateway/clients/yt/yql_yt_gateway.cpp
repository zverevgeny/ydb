#include "yql_yt_gateway.h"
#include "yql_yt_topic_client.h"

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/driver/driver.h>

#include <yt/yt/client/api/client.h>
#include <yt/yt/client/api/rpc_proxy/config.h>
#include <yt/yt/client/api/rpc_proxy/connection.h>

#include <yt/yt/core/concurrency/scheduler_api.h>

#include <util/generic/hash.h>
#include <util/generic/hash_set.h>
#include <util/generic/is_in.h>
#include <util/generic/yexception.h>
#include <util/string/builder.h>
#include <util/system/mutex.h>

namespace NYql {

namespace {

using namespace NYdb;
using namespace NYdb::NTopic;
using namespace NYdb::NFederatedTopic;

// Holds the parameters needed to (lazily) establish a YT connection for a
// logical cluster, plus the shared client once connected.
struct TYtCluster {
    TYtPqGatewaySettings::TCluster Config;
    NYT::NApi::IClientPtr Client;
};

class TYtPqGateway final : public IPqGateway {
public:
    explicit TYtPqGateway(const TYtPqGatewaySettings& settings) {
        for (const auto& cluster : settings.Clusters) {
            Clusters[cluster.Name] = TYtCluster{.Config = cluster, .Client = nullptr};
        }
    }

    NThreading::TFuture<void> OpenSession(const TString& sessionId, const TString& username) final {
        Y_UNUSED(username);
        with_lock (Mutex) {
            Y_ENSURE(sessionId);
            Y_ENSURE(!IsIn(OpenedSessions, sessionId), "Session " << sessionId << " is already opened");
            OpenedSessions.insert(sessionId);
        }
        return NThreading::MakeFuture();
    }

    NThreading::TFuture<void> CloseSession(const TString& sessionId) final {
        with_lock (Mutex) {
            OpenedSessions.erase(sessionId);
        }
        return NThreading::MakeFuture();
    }

    ::NPq::NConfigurationManager::TAsyncDescribePathResult DescribePath(
        const TString& sessionId, const TString& cluster, const TString& database, const TString& path, const TString& token) final
    {
        Y_UNUSED(sessionId, database, token);
        try {
            auto client = GetOrCreateClient(cluster);
            const auto& config = GetClusterConfig(cluster);
            const auto attrPath = JoinPath(config.PathPrefix, path) + "/@tablet_count";
            auto node = NYT::NConcurrency::WaitFor(client->GetNode(attrPath)).ValueOrThrow();
            const auto partitionsCount = NYT::NYTree::ConvertTo<i64>(node);

            ::NPq::NConfigurationManager::TTopicDescription desc(path);
            desc.PartitionsCount = static_cast<ui32>(partitionsCount);
            return NThreading::MakeFuture<::NPq::NConfigurationManager::TDescribePathResult>(
                ::NPq::NConfigurationManager::TDescribePathResult::Make<::NPq::NConfigurationManager::TTopicDescription>(desc));
        } catch (const std::exception& ex) {
            return NThreading::MakeErrorFuture<::NPq::NConfigurationManager::TDescribePathResult>(
                std::make_exception_ptr(::NPq::NConfigurationManager::TException{::NPq::NConfigurationManager::EStatus::NOT_FOUND}
                    << "YT queue " << path << " not found on cluster " << cluster << ": " << ex.what()));
        }
    }

    NThreading::TFuture<TListStreams> ListStreams(
        const TString& sessionId, const TString& cluster, const TString& database, const TString& token, ui32 limit, const TString& exclusiveStartStreamName) final
    {
        Y_UNUSED(sessionId, cluster, database, token, limit, exclusiveStartStreamName);
        return NThreading::MakeFuture<TListStreams>();
    }

    TAsyncDescribeFederatedTopicResult DescribeFederatedTopic(
        const TString& sessionId, const TString& cluster, const TString& database, const TString& path, const TString& token) final
    {
        Y_UNUSED(sessionId, database, token);
        try {
            auto client = GetOrCreateClient(cluster);
            const auto& config = GetClusterConfig(cluster);
            const auto attrPath = JoinPath(config.PathPrefix, path) + "/@tablet_count";
            auto node = NYT::NConcurrency::WaitFor(client->GetNode(attrPath)).ValueOrThrow();
            const auto partitionsCount = NYT::NYTree::ConvertTo<i64>(node);

            TDescribeFederatedTopicResult result;
            auto& info = result.emplace_back();
            info.PartitionsCount = static_cast<ui32>(partitionsCount);
            return NThreading::MakeFuture<TDescribeFederatedTopicResult>(result);
        } catch (const std::exception& ex) {
            return NThreading::MakeErrorFuture<TDescribeFederatedTopicResult>(
                std::make_exception_ptr(yexception()
                    << "YT queue " << path << " not found on cluster " << cluster << ": " << ex.what()));
        }
    }

    void UpdateClusterConfigs(const TString& clusterName, const TString& endpoint, const TString& database, bool secure) final {
        Y_UNUSED(database, secure);
        with_lock (Mutex) {
            auto& cluster = Clusters[clusterName];
            cluster.Config.Name = clusterName;
            cluster.Config.Endpoint = endpoint;
            cluster.Client = nullptr; // force reconnect on next use
        }
    }

    void UpdateClusterConfigs(const TPqGatewayConfigPtr& config) final {
        Y_UNUSED(config);
    }

    void AddCluster(const NYql::TPqClusterConfig& cluster) final {
        Y_UNUSED(cluster);
    }

    ITopicClient::TPtr GetTopicClient(const TDriver& driver, const TTopicClientSettings& settings) final {
        Y_UNUSED(driver);
        // The cluster is selected via the database field which carries the
        // logical YT cluster name in the YT-queue provider configuration.
        const TString clusterName = settings.Database_.value_or("");
        auto client = GetOrCreateClient(clusterName);
        const auto& config = GetClusterConfig(clusterName);

        TYtTopicClientSettings topicSettings;
        topicSettings.Client = client;
        topicSettings.PathPrefix = config.PathPrefix;
        topicSettings.DataColumn = config.DataColumn;
        return CreateYtTopicClient(topicSettings);
    }

    IFederatedTopicClient::TPtr GetFederatedTopicClient(const TDriver& driver, const TFederatedTopicClientSettings& settings) final {
        Y_UNUSED(driver, settings);
        ythrow yexception() << "Federated topic client is not supported by the YT queue gateway";
    }

    TTopicClientSettings GetTopicClientSettings() const final {
        return {};
    }

    TFederatedTopicClientSettings GetFederatedTopicClientSettings() const final {
        return {};
    }

private:
    static TString JoinPath(const TString& prefix, const TString& path) {
        if (prefix.empty() || path.StartsWith('/')) {
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

    TYtPqGatewaySettings::TCluster GetClusterConfig(const TString& clusterName) const {
        with_lock (Mutex) {
            const auto it = Clusters.find(clusterName);
            Y_ENSURE(it != Clusters.end(), "Unknown YT cluster: " << clusterName);
            return it->second.Config;
        }
    }

    NYT::NApi::IClientPtr GetOrCreateClient(const TString& clusterName) {
        with_lock (Mutex) {
            auto it = Clusters.find(clusterName);
            Y_ENSURE(it != Clusters.end(), "Unknown YT cluster: " << clusterName);
            if (!it->second.Client) {
                it->second.Client = CreateClient(it->second.Config);
            }
            return it->second.Client;
        }
    }

    static NYT::NApi::IClientPtr CreateClient(const TYtPqGatewaySettings::TCluster& config) {
        auto connectionConfig = NYT::New<NYT::NApi::NRpcProxy::TConnectionConfig>();
        connectionConfig->ClusterUrl = std::string(config.Endpoint);
        connectionConfig->ConnectionType = NYT::NApi::EConnectionType::Rpc;

        auto connection = NYT::NApi::NRpcProxy::CreateConnection(connectionConfig);

        NYT::NApi::TClientOptions options;
        if (!config.Token.empty()) {
            options = NYT::NApi::TClientOptions::FromToken(std::string(config.Token));
        }
        return connection->CreateClient(options);
    }

    mutable TMutex Mutex;
    THashMap<TString, TYtCluster> Clusters;
    THashSet<TString> OpenedSessions;
};

} // anonymous namespace

IPqGateway::TPtr CreateYtPqGateway(const TYtPqGatewaySettings& settings) {
    return MakeIntrusive<TYtPqGateway>(settings);
}

} // namespace NYql
