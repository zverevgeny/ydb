"""YT client for integration tests.

Provides a simple interface to interact with YT cluster running in Docker.
"""

import json
import logging
import subprocess
import time
import urllib.error
import urllib.request
from urllib.parse import urlencode
import yatest.common


logger = logging.getLogger(__name__)

CONTAINER_NAME = "tests-yt-integration-yt"
_DOCKER_COMPOSE_FILE_PATH = "ydb/tests/yt_integration/yt_in_docker/docker-compose.yml"


class YtClient:
    """Client for interacting with YT cluster.

    On construction the client waits for the YT cluster to become healthy
    and caches the proxy endpoint for subsequent API calls.

    Example:
        client = YtClient()
        client.create_table("//tmp/my_table")
        client.write_table("//tmp/my_table", [{"key": "value"}])
        rows = client.read_table("//tmp/my_table")
    """

    def __init__(self, max_attempts=90, sleep_interval=2):
        self.container_name = CONTAINER_NAME
        self._proxy_url = self._resolve_proxy_url()
        self._wait_for_healthy(max_attempts, sleep_interval)

    def _resolve_proxy_url(self):
        """Resolve the YT proxy URL from docker-compose and return it."""
        docker_compose_file_abs_path = yatest.common.source_path(_DOCKER_COMPOSE_FILE_PATH)
        cmd = ["docker", "compose", "-f", docker_compose_file_abs_path, "port", "yt", "80"]
        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if result.returncode != 0:
            raise RuntimeError(f"Failed to get YT port: {result.stderr}")
        # Use rsplit to safely handle IPv6 addresses like [::1]:PORT
        output = result.stdout.strip()
        port = int(output.rsplit(":", 1)[1])
        return f"http://localhost:{port}"

    def _wait_for_healthy(self, max_attempts, sleep_interval):
        """Wait for YT cluster to become healthy.

        Args:
            max_attempts: Maximum number of attempts
            sleep_interval: Sleep between attempts in seconds

        Raises:
            RuntimeError: If YT doesn't become healthy in time
        """
        for attempt in range(max_attempts):
            try:
                result = self.list("//tmp")
                if "value" in result:
                    return
            except Exception as e:
                logger.debug("YT health check attempt %d failed: %s", attempt, e)
            if attempt < max_attempts - 1:
                time.sleep(sleep_interval)
        raise RuntimeError("YT cluster did not become healthy in time")

    def _api_call(self, method, params=None, data=None):
        """Make a call to YT HTTP API v4.

        Args:
            method: API method name (list, get, create, remove, etc.)
            params: Query parameters dict
            data: Request body (will be encoded to bytes if string)

        Returns:
            Parsed JSON response as dict
        """
        url = f"{self._proxy_url}/api/v4/{method}"
        if params:
            url += f"?{urlencode(params)}"

        req = urllib.request.Request(url)
        if data is not None:
            if isinstance(data, str):
                data = data.encode()
            req.data = data
            req.add_header("Content-Type", "application/json")

        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                return json.loads(resp.read().decode())
        except urllib.error.HTTPError as e:
            raise RuntimeError(f"YT API error: {e.code} {e.read().decode()[:500]}")

    def _run_yt_cli(self, args, check=True, timeout=60, input_data=None):
        """Run yt CLI command inside the Docker container via docker exec.

        Used for table operations (write-table/read-table) which are not
        available through the HTTP API v4.

        Args:
            args: Command line arguments for yt CLI
            check: If True, raise exception on non-zero exit code
            timeout: Timeout in seconds
            input_data: Data to pass to stdin

        Returns:
            subprocess.CompletedProcess result
        """
        if input_data is not None:
            cmd = ["docker", "exec", "-i", self.container_name, "yt", "--proxy", "localhost:80"] + args
        else:
            cmd = ["docker", "exec", self.container_name, "yt", "--proxy", "localhost:80"] + args
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=False, input=input_data)
        if check and result.returncode != 0:
            raise RuntimeError(f"yt command failed: {result.stderr[:500]}")
        return result

    def list(self, path):
        """List nodes at the given path.

        Args:
            path: YT path to list

        Returns:
            JSON response dict with 'value' key containing list of children
        """
        return self._api_call("list", {"path": path})

    def create_table(self, path):
        """Create a table at the given path.

        Args:
            path: YT path for the table
        """
        self._run_yt_cli(["create", "table", path], check=True)

    def remove(self, path):
        """Remove a node at the given path.

        Args:
            path: YT path to remove
        """
        self._run_yt_cli(["remove", path, "--force"], check=True)

    def write_table(self, path, rows):
        """Write rows to a table.

        Args:
            path: YT table path
            rows: List of dicts representing rows
        """
        data = "\n".join(json.dumps(row) for row in rows)
        self._run_yt_cli(
            ["write-table", "--format=json", path],
            input_data=data, check=True, timeout=60,
        )

    def read_table(self, path):
        """Read rows from a table.

        Args:
            path: YT table path

        Returns:
            List of dicts representing rows
        """
        result = self._run_yt_cli(
            ["read-table", "--format=json", path],
            timeout=60,
        )
        lines = [line.strip() for line in result.stdout.strip().split("\n") if line.strip()]
        return [json.loads(line) for line in lines]
