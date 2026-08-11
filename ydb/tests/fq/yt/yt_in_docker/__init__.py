"""YT in Docker integration test utilities.

Provides YtClient for managing a local YT cluster inside Docker containers.
"""

from .yt_client import YtClient

__all__ = ["YtClient"]
