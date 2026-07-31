PY3_PROGRAM(join_like_workload)

PY_SRCS(
    __main__.py
)

PEERDIR(
    ydb/public/sdk/python
    ydb/public/sdk/python/enable_v3_new_behavior
)

END()

RECURSE_FOR_TESTS(
    tests
)
