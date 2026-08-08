PY3TEST()

NO_CHECK_IMPORTS()

SET(DOCKER_COMPOSE_FILE ydb/tests/yt_integration/yt_in_docker/docker-compose.yml)

ENV(COMPOSE_HTTP_TIMEOUT=600)

INCLUDE(${ARCADIA_ROOT}/library/recipes/docker_compose/recipe.inc)

PEERDIR(
    contrib/python/pytest
    ydb/tests/yt_integration/yt_in_docker
)

TEST_SRCS(
    test_yt_integration.py
)

END()
