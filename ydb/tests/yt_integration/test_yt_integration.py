"""Integration test for QYT with local YT in Docker.

The test runs on the host and connects to YT running inside Docker.
"""

import logging

import pytest

from yt_in_docker import YtClient


logger = logging.getLogger(__name__)

MESSAGE_COUNT = 100


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
