# YT client library for integration tests.
# Provides YtClient class to manage YT cluster lifecycle in Docker.

PY3_LIBRARY()

NO_CHECK_IMPORTS()

PY_SRCS(
    __init__.py
    yt_client.py
)

END()
