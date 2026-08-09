"""Integration test for QYT with local YT in Docker.

The test runs on the host and connects to YT running inside Docker.
"""

import pytest

from yt_in_docker.yt_client import YtClient


MESSAGE_COUNT = 1000


class TestYtIntegration:
    """Integration tests with local YT in Docker.

    Uses scope="class" to start the YT cluster once for all tests in this
    class, avoiding the expensive startup cost per test. Tests are ordered
    such that test_yt_connectivity runs first (simple health check) and
    test_write_and_read_table runs second (full read/write cycle).
    """

    @pytest.fixture(scope="class")
    def yt_client(self):
        """Create and return YT client.

        Cluster is started on first use and stopped after all tests in this
        class complete, via the YtClient context manager protocol.
        """
        client = YtClient()
        yield client
        client.stop()

    def test_yt_connectivity(self, yt_client):
        """Test basic YT connectivity by listing //tmp via HTTP API."""
        result = yt_client.list("//tmp")
        assert "value" in result

    def test_write_and_read_table(self, yt_client):
        """Test writing and reading data from YT table."""
        table_path = "//tmp/test_yt_table"

        try:
            # Cleanup and create table
            yt_client.remove(table_path)
            yt_client.create_table(table_path)

            # Write data
            rows = [{"key": f"msg_{i}"} for i in range(MESSAGE_COUNT)]
            yt_client.write_table(table_path, rows)

            # Read data back
            result = yt_client.read_table(table_path)
            assert len(result) == MESSAGE_COUNT, f"Expected {MESSAGE_COUNT} got {len(result)}"
        finally:
            # Cleanup
            yt_client.remove(table_path)
