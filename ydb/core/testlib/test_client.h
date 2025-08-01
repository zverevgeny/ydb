#pragma once
#include "tablet_helpers.h"

#include <ydb/core/base/appdata.h>
#include <ydb/core/base/subdomain.h>
#include <ydb/core/base/tablet_types.h>
#include <ydb/core/base/domain.h>
#include <ydb/core/driver_lib/run/config.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/public/api/protos/ydb_cms.pb.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/driver/driver.h>
#include <ydb/public/lib/deprecated/client/msgbus_client.h>
#include <ydb/core/client/server/grpc_server.h>
#include <ydb/core/scheme/scheme_types_defs.h>
#include <ydb/core/scheme/scheme_type_registry.h>
#include <ydb/core/mind/local.h>
#include <yql/essentials/minikql/mkql_node.h>
#include <yql/essentials/minikql/mkql_node_serialization.h>
#include <yql/essentials/minikql/mkql_program_builder.h>
#include <yql/essentials/minikql/mkql_function_registry.h>
#include <ydb/library/mkql_proto/protos/minikql.pb.h>
#include <ydb/core/blobstorage/dsproxy/mock/dsproxy_mock.h>
#include <ydb/core/blobstorage/dsproxy/mock/model.h>
#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/core/testlib/basics/runtime.h>
#include <ydb/core/testlib/basics/appdata.h>
#include <ydb/core/testlib/mock_transfer_writer_factory.h>
#include <ydb/core/protos/kesus.pb.h>
#include <ydb/core/protos/table_service_config.pb.h>
#include <ydb/core/protos/console_tenant.pb.h>
#include <ydb/core/protos/flat_tx_scheme.pb.h>
#include <ydb/core/kesus/tablet/events.h>
#include <ydb/core/kqp/federated_query/kqp_federated_query_helpers.h>
#include <ydb/core/security/ticket_parser.h>
#include <ydb/core/security/ticket_parser_settings.h>
#include <ydb/core/base/grpc_service_factory.h>
#include <ydb/core/persqueue/actor_persqueue_client_iface.h>
#include <ydb/core/fq/libs/shared_resources/interface/shared_resources.h>
#include <ydb/core/http_proxy/auth_factory.h>
#include <ydb/library/accessor/accessor.h>
#include <ydb/library/yql/providers/s3/actors_factory/yql_s3_actors_factory.h>

#include <ydb/library/grpc/server/grpc_server.h>

#include <google/protobuf/text_format.h>

#include <functional>
#include <algorithm>

namespace NKikimr {
namespace Tests {

#ifdef WITH_VALGRIND
    const ui64 TIME_LIMIT_MS = TDuration::Seconds(600).MilliSeconds();
#else
    #ifdef NDEBUG
    const ui64 TIME_LIMIT_MS = TDuration::Seconds(60).MilliSeconds();
    #else
    const ui64 TIME_LIMIT_MS = TDuration::Seconds(180).MilliSeconds();
    #endif
#endif
    const TDuration ITERATION_DURATION = TDuration::MilliSeconds(50);

    constexpr const char* TestDomainName = "dc-1";
    const ui32 TestDomain = 1;
    const ui64 DummyTablet1 = MakeTabletID(false, 0x840100);
    const ui64 DummyTablet2 = MakeTabletID(false, 0x840101);
    const ui64 Coordinator = MakeTabletID(false, 0x800001);
    const ui64 Mediator = MakeTabletID(false, 0x810001);
    const ui64 TxAllocator = MakeTabletID(false, 0x820001);
    const ui64 SchemeRoot = MakeTabletID(false, 0x850100);
    const ui64 Hive = MakeTabletID(false, 0xA001);

    struct TServerSetup {
        TString IpAddress;
        ui16 Port = 0;

        TServerSetup()
        {}

        TServerSetup(const TString& ipAddress, ui16 port)
            : IpAddress(ipAddress)
            , Port(port)
        {}
    };

    // whether external server is used
    bool IsServerRedirected();
    TServerSetup GetServerSetup();

    inline ui64 ChangeDomain(ui64 tabletId, ui32 id) {
        const ui64 mask = static_cast<ui64>(0xfff) << 44;
        return (tabletId & ~mask) | static_cast<ui64>(id & 0xfff) << 44;
    }

    inline ui64 ChangeStateStorage(ui64 tabletId, ui32 id) {
        const ui64 mask = static_cast<ui64>(0xff) << 56;
        return (tabletId & ~mask) | static_cast<ui64>(id & 0xff) << 56;
    }

    NMiniKQL::IFunctionRegistry* DefaultFrFactory(const NScheme::TTypeRegistry& typeRegistry);

    struct TServerSettings: public TThrRefBase, public TTestFeatureFlagsHolder<TServerSettings> {
        static constexpr ui64 BOX_ID = 999;
        ui64 POOL_ID = 1;

        using TPtr = TIntrusivePtr<TServerSettings>;
        using TConstPtr = TIntrusiveConstPtr<TServerSettings>;

        using TControls = NKikimrConfig::TImmediateControlsConfig;
        using TLoggerInitializer = std::function<void (TTestActorRuntime&)>;
        using TStoragePoolKinds = TDomainsInfo::TDomain::TStoragePoolKinds;

        ui16 Port;
        ui16 GrpcPort = 0;
        int GrpcMaxMessageSize = 0;  // 0 - default (4_MB), -1 - no limit
        ui16 MonitoringPortOffset = 0;
        bool MonitoringTypeAsync = false;
        bool NeedStatsCollectors = false;
        NKikimrProto::TAuthConfig AuthConfig;
        NKikimrPQ::TPQConfig PQConfig;
        NKikimrPQ::TPQClusterDiscoveryConfig PQClusterDiscoveryConfig;
        NKikimrNetClassifier::TNetClassifierConfig NetClassifierConfig;
        ui32 Domain = TestDomain;
        bool SupportsRedirect = true;
        TString TracePath;
        TString DomainName = TestDomainName;
        ui32 NodeCount = 1;
        ui32 DynamicNodeCount = 0;
        std::optional<ui32> DataCenterCount;
        ui64 StorageGeneration = 0;
        bool FetchPoolsGeneration = false;
        NFake::TStorage CustomDiskParams;
        TControls Controls;
        TAppPrepare::TFnReg FrFactory = &DefaultFrFactory;
        TIntrusivePtr<TFormatFactory> Formats;
        bool EnableMockOnSingleNode = true;
        TAutoPtr<TLogBackend> LogBackend;
        std::shared_ptr<std::vector<std::string>> AuditLogBackendLines;
        TLoggerInitializer LoggerInitializer;
        TStoragePoolKinds StoragePoolTypes;
        TVector<NKikimrKqp::TKqpSetting> KqpSettings;
        bool EnableForceFollowers = false;
        bool EnableConsole = true;
        bool EnableNodeBroker = false;
        bool EnableConfigsDispatcher = true;
        bool EnableFeatureFlagsConfigurator = false;
        bool UseRealThreads = true;
        bool EnableKqpSpilling = false;
        bool EnableYq = false;
        bool EnableYqGrpc = false;
        bool EnableScriptExecutionBackgroundChecks = true;
        TDuration KeepSnapshotTimeout = TDuration::Zero();
        ui64 ChangesQueueItemsLimit = 0;
        ui64 ChangesQueueBytesLimit = 0;
        std::shared_ptr<NKikimrConfig::TAppConfig> AppConfig;
        std::shared_ptr<TKikimrRunConfig> KikimrRunConfig;
        NKikimrConfig::TCompactionConfig CompactionConfig;
        TMap<ui32, TString> NodeKeys;
        ui64 DomainPlanResolution = 0;
        ui32 DomainTimecastBuckets = 0;
        std::shared_ptr<NKikimr::NMsgBusProxy::IPersQueueGetReadSessionsInfoWorkerFactory> PersQueueGetReadSessionsInfoWorkerFactory;
        std::shared_ptr<NKikimr::NHttpProxy::IAuthFactory> DataStreamsAuthFactory;
        std::shared_ptr<NKikimr::NPQ::TPersQueueMirrorReaderFactory> PersQueueMirrorReaderFactory = std::make_shared<NKikimr::NPQ::TPersQueueMirrorReaderFactory>();
        std::shared_ptr<NKikimr::NReplication::NService::ITransferWriterFactory> TransferWriterFactory = std::make_shared<MockTransferWriterFactory>();

        bool EnableMetering = false;
        TString MeteringFilePath;
        TString AwsRegion;
        NKqp::IKqpFederatedQuerySetupFactory::TPtr FederatedQuerySetupFactory = std::make_shared<NKqp::TKqpFederatedQuerySetupFactoryNoop>();
        NYql::ISecuredServiceAccountCredentialsFactory::TPtr CredentialsFactory;
        NMiniKQL::TComputationNodeFactory ComputationFactory;
        NYql::IYtGateway::TPtr YtGateway;
        NYql::ISolomonGateway::TPtr SolomonGateway;
        NYql::IPqGateway::TPtr PqGateway;
        NYql::TTaskTransformFactory DqTaskTransformFactory;
        bool InitializeFederatedQuerySetupFactory = false;
        TString ServerCertFilePath;
        bool Verbose = true;
        bool UseSectorMap = false;
        TVector<TIntrusivePtr<NFake::TProxyDS>> ProxyDSMocks;

        std::function<IActor*(const TTicketParserSettings&)> CreateTicketParser = NKikimr::CreateTicketParser;
        std::shared_ptr<TGrpcServiceFactory> GrpcServiceFactory;
        std::shared_ptr<NYql::NDq::IS3ActorsFactory> S3ActorsFactory = NYql::NDq::CreateDefaultS3ActorsFactory();

        TServerSettings& SetGrpcPort(ui16 value) { GrpcPort = value; return *this; }
        TServerSettings& SetGrpcMaxMessageSize(int value) { GrpcMaxMessageSize = value; return *this; }
        TServerSettings& SetMonitoringPortOffset(ui16 value, bool monitoringTypeAsync = false) { MonitoringPortOffset = value; MonitoringTypeAsync = monitoringTypeAsync; return *this; }
        TServerSettings& SetNeedStatsCollectors(bool value) { NeedStatsCollectors = value; return *this; }
        TServerSettings& SetSupportsRedirect(bool value) { SupportsRedirect = value; return *this; }
        TServerSettings& SetTracePath(const TString& value) { TracePath = value; return *this; }
        TServerSettings& SetDomain(ui32 value) { Domain = value; return *this; }
        TServerSettings& SetDomainName(const TString& value);
        TServerSettings& SetNodeCount(ui32 value) { NodeCount = value; return *this; }
        TServerSettings& SetDynamicNodeCount(ui32 value) { DynamicNodeCount = value; return *this; }
        TServerSettings& SetDataCenterCount(ui32 value) { DataCenterCount = value; return *this; }
        TServerSettings& SetStorageGeneration(ui64 storageGeneration, bool fetchPoolsGeneration = false) { StorageGeneration = storageGeneration; FetchPoolsGeneration = fetchPoolsGeneration; return *this; }
        TServerSettings& SetCustomDiskParams(const NFake::TStorage& value) { CustomDiskParams = value; return *this; }
        TServerSettings& SetControls(const TControls& value) { Controls = value; return *this; }
        TServerSettings& SetFrFactory(const TAppPrepare::TFnReg& value) { FrFactory = value; return *this; }
        TServerSettings& SetEnableMockOnSingleNode(bool value) { EnableMockOnSingleNode = value; return *this; }
        TServerSettings& SetLogBackend(TAutoPtr<TLogBackend> value) { LogBackend = value; return *this; }
        TServerSettings& SetAuditLogBackendLines(std::shared_ptr<std::vector<std::string>> value) { AuditLogBackendLines = value; return *this; }
        TServerSettings& SetLoggerInitializer(TLoggerInitializer value) { LoggerInitializer = std::move(value); return *this; }
        TServerSettings& AddStoragePoolType(const TString& poolKind, ui32 encryptionMode = 0);
        TServerSettings& AddStoragePool(const TString& poolKind, const TString& poolName = {}, ui32 numGroups = 1, ui32 encryptionMode = 0);
        TServerSettings& SetKqpSettings(const TVector<NKikimrKqp::TKqpSetting>& settings) { KqpSettings = settings; return *this; }
        TServerSettings& SetEnableConsole(bool value) { EnableConsole = value; return *this; }
        TServerSettings& SetEnableNodeBroker(bool value) { EnableNodeBroker = value; return *this; }
        TServerSettings& SetEnableConfigsDispatcher(bool value) { EnableConfigsDispatcher = value; return *this; }
        TServerSettings& SetEnableFeatureFlagsConfigurator(bool value) { EnableFeatureFlagsConfigurator = value; return *this; }
        TServerSettings& SetUseRealThreads(bool value) { UseRealThreads = value; return *this; }
        TServerSettings& SetAppConfig(const NKikimrConfig::TAppConfig& value) { AppConfig = std::make_shared<NKikimrConfig::TAppConfig>(value); return *this; }
        TServerSettings& InitKikimrRunConfig() { KikimrRunConfig = std::make_shared<TKikimrRunConfig>(*AppConfig); return *this; }
        TServerSettings& SetKeyFor(ui32 nodeId, TString keyValue) { NodeKeys[nodeId] = keyValue; return *this; }
        TServerSettings& SetEnableKqpSpilling(bool value) { EnableKqpSpilling = value; return *this; }
        TServerSettings& SetEnableForceFollowers(bool value) { EnableForceFollowers = value; return *this; }
        TServerSettings& SetDomainPlanResolution(ui64 resolution) { DomainPlanResolution = resolution; return *this; }
        TServerSettings& SetDomainTimecastBuckets(ui32 buckets) { DomainTimecastBuckets = buckets; return *this; }
        TServerSettings& SetFeatureFlags(const NKikimrConfig::TFeatureFlags& value) { FeatureFlags = value; return *this; }
        TServerSettings& SetCompactionConfig(const NKikimrConfig::TCompactionConfig& value) { CompactionConfig = value; return *this; }
        TServerSettings& SetEnableDbCounters(bool value) { FeatureFlags.SetEnableDbCounters(value); return *this; }
        TServerSettings& SetEnablePersistentQueryStats(bool value) { FeatureFlags.SetEnablePersistentQueryStats(value); return *this; }
        TServerSettings& SetEnableYq(bool value) { EnableYq = value; return *this; }
        TServerSettings& SetEnableYqGrpc(bool value) { EnableYqGrpc = value; return *this; }
        TServerSettings& SetKeepSnapshotTimeout(TDuration value) { KeepSnapshotTimeout = value; return *this; }
        TServerSettings& SetChangesQueueItemsLimit(ui64 value) { ChangesQueueItemsLimit = value; return *this; }
        TServerSettings& SetChangesQueueBytesLimit(ui64 value) { ChangesQueueBytesLimit = value; return *this; }
        TServerSettings& SetMeteringFilePath(const TString& path) { EnableMetering = true; MeteringFilePath = path; return *this; }
        TServerSettings& SetAwsRegion(const TString& value) { AwsRegion = value; return *this; }
        TServerSettings& SetFederatedQuerySetupFactory(NKqp::IKqpFederatedQuerySetupFactory::TPtr value) { FederatedQuerySetupFactory = value; return *this; }
        TServerSettings& SetCredentialsFactory(NYql::ISecuredServiceAccountCredentialsFactory::TPtr credentialsFactory) { CredentialsFactory = std::move(credentialsFactory); return *this; }
        TServerSettings& SetComputationFactory(NMiniKQL::TComputationNodeFactory computationFactory) { ComputationFactory = std::move(computationFactory); return *this; }
        TServerSettings& SetYtGateway(NYql::IYtGateway::TPtr ytGateway) { YtGateway = std::move(ytGateway); return *this; }
        TServerSettings& SetSolomonGateway(NYql::ISolomonGateway::TPtr solomonGateway) { SolomonGateway = std::move(solomonGateway); return *this; }
        TServerSettings& SetDqTaskTransformFactory(NYql::TTaskTransformFactory value) { DqTaskTransformFactory = std::move(value); return *this; }
        TServerSettings& SetInitializeFederatedQuerySetupFactory(bool value) { InitializeFederatedQuerySetupFactory = value; return *this; }
        TServerSettings& SetVerbose(bool value) { Verbose = value; return *this; }
        TServerSettings& SetUseSectorMap(bool value) { UseSectorMap = value; return *this; }
        TServerSettings& SetEnableScriptExecutionBackgroundChecks(bool value) { EnableScriptExecutionBackgroundChecks = value; return *this; }
        TServerSettings& SetScanReaskToResolve(const ui32 count) {
            AppConfig->MutableTableServiceConfig()->MutableResourceManager()->MutableShardsScanningPolicy()->SetReaskShardRetriesCount(count);
            return *this;
        }
        TServerSettings& SetColumnShardReaderClassName(const TString& className) {
            AppConfig->MutableColumnShardConfig()->SetReaderClassName(className);
            return *this;
        }
        TServerSettings& SetPersQueueGetReadSessionsInfoWorkerFactory(
            std::shared_ptr<NKikimr::NMsgBusProxy::IPersQueueGetReadSessionsInfoWorkerFactory> factory
        ) {
            PersQueueGetReadSessionsInfoWorkerFactory = factory;
            return *this;
        }
        TServerSettings& SetDataStreamsAuthFactory(
            std::shared_ptr<NKikimr::NHttpProxy::IAuthFactory> factory
        ) {
            DataStreamsAuthFactory = factory;
            return *this;
        }
        TServerSettings& SetEnableOltpSink(bool withOltpSink) {
            AppConfig->MutableTableServiceConfig()->SetEnableOltpSink(withOltpSink);
            return *this;
        }
        TServerSettings& SetEnableOlapSink(bool withOlapSink) {
            AppConfig->MutableTableServiceConfig()->SetEnableOlapSink(withOlapSink);
            return *this;
        }
        TServerSettings& SetEnableHtapTx(bool withHtapTx) {
            AppConfig->MutableTableServiceConfig()->SetEnableHtapTx(withHtapTx);
            return *this;
        }
        TServerSettings& SetAllowOlapDataQuery(bool withAllowOlapDataQuery) {
            AppConfig->MutableTableServiceConfig()->SetAllowOlapDataQuery(withAllowOlapDataQuery);
            return *this;
        }

        TServerSettings& SetColumnShardAlterObjectEnabled(bool enable) {
            AppConfig->MutableColumnShardConfig()->SetAlterObjectEnabled(enable);
            return *this;
        }

        TServerSettings& SetProxyDSMocks(const TVector<TIntrusivePtr<NFake::TProxyDS>>& proxyDSMocks) {
            ProxyDSMocks = proxyDSMocks;
            return *this;
        }

        template <typename TService, typename...TParams>
        TServerSettings& RegisterGrpcService(
            const TString& name,
            std::optional<NActors::TActorId> proxyId = std::nullopt,
            TParams...params
        ) {
            if (!GrpcServiceFactory) {
                GrpcServiceFactory = std::make_shared<TGrpcServiceFactory>();
            }
            GrpcServiceFactory->Register<TService>(name, true, proxyId, params...);
            return *this;
        }

        explicit TServerSettings(ui16 port, const NKikimrProto::TAuthConfig authConfig = {}, const NKikimrPQ::TPQConfig pqConfig = {})
            : Port(port)
            , AuthConfig(authConfig)
            , PQConfig(pqConfig)
        {
            AddStoragePool("test", "/" + DomainName + ":test");
            AppConfig = std::make_shared<NKikimrConfig::TAppConfig>();
            AppConfig->MutableTableServiceConfig()->MutableResourceManager()->MutableShardsScanningPolicy()->SetParallelScanningAvailable(true);
            AppConfig->MutableTableServiceConfig()->MutableResourceManager()->MutableShardsScanningPolicy()->SetShardSplitFactor(16);
            AppConfig->MutableHiveConfig()->SetWarmUpBootWaitingPeriod(10);
            AppConfig->MutableHiveConfig()->SetMaxNodeUsageToKick(100);
            AppConfig->MutableHiveConfig()->SetMinCounterScatterToBalance(100);
            AppConfig->MutableHiveConfig()->SetMinScatterToBalance(100);
            AppConfig->MutableHiveConfig()->SetObjectImbalanceToBalance(100);
            AppConfig->MutableColumnShardConfig()->SetDisabledOnSchemeShard(false);
            AppConfig->MutableQueryServiceConfig()->AddAvailableExternalDataSources("ObjectStorage");
            FeatureFlags.SetEnableSeparationComputeActorsFromRead(true);
            FeatureFlags.SetEnableWritePortionsOnInsert(true);
            FeatureFlags.SetEnableFollowerStats(true);
            FeatureFlags.SetEnableColumnStore(true);
        }

        TServerSettings() = default;
        TServerSettings(const TServerSettings& settings) = default;
        TServerSettings& operator=(const TServerSettings& settings) = default;
    private:
        YDB_FLAG_ACCESSOR(EnableMetadataProvider, true);
        YDB_FLAG_ACCESSOR(EnableExternalIndex, false);
    };

    class TServer : public TThrRefBase, TMoveOnly {
    protected:
        void SetupStorage();

        void SetupActorSystemConfig();
        void SetupMessageBus(ui16 port);
        void SetupDomains(TAppPrepare&);
        void CreateBootstrapTablets();
        void SetupLocalConfig(TLocalConfig &localConfig, const NKikimr::TAppData &appData);
        void SetupDomainLocalService(ui32 nodeIdx);
        void SetupLocalService(ui32 nodeIdx, const TString &domainName);
        void SetupConfigurators(ui32 nodeIdx);
        void SetupProxies(ui32 nodeIdx);
        void SetupLogging();
        void AddSysViewsRosterUpdateObserver();
        void WaitForSysViewsRosterUpdate();

        void Initialize();

    public:
        using TPtr = TIntrusivePtr<TServer>;
        using TMapStoragePool = TDomainsInfo::TDomain::TStoragePoolKinds;

        TServer(const TServerSettings& settings, bool defaultInit = true);
        TServer(TServerSettings::TConstPtr settings, bool defaultInit = true);

        TServer(TServer&& server) = default;
        TServer& operator =(TServer&& server) = default;
        virtual ~TServer();

        void EnableGRpc(const NYdbGrpc::TServerOptions& options, ui32 grpcServiceNodeId = 0, const std::optional<TString>& tenant = std::nullopt);
        void EnableGRpc(ui16 port, ui32 grpcServiceNodeId = 0, const std::optional<TString>& tenant = std::nullopt);
        void SetupRootStoragePools(const TActorId sender) const;

        void SetupDefaultProfiles();

        TIntrusivePtr<::NMonitoring::TDynamicCounters> GetGRpcServerRootCounters() const {
            const auto tenantIt = TenantsGRpc.find(Settings->DomainName);
            Y_ABORT_UNLESS(tenantIt != TenantsGRpc.end());
            Y_ABORT_UNLESS(!tenantIt->second.empty());
            return tenantIt->second.begin()->second.GRpcServerRootCounters;
        }

        void ShutdownGRpc() {
            for (auto& [_, tenantGRpc] : TenantsGRpc) {
                for (auto& [_, nodeGRpc] : tenantGRpc) {
                    nodeGRpc.Shutdown();
                }
            }
        }

        void StartDummyTablets();
        TVector<ui64> StartPQTablets(ui32 pqTabletsN, bool wait = true);
        TTestActorRuntime* GetRuntime() const;
        const TServerSettings& GetSettings() const;
        const NScheme::TTypeRegistry* GetTypeRegistry();
        const NMiniKQL::IFunctionRegistry* GetFunctionRegistry();
        const NYdb::TDriver& GetDriver() const;
        const NYdbGrpc::TGRpcServer& GetGRpcServer() const;
        const NYdbGrpc::TGRpcServer& GetTenantGRpcServer(const TString& tenant) const;

        ui32 StaticNodes() const {
            return Settings->NodeCount;
        }
        ui32 DynamicNodes() const {
            return Settings->DynamicNodeCount;
        }
        void SetupDynamicLocalService(ui32 nodeIdx, const TString &tenantName);
        void DestroyDynamicLocalService(ui32 nodeIdx);
        void WaitFinalization();

    protected:
        const TServerSettings::TConstPtr Settings;
        const bool UseStoragePools;

        std::shared_ptr<void> KqpLoggerScope;
        THolder<TTestActorRuntime> Runtime;
        THolder<NYdb::TDriver> Driver;
        TIntrusivePtr<NBus::TBusMessageQueue> Bus;
        const NBus::TBusServerSessionConfig BusServerSessionConfig; //BusServer hold const & on config
        TAutoPtr<NMsgBusProxy::IMessageBusServer> BusServer;
        NFq::IYqSharedResources::TPtr YqSharedResources;

        TTestActorRuntime::TEventObserverHolder SysViewsRosterUpdateObserver;
        bool SysViewsRosterUpdateFinished;

        struct TGRpcInfo {
            std::unique_ptr<NYdbGrpc::TGRpcServer> GRpcServer;
            TIntrusivePtr<NMonitoring::TDynamicCounters> GRpcServerRootCounters;

            void Shutdown() {
                if (GRpcServer) {
                    GRpcServer->Stop();
                    GRpcServer = nullptr;
                }
            }
        };

        std::unordered_map<TString, std::unordered_map<ui32, TGRpcInfo>> TenantsGRpc;  // tenant -> nodeIdx -> GRpcInfo
    };

    class TClient {
    public:
        struct TFlatQueryOptions {
            TString Params;
            bool IsQueryCompiled = false;
            bool CollectStats = false;
        };

        struct TPathVersion {
            ui64 OwnerId = 0;
            ui64 PathId = 0;
            ui64 Version = 0;
        };

        struct TCreateUserOption {
            TString User;
            TString Password;
            bool CanLogin = true;
        };

        struct TModifyUserOption {
            TString User;
            std::optional<TString> Password;
            std::optional<bool> CanLogin;
        };

        using TApplyIf = TVector<TPathVersion>;

        TClient(const TServerSettings& settings);
        virtual ~TClient();

        const NMsgBusProxy::TMsgBusClientConfig& GetClientConfig() const;
        std::shared_ptr<NMsgBusProxy::TMsgBusClient> GetClient() const;
        bool LoadTypes();
        const NScheme::TTypeRegistry& GetTypeRegistry() const;
        const NScheme::TTypeMetadataRegistry& GetTypeMetadataRegistry() const;
        const NMiniKQL::IFunctionRegistry& GetFunctionRegistry() const;

        template <typename T>
        void PrepareRequest(TAutoPtr<T>&) {}

        void PrepareRequest(TAutoPtr<NMsgBusProxy::TBusRequest>& request) {
            if (!SecurityToken.empty())
                request->Record.SetSecurityToken(SecurityToken);
        }

        void PrepareRequest(TAutoPtr<NMsgBusProxy::TBusPersQueue>& request) {
            if (!SecurityToken.empty())
                request->Record.SetSecurityToken(SecurityToken);
        }

        void PrepareRequest(TAutoPtr<NMsgBusProxy::TBusSchemeOperation>& request) {
            if (!SecurityToken.empty())
                request->Record.SetSecurityToken(SecurityToken);
        }

        void PrepareRequest(TAutoPtr<NMsgBusProxy::TBusSchemeInitRoot>& request) {
            if (!SecurityToken.empty())
                request->Record.SetSecurityToken(SecurityToken);
        }

        void PrepareRequest(TAutoPtr<NMsgBusProxy::TBusSchemeDescribe>& request) {
            if (!SecurityToken.empty())
                request->Record.SetSecurityToken(SecurityToken);
        }

        template <typename T>
        NBus::EMessageStatus SyncCall(TAutoPtr<T> msgHolder, TAutoPtr<NBus::TBusMessage> &reply) {
            NBus::EMessageStatus msgbusStatus = NBus::EMessageStatus::MESSAGE_TIMEOUT;
            const ui64 finishTimeMs = TInstant::Now().MilliSeconds() +  TIME_LIMIT_MS;
            PrepareRequest(msgHolder);
            while (TInstant::Now().MilliSeconds() < finishTimeMs) {
                T* msgCopy(new T());
                msgCopy->Record = msgHolder->Record;
                msgbusStatus = Client->SyncCall(msgCopy, reply);
                if (msgbusStatus == NBus::MESSAGE_CONNECT_FAILED) {
                    Sleep(ITERATION_DURATION);
                    continue;
                } else {
                    break;
                }
            }
            return msgbusStatus;
        }

        static ui64 GetPatchedSchemeRoot(ui64 schemeRoot, ui32 domain, bool supportsRedirect);
        void WaitRootIsUp(const TString& root);
        TAutoPtr<NBus::TBusMessage> InitRootSchemeWithReply(const TString& root);
        void InitRootScheme();
        void InitRootScheme(const TString& root);

        // Flat DB operations
        // Plain methods return request status that should be checked.
        // `Test` prefixed methods check for the success internally.
        NMsgBusProxy::EResponseStatus WaitCreateTx(TTestActorRuntime* runtime, const TString& path, TDuration timeout);
        NMsgBusProxy::EResponseStatus MkDir(const TString& parent, const TString& name, const TApplyIf& applyIf = {});
        void TestMkDir(const TString& parent, const TString& name, const TApplyIf& applyIf = {});
        NMsgBusProxy::EResponseStatus RmDir(const TString& parent, const TString& name, const TApplyIf& applyIf = {});
        void TestRmDir(const TString& parent, const TString& name, const TApplyIf& applyIf = {});
        NMsgBusProxy::EResponseStatus CreateSubdomain(const TString &parent, const TString &description);
        NMsgBusProxy::EResponseStatus CreateSubdomain(const TString& parent, const NKikimrSubDomains::TSubDomainSettings &subdomain);
        NMsgBusProxy::EResponseStatus CreateExtSubdomain(const TString &parent, const TString &description);
        NMsgBusProxy::EResponseStatus CreateExtSubdomain(const TString& parent, const NKikimrSubDomains::TSubDomainSettings &subdomain);
        NMsgBusProxy::EResponseStatus AlterExtSubdomain(const TString &parent, const NKikimrSubDomains::TSubDomainSettings &subdomain, TDuration timeout = TDuration::Seconds(5000));
        NMsgBusProxy::EResponseStatus AlterUserAttributes(const TString &parent, const TString &name, const TVector<std::pair<TString, TString>>& addAttrs, const TVector<TString>& dropAttrs = {}, const TApplyIf& applyIf = {});
        NMsgBusProxy::EResponseStatus AlterSubdomain(const TString &parent, const TString &description, TDuration timeout = TDuration::Seconds(5000));
        NMsgBusProxy::EResponseStatus AlterSubdomain(const TString& parent, const NKikimrSubDomains::TSubDomainSettings &subdomain, TDuration timeout = TDuration::Seconds(5000));
        NMsgBusProxy::EResponseStatus DeleteSubdomain(const TString& parent, const TString &name);
        NMsgBusProxy::EResponseStatus ForceDeleteSubdomain(const TString& parent, const TString &name);
        NMsgBusProxy::EResponseStatus ForceDeleteUnsafe(const TString& parent, const TString &name);

        NMsgBusProxy::EResponseStatus CreateTable(const TString& parent, const TString& scheme, TDuration timeout = TDuration::Seconds(5000));
        NMsgBusProxy::EResponseStatus CreateTable(const TString& parent, const NKikimrSchemeOp::TTableDescription &table, TDuration timeout = TDuration::Seconds(5000));
        NMsgBusProxy::EResponseStatus CreateTableWithUniformShardedIndex(const TString& parent,
            const NKikimrSchemeOp::TTableDescription &table, const TString& indexName,
            const TVector<TString> indexColumns, NKikimrSchemeOp::EIndexType type,
            const TVector<TString> dataColumns = {}, TDuration timeout = TDuration::Seconds(5000));
        NMsgBusProxy::EResponseStatus SplitTable(const TString& table, ui64 datashardId, ui64 border, TDuration timeout = TDuration::Seconds(5000));
        NMsgBusProxy::EResponseStatus CopyTable(const TString& parent, const TString& name, const TString& src);
        NMsgBusProxy::EResponseStatus CreateKesus(const TString& parent, const TString& name);
        NMsgBusProxy::EResponseStatus DeleteKesus(const TString& parent, const TString& name);
        NMsgBusProxy::EResponseStatus ConsistentCopyTables(TVector<std::pair<TString, TString>> desc, TDuration timeout = TDuration::Seconds(5000));
        NMsgBusProxy::EResponseStatus DeleteTable(const TString& parent, const TString& name);
        NMsgBusProxy::EResponseStatus AlterTable(const TString& parent, const NKikimrSchemeOp::TTableDescription& update);
        NMsgBusProxy::EResponseStatus AlterTable(const TString& parent, const TString& alter);
        TAutoPtr<NMsgBusProxy::TBusResponse> AlterTable(const TString& parent, const NKikimrSchemeOp::TTableDescription& update, const TString& userToken);
        TAutoPtr<NMsgBusProxy::TBusResponse> AlterTable(const TString& parent, const TString& alter, const TString& userToken);

        TAutoPtr<NMsgBusProxy::TBusResponse> MoveIndex(const TString& table, const TString& src, const TString& dst, bool allowOverwrite, const TString& userToken);

        NMsgBusProxy::EResponseStatus CreateOlapStore(const TString& parent, const TString& scheme);
        NMsgBusProxy::EResponseStatus CreateOlapStore(const TString& parent, const NKikimrSchemeOp::TColumnStoreDescription& store);
        NMsgBusProxy::EResponseStatus CreateColumnTable(const TString& parent, const TString& scheme);
        NMsgBusProxy::EResponseStatus CreateColumnTable(const TString& parent, const NKikimrSchemeOp::TColumnTableDescription& table);
#if 1 // legacy names
        NMsgBusProxy::EResponseStatus CreateOlapTable(const TString& parent, const TString& scheme) {
            return CreateColumnTable(parent, scheme);
        }
        NMsgBusProxy::EResponseStatus CreateOlapTable(const TString& parent, const NKikimrSchemeOp::TColumnTableDescription& table) {
            return CreateColumnTable(parent, table);
        }
#endif
        NMsgBusProxy::EResponseStatus CreateTopic(const TString& parent, const NKikimrSchemeOp::TPersQueueGroupDescription& topic);
        NMsgBusProxy::EResponseStatus CreateSolomon(const TString& parent, const TString& name, ui32 parts = 4, ui32 channelProfile = 0);
        NMsgBusProxy::EResponseStatus StoreTableBackup(const TString& parent, const NKikimrSchemeOp::TBackupTask& task);
        NMsgBusProxy::EResponseStatus DeleteTopic(const TString& parent, const TString& name);
        TAutoPtr<NMsgBusProxy::TBusResponse> TryDropPersQueueGroup(const TString& parent, const TString& name);
        TAutoPtr<NMsgBusProxy::TBusResponse> Ls(const TString& path);
        static TPathVersion ExtractPathVersion(const TAutoPtr<NMsgBusProxy::TBusResponse>& describe);
        static TVector<ui64> ExtractTableShards(const TAutoPtr<NMsgBusProxy::TBusResponse>& resp);
        bool FlatQuery(TTestActorRuntime* runtime, const TString& mkql, NKikimrMiniKQL::TResult& result);
        bool FlatQuery(TTestActorRuntime* runtime, const TString& mkql, TFlatQueryOptions& opts, NKikimrMiniKQL::TResult& result,
                       const NKikimrClient::TResponse& expectedResponse);
        bool FlatQuery(TTestActorRuntime* runtime, const TString& mkql, TFlatQueryOptions& opts, NKikimrMiniKQL::TResult& result,
                       ui32 expectedStatus = NMsgBusProxy::MSTATUS_OK);

        // returns NMsgBusProxy::MSTATUS_* and the raw response
        ui32 FlatQueryRaw(TTestActorRuntime* runtime, const TString &query, TFlatQueryOptions& opts, NKikimrClient::TResponse& response, int retryCnt = 10);

        bool Compile(const TString &mkql, TString &compiled);
        NKikimrScheme::TEvDescribeSchemeResult Describe(TTestActorRuntime* runtime, const TString& path, ui64 tabletId = SchemeRoot, bool showPrivateTable = false);
        TString CreateStoragePool(const TString& poolKind, const TString& partOfName, ui32 groups = 1);
        NKikimrBlobStorage::TDefineStoragePool DescribeStoragePool(const TString& name);
        void RemoveStoragePool(const TString& name);

        void SetSecurityToken(const TString& token) { SecurityToken = token; }

        // User operations
        NMsgBusProxy::EResponseStatus CreateUser(const TString& parent, const TCreateUserOption& options, const TString& userToken = "");
        void TestCreateUser(const TString& parent, const TCreateUserOption& options, const TString& userToken = "");
        NMsgBusProxy::EResponseStatus CreateUser(const TString& parent, const TString& user, const TString& password, const TString& userToken = "");
        void TestCreateUser(const TString& parent, const TString& user, const TString& password, const TString& userToken = "");
        NMsgBusProxy::EResponseStatus ModifyUser(const TString& parent, const TModifyUserOption& options, const TString& userToken = "");
        void TestModifyUser(const TString& parent, const TModifyUserOption& options, const TString& userToken = "");
        NMsgBusProxy::EResponseStatus DeleteUser(const TString& parent, const TModifyUserOption& options, const TString& userToken = "");
        void TestDeleteUser(const TString& parent, const TModifyUserOption& options, const TString& userToken = "");
        NMsgBusProxy::EResponseStatus CreateGroup(const TString& parent, const TString& group);
        void TestCreateGroup(const TString& parent, const TString& group);
        NMsgBusProxy::EResponseStatus AddGroupMembership(const TString& parent, const TString& group, const TString& member);
        void TestAddGroupMembership(const TString& parent, const TString& group, const TString& member);
        NMsgBusProxy::EResponseStatus DeleteGroup(const TString& parent, const TString& group);
        void TestDeleteGroup(const TString& parent, const TString& group);
        NKikimrScheme::TEvLoginResult Login(TTestActorRuntime& runtime, const TString& user, const TString& password);

        // ACL operations
        NMsgBusProxy::EResponseStatus ModifyOwner(const TString& parent, const TString& name, const TString& owner);
        void TestModifyOwner(const TString& parent, const TString& name, const TString& owner);
        NMsgBusProxy::EResponseStatus ModifyACL(const TString& parent, const TString& name, const TString& acl);
        void TestModifyACL(const TString& parent, const TString& name, const TString& acl);

        NMsgBusProxy::EResponseStatus Grant(const TString& parent, const TString& name, const TString& subject, NACLib::EAccessRights rights);
        void TestGrant(const TString& parent, const TString& name, const TString& subject, NACLib::EAccessRights rights);
        NMsgBusProxy::EResponseStatus GrantConnect(const TString& subject);
        void TestGrantConnect(const TString& subject);

        // Helper functions
        TAutoPtr<NMsgBusProxy::TBusResponse> HiveCreateTablet(ui32 domainUid, ui64 owner, ui64 owner_index, TTabletTypes::EType tablet_type,
                const TVector<ui32>& allowed_node_ids, const TVector<TSubDomainKey>& allowed_domains = {}, const TChannelsBindings& binding = {});

        TString SendTabletMonQuery(TTestActorRuntime* runtime, ui64 tabletId, TString query);
        TString MarkNodeInHive(TTestActorRuntime* runtime, ui32 nodeIdx, bool up);
        TString KickNodeInHive(TTestActorRuntime* runtime, ui32 nodeIdx);
        bool WaitForTabletAlive(TTestActorRuntime* runtime, ui64 tabletId, bool leader, TDuration timeout);
        bool WaitForTabletDown(TTestActorRuntime* runtime, ui64 tabletId, bool leader, TDuration timeout);
        ui32 GetLeaderNode(TTestActorRuntime* runtime, ui64 tabletId);
        bool TabletExistsInHive(TTestActorRuntime* runtime, ui64 tabletId, bool evenInDeleting = false);
        TVector<ui32> GetFollowerNodes(TTestActorRuntime *runtime, ui64 tabletId);

        void GetTabletInfoFromHive(TTestActorRuntime* runtime, ui64 tabletId, bool returnFollowers, NKikimrHive::TEvResponseHiveInfo& res);
        void GetTabletStorageInfoFromHive(TTestActorRuntime* runtime, ui64 tabletId, NKikimrHive::TEvGetTabletStorageInfoResult& res);

        static void RefreshPathCache(TTestActorRuntime* runtime, const TString& path, ui32 nodeIdx = 0);

        ui64 GetKesusTabletId(const TString& kesusPath);
        Ydb::StatusIds::StatusCode AddQuoterResource(TTestActorRuntime* runtime, const TString& kesusPath, const TString& resourcePath, const NKikimrKesus::THierarchicalDRRResourceConfig& props);
        Ydb::StatusIds::StatusCode AddQuoterResource(TTestActorRuntime* runtime, const TString& kesusPath, const TString& resourcePath, const TMaybe<double> maxUnitsPerSecond = Nothing());

        THolder<NKesus::TEvKesus::TEvGetConfigResult> GetKesusConfig(TTestActorRuntime* runtime, const TString& kesusPath);

    protected:
        TString PrintToString(const ::google::protobuf::Message& msg, size_t maxSz = 1000) {
            TString s;
            ::google::protobuf::TextFormat::PrintToString(msg, &s);
            if (s.size() > maxSz) {
                s.resize(maxSz);
                s += "...\n(TRUNCATED)\n";
            }
            return s;
        }

        template <class TMsg>
        TString PrintToString(const NBus::TBusMessage* msg, size_t maxSz = 1000) {
            auto res = dynamic_cast<const TMsg*>(msg);
            return PrintToString(res->Record, maxSz);
        }

        // Waits for kikimr server to become ready
        template <class TReq>
        NBus::EMessageStatus SendWhenReady(TAutoPtr<TReq> request, TAutoPtr<NBus::TBusMessage>& reply, const ui32 timeout = 5000) {
            TInstant deadline = TInstant::Now() + TDuration::MilliSeconds(timeout);
            NBus::EMessageStatus status = NBus::MESSAGE_UNKNOWN;
            // Server might not be ready
            do {
                TAutoPtr<TReq> msgCopy(new TReq());
                msgCopy->Record = request->Record;
                status = SyncCall(msgCopy, reply);

                if (status != NBus::MESSAGE_OK)
                    return status;

                const NMsgBusProxy::TBusResponse* notReadyResp = dynamic_cast<const NMsgBusProxy::TBusResponse*>(reply.Get());
                if (!notReadyResp)
                    break;

                if (notReadyResp->Record.GetStatus() != NMsgBusProxy::MSTATUS_NOTREADY)
                    break;

                // Retry if the server wasn't ready yet
                Sleep(TDuration::MilliSeconds(10));
            } while (TInstant::Now() < deadline);

            return status;
        }

        // Waits for scheme operation to complete
        NBus::EMessageStatus WaitCompletion(ui64 txId, ui64 schemeshard, ui64 pathId,
                                            TAutoPtr<NBus::TBusMessage>& reply,
                                            TDuration timeout = TDuration::Seconds(1000));
        NBus::EMessageStatus SendAndWaitCompletion(TAutoPtr<NMsgBusProxy::TBusSchemeOperation> request,
                                                   TAutoPtr<NBus::TBusMessage>& reply,
                                                   TDuration timeout = TDuration::Seconds(1000));

        ui32 NodeIdToIndex(TTestActorRuntime* runtime, ui32 id) {
            ui32 offset = runtime->GetNodeId(0);
            Y_ABORT_UNLESS(id >= offset, "NodeId# %" PRIu32 " offset# %" PRIu32, id, offset);
            return id - offset;
        }

        TAutoPtr<NMsgBusProxy::TBusResponse> LsImpl(const TString& path);

        static void SetApplyIf(NKikimrSchemeOp::TModifyScheme& transaction, const TApplyIf& applyIf) {
            for (auto& pathVer: applyIf) {
                auto item = transaction.AddApplyIf();
                item->SetPathId(pathVer.PathId);
                item->SetPathVersion(pathVer.Version);
            }
        }

    protected:
        using TStoragePoolKinds = TDomainsInfo::TDomain::TStoragePoolKinds;

        const ui32 Domain;
        const TString DomainName;
        const bool SupportsRedirect;
        const TStoragePoolKinds StoragePoolTypes;
        const bool Verbose;
        NScheme::TKikimrTypeRegistry TypeRegistry;
        TIntrusivePtr<NMiniKQL::IFunctionRegistry> FunctionRegistry;
        NMsgBusProxy::TMsgBusClientConfig ClientConfig;
        std::shared_ptr<NMsgBusProxy::TMsgBusClient> Client;
        TMaybe<ui64> TypesEtag;
        NScheme::TTypeMetadataRegistry LoadedTypeMetadataRegistry;
        TIntrusivePtr<NMiniKQL::IFunctionRegistry> LoadedFunctionRegistry;
        TString SecurityToken;
    };

    struct TTenants {
    private:
        Tests::TServer::TPtr Server;

        TVector<ui32> VacantNodes;
        TMap<TString, TVector<ui32>> Tenants;

    public:
        TTenants(Tests::TServer::TPtr server);
        ~TTenants();

        void Run(const TString &name, ui32 nodes = 1);
        void Stop(const TString &name);
        void Stop();

        void Add(const TString &name, ui32 nodes = 1);
        void Free(const TString &name, ui32 nodes = 1);

        bool IsActive(const TString &name, ui32 nodeIdx) const;
        void FreeNode(const TString &name, ui32 nodeIdx);

        bool IsStaticNode(ui32 nodeIdx) const;
        const TVector<ui32>& List(const TString &name) const;
        ui32 Size(const TString &name) const;
        ui32 Size() const;
        ui32 Availabe() const;
        ui32 Capacity() const;

        void CreateTenant(Ydb::Cms::CreateDatabaseRequest request, ui32 nodes = 1, TDuration timeout = TDuration::Seconds(30), bool acceptAlreadyExist = false);

    private:
        TVector<ui32>& Nodes(const TString &name);
        void StopNode(const TString /*name*/, ui32 nodeIdx);
        void RunNode(const TString &name, ui32 nodeIdx);
        void StopPaticularNode(const TString &name, ui32 nodeIdx);
        void StopNodes(const TString &name, ui32 count);
        void RunNodes(const TString &name, ui32 count);
        ui32 AllocNodeIdx();
        void FreeNodeIdx(ui32 nodeIdx);
    };

}
}
