"""YT client for integration tests.

Provides a simple interface to interact with YT cluster running in Docker.
The cluster lifecycle is managed by the yatest docker_compose recipe — this
client only connects to the already-running cluster.
"""

import json
import logging
import subprocess
import time
import urllib.error
import urllib.request
from typing import Any, Dict, List, Optional
from urllib.parse import urlencode
import yatest.common


logger = logging.getLogger(__name__)

_DOCKER_COMPOSE_FILE_PATH = "ydb/tests/fq/yt/yt_in_docker/docker-compose.yml"


class YtClient:
    """Client for interacting with YT cluster managed by docker_compose recipe.

    The cluster is started by the recipe before tests and stopped after.
    This client discovers the running container and connects to it.

    Example:
        client = YtClient()
        client.create_table("//tmp/my_table")
        client.write_table("//tmp/my_table", [{"key": "value"}])
        rows = client.read_table("//tmp/my_table")
    """

    def __init__(self, max_attempts: int = 90, sleep_interval: float = 2.0) -> None:
        self._compose_project_name: str = self._get_recipe_project_name()
        self._container_name: str = self._discover_container_name()
        self._proxy_url: str = self._resolve_proxy_url()
        self._rpc_proxy_address: str = self._resolve_rpc_proxy_address()
        self._wait_for_healthy(max_attempts, sleep_interval)
        self._configure_cluster()

    def __enter__(self) -> "YtClient":
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> bool:
        self.stop()
        return False

    def stop(self) -> None:
        # Cluster is managed by the recipe — nothing to stop here
        pass

    @property
    def proxy_url(self) -> str:
        """Return the YT HTTP proxy URL for HTTP API access."""
        return self._proxy_url

    @property
    def rpc_proxy_address(self) -> str:
        """Return the YT RPC proxy address (host:port) for RPC-based tools like qyt_cli."""
        return self._rpc_proxy_address

    def _get_compose_file_abs_path(self) -> str:
        return yatest.common.source_path(_DOCKER_COMPOSE_FILE_PATH)

    @staticmethod
    def _get_recipe_project_name() -> str:
        """Derive the compose project name used by the docker_compose recipe.

        The recipe runs docker compose from the source root with -f pointing
        to the compose file, so the project name is the directory basename.
        """
        # Project name is the directory containing the compose file
        return "yt_in_docker"

    def _discover_container_name(self) -> str:
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

    def _resolve_rpc_proxy_address(self) -> str:
        compose_file = self._get_compose_file_abs_path()
        cmd = [
            "docker", "compose",
            "-f", compose_file,
            "-p", self._compose_project_name,
            "port", "yt", "8443",
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30, check=False)
        if result.returncode != 0:
            raise RuntimeError(f"Failed to get YT RPC proxy port: {result.stderr}")
        output = result.stdout.strip()
        if not output:
            raise RuntimeError("docker compose port (8443) returned empty output")
        port = int(output.rsplit(":", 1)[1])
        return f"localhost:{port}"

    def _resolve_proxy_url(self) -> str:
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

        port = int(output.rsplit(":", 1)[1])
        return f"http://localhost:{port}"

    def _configure_cluster(self) -> None:
        """Apply one-time configuration to the cluster after it becomes healthy.

        1. Waits until the default tablet cell bundle is healthy so that
           dynamic tables can be mounted.  The container is started with
           --wait-tablet-cell-initialization so a cell already exists; this
           loop just waits for it to finish initialisation.
        2. Raises the tablet count limit for the //tmp account.
        """
        # Wait for the default tablet bundle to become healthy (up to 60 s).
        logger.info("Waiting for tablet cell bundle to become healthy")
        for attempt in range(60):
            result = self._run_yt_cli(
                ["get", "//sys/tablet_cell_bundles/default/@health"],
                check=False, timeout=10,
            )
            if "good" in result.stdout:
                logger.info("Tablet bundle healthy after %d attempts", attempt + 1)
                break
            time.sleep(1)
        else:
            logger.warning("Tablet cell bundle did not become healthy in time")

        # Increase tablet count limit for the tmp account.
        try:
            self._run_yt_cli(
                ["set", "//sys/accounts/tmp/@resource_limits/tablet_count", "1000"],
                check=True, timeout=30,
            )
        except Exception as e:
            logger.warning("Failed to configure cluster (tablet count limit): %s", e)

    def _wait_for_healthy(self, max_attempts: int, sleep_interval: float) -> None:
        logger.info("Waiting for YT cluster to become healthy (%d attempts)", max_attempts)
        for attempt in range(max_attempts):
            try:
                result = self.list("//tmp")
                if "value" in result:
                    logger.info("YT cluster is healthy after %d attempts", attempt + 1)
                    return
            except Exception as e:
                logger.debug("YT health check attempt %d failed: %s", attempt, e)
            if attempt < max_attempts - 1:
                time.sleep(sleep_interval)
        raise RuntimeError("YT cluster did not become healthy in time")

    def _api_call(
        self,
        method: str,
        params: Optional[Dict[str, str]] = None,
        data: Optional[str] = None,
        timeout: int = 60,
        max_retries: int = 2,
        http_method: str = "GET",
    ) -> Dict[str, Any]:
        url = f"{self._proxy_url}/api/v4/{method}"
        if params:
            url += f"?{urlencode(params)}"

        last_error: Optional[BaseException] = None
        for attempt in range(max_retries + 1):
            req = urllib.request.Request(url, method=http_method)
            if data is not None:
                req.data = data.encode()
                req.add_header("Content-Type", "application/json")

            try:
                with urllib.request.urlopen(req, timeout=timeout) as resp:
                    return json.loads(resp.read().decode())
            except urllib.error.HTTPError as e:
                error_body = e.read().decode()[:1000]
                last_error = RuntimeError(f"YT API error: {e.code} {error_body}")
                # Retry on 5xx server errors
                if e.code >= 500 and attempt < max_retries:
                    time.sleep(0.5 * (attempt + 1))
                    continue
                raise last_error
            except (urllib.error.URLError, OSError) as e:
                last_error = RuntimeError(f"YT API connection error: {e}")
                if attempt < max_retries:
                    time.sleep(0.5 * (attempt + 1))
                    continue
                raise last_error

    def _run_yt_cli(
        self,
        args: List[str],
        check: bool = True,
        timeout: int = 60,
        input_data: Optional[str] = None,
        max_retries: int = 1,
    ) -> subprocess.CompletedProcess[str]:
        """Run yt CLI command inside the Docker container via docker exec.
        """
        if input_data is not None:
            cmd = ["docker", "exec", "-i", self._container_name, "yt", "--proxy", "localhost:80"] + args
        else:
            cmd = ["docker", "exec", self._container_name, "yt", "--proxy", "localhost:80"] + args

        last_error: Optional[BaseException] = None
        for attempt in range(max_retries + 1):
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=False, input=input_data)
            if not check or result.returncode == 0:
                return result
            last_error = RuntimeError(f"yt command failed: {result.stderr[:500]}")
            if attempt < max_retries:
                time.sleep(0.5 * (attempt + 1))
                continue
            raise last_error

    def list(self, path: str) -> Dict[str, Any]:
        return self._api_call("list", {"path": path})

    def create_table(self, path: str, columns: Dict[str, str]) -> None:
        """Create a table at the given path with provided schema."""
        attributes = {
            "columns": [
                {"name": name, "type": col_type}
                for name, col_type in columns.items()
            ]
        }
        self._api_call(
            "create",
            params={"path": path, "type": "table"},
            data=json.dumps({"attributes": attributes}),
            http_method="POST",
        )

    def exists(self, path: str) -> bool:
        """Check if a node exists at the given path. """
        try:
            self._api_call("get", params={"path": path}, data=json.dumps({
                "attributes": {"node_type": None}
            }))
            return True
        except RuntimeError as e:
            error_str = str(e)
            # YT API returns errors for missing nodes with various messages
            if ("NOT_FOUND" in error_str
                    or "NODE_NOT_FOUND" in error_str
                    or "has no child" in error_str):
                return False
            raise

    def remove(self, path: str, recursive: bool = True) -> None:
        if not self.exists(path):
            return
        # Attempt to unmount dynamic tables before removal; errors are suppressed
        # for static tables that don't support unmount.
        try:
            self._run_yt_cli(
                ["unmount-table", "--sync", path],
                check=True, timeout=60,
            )
        except Exception:
            pass
        params = {"path": path}
        if recursive:
            params["recursive"] = "true"
        self._api_call("remove", params=params, http_method="POST")

    def create_queue(self, path: str, data_column: str = "data", timeout: int = 60) -> None:
        """Create an ordered dynamic table at the given path and mount it as a queue.

        The table is mounted synchronously so it is ready for queue operations
        immediately after this call returns.
        """
        attrs = "{dynamic=%true;schema=[{name=" + data_column + ";type=string}]}"
        self._run_yt_cli(
            ["create", "table", path, "--attributes", attrs],
            check=True, timeout=timeout,
        )
        self._run_yt_cli(
            ["mount-table", path, "--sync"],
            check=True, timeout=timeout,
        )

    def register_consumer(
        self,
        queue_path: str,
        consumer_path: str,
        vital: bool = False,
        timeout: int = 60,
    ) -> None:
        """Register a YT queue consumer linking consumer_path to queue_path."""
        vital_flag = "--vital" if vital else "--non-vital"
        self._run_yt_cli(
            ["register-queue-consumer", queue_path, consumer_path, vital_flag],
            check=True, timeout=timeout,
        )

    def insert_rows(self, path: str, rows: List[Dict[str, Any]], timeout: int = 120) -> None:
        """Insert rows into a mounted dynamic table via JSON newline-delimited format."""
        if not rows:
            return
        data = "\n".join(json.dumps(row) for row in rows) + "\n"
        self._run_yt_cli(
            ["insert-rows", "--format=json", path],
            input_data=data, check=True, timeout=timeout,
        )

    def write_table(self, path: str, rows: List[Dict[str, Any]], timeout: int = 120) -> None:
        if not rows:
            return
        data = "\n".join(json.dumps(row) for row in rows) + "\n"
        self._run_yt_cli(
            ["write-table", "--format=json", path],
            input_data=data, check=True, timeout=timeout,
        )

    def read_table(self, path: str) -> List[Dict[str, Any]]:
        result = self._run_yt_cli(
            ["read-table", "--format=json", path],
            timeout=60,
        )
        lines = [line.strip() for line in result.stdout.strip().split("\n") if line.strip()]
        rows: List[Dict[str, Any]] = []
        for line in lines:
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as e:
                raise RuntimeError(f"Failed to parse table row: {line!r} — {e}") from None
        return rows

    def get_attribute(self, path: str) -> Any:
        """Read a single attribute value from the given path (e.g. @tablet_count)."""
        result = self._api_call("get", params={"path": path})
        return result.get("value")
