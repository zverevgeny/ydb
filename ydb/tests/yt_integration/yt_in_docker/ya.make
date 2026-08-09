# YT client library for integration tests.
# Provides YtClient class to manage YT cluster lifecycle in Docker.

PY3_LIBRARY()

# Disable import checks since `yatest` is a test-only dependency
# injected by the test framework at runtime, not a build-time peer.
NO_CHECK_IMPORTS()

PY_SRCS(
    __init__.py
    yt_client.py
)

END()
