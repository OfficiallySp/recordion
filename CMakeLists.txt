cmake_minimum_required(VERSION 3.28)
project(recordion VERSION 0.1.0 LANGUAGES CXX RC)

if(NOT MSVC)
    message(FATAL_ERROR "Recordion targets Windows with MSVC only")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(wil CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(VPL CONFIG REQUIRED)
find_package(doctest CONFIG REQUIRED)

# Shared compile/link options for everything we build.
add_library(recordion_options INTERFACE)
target_compile_options(recordion_options INTERFACE
    /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc /bigobj /MP
    /external:anglebrackets /external:W0
    $<$<CONFIG:Release>:/O2 /Oi /GL /Gy /Gw>)
target_compile_definitions(recordion_options INTERFACE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=NTDDI_WIN11_GE)
target_link_options(recordion_options INTERFACE
    $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)

# Vendored, header-only GPU encoder SDK interfaces (runtimes ship with the drivers).
add_library(amf_headers INTERFACE)
target_include_directories(amf_headers SYSTEM INTERFACE third_party/amf/include)
add_library(nvenc_headers INTERFACE)
target_include_directories(nvenc_headers SYSTEM INTERFACE third_party/nvenc/include)

# HLSL compiled at build time into headers (no runtime d3dcompiler dependency).
find_program(FXC_EXE fxc REQUIRED)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
set(SHADER_HEADERS)
function(compile_shader SRC ENTRY PROFILE VAR NAME)
    set(out ${SHADER_OUT}/${NAME}.h)
    add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT}
        COMMAND ${FXC_EXE} /nologo /O3 /T ${PROFILE} /E ${ENTRY} /Vn ${VAR} /Fh ${out} ${SRC}
        DEPENDS ${SRC} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${out} PARENT_SCOPE)
endfunction()
compile_shader(${CMAKE_SOURCE_DIR}/src/capture/FakeCapture.hlsl vsMain vs_5_0 g_fakeCaptureVS FakeCaptureVS)
compile_shader(${CMAKE_SOURCE_DIR}/src/capture/FakeCapture.hlsl psMain ps_5_0 g_fakeCapturePS FakeCapturePS)

# Everything except the WinMain shell, so tests can link it.
add_library(recordion_core STATIC
    src/core/Clock.cpp
    src/core/FrameTimer.cpp
    src/core/Hotkey.cpp
    src/core/Log.cpp
    src/core/Paths.cpp
    src/core/Settings.cpp
    src/core/ThreadQos.cpp
    src/core/Utf.cpp
    src/gpu/D3DDevice.cpp
    src/gpu/ColorConverter.cpp
    src/gpu/GpuTimer.cpp
    src/capture/WgcCapture.cpp
    src/capture/FakeCapture.cpp
    src/mux/Bitstream.cpp
    src/mux/Mp4Writer.cpp
    src/encode/AmfEncoder.cpp
    src/buffer/ReplayBuffer.cpp
    src/output/FileNaming.cpp
    src/output/AsyncFileWriter.cpp
    src/output/ClipSaver.cpp
    src/output/Recorder.cpp
    src/pipeline/CaptureSession.cpp
    src/audio/AacEncoder.cpp
    src/audio/WasapiSource.cpp
    src/audio/AudioPipeline.cpp
)
target_sources(recordion_core PRIVATE ${SHADER_HEADERS})
target_include_directories(recordion_core PUBLIC src PRIVATE ${SHADER_OUT})
target_link_libraries(recordion_core
    PUBLIC recordion_options WIL::WIL amf_headers nvenc_headers
    PRIVATE nlohmann_json::nlohmann_json
    PUBLIC d3d11 dxgi avrt shell32 ole32 windowsapp mfplat mfuuid wmcodecdspuuid mmdevapi)

add_executable(recordion WIN32
    src/app/main.cpp
    src/app/App.cpp
    src/app/Bench.cpp
    src/app/Hotkeys.cpp
    src/app/Notifier.cpp
    src/app/Tray.cpp
    src/app/recordion.rc
    src/app/recordion.manifest
)
target_link_libraries(recordion PRIVATE recordion_core psapi d2d1 dwrite winmm shcore)

enable_testing()
add_executable(recordion_tests
    tests/test_main.cpp
    tests/test_hotkey.cpp
    tests/test_settings.cpp
    tests/test_bitstream.cpp
    tests/test_mp4.cpp
    tests/test_naming.cpp
    tests/test_replay_buffer.cpp
    tests/test_file_writer.cpp
    tests/test_audio.cpp
)
target_link_libraries(recordion_tests PRIVATE recordion_core doctest::doctest)
target_compile_definitions(recordion_tests PRIVATE RECORDION_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
# Optional: validate muxed files with ffprobe/ffmpeg when they're installed (dev machines only).
file(GLOB FFMPEG_WINGET_DIRS "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Packages/Gyan.FFmpeg*/*/bin")
find_program(FFPROBE_EXE ffprobe HINTS ${FFMPEG_WINGET_DIRS})
find_program(FFMPEG_EXE ffmpeg HINTS ${FFMPEG_WINGET_DIRS})
if(FFPROBE_EXE AND FFMPEG_EXE)
    target_compile_definitions(recordion_tests PRIVATE RECORDION_FFPROBE="${FFPROBE_EXE}" RECORDION_FFMPEG="${FFMPEG_EXE}")
endif()
add_test(NAME recordion_tests COMMAND recordion_tests)
