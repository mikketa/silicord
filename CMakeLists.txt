cmake_minimum_required(VERSION 3.20)
project(silicord VERSION 0.0.1 LANGUAGES C)

set(CMAKE_ASM_NASM_OBJECT_FORMAT win64)
enable_language(ASM_NASM)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Silicord builds for Windows x64 with MSVC (cl) or clang-cl.")
endif()

# No C runtime: /RTC1 needs the CRT, drop it from Debug.
string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")

add_executable(silicord
  src/main.c
  src/console.c
  src/rt.c
  asm/str.asm
)

target_include_directories(silicord PRIVATE src)
target_compile_definitions(silicord PRIVATE WIN32_LEAN_AND_MEAN SILICORD_VERSION="${PROJECT_VERSION}")
target_compile_options(silicord PRIVATE
  $<$<COMPILE_LANGUAGE:C>:/W4 /GS- /utf-8>
)
target_link_options(silicord PRIVATE
  /NODEFAULTLIB
  /ENTRY:entry
  /SUBSYSTEM:CONSOLE
  /MANIFEST:NO
)
target_link_libraries(silicord PRIVATE kernel32)
