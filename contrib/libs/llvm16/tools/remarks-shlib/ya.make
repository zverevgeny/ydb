# Generated by devtools/yamaker.

LIBRARY()

VERSION(16.0.0)

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/llvm16
    contrib/libs/llvm16/lib/Remarks
)

ADDINCL(
    contrib/libs/llvm16/tools/remarks-shlib
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    libremarks.cpp
)

END()
