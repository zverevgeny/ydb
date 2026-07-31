# -*- coding: utf-8 -*-
import os
import pytest
import yatest
from ydb.tests.library.common.types import Erasure

from ydb.tests.library.stress.fixtures import StressFixture


class TestYdbWorkload(StressFixture):
    @pytest.fixture(autouse=True, scope="function")
    def setup(self):
        yield from self.setup_cluster(
            erasure=Erasure.NONE,
            table_service_config={
                "enable_new_rbo": True,
            },
        )

    def test(self):
        # Run several bounded input sizes; for each size the workload runs the
        # JOIN both WITHOUT and WITH the `ydb.EnableLikeJoinHyperscan` pragma,
        # verifies the LIKE predicate, and fails if the two result sets differ.
        yatest.common.execute([
            yatest.common.binary_path(os.environ["YDB_WORKLOAD_PATH"]),
            "--endpoint", self.endpoint,
            "--database", self.database,
            "--sizes", "50:5,500:20,5000:50",
        ])
