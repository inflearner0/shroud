find_package(Python3 COMPONENTS Interpreter)

add_executable(shroud_unit_tests
  unit/unit_tests.cpp
  ${CMAKE_SOURCE_DIR}/src/Config.cpp
  ${CMAKE_SOURCE_DIR}/src/RNG.cpp
)
target_include_directories(shroud_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_test(NAME unit COMMAND shroud_unit_tests)

find_program(SHROUD_TEST_CLANG NAMES clang-18 clang)

if(SHROUD_TEST_CLANG AND Python3_Interpreter_FOUND)
  add_test(NAME differential_obf_target
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/differential.py
      --clang ${SHROUD_TEST_CLANG}
      --plugin $<TARGET_FILE:shroud>
      --program ${CMAKE_CURRENT_SOURCE_DIR}/programs/obf_target.c
      --workdir ${CMAKE_BINARY_DIR}/testing/obf_target
      --min-transformed 10
      --min-vm 8)

  add_test(NAME differential_obf_target_scalar
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/differential.py
      --clang ${SHROUD_TEST_CLANG}
      --plugin $<TARGET_FILE:shroud>
      --program ${CMAKE_CURRENT_SOURCE_DIR}/programs/obf_target.c
      --workdir ${CMAKE_BINARY_DIR}/testing/obf_target_scalar
      --cflag=-fno-vectorize
      --cflag=-fno-slp-vectorize
      --min-transformed 20
      --min-vm 18)

  add_test(NAME differential_hello
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/differential.py
      --clang ${SHROUD_TEST_CLANG}
      --plugin $<TARGET_FILE:shroud>
      --program ${CMAKE_CURRENT_SOURCE_DIR}/programs/hello.c
      --workdir ${CMAKE_BINARY_DIR}/testing/hello)

  add_test(NAME crackme
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/crackme_diff.py
      --clang ${SHROUD_TEST_CLANG}
      --plugin $<TARGET_FILE:shroud>
      --program ${CMAKE_CURRENT_SOURCE_DIR}/programs/crackme.c
      --workdir ${CMAKE_BINARY_DIR}/testing/crackme)

  add_test(NAME differential_profile
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/differential.py
      --clang ${SHROUD_TEST_CLANG}
      --plugin $<TARGET_FILE:shroud>
      --program ${CMAKE_CURRENT_SOURCE_DIR}/programs/profile_target.c
      --workdir ${CMAKE_BINARY_DIR}/testing/profile
      --profile ${CMAKE_CURRENT_SOURCE_DIR}/profile.txt
      --min-transformed 2
      --min-vm 1)
endif()
