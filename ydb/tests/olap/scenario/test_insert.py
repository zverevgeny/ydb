from conftest import BaseTestSet
from ydb.tests.olap.scenario.helpers import (
    ScenarioTestHelper,
    TestContext,
    CreateTable,
)
from helpers.thread_helper import TestThread
from ydb import PrimitiveType
from typing import List, Dict, Any
from ydb.tests.olap.lib.utils import get_external_param
import threading
import helpers.data_generators as dg


class TestInsertAndSelect(BaseTestSet):
    table_name = "table"
    schema = (
        ScenarioTestHelper.Schema()
        .with_column(name="key", type=PrimitiveType.Int32, not_null=True)
        .with_column(name="c", type=PrimitiveType.Int64)
        .with_key_columns("key")
    )
    batch_size = int(get_external_param("batch_size", "10000"))
    batch_count = int(get_external_param("batch_count", "100000"))
    stop = threading.Event()


    def _loop_upsert_data(self, ctx: TestContext):
        sth = ScenarioTestHelper(ctx)
        for _ in range(self.batch_count):
            sth.bulk_upsert(
                self.table_name,
                dg.DataGeneratorPerColumn(self.schema, self.batch_size)
                    .with_column("key", dg.ColumnValueGeneratorRandom(null_probability=0))
                    .with_column("c", dg.ColumnValueGeneratorRandom(null_probability=0))
            )
        self.stop.set()

    def _loop_select_count(self, ctx: TestContext):
        sth = ScenarioTestHelper(ctx)
        full_table_name = sth.get_full_path(self.table_name)
        while not self.stop.is_set():
            sth.execute_query(
                f'SELECT COUNT(*) FROM `{full_table_name}`'
            )

    def scenario_read_data_during_bulk_upsert(self, ctx: TestContext):
        sth = ScenarioTestHelper(ctx)
        sth.execute_scheme_query(
            CreateTable(self.table_name).with_schema(self.schema)
        )

        upsert_thread = TestThread(target=self._loop_upsert_data, args=[ctx])
        select_thread = TestThread(target=self._loop_select_count, args=[ctx])

        upsert_thread.start()
        select_thread.start()

        upsert_thread.join()
        select_thread.join()

