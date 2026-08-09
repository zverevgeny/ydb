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
from typing import Any, Dict, List, Optional
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

    def __init__(self, max_attempts: int = 90, sleep_interval: float = 2.0) -> None:
        self._compose_project_name: Optional[str] = None
        self._container_name: Optional[str] = None
        self._proxy_url: Optional[str] = None

        # Start docker-compose
        self._start_cluster()
        try:
            self._proxy_url = self._resolve_proxy_url()
            self._wait_for_healthy(max_attempts, sleep_interval)
        except Exception:
            # On any failure, stop the cluster
            self.stop()
            raise

    def __enter__(self) -> "YtClient":
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> bool:
        self.stop()
        return False

    def stop(self) -> None:
        self._stop_cluster()

    def _get_compose_file_abs_path(self) -> str:
        return yatest.common.source_path(_DOCKER_COMPOSE_FILE_PATH)

    def _start_cluster(self) -> None:
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
        logger.info("Starting YT cluster with project %s (timeout: 300s)", self._compose_project_name)
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300, check=False)
        if result.returncode != 0:
            logger.error("YT cluster startup stderr: %s", result.stderr[:1000])
            raise RuntimeError(f"Failed to start YT cluster: {result.stderr}")

        # Discover the actual container name
        self._container_name = self._discover_container_name()
        logger.info("YT cluster started in container %s", self._container_name)

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

    def _stop_cluster(self) -> None:
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
            logger.info("Stopping YT cluster %s", self._compose_project_name)
            subprocess.run(
                cmd, capture_output=True, text=True, timeout=120, check=False,
            )
        except Exception as e:
            logger.warning("Failed to stop YT cluster: %s", e)
        finally:
            self._compose_project_name = None
            self._container_name = None
            self._proxy_url = None

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

        if output.startswith("["):
            # IPv6 format: [host]:port
            bracket_end = output.rfind("]")
            if bracket_end < 0 or bracket_end + 1 >= len(output) or output[bracket_end + 1] != ":":
                raise RuntimeError(f"Unexpected IPv6 port output format: {output!r}")
            host = output[:bracket_end + 1]  # Includes brackets, e.g. [::1]
            port_str = output[bracket_end + 2:]
        else:
            # IPv4 format: host:port
            if ":" not in output:
                raise RuntimeError(f"Unexpected port output format: {output!r}")
            host, port_str = output.rsplit(":", 1)

        try:
            port = int(port_str)
        except ValueError:
            raise RuntimeError(f"Invalid port number in output: {output!r}") from None

        # Always use localhost to avoid IPv4/IPv6 binding issues.
        return f"http://localhost:{port}"

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

        raise last_error

    def list(self, path: str) -> Dict[str, Any]:
        return self._api_call("list", {"path": path})

    def create_table(self, path: str, columns: Dict[str, str]) -> None:
        """Create a table at the given path with provided schema."""
        params = {"path": path, "type": "table"}
        for idx, (name, col_type) in enumerate(columns.items()):
            params[f"columns.{idx}.name"] = name
            params[f"columns.{idx}.type"] = col_type
        self._api_call("create", params=params, http_method="POST")

    def exists(self, path: str) -> bool:
        try:
            self._api_call("get", params={"path": path})
            return True
        except RuntimeError as e:
            error_str = str(e).lower()
            if "not found" in error_str or "has no child" in error_str:
                return False
            raise

    def remove(self, path: str, recursive: bool = True) -> None:
        if not self.exists(path):
            return
        params = {"path": path}
        if recursive:
            params["recursive"] = "true"
        self._api_call("remove", params=params, http_method="POST")

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
