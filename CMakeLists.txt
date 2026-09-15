cmake_minimum_required(VERSION 3.20)

project(Shroud VERSION 0.1.0 LANGUAGES C CXX)

option(SHROUD_BUILD_TESTS "Build Shroud tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LLVM REQUIRED CONFIG)
if(LLVM_PACKAGE_VERSION VERSION_LESS 18)
  message(FATAL_ERROR "Shroud requires LLVM 18, found ${LLVM_PACKAGE_VERSION}")
endif()
if(LLVM_PACKAGE_VERSION VERSION_GREATER_EQUAL 19)
  message(WARNING
    "Shroud is only tested against LLVM 18; found ${LLVM_PACKAGE_VERSION}. "
    "Pass-plugin APIs changed in newer LLVM releases and the build may fail.")
endif()
message(STATUS "Shroud: using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

find_program(SHROUD_CLANG NAMES clang-18 clang)
if(NOT SHROUD_CLANG)
  message(FATAL_ERROR "Shroud requires clang to compile the VM runtime to bitcode")
endif()
message(STATUS "Shroud: using clang at ${SHROUD_CLANG}")

add_subdirectory(src)

if(SHROUD_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
