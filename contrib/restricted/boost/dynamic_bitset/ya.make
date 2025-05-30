# Generated by devtools/yamaker from nixpkgs 24.05.

LIBRARY()

LICENSE(BSL-1.0)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(1.88.0)

ORIGINAL_SOURCE(https://github.com/boostorg/dynamic_bitset/archive/boost-1.88.0.tar.gz)

PEERDIR(
    contrib/restricted/boost/assert
    contrib/restricted/boost/config
    contrib/restricted/boost/container_hash
    contrib/restricted/boost/core
    contrib/restricted/boost/integer
    contrib/restricted/boost/move
    contrib/restricted/boost/static_assert
    contrib/restricted/boost/throw_exception
)

ADDINCL(
    GLOBAL contrib/restricted/boost/dynamic_bitset/include
)

NO_COMPILER_WARNINGS()

NO_UTIL()

END()
