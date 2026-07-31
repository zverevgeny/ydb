PY3TEST()
INCLUDE(${ARCADIA_ROOT}/ydb/tests/harness_dep.inc)

ENV(YDB_WORKLOAD_PATH="ydb/tests/stress/join_like_workload/join_like_workload")

TEST_SRCS(
    test_workload.py
)

REQUIREMENTS(ram:32 cpu:4)

SIZE(MEDIUM)

DEPENDS(
    ydb/tests/stress/join_like_workload
)

PEERDIR(
    ydb/tests/library
    ydb/tests/library/stress
)

END()
