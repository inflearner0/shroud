file(READ "${INPUT}" HEXDATA HEX)
string(LENGTH "${HEXDATA}" HEXLEN)
math(EXPR BYTES "${HEXLEN} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," HEXDATA "${HEXDATA}")
file(WRITE "${OUTPUT}"
"#include \"shroud/VMRuntimeData.h\"\n\nnamespace shroud {\n\nconst unsigned char vm_runtime_bc[] = {\n${HEXDATA}\n};\n\nconst std::size_t vm_runtime_bc_size = ${BYTES};\n\n}\n")
