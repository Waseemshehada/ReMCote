cmake_minimum_required(VERSION 3.24)
project(ReMCoteHost VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "ReMCoteHost targets Windows x64 only.")
endif()

# Dependencies via Conan 2 (see conanfile.txt):
#   libdatachannel/0.24.0 — WebRTC (peer connection, DTLS/SRTP, H264 packetization).
#     Media support is enabled in the ConanCenter package and RTC_ENABLE_MEDIA=1
#     is propagated by the package itself — do NOT define it manually here.
#   nlohmann_json — signaling / input message JSON
find_package(LibDataChannel CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# NVIDIA Video Codec SDK (NVENC). Headers only; nvEncodeAPI is loaded
# dynamically from the NVIDIA driver at runtime, so no SDK .lib is required.
set(NVCODEC_SDK_DIR "" CACHE PATH "Path to NVENC headers (dir containing ffnvcodec/nvEncodeAPI.h)")
if(NVCODEC_SDK_DIR STREQUAL "")
  set(NVCODEC_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nv-codec-headers/include")
endif()

# WebView2 embedded viewer — temporarily disabled while the underlying MSVC
# compile error in WebView2.h on the CI runner is investigated.
# DesktopViewer.h/.cpp use a no-op stub for now; restore by re-enabling the
# WebView2 SDK block below and linking WebView2LoaderStatic.lib.

# Download WebView2 Runtime bootstrapper for the installer if not already present.
# The installer still ships the Edge WebView2 runtime for the remote viewer web page.
set(_wv2_bootstrapper "${CMAKE_CURRENT_SOURCE_DIR}/installer/MicrosoftEdgeWebview2Setup.exe")
if(NOT EXISTS "${_wv2_bootstrapper}")
  message(STATUS "Downloading WebView2 Runtime bootstrapper for installer...")
  execute_process(
    COMMAND powershell -NonInteractive -ExecutionPolicy Bypass -Command
      "& { $ProgressPreference='SilentlyContinue'; Invoke-WebRequest -Uri 'https://go.microsoft.com/fwlink/?LinkId=2124703' -OutFile '${_wv2_bootstrapper}' -UseBasicParsing }"
    RESULT_VARIABLE _wv2b_result
    ERROR_VARIABLE  _wv2b_error
  )
  if(NOT _wv2b_result EQUAL 0)
    message(WARNING "WebView2 Runtime bootstrapper download failed (${_wv2b_error}) — installer will not bundle it.")
  endif()
endif()

add_executable(ReMCoteHost WIN32
  src/main.cpp
  src/DeviceRegistration.cpp
  src/CaptureEngine.cpp
  src/EncoderEngine.cpp
  src/WebRtcTransport.cpp
  src/InputEngine.cpp
  src/PerformanceMonitor.cpp
  src/HostUI.cpp
  src/Logger.cpp
  src/DesktopViewer.cpp
)

target_include_directories(ReMCoteHost PRIVATE
  src
  ${NVCODEC_SDK_DIR}
)

target_link_libraries(ReMCoteHost PRIVATE
  LibDataChannel::LibDataChannel
  nlohmann_json::nlohmann_json
  d3d11
  dxgi
  ws2_32
  user32
  gdi32
  shell32
  ole32
)

target_compile_definitions(ReMCoteHost PRIVATE
  UNICODE _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  _CRT_SECURE_NO_WARNINGS         # suppress C4996 deprecation on wcstombs etc.
  _CRT_NONSTDC_NO_WARNINGS        # suppress C4996 on POSIX-named functions
)

set_target_properties(ReMCoteHost PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
