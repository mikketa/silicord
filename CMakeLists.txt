cmake_minimum_required(VERSION 3.20)
project(silicord VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_ASM_NASM_OBJECT_FORMAT win64)
enable_language(ASM_NASM)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Silicord builds for Windows x64 with MSVC (cl) or clang-cl.")
endif()

# No C runtime: /RTC1 needs the CRT, drop it from Debug. C++ goes without exceptions or RTTI.
string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

function(silicord_no_crt target)
  set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  target_include_directories(${target} PRIVATE src)
  target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN SILICORD_VERSION="${PROJECT_VERSION}")
  target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C>:/W4 /GS- /utf-8>
    $<$<COMPILE_LANGUAGE:CXX>:/W4 /GS- /utf-8 /GR- /EHs-c- /Zc:threadSafeInit->)
endfunction()

# Platform-independent pieces, shared by the client and the tests.
add_library(silicord_core STATIC
  src/b64.c
  src/crypto.c
  src/json.c
  src/md.c
  src/mem.c
  src/model.c
  src/msg.c
  src/qr.c
  src/rt.c
  src/sb.c
  src/utf.c
  asm/str.asm
)
silicord_no_crt(silicord_core)

add_executable(silicord
  src/main.c
  src/console.c
  src/cred.c
  src/gw.c
  src/http.c
  src/img.c
  src/ra.c
  src/render.cpp
  src/ui.c
  src/ws.c
  src/silicord.manifest
)
silicord_no_crt(silicord)
target_link_options(silicord PRIVATE /NODEFAULTLIB /ENTRY:entry /SUBSYSTEM:WINDOWS)
target_link_libraries(silicord PRIVATE silicord_core kernel32 user32 gdi32 shell32 ole32 d2d1 dwrite windowscodecs dwmapi advapi32 bcrypt winhttp)

enable_testing()
function(silicord_test name)
  add_executable(${name} tests/${name}.c)
  silicord_no_crt(${name})
  target_include_directories(${name} PRIVATE tests)
  target_link_options(${name} PRIVATE /NODEFAULTLIB /ENTRY:entry /SUBSYSTEM:CONSOLE /MANIFEST:NO)
  target_link_libraries(${name} PRIVATE silicord_core kernel32 ${ARGN})
endfunction()

silicord_test(json_test)
add_test(NAME json COMMAND json_test)

silicord_test(crypto_test bcrypt crypt32)
add_test(NAME crypto COMMAND crypto_test)

silicord_test(msg_test)
add_test(NAME msg COMMAND msg_test)

silicord_test(model_test)
add_test(NAME model COMMAND model_test)

# The QR test needs an independent decoder: pip install numpy zxing-cpp
silicord_test(qr_test)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME qr COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/qr_check.py $<TARGET_FILE:qr_test>)
endif()
