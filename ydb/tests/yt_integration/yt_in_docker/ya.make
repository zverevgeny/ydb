PY3_LIBRARY()

NO_CHECK_IMPORTS()

PY_SRCS(
    __init__.py
    yt_client.py
)

PEERDIR(
    contrib/python/pytest
)

END()
