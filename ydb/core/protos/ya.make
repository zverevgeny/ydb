PROTO_LIBRARY()

SET(PROTOC_TRANSITIVE_HEADERS "no")

GRPC()

IF (OS_WINDOWS)
    NO_OPTIMIZE_PY_PROTOS()
ENDIF()

SRCS(
    alloc.proto
    auth.proto
    backup.proto
    base.proto
    bind_channel_storage_pool.proto
    blob_depot.proto
    blob_depot_config.proto
    blobstorage.proto
    blobstorage_base.proto
    blobstorage_base3.proto
    blobstorage_config.proto
    blobstorage_disk.proto
    blobstorage_disk_color.proto
    blobstorage_distributed_config.proto
    blobstorage_pdisk_config.proto
    blobstorage_vdisk_config.proto
    blobstorage_vdisk_internal.proto
    blockstore_config.proto
    bootstrap.proto
    bootstrapper.proto
    bridge.proto
    change_exchange.proto
    channel_purpose.proto
    cms.proto
    compaction.proto
    compile_service_config.proto
    config.proto
    config_units.proto
    console.proto
    console_base.proto
    console_config.proto
    console_tenant.proto
    counters.proto
    counters_backup.proto
    counters_blob_depot.proto
    counters_bs_controller.proto
    counters_cms.proto
    counters_columnshard.proto
    counters_coordinator.proto
    counters_datashard.proto
    counters_hive.proto
    counters_kesus.proto
    counters_keyvalue.proto
    counters_mediator.proto
    counters_node_broker.proto
    counters_pq.proto
    counters_replication.proto
    counters_schemeshard.proto
    counters_sequenceshard.proto
    counters_statistics_aggregator.proto
    counters_sysview_processor.proto
    counters_testshard.proto
    counters_tx_allocator.proto
    counters_tx_proxy.proto
    data_events.proto
    data_integrity_trails.proto
    database_basic_sausage_metainfo.proto
    datashard_backup.proto
    datashard_config.proto
    datashard_load.proto
    db_metadata_cache.proto
    drivemodel.proto
    export.proto
    external_sources.proto
    feature_flags.proto
    filestore_config.proto
    flat_scheme_op.proto
    flat_tx_scheme.proto
    follower_group.proto
    grpc.proto
    grpc_pq_old.proto
    grpc_status_proxy.proto
    health.proto
    hive.proto
    http_config.proto
    import.proto
    index_builder.proto
    kafka.proto
    kesus.proto
    key.proto
    kqp.proto
    kqp_physical.proto
    kqp_stats.proto
    labeled_counters.proto
    load_test.proto
    local.proto
    long_tx_service.proto
    maintenance.proto
    memory_controller_config.proto
    memory_stats.proto
    metrics.proto
    minikql_engine.proto
    mon.proto
    msgbus.proto
    msgbus_health.proto
    msgbus_kv.proto
    msgbus_pq.proto
    netclassifier.proto
    node_broker.proto
    node_limits.proto
    node_whiteboard.proto
    pdiskfit.proto
    pqconfig.proto
    profiler.proto
    query_stats.proto
    replication.proto
    resource_broker.proto
    s3_settings.proto
    scheme_board.proto
    scheme_board_mon.proto
    scheme_log.proto
    scheme_type_metadata.proto
    scheme_type_operation.proto
    serverless_proxy_config.proto
    shared_cache.proto
    sqs.proto
    statestorage.proto
    statistics.proto
    stream.proto
    subdomains.proto
    sys_view.proto
    sys_view_types.proto
    table_service_config.proto
    table_stats.proto
    tablet.proto
    tablet_counters.proto
    tablet_counters_aggregator.proto
    tablet_database.proto
    tablet_pipe.proto
    tablet_tracing_signals.proto
    tablet_tx.proto
    tenant_pool.proto
    tenant_slot_broker.proto
    test_shard.proto
    tracing.proto
    tx.proto
    tx_columnshard.proto
    tx_datashard.proto
    tx_mediator_timecast.proto
    tx_proxy.proto
    tx_scheme.proto
    tx_sequenceshard.proto
    whiteboard_disk_states.proto
    whiteboard_flags.proto
    workload_manager_config.proto
    ydb_result_set_old.proto
    ydb_table_impl.proto
    yql_translation_settings.proto
)

GENERATE_ENUM_SERIALIZATION(blobstorage_pdisk_config.pb.h)
GENERATE_ENUM_SERIALIZATION(datashard_load.pb.h)
GENERATE_ENUM_SERIALIZATION(shared_cache.pb.h)

PEERDIR(
    ydb/core/config/protos
    ydb/core/fq/libs/config/protos
    ydb/core/protos/nbs
    ydb/core/protos/schemeshard
    ydb/core/scheme/protos
    ydb/core/tx/columnshard/common/protos
    ydb/core/tx/columnshard/engines/protos
    ydb/core/tx/columnshard/engines/scheme/defaults/protos
    ydb/library/actors/protos
    ydb/library/formats/arrow/protos
    ydb/library/login/protos
    ydb/library/mkql_proto/protos
    ydb/library/services
    ydb/library/ydb_issue/proto
    ydb/library/yql/dq/actors/protos
    ydb/library/yql/dq/proto
    ydb/public/api/protos
    ydb/public/api/protos/annotations
    yql/essentials/core/file_storage/proto
    yql/essentials/core/issue/protos
    yql/essentials/providers/common/proto
    yql/essentials/public/issue/protos
    yql/essentials/public/types
)

CPP_PROTO_PLUGIN0(config_proto_plugin ydb/core/config/tools/protobuf_plugin)

EXCLUDE_TAGS(GO_PROTO)

END()
