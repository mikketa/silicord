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
  src/mixer.c
  src/opus.c
  src/opus_celt.c
  src/opus_math.c
  src/opus_rc.c
  src/opus_silk.c
  src/opus_silk_tables.c
  src/vp8.c
  src/vp8_dsp.c
  src/vp8_enc.c
  src/vp8_rtp.c
  src/vp8_tables.c
  src/model.c
  src/p256.c
  src/picture.c
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
  src/audio.c
  src/camera.c
  src/screen.c
  src/ui.c
  src/voice.c
  src/ws.c
  src/silicord.manifest
  src/silicord.rc
)
silicord_no_crt(silicord)
target_compile_definitions(silicord PRIVATE
  $<$<COMPILE_LANGUAGE:RC>:SILICORD_VERSION_RC=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0>
  $<$<COMPILE_LANGUAGE:RC>:SILICORD_ICON="${CMAKE_SOURCE_DIR}/assets/silicord.ico">)
target_link_options(silicord PRIVATE /NODEFAULTLIB /ENTRY:entry /SUBSYSTEM:WINDOWS)
target_link_libraries(silicord PRIVATE silicord_core kernel32 user32 gdi32 shell32 comdlg32 ole32 dwrite windowscodecs dwmapi advapi32 bcrypt winhttp ws2_32 winmm mf mfplat mfreadwrite mfuuid d3d11 dxgi dxguid)

enable_testing()
# tests/<name>_test.c, built like the client; extra arguments are libraries to link.
function(silicord_test_exe name)
  add_executable(${name}_test tests/${name}_test.c)
  silicord_no_crt(${name}_test)
  target_include_directories(${name}_test PRIVATE tests)
  target_link_options(${name}_test PRIVATE /NODEFAULTLIB /ENTRY:entry /SUBSYSTEM:CONSOLE /MANIFEST:NO)
  target_link_libraries(${name}_test PRIVATE silicord_core kernel32 ${ARGN})
endfunction()

# The same, run as test <name>.
function(silicord_test name)
  silicord_test_exe(${name} ${ARGN})
  add_test(NAME ${name} COMMAND ${name}_test)
endfunction()

foreach(name stats str json msg model md profile command aes rtp opus vp8_rtp mixer hpke p256 sha2 search apng inflate
             emoji memberlist picture)
  silicord_test(${name})
endforeach()
foreach(name dave mls_schedule mls_tree mls_path mls_msg mls_group dave_session mls_crypto)
  silicord_test(${name} bcrypt)
endforeach()
silicord_test(crypto bcrypt crypt32)
silicord_test(render ole32 gdi32 user32 dwrite windowscodecs)
target_sources(render_test PRIVATE src/render.cpp)

# The QR test needs an independent decoder: pip install numpy zxing-cpp
silicord_test_exe(qr)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME qr COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/qr_check.py $<TARGET_FILE:qr_test>)
endif()
