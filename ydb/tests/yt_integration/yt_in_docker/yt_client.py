"""YT client for integration tests.

Provides a simple interface to interact with YT cluster running in Docker.
Manages the full lifecycle of the docker-compose service (start/stop).
"""

import json
import logging
import subprocess
import time
import uuid
import urllib.error
import urllib.request
from urllib.parse import urlencode
import yatest.common


logger = logging.getLogger(__name__)

_DOCKER_COMPOSE_FILE_PATH = "ydb/tests/yt_integration/yt_in_docker/docker-compose.yml"


class YtClient:
    """Client for interacting with YT cluster.

    On construction the client starts the YT cluster via docker-compose,
    waits for it to become healthy, and caches the proxy endpoint.
    On destruction the cluster is automatically stopped and removed.

    Example:
        client = YtClient()
        client.create_table("//tmp/my_table")
        client.write_table("//tmp/my_table", [{"key": "value"}])
        rows = client.read_table("//tmp/my_table")
    """

    def __init__(self, max_attempts=90, sleep_interval=2):
        self._compose_dir = None
        self._compose_project_name = None
        self._container_name = None
        self._proxy_url = None

        # Start docker-compose
        self._start_cluster()
        try:
            self._proxy_url = self._resolve_proxy_url()
            self._wait_for_healthy(max_attempts, sleep_interval)
        except Exception:
            # On any failure, stop the cluster
            self._stop_cluster()
            raise

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self._stop_cluster()
        return False

    def _get_compose_file_abs_path(self):
        """Return absolute path to docker-compose.yml."""
        return yatest.common.source_path(_DOCKER_COMPOSE_FILE_PATH)

    def _start_cluster(self):
        """Start YT cluster via docker-compose in a temporary directory.

        Uses a unique project name per test run to avoid conflicts
        between parallel test executions.
        """
        # Check docker availability first
        try:
            subprocess.run(
                ["docker", "info"],
                capture_output=True,
                timeout=30,
                check=True,
            )
        except (subprocess.CalledProcessError, FileNotFoundError) as e:
            raise RuntimeError(f"Docker is not available: {e}") from e

        # Create a unique project name to avoid conflicts
        self._compose_project_name = f"yt_test_{uuid.uuid4().hex[:12]}"

        compose_file = self._get_compose_file_abs_path()
        cmd = [
            "docker", "compose",
            "-f", compose_file,
            "-p", self._compose_project_name,
            "up", "-d",
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300, check=False)
        if result.returncode != 0:
            raise RuntimeError(f"Failed to start YT cluster: {result.stderr}")

        # Discover the actual container name
        self._container_name = self._discover_container_name()

    def _discover_container_name(self):
        """Discover the running container name for this project."""
        compose_file = self._get_compose_file_abs_path()
        cmd = [
            "docker", "compose",
            "-f", compose_file,
            "-p", self._compose_project_name,
            "ps", "--format", "{{.Name}}", "--filter", "status=running",
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30, check=False)
        if result.returncode != 0:
            raise RuntimeError(f"Failed to discover container: {result.stderr}")
        containers = [c.strip() for c in result.stdout.strip().split("\n") if c.strip()]
        if not containers:
            raise RuntimeError("No running containers found for YT cluster")
        return containers[0]

    def _stop_cluster(self):
        """Stop and remove YT cluster."""
        if self._compose_project_name is None:
            return
        try:
            compose_file = self._get_compose_file_abs_path()
            cmd = [
                "docker", "compose",
                "-f", compose_file,
                "-p", self._compose_project_name,
                "down", "-v",
            ]
            subprocess.run(
                cmd, capture_output=True, text=True, timeout=120, check=False,
            )
        except Exception as e:
            logger.warning("Failed to stop YT cluster: %s", e)
        finally:
            self._compose_project_name = None
            self._container_name = None
            self._proxy_url = None

    def _resolve_proxy_url(self):
        """Resolve the YT proxy URL from docker-compose and return it."""
        compose_file = self._get_compose_file_abs_path()
        cmd = [
            "docker", "compose",
            "-f", compose_file,
            "-p", self._compose_project_name,
            "port", "yt", "80",
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30, check=False)
        if result.returncode != 0:
            raise RuntimeError(f"Failed to get YT port: {result.stderr}")

        output = result.stdout.strip()
        if not output:
            raise RuntimeError("docker compose port returned empty output")

        # Use rsplit to safely handle IPv6 addresses like [::1]:PORT
        if ":" not in output:
            raise RuntimeError(f"Unexpected port output format: {output!r}")

        port_str = output.rsplit(":", 1)[1]
        try:
            port = int(port_str)
        except ValueError:
            raise RuntimeError(f"Invalid port number in output: {output!r}") from None

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

    def _api_call(self, method, params=None, data=None, timeout=60):
        """Make a call to YT HTTP API v4.

        Args:
            method: API method name (list, get, create, remove, etc.)
            params: Query parameters dict
            data: Request body (will be encoded to bytes if string)
            timeout: Request timeout in seconds

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
            with urllib.request.urlopen(req, timeout=timeout) as resp:
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
            cmd = ["docker", "exec", "-i", self._container_name, "yt", "--proxy", "localhost:80"] + args
        else:
            cmd = ["docker", "exec", self._container_name, "yt", "--proxy", "localhost:80"] + args
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
        """Create a table at the given path via HTTP API.

        Args:
            path: YT path for the table
        """
        self._api_call("create", params={"path": path, "type": "table"})

    def remove(self, path):
        """Remove a node at the given path.

        Idempotent: does not raise if the node does not exist.

        Args:
            path: YT path to remove
        """
        try:
            self._api_call("remove", params={"path": path})
        except RuntimeError as e:
            # Ignore "node not found" errors — remove is idempotent
            if "has no child with key" not in str(e):
                raise

    def write_table(self, path, rows):
        """Write rows to a table.

        Args:
            path: YT table path
            rows: List of dicts representing rows. Empty list is a no-op.
        """
        if not rows:
            return
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
        rows = []
        for line in lines:
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as e:
                raise RuntimeError(f"Failed to parse table row: {line!r} — {e}") from None
        return rows
