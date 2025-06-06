#include "dispatcher_impl.h"
#include "config.h"
#include "connection.h"

#include <yt/yt/core/concurrency/periodic_executor.h>
#include <yt/yt/core/concurrency/thread_pool_poller.h>

#include <yt/yt/core/actions/invoker_util.h>

#include <yt/yt/core/ytree/ypath_service.h>
#include <yt/yt/core/ytree/fluent.h>

#include <yt/yt/library/profiling/producer.h>

namespace NYT::NBus {

using namespace NConcurrency;
using namespace NProfiling;
using namespace NNet;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

constinit const auto Logger = BusLogger;

static constexpr auto PeriodicCheckPeriod = TDuration::MilliSeconds(100);
static constexpr auto PerConnectionPeriodicCheckPeriod = TDuration::Seconds(10);

////////////////////////////////////////////////////////////////////////////////

TNetworkAddress GetLocalBusAddress(int port)
{
    auto name = Format("yt-local-bus-%v", port);
    return TNetworkAddress::CreateAbstractUnixDomainSocketAddress(name);
}

bool IsLocalBusTransportEnabled()
{
#ifdef _linux_
    return true;
#else
    return false;
#endif
}

////////////////////////////////////////////////////////////////////////////////

TBusNetworkStatistics TBusNetworkCounters::ToStatistics() const
{
    TBusNetworkStatistics result;
    for (auto band : TEnumTraits<EMultiplexingBand>::GetDomainValues()) {
#define XX(camelCaseField, snakeCaseField) result.camelCaseField += PerBandCounters[band].camelCaseField.load(std::memory_order::relaxed);
        ITERATE_BUS_NETWORK_STATISTICS_FIELDS(XX)
#undef XX
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////

const TIntrusivePtr<TTcpDispatcher::TImpl>& TTcpDispatcher::TImpl::Get()
{
    return TTcpDispatcher::Get()->Impl_;
}

const TBusNetworkCountersPtr& TTcpDispatcher::TImpl::GetCounters(const std::string& networkName, bool encrypted)
{
    auto [statistics, ok] = NetworkStatistics_.FindOrInsert(networkName, [] {
        return std::array<TNetworkStatistics, 2>{};
    });

    return (*statistics)[encrypted].Counters;
}

IPollerPtr TTcpDispatcher::TImpl::GetOrCreatePoller(
    IThreadPoolPollerPtr* pollerPtr,
    bool isXfer,
    const TString& threadNamePrefix)
{
    {
        auto guard = ReaderGuard(PollersLock_);
        if (*pollerPtr) {
            return *pollerPtr;
        }
    }

    IPollerPtr poller;
    {
        auto guard = WriterGuard(PollersLock_);
        auto config = Config_.Acquire();
        if (!*pollerPtr) {
            if (isXfer) {
                *pollerPtr = CreateThreadPoolPoller(
                    config->ThreadPoolSize,
                    threadNamePrefix,
                    config->ThreadPoolPollingPeriod);
            } else {
                *pollerPtr = CreateThreadPoolPoller(/*threadCount*/ 1, threadNamePrefix);
            }
        }
        poller = *pollerPtr;
    }

    StartPeriodicExecutors();

    return poller;
}

void TTcpDispatcher::TImpl::DisableNetworking()
{
    YT_LOG_INFO("Networking disabled");

    NetworkingDisabled_.store(true);
}

bool TTcpDispatcher::TImpl::IsNetworkingDisabled()
{
    return NetworkingDisabled_.load();
}

const std::string& TTcpDispatcher::TImpl::GetNetworkNameForAddress(const TNetworkAddress& address)
{
    if (address.IsUnix()) {
        return LocalNetworkName;
    }

    if (!address.IsIP6()) {
        return DefaultNetworkName;
    }

    auto ip6Address = address.ToIP6Address();

    {
        auto guard = ReaderGuard(NetworksLock_);
        for (const auto& [networkAddress, networkName] : Networks_) {
            if (networkAddress.Contains(ip6Address)) {
                return networkName;
            }
        }
    }

    return DefaultNetworkName;
}

TTosLevel TTcpDispatcher::TImpl::GetTosLevelForBand(EMultiplexingBand band)
{
    if (band < TEnumTraits<EMultiplexingBand>::GetMinValue() || band > TEnumTraits<EMultiplexingBand>::GetMaxValue()) {
        return DefaultTosLevel;
    }
    const auto& bandDescriptor = BandToDescriptor_[band];
    return bandDescriptor.TosLevel.load(std::memory_order::relaxed);
}

int TTcpDispatcher::TImpl::GetMultiplexingParallelism(EMultiplexingBand band, int multiplexingParallelism)
{
    if (band < TEnumTraits<EMultiplexingBand>::GetMinValue() || band > TEnumTraits<EMultiplexingBand>::GetMaxValue()) {
        return std::clamp<int>(
            multiplexingParallelism,
            DefaultMinMultiplexingParallelism,
            DefaultMaxMultiplexingParallelism);
    }
    const auto& bandDescriptor = BandToDescriptor_[band];
    return std::clamp<int>(
        multiplexingParallelism,
        bandDescriptor.MinMultiplexingParallelism.load(std::memory_order::relaxed),
        bandDescriptor.MaxMultiplexingParallelism.load(std::memory_order::relaxed));
}

IPollerPtr TTcpDispatcher::TImpl::GetAcceptorPoller()
{
    static const TString ThreadNamePrefix("BusAcpt");
    return GetOrCreatePoller(&AcceptorPoller_, false, ThreadNamePrefix);
}

IPollerPtr TTcpDispatcher::TImpl::GetXferPoller()
{
    static const TString ThreadNamePrefix("BusXfer");
    return GetOrCreatePoller(&XferPoller_, true, ThreadNamePrefix);
}

void TTcpDispatcher::TImpl::Configure(const TTcpDispatcherConfigPtr& config)
{
    {
        auto guard = WriterGuard(PollersLock_);

        Config_.Store(config);

        if (XferPoller_) {
            XferPoller_->SetThreadCount(config->ThreadPoolSize);
            XferPoller_->SetPollingPeriod(config->ThreadPoolPollingPeriod);
        }
    }

    {
        auto guard = WriterGuard(NetworksLock_);

        Networks_.clear();

        for (const auto& [networkName, networkAddresses] : config->Networks) {
            for (const auto& prefix : networkAddresses) {
                Networks_.emplace_back(prefix, networkName);
            }
        }

        // Put more specific networks first in match order.
        std::sort(Networks_.begin(), Networks_.end(), [] (const auto& lhs, const auto& rhs) {
            return lhs.first.GetMaskSize() > rhs.first.GetMaskSize();
        });
    }

    for (auto band : TEnumTraits<EMultiplexingBand>::GetDomainValues()) {
        const auto& bandConfig = config->MultiplexingBands[band];
        auto& bandDescriptor = BandToDescriptor_[band];
        bandDescriptor.TosLevel.store(bandConfig ? bandConfig->TosLevel : DefaultTosLevel);
        bandDescriptor.MinMultiplexingParallelism.store(bandConfig ? bandConfig->MinMultiplexingParallelism : DefaultMinMultiplexingParallelism);
        bandDescriptor.MaxMultiplexingParallelism.store(bandConfig ? bandConfig->MaxMultiplexingParallelism : DefaultMaxMultiplexingParallelism);
    }
}

void TTcpDispatcher::TImpl::RegisterConnection(TTcpConnectionPtr connection)
{
    ConnectionsToRegister_.Enqueue(std::move(connection));
}

void TTcpDispatcher::TImpl::StartPeriodicExecutors()
{
    auto poller = GetXferPoller();
    auto invoker = poller->GetInvoker();

    auto guard = Guard(PeriodicExecutorsLock_);
    if (!PeriodicCheckExecutor_) {
        PeriodicCheckExecutor_ = New<TPeriodicExecutor>(
            invoker,
            BIND(&TImpl::OnPeriodicCheck, MakeWeak(this)),
            PeriodicCheckPeriod);
        PeriodicCheckExecutor_->Start();
    }
}

void TTcpDispatcher::TImpl::CollectSensors(ISensorWriter* writer)
{
    NetworkStatistics_.IterateReadOnly([&] (const auto& name, const auto& statistics) {
        TWithTagGuard networkTagGuard(writer, "network", name);

        for (auto encrypted : { false, true }) {
            TWithTagGuard encryptedTagGuard(writer, "encrypted", ToString(encrypted));

            const auto& counters = statistics[encrypted].Counters;

            for (auto band : TEnumTraits<EMultiplexingBand>::GetDomainValues()) {
                TWithTagGuard bandTagGuard(writer, "band", FormatEnum(band));
                #define XX(camelCaseField, snakeCaseField) writer->AddCounter("/" #snakeCaseField, counters->PerBandCounters[band].camelCaseField.load(std::memory_order::relaxed));
                ITERATE_BUS_NETWORK_STATISTICS_COUNTER_FIELDS(XX)
                #undef XX
                #define XX(camelCaseField, snakeCaseField) writer->AddGauge("/" #snakeCaseField, counters->PerBandCounters[band].camelCaseField.load(std::memory_order::relaxed));
                ITERATE_BUS_NETWORK_STATISTICS_GAUGE_FIELDS(XX)
                #undef XX
            }
        }
    });

    auto config = Config_.Acquire();
    if (config->NetworkBandwidth) {
        writer->AddGauge("/network_bandwidth_limit", *config->NetworkBandwidth);
    }
}

std::vector<TTcpConnectionPtr> TTcpDispatcher::TImpl::GetConnections()
{
    auto connectionList = ConnectionList_.Load();

    std::vector<TTcpConnectionPtr> result;
    result.reserve(connectionList.size());
    for (const auto& weakConnection : connectionList) {
        if (auto connection = weakConnection.Lock()) {
            result.push_back(connection);
        }
    }
    return result;
}

void TTcpDispatcher::TImpl::BuildOrchid(IYsonConsumer* consumer)
{
    std::vector<std::pair<TTcpConnectionPtr, TBusNetworkStatistics>> connectionsWithStatistics;
    for (const auto& connection : GetConnections()) {
        connectionsWithStatistics.emplace_back(connection, connection->GetBusStatistics());
    }
    SortBy(connectionsWithStatistics, [] (const auto& connectionWithStatistics) {
        return -connectionWithStatistics.second.PendingOutBytes;
    });

    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("connections").DoMapFor(connectionsWithStatistics, [] (auto fluent, const auto& connectionWithStatistics) {
                const auto& [connection, statistics] = connectionWithStatistics;
                fluent
                    .Item(ToString(connection->GetId())).BeginMap()
                        .Item("address").Value(connection->GetEndpointAddress())
                        .Item("encrypted").Value(connection->IsEncrypted())
                        .Item("statistics").BeginMap()
                            .Item("in_bytes").Value(statistics.InBytes)
                            .Item("in_packets").Value(statistics.InPackets)
                            .Item("out_bytes").Value(statistics.OutBytes)
                            .Item("out_packets").Value(statistics.OutPackets)
                            .Item("pending_out_bytes").Value(statistics.PendingOutBytes)
                            .Item("pending_out_packets").Value(statistics.PendingOutPackets)
                        .EndMap()
                    .EndMap();
            })
        .EndMap();
}


IYPathServicePtr TTcpDispatcher::TImpl::GetOrchidService()
{
    return IYPathService::FromProducer(BIND(&TImpl::BuildOrchid, MakeStrong(this)))
        ->Via(GetXferPoller()->GetInvoker());
}

void TTcpDispatcher::TImpl::OnPeriodicCheck()
{
    ConnectionList_.Transform([&] (auto& connectionList) {
        for (auto&& connection : ConnectionsToRegister_.DequeueAll()) {
            connectionList.push_back(std::move(connection));
        }

        i64 connectionsToCheck = std::max(
            std::ssize(connectionList) *
            static_cast<i64>(PeriodicCheckPeriod.GetValue()) /
            static_cast<i64>(PerConnectionPeriodicCheckPeriod.GetValue()),
            static_cast<i64>(1));
        for (i64 index = 0; index < connectionsToCheck && !connectionList.empty(); ++index) {
            auto& weakConnection = connectionList[CurrentConnectionListIndex_];
            if (auto connection = weakConnection.Lock()) {
                connection->RunPeriodicCheck();
                ++CurrentConnectionListIndex_;
            } else {
                std::swap(weakConnection, connectionList.back());
                connectionList.pop_back();
            }
            if (CurrentConnectionListIndex_ >= std::ssize(connectionList)) {
                CurrentConnectionListIndex_ = 0;
            }
        }
    });
}

std::optional<TString> TTcpDispatcher::TImpl::GetBusCertsDirectoryPath() const
{
    return Config_.Acquire()->BusCertsDirectoryPath;
}

void TTcpDispatcher::TImpl::RegisterLocalMessageHandler(int port, const ILocalMessageHandlerPtr& handler)
{
    {
        auto guard = WriterGuard(LocalMessageHandlersLock_);
        if (!LocalMessageHandlers_.emplace(port, handler).second) {
            THROW_ERROR_EXCEPTION("Local message handler is already registered for port %v",
                port);
        }
    }

    YT_LOG_INFO("Local message handler registered (Port: %v)",
        port);
}

void TTcpDispatcher::TImpl::UnregisterLocalMessageHandler(int port)
{
    {
        auto guard = WriterGuard(LocalMessageHandlersLock_);
        LocalMessageHandlers_.erase(port);
    }

    YT_LOG_INFO("Local message handler unregistered (Port: %v)",
        port);
}

ILocalMessageHandlerPtr TTcpDispatcher::TImpl::FindLocalBypassMessageHandler(const TNetworkAddress& address)
{
    if (!TAddressResolver::Get()->IsLocalAddress(address)) {
        return nullptr;
    }

    if (!Config_.Acquire()->EnableLocalBypass) {
        return nullptr;
    }

    auto guard = ReaderGuard(LocalMessageHandlersLock_);
    return GetOrDefault(LocalMessageHandlers_, address.GetPort());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NBus
