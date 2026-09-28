# Linux build of the portable core, its unit tests and a fuzzer, under
# AddressSanitizer and UBSan. The Windows build (the top-level CMakeLists.txt)
# stays the reference; this one catches memory errors CI's MSVC build cannot.
#
#   cmake -S tests/host -B build-host && cmake --build build-host && ctest --test-dir build-host
cmake_minimum_required(VERSION 3.20)
project(silicord_host C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(ZLIB REQUIRED)
option(WERROR "Treat warnings as errors" OFF)

set(SANITIZE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
# -Wshadow as MSVC warns (C4456) on shadowed locals and the Windows build uses /WX.
add_compile_options(-std=c11 -g -O1 -fshort-wchar -Wall -Wextra -Wshadow -Wno-unused-parameter -Wno-sign-compare
                    -Wno-unknown-pragmas -Wno-missing-field-initializers ${SANITIZE})
add_link_options(${SANITIZE})
if(WERROR)
  add_compile_options(-Werror)
endif()
# The core's headers use MSVC's __inline and assume WIN32_LEAN_AND_MEAN.
add_compile_definitions(__inline=inline WIN32_LEAN_AND_MEAN _GNU_SOURCE)
include_directories(shim ${ROOT}/src ${ROOT}/tests)

add_library(core STATIC
  ${ROOT}/src/apng.c ${ROOT}/src/b64.c ${ROOT}/src/command.c ${ROOT}/src/emoji.c ${ROOT}/src/emoji_data.c ${ROOT}/src/emoji_alias.c
  ${ROOT}/src/inflate.c ${ROOT}/src/json.c ${ROOT}/src/md.c ${ROOT}/src/mem.c ${ROOT}/src/memberlist.c
  ${ROOT}/src/model.c ${ROOT}/src/msg.c ${ROOT}/src/profile.c ${ROOT}/src/qr.c ${ROOT}/src/sb.c
  ${ROOT}/src/search.c ${ROOT}/src/sha2.c ${ROOT}/src/stats.c ${ROOT}/src/utf.c shim/shim.c)

# The same test sources as on Windows (crypto, render and QR need Windows).
enable_testing()
foreach(name apng command emoji inflate json md memberlist model msg profile search sha2 stats)
  add_executable(${name}_test ${ROOT}/tests/${name}_test.c shim/main.c)
  target_link_libraries(${name}_test core)
  add_test(NAME ${name} COMMAND ${name}_test)
  set_tests_properties(${name} PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL ")
endforeach()

# The fuzzer's dictionaries come from the sources and the tests' JSON.
set(DICT ${CMAKE_CURRENT_BINARY_DIR}/dict)
add_custom_command(
  OUTPUT ${DICT}/keys.inc ${DICT}/strs.inc ${DICT}/seeds.inc ${DICT}/apng.inc
  COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/gen_dict.py ${ROOT} ${DICT}
  DEPENDS gen_dict.py ${ROOT}/tests/model_test.c ${ROOT}/tests/msg_test.c ${ROOT}/tests/apng_test.c
  COMMENT "Fuzzer dictionaries")
add_executable(fuzz fuzz.c ${DICT}/keys.inc ${DICT}/strs.inc ${DICT}/seeds.inc ${DICT}/apng.inc)
target_include_directories(fuzz PRIVATE ${DICT})
target_link_libraries(fuzz core ZLIB::ZLIB)

# A short, fixed run with every test; longer ones by hand: fuzz 100000 json 42
set(FUZZ_ITERATIONS 2000 CACHE STRING "Iterations of the fuzz test")
add_test(NAME fuzz COMMAND fuzz ${FUZZ_ITERATIONS} all 1)

# The QR encoder, decoded by an independent reader (pip install numpy zxing-cpp), as on Windows.
add_executable(qr_test ${ROOT}/tests/qr_test.c shim/main.c)
target_link_libraries(qr_test core)
execute_process(COMMAND ${Python3_EXECUTABLE} -c "import numpy, zxingcpp" RESULT_VARIABLE NO_DECODER OUTPUT_QUIET ERROR_QUIET)
if(NOT NO_DECODER)
  add_test(NAME qr COMMAND Python3::Interpreter ${ROOT}/tests/qr_check.py $<TARGET_FILE:qr_test>)
endif()
