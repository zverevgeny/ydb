# -*- coding: utf-8 -*-
import os
import pytest
import yatest

from ydb.tests.library.common.types import Erasure
from ydb.tests.library.stress.fixtures import StressFixture
from ydb.tests.library.harness.util import LogLevels
import logging

logger = logging.getLogger(__name__)


class TestYdbWorkload(StressFixture):
    @pytest.fixture(autouse=True, scope="function")
    def setup(self):
        yield from self.setup_cluster(
            erasure=Erasure.MIRROR_3_DC,
            extra_feature_flags={
                "enable_external_data_sources": True,
            },
            additional_log_configs={
                'KQP_COMPUTE': LogLevels.DEBUG,
                'KQP_PROXY': LogLevels.DEBUG,
                'KQP_EXECUTER': LogLevels.DEBUG,
            },
        )

    def test(self):
        logger.info("TestYdbWorkload::start test")
        cmd = [
            yatest.common.binary_path(os.getenv("YDB_TEST_PATH")),
            "--endpoint", f"localhost:{self.cluster.nodes[1].port}",
            "--database", self.database,
            "--duration", self.base_duration,
            "--prefix", "yt_queue_stress",
        ]
        # A live YTsaurus cluster is optional. When YT_STRESS_ENDPOINT and
        # YT_STRESS_QUEUE are provided in the environment, the workload will also
        # execute read queries against the YT queue through the EDS. Otherwise it
        # exercises the EDS create/describe/drop path only.
        yt_endpoint = os.getenv("YT_STRESS_ENDPOINT", "")
        yt_token = os.getenv("YT_STRESS_TOKEN", "")
        yt_queue = os.getenv("YT_STRESS_QUEUE", "")
        if yt_endpoint:
            cmd += ["--yt-endpoint", yt_endpoint]
        if yt_token:
            cmd += ["--yt-token", yt_token]
        if yt_queue:
            cmd += ["--yt-queue", yt_queue]

        yatest.common.execute(cmd, wait=True)
