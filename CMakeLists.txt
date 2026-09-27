cmake_minimum_required(VERSION 3.20)
project(silicord VERSION 0.0.1 LANGUAGES C)

set(CMAKE_ASM_NASM_OBJECT_FORMAT win64)
enable_language(ASM_NASM)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Silicord builds for Windows x64 with MSVC (cl) or clang-cl.")
endif()

# No C runtime: /RTC1 needs the CRT, drop it from Debug.
string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")

function(silicord_no_crt target)
  set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  target_include_directories(${target} PRIVATE src)
  target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN SILICORD_VERSION="${PROJECT_VERSION}")
  target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C>:/W4 /GS- /utf-8>)
endfunction()

# Platform-independent pieces, shared by the client and the tests.
add_library(silicord_core STATIC
  src/json.c
  src/mem.c
  src/rt.c
  src/sb.c
  src/utf.c
  asm/str.asm
)
silicord_no_crt(silicord_core)

add_executable(silicord
  src/main.c
  src/console.c
)
silicord_no_crt(silicord)
target_link_options(silicord PRIVATE /NODEFAULTLIB /ENTRY:entry /SUBSYSTEM:CONSOLE /MANIFEST:NO)
target_link_libraries(silicord PRIVATE silicord_core kernel32)
