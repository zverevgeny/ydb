# Integration tests for QYT with local YT in Docker.
# Uses docker-compose to spin up a YT cluster for each test run.

PY3TEST()

# Disable import checks because the test imports `yt_in_docker` which is
# a local package resolved at runtime via the peer dependency below.
NO_CHECK_IMPORTS()

SET(DOCKER_COMPOSE_FILE ydb/tests/yt_integration/yt_in_docker/docker-compose.yml)

ENV(COMPOSE_HTTP_TIMEOUT=600)

INCLUDE(${ARCADIA_ROOT}/library/recipes/docker_compose/recipe.inc)

PEERDIR(
    contrib/python/pytest
    ydb/tests/yt_integration/yt_in_docker
)

DEPENDS(
    ydb/library/yql/providers/pq/gateway/clients/qyt/tools
)

ENV(QYT_CLI_BINARY="ydb/library/yql/providers/pq/gateway/clients/qyt/tools/qyt_cli")

TEST_SRCS(
    test_yt_integration.py
)

END()
