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
  # /WX: the build has no warnings at /W4, and stays so.
  target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C>:/W4 /WX /GS- /utf-8>
    $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX /GS- /utf-8 /GR- /EHs-c- /Zc:threadSafeInit->)
endfunction()

# Platform-independent pieces, shared by the client and the tests.
add_library(silicord_core STATIC
  src/aes.c
  src/apng.c
  src/b64.c
  src/command.c
  src/crypto.c
  src/dave.c
  src/dave_session.c
  src/emoji.c
  src/emoji_data.c
  src/emoji_alias.c
  src/hpke.c
  src/inflate.c
  src/json.c
  src/md.c
  src/mem.c
  src/memberlist.c
  src/mls_crypto.c
  src/mls_group.c
  src/mls_msg.c
  src/mls_path.c
  src/mls_schedule.c
  src/mls_tree.c
  src/model.c
  src/p256.c
  src/msg.c
  src/profile.c
  src/qr.c
  src/rt.c
  src/rtp.c
  src/rng.c
  src/sb.c
  src/search.c
  src/sha2.c
  src/stats.c
  src/tls.c
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
  src/silicord.rc
)
silicord_no_crt(silicord)
target_compile_definitions(silicord PRIVATE
  $<$<COMPILE_LANGUAGE:RC>:SILICORD_VERSION_RC=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0>
  $<$<COMPILE_LANGUAGE:RC>:SILICORD_ICON="${CMAKE_SOURCE_DIR}/assets/silicord.ico">)
target_link_options(silicord PRIVATE /NODEFAULTLIB /ENTRY:entry /SUBSYSTEM:WINDOWS)
target_link_libraries(silicord PRIVATE silicord_core kernel32 user32 gdi32 shell32 comdlg32 ole32 dwrite windowscodecs dwmapi advapi32 bcrypt winhttp)

enable_testing()
function(silicord_test name)
  add_executable(${name} tests/${name}.c)
  silicord_no_crt(${name})
  target_include_directories(${name} PRIVATE tests)
  target_link_options(${name} PRIVATE /NODEFAULTLIB /ENTRY:entry /SUBSYSTEM:CONSOLE /MANIFEST:NO)
  target_link_libraries(${name} PRIVATE silicord_core kernel32 ${ARGN})
endfunction()

silicord_test(stats_test)
add_test(NAME stats COMMAND stats_test)

silicord_test(str_test)
add_test(NAME str COMMAND str_test)

silicord_test(json_test)
add_test(NAME json COMMAND json_test)

silicord_test(crypto_test bcrypt crypt32)
add_test(NAME crypto COMMAND crypto_test)

silicord_test(msg_test)
add_test(NAME msg COMMAND msg_test)

silicord_test(model_test)
add_test(NAME model COMMAND model_test)

silicord_test(md_test)
add_test(NAME md COMMAND md_test)

silicord_test(profile_test)
add_test(NAME profile COMMAND profile_test)

silicord_test(command_test)
add_test(NAME command COMMAND command_test)

silicord_test(aes_test)
add_test(NAME aes COMMAND aes_test)

silicord_test(dave_test bcrypt)
add_test(NAME dave COMMAND dave_test)

silicord_test(mls_schedule_test bcrypt)
add_test(NAME mls_schedule COMMAND mls_schedule_test)
silicord_test(mls_tree_test bcrypt)
add_test(NAME mls_tree COMMAND mls_tree_test)
silicord_test(mls_path_test bcrypt)
add_test(NAME mls_path COMMAND mls_path_test)
silicord_test(mls_msg_test bcrypt)
add_test(NAME mls_msg COMMAND mls_msg_test)
silicord_test(mls_group_test bcrypt)
add_test(NAME mls_group COMMAND mls_group_test)
silicord_test(dave_session_test bcrypt)
add_test(NAME dave_session COMMAND dave_session_test)
silicord_test(rtp_test)
add_test(NAME rtp COMMAND rtp_test)

silicord_test(mls_crypto_test bcrypt)
add_test(NAME mls_crypto COMMAND mls_crypto_test)

silicord_test(hpke_test)
add_test(NAME hpke COMMAND hpke_test)

silicord_test(p256_test)
add_test(NAME p256 COMMAND p256_test)

silicord_test(sha2_test)
add_test(NAME sha2 COMMAND sha2_test)

silicord_test(search_test)
add_test(NAME search COMMAND search_test)

silicord_test(apng_test)
add_test(NAME apng COMMAND apng_test)

silicord_test(inflate_test)
add_test(NAME inflate COMMAND inflate_test)

silicord_test(emoji_test)
add_test(NAME emoji COMMAND emoji_test)

silicord_test(memberlist_test)
add_test(NAME memberlist COMMAND memberlist_test)

silicord_test(render_test ole32 gdi32 user32 dwrite windowscodecs)
target_sources(render_test PRIVATE src/render.cpp)
add_test(NAME render COMMAND render_test)

# The QR test needs an independent decoder: pip install numpy zxing-cpp
silicord_test(qr_test)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME qr COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/qr_check.py $<TARGET_FILE:qr_test>)
endif()
