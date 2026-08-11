"""Integration test for QYT with local YT in Docker.

The test runs on the host and connects to YT running inside Docker.
"""

import logging
import os
import subprocess

import pytest
import yatest.common

from ydb.tests.fq.yt.yt_in_docker import YtClient


logger = logging.getLogger(__name__)

MESSAGE_COUNT = 100


def _get_qyt_cli_path() -> str:
    """Resolve the path to the built qyt_cli binary."""
    import glob
    import pathlib
    binary = os.environ.get("QYT_CLI_BINARY", "ydb/library/yql/providers/pq/gateway/clients/qyt/tools/qyt_cli")
    # yatest.common.build_path may return source path; try build output first
    build_path = yatest.common.build_path(binary)
    if os.path.isfile(build_path) and os.access(build_path, os.X_OK):
        return build_path
    # Try to find in build directory using glob
    build_root = pathlib.Path(os.environ.get("ARCADIA_BUILD_ROOT", ""))
    if build_root and build_root.exists():
        pattern = str(build_root / "**" / "qyt_cli")
        candidates = glob.glob(pattern, recursive=True)
        for c in candidates:
            if os.path.isfile(c) and os.access(c, os.X_OK):
                return c
    return build_path


def _run_qyt_cli(args: list, timeout: int = 30) -> subprocess.CompletedProcess[str]:
    """Run qyt_cli with the given arguments."""
    cli_path = _get_qyt_cli_path()
    cmd = [cli_path] + args
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=False)


class TestYtIntegration:
    """Integration tests with local YT in Docker.

    Uses scope="class" to start the YT cluster once for all tests in this
    class, avoiding the expensive startup cost per test.
    """

    @pytest.fixture(scope="class")
    def yt_client(self):
        client = YtClient()
        yield client
        client.stop()

    def test_yt_connectivity(self, yt_client: YtClient) -> None:
        result = yt_client.list("//tmp")
        assert "value" in result

    def test_write_and_read_table(self, yt_client: YtClient) -> None:
        table_path = "//tmp/test_yt_table"

        try:
            yt_client.remove(table_path)
            yt_client.create_table(
                table_path,
                columns={"key": "string", "value": "int64", "label": "string"},
            )

            rows = [
                {"key": f"msg_{i}", "value": i, "label": f"tag_{i % 5}"}
                for i in range(MESSAGE_COUNT)
            ]
            yt_client.write_table(table_path, rows)

            result = yt_client.read_table(table_path)
            assert len(result) == MESSAGE_COUNT, f"Expected {MESSAGE_COUNT} got {len(result)}"

            # Verify all rows match what was written (order may differ)
            result_sorted = sorted(result, key=lambda r: r["key"])
            expected_sorted = sorted(rows, key=lambda r: r["key"])
            for actual, expected in zip(result_sorted, expected_sorted):
                assert actual["key"] == expected["key"], (
                    f"Key mismatch: {actual['key']} != {expected['key']}"
                )
                assert actual["value"] == expected["value"], (
                    f"Value mismatch for {actual['key']}: {actual['value']} != {expected['value']}"
                )
                assert actual["label"] == expected["label"], (
                    f"Label mismatch for {actual['key']}: {actual['label']} != {expected['label']}"
                )
        finally:
            try:
                yt_client.remove(table_path)
            except Exception:
                logger.warning("Failed to cleanup table %s", table_path, exc_info=True)

    def test_create_topic_contract(self, yt_client: YtClient) -> None:
        """Test the contract TQytTopicClient::CreateTopic relies on.

        CreateTopic checks NodeExists on the given path. If the node exists
        (table was created out of band), it returns success. Otherwise it
        returns BAD_REQUEST. This test verifies that create_table + exists
        correctly implements that contract.
        """
        table_path = "//tmp/test_qyt_topic"

        try:
            # Topic does not exist yet
            assert not yt_client.exists(table_path), "Table should not exist yet"

            # Create the underlying table (simulates out-of-band queue creation)
            yt_client.create_table(
                table_path,
                columns={"data": "string"},
            )

            # Now CreateTopic should find the node and return success
            assert yt_client.exists(table_path), "Table should exist after creation"

            # Verify we can write and read data through the table
            yt_client.write_table(table_path, [{"data": "hello_qyt"}])
            result = yt_client.read_table(table_path)
            assert len(result) == 1
            assert result[0]["data"] == "hello_qyt"
        finally:
            try:
                yt_client.remove(table_path)
            except Exception:
                logger.warning("Failed to cleanup table %s", table_path, exc_info=True)

    def test_describe_topic_via_qyt_cli(self, yt_client: YtClient) -> None:
        """Test TQytTopicClient::DescribeTopic via qyt_cli.

        DescribeTopic reads @tablet_count attribute from YT and returns
        partition information. This test verifies the full path from
        qyt_cli through TQytTopicClient to the YT cluster.

        Requires a mounted ordered dynamic table (queue) so that
        @tablet_count is available on the node.
        """
        table_path = "//tmp/test_describe_topic"

        try:
            # Check if qyt_cli is available
            cli_path = _get_qyt_cli_path()
            assert os.path.isfile(cli_path) and os.access(cli_path, os.X_OK), (
                f"qyt_cli not found at {cli_path}"
            )

            # Create a mounted ordered dynamic table — DescribeTopic needs
            # a node with the @tablet_count attribute, which only exists on
            # dynamic tables.
            yt_client.create_queue(table_path)

            # Run qyt_cli describe-topic with the RPC proxy address.
            # qyt_cli uses the YT RPC protocol, not the HTTP API.
            result = _run_qyt_cli(
                [yt_client.rpc_proxy_address, "describe-topic", table_path], timeout=30
            )

            # Verify the CLI succeeded
            assert result.returncode == 0, (
                f"qyt_cli describe-topic failed: {result.stderr}"
            )
            assert "OK: Topic" in result.stdout, (
                f"Unexpected output: {result.stdout}"
            )
        finally:
            try:
                yt_client.remove(table_path)
            except Exception:
                logger.warning("Failed to cleanup table %s", table_path, exc_info=True)

    def test_write_via_qyt_cli(self, yt_client: YtClient) -> None:
        """Test TQytTopicClient::CreateWriteSession via qyt_cli write command.

        This test verifies the write path through TQytTopicClient to the YT cluster.
        Requires a mounted ordered dynamic table (queue).
        """
        table_path = "//tmp/test_qyt_write"
        consumer_path = "//tmp/test_qyt_write_consumer"

        try:
            cli_path = _get_qyt_cli_path()
            assert os.path.isfile(cli_path) and os.access(cli_path, os.X_OK), (
                f"qyt_cli not found at {cli_path}"
            )

            # Create queue as an ordered dynamic table and register a consumer.
            yt_client.create_queue(table_path)
            yt_client.register_consumer(table_path, consumer_path)

            # Write messages via qyt_cli using the RPC proxy address.
            result = _run_qyt_cli(
                [yt_client.rpc_proxy_address, "write", table_path, consumer_path,
                 "hello", "world", "qyt"],
                timeout=30
            )

            assert result.returncode == 0, (
                f"qyt_cli write failed: {result.stderr}"
            )
            assert "OK: Wrote 3 messages" in result.stdout, (
                f"Unexpected output: {result.stdout}"
            )
        finally:
            try:
                yt_client.remove(consumer_path)
            except Exception:
                logger.warning("Failed to cleanup consumer %s", consumer_path, exc_info=True)
            try:
                yt_client.remove(table_path)
            except Exception:
                logger.warning("Failed to cleanup table %s", table_path, exc_info=True)

    def test_read_via_qyt_cli(self, yt_client: YtClient) -> None:
        """Test TQytTopicClient::CreateReadSession via qyt_cli read command.

        This test verifies the read path through TQytTopicClient to the YT cluster.
        First writes messages via insert-rows, then reads them back via qyt_cli.

        Requires a mounted ordered dynamic table (queue) with a registered consumer.
        """
        table_path = "//tmp/test_qyt_read"
        consumer_path = "//tmp/test_qyt_read_consumer"

        try:
            cli_path = _get_qyt_cli_path()
            assert os.path.isfile(cli_path) and os.access(cli_path, os.X_OK), (
                f"qyt_cli not found at {cli_path}"
            )

            # Create queue as an ordered dynamic table, register a consumer,
            # and insert test data via insert-rows (supported by dynamic tables).
            yt_client.create_queue(table_path)
            yt_client.register_consumer(table_path, consumer_path)
            yt_client.insert_rows(table_path, [
                {"data": "msg1"},
                {"data": "msg2"},
                {"data": "msg3"},
            ])

            # Read messages via qyt_cli using the RPC proxy address.
            result = _run_qyt_cli(
                [yt_client.rpc_proxy_address, "read", table_path, consumer_path, "5"],
                timeout=30
            )

            assert result.returncode == 0, (
                f"qyt_cli read failed: {result.stderr}"
            )
            assert "OK: Read" in result.stdout, (
                f"Unexpected output: {result.stdout}"
            )
        finally:
            try:
                yt_client.remove(consumer_path)
            except Exception:
                logger.warning("Failed to cleanup consumer %s", consumer_path, exc_info=True)
            try:
                yt_client.remove(table_path)
            except Exception:
                logger.warning("Failed to cleanup table %s", table_path, exc_info=True)
