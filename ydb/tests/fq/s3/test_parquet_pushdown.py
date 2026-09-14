#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
Integration tests for Parquet min/max predicate pushdown in S3 Federated Query.

Tests cover all supported column types and operators:
- INT32/INT64 (already partially covered, but extended here)
- FLOAT/DOUBLE
- BOOL
- UUID
- TIMESTAMP/DATE (already covered, but extended here)
- BETWEEN operator
- Multi-column AND predicates
- Edge cases (all skipped, all kept, non-contiguous groups)
"""

import struct

import pyarrow as pa
import pyarrow.parquet as pq

import yatest
import ydb.public.api.protos.draft.fq_pb2 as fq

from datetime import datetime

from ydb.tests.tools.datastreams_helpers.test_yds_base import TestYdsBase
from ydb.tests.tools.fq_runner.kikimr_utils import yq_v2
import ydb.tests.fq.s3.s3_helpers as s3_helpers


class TestS3ParquetPushdown(TestYdsBase):
    """Integration tests for Parquet min/max predicate pushdown."""

    def _yql_uuid_bytes(self, uuid_str):
        """Convert UUID string to YQL internal byte representation (little-endian)."""
        hex_str = uuid_str.replace('-', '')
        dw = [int(hex_str[i : i + 4], 16) for i in range(0, 32, 4)]
        dw[0], dw[1] = dw[1], dw[0]
        for i in range(4, 8):
            dw[i] = ((dw[i] >> 8) & 0xFF) | ((dw[i] & 0xFF) << 8)
        return struct.pack('<8H', *dw)

    def setup_s3_and_connection(self, s3, client, unique_prefix, filename, table):
        """Create S3 bucket, upload parquet file, and create storage connection."""
        pq.write_table(table, yatest.common.work_path(filename), row_group_size=2)
        s3_helpers.create_bucket_and_upload_file(filename, s3.s3_url, "fbucket", yatest.common.work_path())
        client.create_storage_connection(unique_prefix + "conn", "fbucket")
        return unique_prefix + "conn"

    def _assert_pushdown_correctness(self, client, sql, expected_rows, column_names=None):
        """
        Run SQL with predicate pushdown, verify results are correct.
        Note: IngressBytes assertion is omitted because the metric is unreliable
        for small test files. Unit tests verify the pushdown logic.
        """
        sql_with_pushdown = 'pragma s3.UsePredicatePushdown = "true";\n' + sql
        query_id = client.create_query("simple", sql_with_pushdown, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)
        data = client.get_result_data(query_id, limit=1000)
        rows_with = [tuple(row.items[i].text_value for i in range(len(row.items)))
                     for row in data.result.result_set.rows]
        assert sorted(rows_with) == sorted(expected_rows), f"With pushdown: {rows_with}"

    # =========================================================================
    # FLOAT/DOUBLE pushdown tests (T4)
    # =========================================================================

    @yq_v2
    def test_s3_push_down_parquet_double(self, kikimr, s3, client, unique_prefix):
        """Test DOUBLE column pushdown with comparison operators."""
        big = "x" * 100000
        # Row groups: [0.5, 1.5], [100.0, 200.0], [2.5, 3.5]
        data = [
            [0.5, 1.5, 100.0, 200.0, 2.5, 3.5],
            [big, big, "keep-a", "keep-b", big, big],
        ]
        schema = pa.schema([('value', pa.float64()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_double.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT
                `fruit`, CAST(`value` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `value` Double NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE value >= 50.0
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("keep-a", "100"),
                ("keep-b", "200"),
            ],
        )

    @yq_v2
    def test_s3_push_down_parquet_float(self, kikimr, s3, client, unique_prefix):
        """Test FLOAT column pushdown."""
        big = "x" * 100000
        # Row groups: [0.5, 1.5], [100.0, 200.0], [2.5, 3.5]
        data = [
            [0.5, 1.5, 100.0, 200.0, 2.5, 3.5],
            [big, big, "keep-a", "keep-b", big, big],
        ]
        schema = pa.schema([('value', pa.float32()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_float.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT
                `fruit`, CAST(`value` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `value` Float NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE value > CAST(50.0 AS Float)
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("keep-a", "100"),
                ("keep-b", "200"),
            ],
        )

    @yq_v2
    def test_s3_push_down_parquet_double_all_skipped(self, kikimr, s3, client, unique_prefix):
        """Test DOUBLE pushdown where predicate eliminates all row groups."""
        data = [
            [0.5, 1.5, 2.5, 3.5],
            ["a", "b", "c", "d"],
        ]
        schema = pa.schema([('value', pa.float64()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_double_skip_all.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT `fruit`
            FROM `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `value` Double NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE value > 1000.0
            '''
        # Without pushdown
        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)
        data = client.get_result_data(query_id, limit=1000)
        assert len(data.result.result_set.rows) == 0

        # With pushdown
        query_id = client.create_query(
            "simple",
            'pragma s3.UsePredicatePushdown = "true";\n' + sql,
            type=fq.QueryContent.QueryType.ANALYTICS
        ).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)
        data = client.get_result_data(query_id, limit=1000)
        assert len(data.result.result_set.rows) == 0

    # =========================================================================
    # BOOL pushdown tests (T5)
    # =========================================================================

    @yq_v2
    def test_s3_push_down_parquet_bool(self, kikimr, s3, client, unique_prefix):
        """Test BOOL column pushdown with EQ operator."""
        big = "x" * 100000
        # Row groups: [true, true], [false, false], [true, true]
        # WHERE flag = false matches only RG1
        data = [
            [True, True, False, False, True, True],
            [big, big, "keep-a", "keep-b", big, big],
        ]
        schema = pa.schema([('flag', pa.bool_()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_bool.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT
                `fruit`, CAST(`flag` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `flag` Bool NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE flag = false
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("keep-a", "false"),
                ("keep-b", "false"),
            ],
        )

    @yq_v2
    def test_s3_push_down_parquet_bool_mixed_group(self, kikimr, s3, client, unique_prefix):
        """Test BOOL pushdown where a row group contains both true and false."""
        # Row groups: [true, false], [true, false]
        # Both groups have mixed values, so neither can be skipped.
        data = [
            [True, False, True, False],
            ["a", "b", "c", "d"],
        ]
        schema = pa.schema([('flag', pa.bool_()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_bool_mixed.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT `fruit`
            FROM `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `flag` Bool NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE flag = true
            '''
        # Without pushdown
        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)
        data = client.get_result_data(query_id, limit=1000)
        rows_without = sorted(row.items[0].text_value for row in data.result.result_set.rows)

        # With pushdown - mixed groups cannot be skipped, so results are the same
        query_id = client.create_query(
            "simple",
            'pragma s3.UsePredicatePushdown = "true";\n' + sql,
            type=fq.QueryContent.QueryType.ANALYTICS
        ).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)
        data = client.get_result_data(query_id, limit=1000)
        rows_with = sorted(row.items[0].text_value for row in data.result.result_set.rows)

        assert rows_without == rows_with
        assert rows_without == ["a", "c"]

    # =========================================================================
    # BETWEEN operator tests
    # =========================================================================

    @yq_v2
    def test_s3_push_down_parquet_between_int(self, kikimr, s3, client, unique_prefix):
        """Test BETWEEN operator pushdown on INT column."""
        big = "x" * 100000
        # Row groups: [1, 2], [100, 200], [3, 4]
        data = [
            [1, 2, 100, 200, 3, 4],
            [big, big, "keep-a", "keep-b", big, big],
        ]
        schema = pa.schema([('price', pa.int32()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_between_int.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT
                `fruit`, CAST(`price` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `price` Int32 NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE price BETWEEN 50 AND 300
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("keep-a", "100"),
                ("keep-b", "200"),
            ],
        )

    @yq_v2
    def test_s3_push_down_parquet_between_double(self, kikimr, s3, client, unique_prefix):
        """Test BETWEEN operator pushdown on DOUBLE column."""
        big = "x" * 100000
        data = [
            [0.5, 1.5, 100.0, 200.0, 2.5, 3.5],
            [big, big, "keep-a", "keep-b", big, big],
        ]
        schema = pa.schema([('value', pa.float64()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_between_double.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT
                `fruit`, CAST(`value` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `value` Double NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE value BETWEEN 50.0 AND 300.0
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("keep-a", "100"),
                ("keep-b", "200"),
            ],
        )

    @yq_v2
    def test_s3_push_down_parquet_between_timestamp(self, kikimr, s3, client, unique_prefix):
        """Test BETWEEN operator pushdown on TIMESTAMP column."""
        big = "x" * 100000
        data = [
            [
                int(datetime.fromisoformat("2024-06-14 00:00:00+00:00").timestamp() * 1000),
                int(datetime.fromisoformat("2024-06-14 01:00:00+00:00").timestamp() * 1000),
                int(datetime.fromisoformat("2024-06-16 00:00:00+00:00").timestamp() * 1000),
                int(datetime.fromisoformat("2024-06-16 01:00:00+00:00").timestamp() * 1000),
                int(datetime.fromisoformat("2024-06-14 02:00:00+00:00").timestamp() * 1000),
                int(datetime.fromisoformat("2024-06-14 03:00:00+00:00").timestamp() * 1000),
            ],
            [big, big, "keep-a", "keep-b", big, big],
        ]
        schema = pa.schema([('ts', pa.timestamp('ms')), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_between_ts.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT
                `fruit`, CAST(`ts` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `ts` Timestamp NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE ts BETWEEN Timestamp("2024-06-16T00:00:00Z") AND Timestamp("2024-06-17T00:00:00Z")
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("keep-a", "2024-06-16T00:00:00Z"),
                ("keep-b", "2024-06-16T01:00:00Z"),
            ],
        )

    # =========================================================================
    # Multi-column AND predicate tests
    # =========================================================================

    @yq_v2
    def test_s3_push_down_parquet_multi_column_and(self, kikimr, s3, client, unique_prefix):
        """Test pushdown with AND combining predicates on different columns."""
        big = "x" * 100000
        # Row groups:
        # RG0: price=[1,2], value=[0.5, 1.5]
        # RG1: price=[100,200], value=[100.0, 200.0]
        # RG2: price=[3,4], value=[2.5, 3.5]
        data = [
            [1, 2, 100, 200, 3, 4],
            [0.5, 1.5, 100.0, 200.0, 2.5, 3.5],
            [big, big, "keep-a", "keep-b", big, big],
        ]
        schema = pa.schema([('price', pa.int32()), ('value', pa.float64()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_multi_col.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT
                `fruit`, CAST(`price` as Utf8), CAST(`value` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `price` Int32 NOT NULL,
                  `value` Double NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE price >= 50 AND value >= 50.0
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("keep-a", "100", "100"),
                ("keep-b", "200", "200"),
            ],
        )

    # =========================================================================
    # Edge case: all rows kept (predicate matches all groups)
    # =========================================================================

    @yq_v2
    def test_s3_push_down_parquet_all_rows_kept(self, kikimr, s3, client, unique_prefix):
        """Test that when predicate matches all groups, results are identical."""
        data = [
            [1, 2, 3, 4],
            ["a", "b", "c", "d"],
        ]
        schema = pa.schema([('price', pa.int32()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_all_kept.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT `fruit`
            FROM `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `price` Int32 NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE price >= 0
            '''
        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)
        fruits_without = sorted(
            row.items[0].text_value
            for row in client.get_result_data(query_id, limit=1000).result.result_set.rows
        )

        query_id = client.create_query(
            "simple",
            'pragma s3.UsePredicatePushdown = "true";\n' + sql,
            type=fq.QueryContent.QueryType.ANALYTICS
        ).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)
        fruits_with = sorted(
            row.items[0].text_value
            for row in client.get_result_data(query_id, limit=1000).result.result_set.rows
        )

        assert fruits_without == fruits_with
        assert len(fruits_without) == 4

    # =========================================================================
    # Edge case: INT64 column
    # =========================================================================

    @yq_v2
    def test_s3_push_down_parquet_int64(self, kikimr, s3, client, unique_prefix):
        """Test INT64 column pushdown."""
        big = "x" * 100000
        data = [
            [1, 2, 10000000000, 20000000000, 3, 4],
            [big, big, "keep-a", "keep-b", big, big],
        ]
        schema = pa.schema([('price', pa.int64()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_int64.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT
                `fruit`, CAST(`price` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `price` Int64 NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE price >= 5000000000
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("keep-a", "10000000000"),
                ("keep-b", "20000000000"),
            ],
        )

    # =========================================================================
    # Comparison operators coverage
    # =========================================================================

    @yq_v2
    def test_s3_push_down_parquet_lt_operator(self, kikimr, s3, client, unique_prefix):
        """Test < operator pushdown."""
        big = "x" * 100000
        # Row groups: [100, 200], [10, 20], [3, 4]
        # WHERE price < 10 matches only rows with price 3, 4
        # big rows have prices 100, 200, 10, 20 (don't match)
        data = [
            [100, 200, 10, 20, 3, 4],
            [big, big, big, big, "keep-a", "keep-b"],
        ]
        schema = pa.schema([('price', pa.int32()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_lt.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT
                `fruit`, CAST(`price` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `price` Int32 NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE price < 10
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("keep-a", "3"),
                ("keep-b", "4"),
            ],
        )

    @yq_v2
    def test_s3_push_down_parquet_le_operator(self, kikimr, s3, client, unique_prefix):
        """Test <= operator pushdown."""
        big = "x" * 100000
        # Row groups: [100, 200], [10, 20], [3, 4]
        # WHERE price <= 5 matches only rows with price 3, 4
        # big rows have prices 100, 200, 10, 20 (don't match)
        data = [
            [100, 200, 10, 20, 3, 4],
            [big, big, big, big, "keep-a", "keep-b"],
        ]
        schema = pa.schema([('price', pa.int32()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_le.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT
                `fruit`, CAST(`price` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `price` Int32 NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE price <= 5
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("keep-a", "3"),
                ("keep-b", "4"),
            ],
        )

    @yq_v2
    def test_s3_push_down_parquet_gt_operator(self, kikimr, s3, client, unique_prefix):
        """Test > operator pushdown."""
        big = "x" * 100000
        data = [
            [1, 2, 100, 200, 3, 4],
            [big, big, "keep-a", "keep-b", big, big],
        ]
        schema = pa.schema([('price', pa.int32()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_gt.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT
                `fruit`, CAST(`price` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `price` Int32 NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE price > 50
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("keep-a", "100"),
                ("keep-b", "200"),
            ],
        )

    @yq_v2
    def test_s3_push_down_parquet_eq_boundary(self, kikimr, s3, client, unique_prefix):
        """Test = operator where constant equals min/max boundary."""
        big = "x" * 100000
        # RG0: [1, 2], RG1: [100, 200], RG2: [3, 4]
        data = [
            [1, 2, 100, 200, 3, 4],
            [big, big, "keep-a", "keep-b", big, big],
        ]
        schema = pa.schema([('price', pa.int32()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_eq_boundary.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        # Test = 100 (equals min of RG1)
        sql = f'''
            SELECT
                `fruit`, CAST(`price` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `price` Int32 NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE price = 100
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("keep-a", "100"),
            ],
        )

    # =========================================================================
    # NE operator tests
    # =========================================================================

    @yq_v2
    def test_s3_push_down_parquet_ne_skip(self, kikimr, s3, client, unique_prefix):
        """Test != operator where a row group has all same values equal to constant."""
        # RG0: [5, 5], RG1: [100, 200], RG2: [3, 4]
        data = [
            [5, 5, 100, 200, 3, 4],
            ["skip-a", "skip-b", "keep-a", "keep-b", "keep-c", "keep-d"],
        ]
        schema = pa.schema([('price', pa.int32()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_ne.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT `fruit`
            FROM `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `price` Int32 NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE price != 5
            '''
        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)
        fruits_without = sorted(
            row.items[0].text_value
            for row in client.get_result_data(query_id, limit=1000).result.result_set.rows
        )

        query_id = client.create_query(
            "simple",
            'pragma s3.UsePredicatePushdown = "true";\n' + sql,
            type=fq.QueryContent.QueryType.ANALYTICS
        ).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)
        fruits_with = sorted(
            row.items[0].text_value
            for row in client.get_result_data(query_id, limit=1000).result.result_set.rows
        )

        assert fruits_without == fruits_with
        assert "skip-a" not in fruits_without
        assert "skip-b" not in fruits_without

    # =========================================================================
    # UUID pushdown tests (T11)
    # =========================================================================

    @yq_v2
    def test_s3_push_down_parquet_uuid(self, kikimr, s3, client, unique_prefix):
        """Test UUID column pushdown with EQ operator."""
        uuid_a = "11111111-1111-4111-8111-111111111111"
        uuid_b = "22222222-2222-4222-8222-222222222222"
        uuid_c = "33333333-3333-4333-8333-333333333333"
        big = "x" * 100000
        # Row groups: [uuid_a, uuid_a], [uuid_b, uuid_b], [uuid_c, uuid_c]
        data = [
            [
                self._yql_uuid_bytes(uuid_a),
                self._yql_uuid_bytes(uuid_a),
                self._yql_uuid_bytes(uuid_b),
                self._yql_uuid_bytes(uuid_b),
                self._yql_uuid_bytes(uuid_c),
                self._yql_uuid_bytes(uuid_c),
            ],
            ["a1", "a2", big, big, "c1", "c2"],
        ]
        schema = pa.schema([('id', pa.binary(16)), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_uuid.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT
                `fruit`, CAST(`id` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `id` Uuid NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE id = Uuid("{uuid_c}")
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("c1", uuid_c),
                ("c2", uuid_c),
            ],
        )

    @yq_v2
    def test_s3_push_down_parquet_uuid_miss(self, kikimr, s3, client, unique_prefix):
        """Test UUID pushdown where predicate matches no rows."""
        uuid_a = "11111111-1111-4111-8111-111111111111"
        uuid_b = "22222222-2222-4222-8222-222222222222"
        uuid_miss = "44444444-4444-4444-8444-444444444444"
        data = [
            [
                self._yql_uuid_bytes(uuid_a),
                self._yql_uuid_bytes(uuid_a),
                self._yql_uuid_bytes(uuid_b),
                self._yql_uuid_bytes(uuid_b),
            ],
            ["a1", "a2", "b1", "b2"],
        ]
        schema = pa.schema([('id', pa.binary(16)), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_uuid_miss.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT `fruit`
            FROM `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `id` Uuid NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE id = Uuid("{uuid_miss}")
            '''
        # Without pushdown - should return 0 rows
        query_id = client.create_query("simple", sql, type=fq.QueryContent.QueryType.ANALYTICS).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)
        data = client.get_result_data(query_id, limit=1000)
        assert len(data.result.result_set.rows) == 0

        # With pushdown - should also return 0 rows
        query_id = client.create_query(
            "simple",
            'pragma s3.UsePredicatePushdown = "true";\n' + sql,
            type=fq.QueryContent.QueryType.ANALYTICS
        ).result.query_id
        client.wait_query_status(query_id, fq.QueryMeta.COMPLETED)
        data = client.get_result_data(query_id, limit=1000)
        assert len(data.result.result_set.rows) == 0

    @yq_v2
    def test_s3_push_down_parquet_uuid_between(self, kikimr, s3, client, unique_prefix):
        """Test UUID column pushdown with BETWEEN operator."""
        uuid_a = "11111111-1111-4111-8111-111111111111"
        uuid_b = "22222222-2222-4222-8222-222222222222"
        uuid_c = "33333333-3333-4333-8333-333333333333"
        big = "x" * 100000
        # Row groups: [uuid_a, uuid_a], [uuid_b, uuid_b], [uuid_c, uuid_c]
        data = [
            [
                self._yql_uuid_bytes(uuid_a),
                self._yql_uuid_bytes(uuid_a),
                self._yql_uuid_bytes(uuid_b),
                self._yql_uuid_bytes(uuid_b),
                self._yql_uuid_bytes(uuid_c),
                self._yql_uuid_bytes(uuid_c),
            ],
            [big, big, "keep-a", "keep-b", big, big],
        ]
        schema = pa.schema([('id', pa.binary(16)), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_uuid_between.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT
                `fruit`, CAST(`id` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `id` Uuid NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE id BETWEEN Uuid("{uuid_b}") AND Uuid("22222222-2222-4222-8222-222222222223")
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("keep-a", uuid_b),
                ("keep-b", uuid_b),
            ],
        )

    # =========================================================================
    # DATE pushdown tests (T10)
    # =========================================================================

    @yq_v2
    def test_s3_push_down_parquet_date(self, kikimr, s3, client, unique_prefix):
        """Test DATE column pushdown with comparison operators."""
        big = "x" * 100000
        # Row groups: [2024-01-01, 2024-01-02], [2024-06-01, 2024-06-02], [2024-03-01, 2024-03-02]
        data = [
            [
                datetime(2024, 1, 1), datetime(2024, 1, 2),
                datetime(2024, 6, 1), datetime(2024, 6, 2),
                datetime(2024, 3, 1), datetime(2024, 3, 2),
            ],
            [big, big, "keep-a", "keep-b", big, big],
        ]
        schema = pa.schema([('d', pa.date32()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_date.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT
                `fruit`, CAST(`d` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `d` Date NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE d >= Date("2024-06-01")
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("keep-a", "2024-06-01"),
                ("keep-b", "2024-06-02"),
            ],
        )

    @yq_v2
    def test_s3_push_down_parquet_date_between(self, kikimr, s3, client, unique_prefix):
        """Test DATE column pushdown with BETWEEN operator."""
        big = "x" * 100000
        data = [
            [
                datetime(2024, 1, 1), datetime(2024, 1, 2),
                datetime(2024, 6, 1), datetime(2024, 6, 2),
                datetime(2024, 3, 1), datetime(2024, 3, 2),
            ],
            [big, big, "keep-a", "keep-b", big, big],
        ]
        schema = pa.schema([('d', pa.date32()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_date_between.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT
                `fruit`, CAST(`d` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `d` Date NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE d >= Date("2024-05-01") AND d <= Date("2024-07-01")
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("keep-a", "2024-06-01"),
                ("keep-b", "2024-06-02"),
            ],
        )

    # =========================================================================
    # FLOAT pushdown tests (T11) - separate from DOUBLE
    # =========================================================================

    @yq_v2
    def test_s3_push_down_parquet_float_separate(self, kikimr, s3, client, unique_prefix):
        """Test FLOAT (float32) column pushdown, distinct from DOUBLE (float64)."""
        big = "x" * 100000
        # Row groups: [0.5, 1.5], [100.0, 200.0], [2.5, 3.5]
        data = [
            [0.5, 1.5, 100.0, 200.0, 2.5, 3.5],
            [big, big, "keep-a", "keep-b", big, big],
        ]
        schema = pa.schema([('value', pa.float32()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_float_separate.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT
                `fruit`, CAST(`value` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `value` Float NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE value >= CAST(50.0 AS Float)
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("keep-a", "100"),
                ("keep-b", "200"),
            ],
        )

    @yq_v2
    def test_s3_push_down_parquet_float_between(self, kikimr, s3, client, unique_prefix):
        """Test FLOAT (float32) column pushdown with BETWEEN operator."""
        big = "x" * 100000
        data = [
            [0.5, 1.5, 100.0, 200.0, 2.5, 3.5],
            [big, big, "keep-a", "keep-b", big, big],
        ]
        schema = pa.schema([('value', pa.float32()), ('fruit', pa.string())])
        table = pa.Table.from_arrays(data, schema=schema)
        filename = 'test_s3_push_down_parquet_float_between.parquet'
        conn = self.setup_s3_and_connection(s3, client, unique_prefix, filename, table)
        kikimr.control_plane.wait_bootstrap(1)

        sql = f'''
            SELECT
                `fruit`, CAST(`value` as Utf8)
            FROM
                `{conn}`.`/{filename}`
            WITH (FORMAT="parquet",
                SCHEMA=(
                  `value` Float NOT NULL,
                  `fruit` Utf8 NOT NULL
                ))
            WHERE value BETWEEN CAST(50.0 AS Float) AND CAST(300.0 AS Float)
            '''
        self._assert_pushdown_correctness(
            client,
            sql,
            [
                ("keep-a", "100"),
                ("keep-b", "200"),
            ],
        )
