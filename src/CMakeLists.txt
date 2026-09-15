set(SHROUD_RUNTIME_BC ${CMAKE_CURRENT_BINARY_DIR}/vm_runtime.bc)
set(SHROUD_RUNTIME_CPP ${CMAKE_CURRENT_BINARY_DIR}/VMRuntimeData.cpp)

add_custom_command(
  OUTPUT ${SHROUD_RUNTIME_BC}
  COMMAND ${SHROUD_CLANG} -O1 -emit-llvm -c -I${CMAKE_SOURCE_DIR}/include
          ${CMAKE_SOURCE_DIR}/runtime/vm_runtime.c -o ${SHROUD_RUNTIME_BC}
  DEPENDS ${CMAKE_SOURCE_DIR}/runtime/vm_runtime.c
          ${CMAKE_SOURCE_DIR}/include/shroud/VMBytecode.h
  COMMENT "Compiling Shroud VM runtime to bitcode"
  VERBATIM)

add_custom_command(
  OUTPUT ${SHROUD_RUNTIME_CPP}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${SHROUD_RUNTIME_BC} -DOUTPUT=${SHROUD_RUNTIME_CPP}
          -P ${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake
  DEPENDS ${SHROUD_RUNTIME_BC} ${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake
  COMMENT "Embedding VM runtime bitcode"
  VERBATIM)

add_library(shroud MODULE
  Annotation.cpp
  Config.cpp
  RNG.cpp
  RuntimeSupport.cpp
  ShroudPass.cpp
  Transforms.cpp
  VMVirtualizer.cpp
  ${SHROUD_RUNTIME_CPP}
)

target_include_directories(shroud PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(shroud SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})

if(LLVM_DEFINITIONS)
  separate_arguments(SHROUD_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
  target_compile_options(shroud PRIVATE ${SHROUD_LLVM_DEFINITIONS})
endif()

set(SHROUD_USE_LLVM_TARGET ON)
if(WIN32)
  set(SHROUD_USE_LLVM_TARGET OFF)
endif()

if(SHROUD_USE_LLVM_TARGET AND TARGET LLVM)
  target_link_libraries(shroud PRIVATE LLVM)
else()
  llvm_map_components_to_libnames(SHROUD_LLVM_LIBS
    AggressiveInstCombine Analysis AsmParser BinaryFormat BitReader BitWriter
    BitstreamReader CFGuard CodeGen CodeGenTypes Core Coroutines DebugInfoBTF
    DebugInfoCodeView DebugInfoDWARF DebugInfoMSF DebugInfoPDB Demangle
    FrontendOffloading FrontendOpenMP HipStdPar IRPrinter IRReader InstCombine
    Instrumentation Linker MC MCParser ObjCARCOpts Object Passes ProfileData
    Remarks ScalarOpts Support Symbolize Target TargetParser TextAPI
    TransformUtils Vectorize ipo)
  target_link_libraries(shroud PRIVATE ${SHROUD_LLVM_LIBS})
  if(LLVM_SYSTEM_LIBS)
    set(SHROUD_SYSTEM_LIBS ${LLVM_SYSTEM_LIBS})
    list(FILTER SHROUD_SYSTEM_LIBS EXCLUDE REGEX "diaguids")
    target_link_libraries(shroud PRIVATE ${SHROUD_SYSTEM_LIBS})
  endif()
  if(WIN32)
    target_link_options(shroud PRIVATE "/NODEFAULTLIB:diaguids")
  endif()
endif()

if(NOT LLVM_ENABLE_RTTI)
  if(MSVC)
    target_compile_options(shroud PRIVATE /GR-)
  else()
    target_compile_options(shroud PRIVATE -fno-rtti)
  endif()
endif()

set_target_properties(shroud PROPERTIES PREFIX "" OUTPUT_NAME "shroud")

if(WIN32)
  set_target_properties(shroud PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
