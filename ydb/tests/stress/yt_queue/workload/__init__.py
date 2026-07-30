# -*- coding: utf-8 -*-
import ydb
from ydb.tests.stress.common.instrumented_pools import InstrumentedQuerySessionPool
import time
import logging

logger = logging.getLogger(__name__)


class Workload():
    """
    Stress workload that repeatedly creates External Data Sources pointing to a
    YTsaurus cluster and (optionally) executes read queries against YT queues
    through those EDS.

    When ``yt_endpoint`` is not provided the workload only exercises the
    EDS-to-YT create/describe/drop path (SchemeShard + query planning), which is
    what is available in CI without a live YTsaurus cluster. When a real YT
    endpoint and token are supplied, it additionally runs SELECT queries against
    YT queues referenced through the EDS.
    """

    def __init__(self, endpoint: str, database: str, duration: int, prefix: str,
                 yt_endpoint: str = "", yt_token: str = "", yt_queue: str = ""):
        self.database = database
        self.endpoint = endpoint
        self.driver = ydb.Driver(ydb.DriverConfig(endpoint, database))
        self.pool = InstrumentedQuerySessionPool(self.driver)
        self.duration = duration
        self.prefix = prefix
        self.source_name = f'{prefix}/yt_source'
        self.yt_endpoint = yt_endpoint
        self.yt_token = yt_token
        self.yt_queue = yt_queue
        logger.info("Workload::init")

    def create_external_data_source(self):
        logger.info("Workload::create_external_data_source")
        # If a token was supplied use TOKEN auth, otherwise NONE. Both auth
        # methods are supported by the YT external data source.
        if self.yt_token:
            auth = 'AUTH_METHOD="TOKEN", TOKEN_SECRET_NAME="yt_token_secret"'
        else:
            auth = 'AUTH_METHOD="NONE"'
        location = self.yt_endpoint or "yt-cluster.example.com:8080"
        self.pool.execute_with_retries(
            f"""
                CREATE EXTERNAL DATA SOURCE `{self.source_name}` WITH (
                    SOURCE_TYPE="YT",
                    LOCATION="{location}",
                    {auth});
            """
        )

    def drop_external_data_source(self):
        logger.info("Workload::drop_external_data_source")
        self.pool.execute_with_retries(
            f"""
                DROP EXTERNAL DATA SOURCE `{self.source_name}`;
            """
        )

    def check_external_data_source_exists(self):
        logger.info("Workload::check_external_data_source_exists")
        # Assert that the EDS object was created and is resolvable in the scheme.
        self.driver.scheme_client.describe_path(f"{self.database}/{self.source_name}")

    def read_from_yt_queue(self):
        if not self.yt_endpoint or not self.yt_queue:
            logger.info("Workload::read_from_yt_queue skipped (no YT endpoint/queue configured)")
            return
        logger.info("Workload::read_from_yt_queue")
        self.pool.execute_with_retries(
            f"""
                SELECT * FROM `{self.source_name}`.`{self.yt_queue}` WITH (
                    FORMAT = 'json_each_row',
                    SCHEMA (data String NOT NULL)
                ) LIMIT 100;
            """
        )

    def loop(self):
        finished_at = time.time() + self.duration
        iterations = 0
        while time.time() < finished_at:
            self.create_external_data_source()
            self.check_external_data_source_exists()
            self.read_from_yt_queue()
            self.drop_external_data_source()
            iterations += 1
        logger.info(f"Workload::loop finished after {iterations} iterations")
        assert iterations > 0, "workload did not complete a single iteration"

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.pool.stop()
        self.driver.stop()
