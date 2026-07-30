PY3_PROGRAM(yt_queue)

PY_SRCS(
    __main__.py
)

PEERDIR(
    ydb/tests/stress/yt_queue/workload
)

END()

RECURSE_FOR_TESTS(
    tests
)
