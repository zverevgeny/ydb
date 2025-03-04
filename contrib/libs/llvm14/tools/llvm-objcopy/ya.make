# Generated by devtools/yamaker.

PROGRAM()

SUBSCRIBER(g:cpp-contrib)

VERSION(14.0.6)

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/llvm14
    contrib/libs/llvm14/include
    contrib/libs/llvm14/lib/BinaryFormat
    contrib/libs/llvm14/lib/Bitcode/Reader
    contrib/libs/llvm14/lib/Bitstream/Reader
    contrib/libs/llvm14/lib/Demangle
    contrib/libs/llvm14/lib/IR
    contrib/libs/llvm14/lib/MC
    contrib/libs/llvm14/lib/MC/MCParser
    contrib/libs/llvm14/lib/Object
    contrib/libs/llvm14/lib/Option
    contrib/libs/llvm14/lib/Remarks
    contrib/libs/llvm14/lib/Support
    contrib/libs/llvm14/lib/TextAPI
)

ADDINCL(
    ${ARCADIA_BUILD_ROOT}/contrib/libs/llvm14/tools/llvm-objcopy
    contrib/libs/llvm14/tools/llvm-objcopy
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    COFF/COFFObjcopy.cpp
    COFF/Object.cpp
    COFF/Reader.cpp
    COFF/Writer.cpp
    ConfigManager.cpp
    ELF/ELFObjcopy.cpp
    ELF/Object.cpp
    MachO/MachOLayoutBuilder.cpp
    MachO/MachOObjcopy.cpp
    MachO/MachOReader.cpp
    MachO/MachOWriter.cpp
    MachO/Object.cpp
    llvm-objcopy.cpp
    wasm/Object.cpp
    wasm/Reader.cpp
    wasm/WasmObjcopy.cpp
    wasm/Writer.cpp
)

END()
