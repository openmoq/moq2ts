cmake_minimum_required(VERSION 3.24)

project(moq2ts-ui
    VERSION 1.3.0
    DESCRIPTION "Cross-platform MOQ live publisher for H.264/AAC-Opus M2TS"
    LANGUAGES C CXX)

option(MOQ2TS_BUILD_WITH_MOCK_MOQXR "Build without linking to a moqxr SDK and use the mock publisher adapter" OFF)
option(MOQ2TS_BUILD_RECEIVER "Build the moq5 MPEG-TS receiver CLI" OFF)
option(MOQ2TS_ENABLE_OPENH264 "Enable OpenH264 integration for H.264 encoding" ON)
option(MOQ2TS_ENABLE_LIBAV_AUDIO "Enable libavcodec/libavformat integration" ON)
option(MOQ2TS_ENABLE_LIBOPUS "Enable libopus fallback audio encoder path" ON)
set(MOQXR_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../moqxr" CACHE PATH "Path to local moqxr checkout")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------- Qt discovery ----------------
# QT_LIBS      -> Widgets(+Multimedia); linked only by the GUI executable.
# CORE_QT_LIBS -> Core + Gui; linked by the reusable streaming core and the
#                 headless CLI (no Widgets, no display server required).
set(QT_LIBS)
set(CORE_QT_LIBS)
if(NOT QT_DEFAULT_MAJOR_VERSION)
    set(QT_OPTIONAL_COMPONENTS)
    if(WIN32)
        list(APPEND QT_OPTIONAL_COMPONENTS Multimedia)
    endif()

    find_package(Qt6 QUIET COMPONENTS Core Gui Widgets OPTIONAL_COMPONENTS ${QT_OPTIONAL_COMPONENTS})
    if(Qt6_FOUND)
        list(APPEND QT_LIBS Qt6::Widgets)
        list(APPEND CORE_QT_LIBS Qt6::Core Qt6::Gui)
        if(TARGET Qt6::Multimedia)
            list(APPEND QT_LIBS Qt6::Multimedia)
            add_compile_definitions(MOQ2TS_HAVE_QT_MULTIMEDIA)
        endif()
    else()
        find_package(Qt5 QUIET COMPONENTS Core Gui Widgets OPTIONAL_COMPONENTS ${QT_OPTIONAL_COMPONENTS})
        if(Qt5_FOUND)
            list(APPEND QT_LIBS Qt5::Widgets)
            list(APPEND CORE_QT_LIBS Qt5::Core Qt5::Gui)
            if(TARGET Qt5::Multimedia)
                list(APPEND QT_LIBS Qt5::Multimedia)
                add_compile_definitions(MOQ2TS_HAVE_QT_MULTIMEDIA)
            endif()
        endif()
    endif()
endif()

if(NOT QT_LIBS)
    message(FATAL_ERROR "Qt Widgets was not found. Install Qt5/Qt6 development packages before building.")
endif()

# ---------------- External codec dependencies ----------------

# openh264
if(MOQ2TS_ENABLE_OPENH264)
    find_path(OPENH264_INCLUDE_DIR wels/codec_api.h)
    find_library(OPENH264_LIBRARY NAMES openh264)
    if(OPENH264_INCLUDE_DIR AND OPENH264_LIBRARY)
        set(OPENH264_FOUND TRUE)
        add_library(openh264::openh264 UNKNOWN IMPORTED)
        set_target_properties(openh264::openh264 PROPERTIES
            IMPORTED_LOCATION "${OPENH264_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${OPENH264_INCLUDE_DIR}")
        add_compile_definitions(MOQ2TS_HAVE_OPENH264)
    else()
        set(OPENH264_FOUND FALSE)
        message(WARNING "OpenH264 not found. UI and packetization will still work; add libopenh264-dev or equivalent to enable full encode path.")
    endif()
endif()

# libav via pkg-config
if(MOQ2TS_ENABLE_LIBAV_AUDIO)
    include(FindPkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBAVCODEC QUIET IMPORTED_TARGET libavcodec)
        pkg_check_modules(LIBAVDEVICE QUIET IMPORTED_TARGET libavdevice)
        pkg_check_modules(LIBAVFORMAT QUIET IMPORTED_TARGET libavformat)
        pkg_check_modules(LIBAVUTIL QUIET IMPORTED_TARGET libavutil)
        pkg_check_modules(LIBSWRESAMPLE QUIET IMPORTED_TARGET libswresample)
        pkg_check_modules(LIBSWSCALE QUIET IMPORTED_TARGET libswscale)
    endif()
    if(LIBAVCODEC_FOUND AND LIBAVFORMAT_FOUND AND LIBAVUTIL_FOUND)
        set(FFMPEG_FOUND TRUE)
        add_compile_definitions(MOQ2TS_HAVE_LIBAV)
        if(LIBAVDEVICE_FOUND AND LIBSWRESAMPLE_FOUND AND LIBSWSCALE_FOUND)
            add_compile_definitions(MOQ2TS_HAVE_LIBAV_CAPTURE)
        endif()
    else()
        set(FFMPEG_FOUND FALSE)
        message(WARNING "libav libraries were not found via pkg-config. Install libavcodec-dev/libavformat-dev/libavutil-dev for optional transcoding support.")
    endif()
endif()

# libopus (optional)
if(MOQ2TS_ENABLE_LIBOPUS)
    find_library(OPUS_LIBRARY NAMES opus libopus)
    find_path(OPUS_INCLUDE_DIR opus/opus.h)
    if(OPUS_LIBRARY AND OPUS_INCLUDE_DIR)
        set(OPUS_FOUND TRUE)
        add_library(opus::opus UNKNOWN IMPORTED)
        set_target_properties(opus::opus PROPERTIES
            IMPORTED_LOCATION "${OPUS_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${OPUS_INCLUDE_DIR}")
        add_compile_definitions(MOQ2TS_HAVE_LIBOPUS)
    else()
        set(OPUS_FOUND FALSE)
        message(WARNING "libopus not found. Opus encode path will be unavailable.")
    endif()
endif()

# moqxr API discovery.
#
# Two ways to link the real (non-mock) publisher:
# - MOQXR_SDK_DIR: a prebuilt SDK extracted from an openmoq-publisher release
#     (include/ + lib/ of static libraries). Preferred for CI and releases.
# - MOQXR_SOURCE_DIR: a local moqxr source checkout, built via add_subdirectory.
set(MOQXR_SDK_DIR "" CACHE PATH "Path to a prebuilt openmoq publisher SDK (include/ + lib/). When set, link the prebuilt libraries instead of building moqxr from source.")
set(MOQXR_USE_PREBUILT_SDK FALSE)
if(MOQXR_SDK_DIR)
    set(MOQXR_USE_PREBUILT_SDK TRUE)
endif()

set(MOQXR_PUBLISH_INCLUDE_DIR_HINT "${MOQXR_SOURCE_DIR}/include" CACHE PATH "Optional moqxr include directory")
find_path(MOQXR_PUBLISH_INCLUDE_DIR
    NAMES openmoq/publisher/publisher_api.h
    PATHS "${MOQXR_SDK_DIR}/include" ${MOQXR_PUBLISH_INCLUDE_DIR_HINT} /usr/include /usr/local/include /opt/include
)
if(MOQXR_PUBLISH_INCLUDE_DIR)
    set(MOQXR_PUBLISH_FOUND TRUE)
else()
    set(MOQXR_PUBLISH_FOUND FALSE)
    if(NOT MOQ2TS_BUILD_WITH_MOCK_MOQXR)
        message(FATAL_ERROR "moqxr publish headers not found. Set MOQ2TS_BUILD_WITH_MOCK_MOQXR=ON or provide MOQXR_SDK_DIR/MOQXR_SOURCE_DIR/MOQXR_PUBLISH_INCLUDE_DIR_HINT.")
    endif()
    message(STATUS "moqxr headers not found. Building mock publisher adapter.")
endif()

if(MOQXR_PUBLISH_FOUND AND NOT MOQ2TS_BUILD_WITH_MOCK_MOQXR)
    add_compile_definitions(MOQ2TS_HAS_MOQXR)
    if(MOQXR_USE_PREBUILT_SDK)
        # Link every static library shipped in the SDK (the publisher archive plus
        # its picoquic/picotls/picohttp dependencies). OpenSSL is a system dep.
        file(GLOB MOQXR_SDK_LIBS "${MOQXR_SDK_DIR}/lib/*.a" "${MOQXR_SDK_DIR}/lib/*.lib")
        if(NOT MOQXR_SDK_LIBS)
            message(FATAL_ERROR "MOQXR_SDK_DIR=${MOQXR_SDK_DIR} contains no lib/*.a or lib/*.lib. Point it at an extracted openmoq publisher SDK.")
        endif()
        find_package(OpenSSL REQUIRED)
    elseif(EXISTS "${MOQXR_SOURCE_DIR}/CMakeLists.txt" AND NOT TARGET openmoq_publisher_lib)
        set(OPENMOQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        # moq2ts receives SRT itself (SrtSource) and never uses moqxr's SRT
        # ingest. Left on, moqxr links the default OpenSSL libsrt while FFmpeg's
        # libavformat loads libsrt-gnutls. The two copies export the same
        # symbols, and libsrt-gnutls's exit-time destructors then run against the
        # other copy's state: a SIGSEGV on every exit, after srt_cleanup().
        set(OPENMOQ_ENABLE_SRT OFF CACHE BOOL "" FORCE)
        # moqxr hides the pkg-config search path while it configures picotls,
        # then restores PKG_CONFIG_LIBDIR as an empty string when it was unset.
        # pkg-config then searches no directory at all, so every
        # pkg_check_modules below fails: libsrt among them, which loses the
        # libsrt-gnutls choice further down. Put the variable back as it was.
        if(DEFINED ENV{PKG_CONFIG_LIBDIR})
            set(_moq2ts_pkg_config_libdir "$ENV{PKG_CONFIG_LIBDIR}")
        endif()
        add_subdirectory("${MOQXR_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/moqxr" EXCLUDE_FROM_ALL)
        if(DEFINED _moq2ts_pkg_config_libdir)
            set(ENV{PKG_CONFIG_LIBDIR} "${_moq2ts_pkg_config_libdir}")
            unset(_moq2ts_pkg_config_libdir)
        else()
            unset(ENV{PKG_CONFIG_LIBDIR})
        endif()
    endif()
endif()

# ---------------- Reusable streaming core (no Qt Widgets) ----------------
# The media/publish pipeline is compiled once into an OBJECT library that both the
# GUI executable and the headless CLI link. It uses Qt types but only Core + Gui at
# runtime (QImage lives in QtGui), so it never pulls in Widgets or a display server.
set(CORE_SOURCES
    src/media/LivePipeline.cpp
    src/media/LibavCaptureSource.cpp
    src/media/V4l2Capabilities.cpp
    src/media/LibavPreviewWorker.cpp
    src/media/M2tsPacketizer.cpp
    src/media/MsftsMuxer.cpp
    src/publish/MoqxrPublisher.cpp
)

if(APPLE)
    list(APPEND CORE_SOURCES
        src/media/MacCaptureEnumerator.mm
        src/media/MacAvCapture.mm
    )
    enable_language(OBJCXX)
    set_source_files_properties(
        src/media/MacCaptureEnumerator.mm
        src/media/MacAvCapture.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc"
    )
endif()

set(CORE_HEADERS
    src/app/PublishConfig.h
    src/media/LivePipeline.h
    src/media/LibavCaptureSource.h
    src/media/V4l2Capabilities.h
    src/media/LibavPreviewWorker.h
    src/media/M2tsPacketizer.h
    src/media/SiTables.h
    src/media/MsftsMuxer.h
    src/publish/MoqxrPublisher.h
    src/publish/ObjectLookahead.h
)

add_library(moq2ts-core OBJECT ${CORE_SOURCES} ${CORE_HEADERS})
set_target_properties(moq2ts-core PROPERTIES
    AUTOMOC TRUE
    AUTOUIC TRUE
    AUTORCC TRUE
    AUTOGEN_BUILD_DIR ${CMAKE_BINARY_DIR}/generated-core
    POSITION_INDEPENDENT_CODE ON
)
# PUBLIC so the include dirs / link libs propagate to every executable that links
# the object library.
target_include_directories(moq2ts-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(moq2ts-core PUBLIC ${CORE_QT_LIBS})

if(APPLE)
    target_link_libraries(moq2ts-core PUBLIC
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework Foundation"
    )
endif()

if(OPENH264_FOUND)
    target_link_libraries(moq2ts-core PUBLIC openh264::openh264)
endif()

if(FFMPEG_FOUND)
    target_include_directories(moq2ts-core PUBLIC ${LIBAVCODEC_INCLUDE_DIRS} ${LIBAVDEVICE_INCLUDE_DIRS} ${LIBAVFORMAT_INCLUDE_DIRS} ${LIBAVUTIL_INCLUDE_DIRS} ${LIBSWRESAMPLE_INCLUDE_DIRS} ${LIBSWSCALE_INCLUDE_DIRS})
    target_link_libraries(moq2ts-core PUBLIC PkgConfig::LIBAVCODEC PkgConfig::LIBAVFORMAT PkgConfig::LIBAVUTIL)
    if(LIBAVDEVICE_FOUND)
        target_link_libraries(moq2ts-core PUBLIC PkgConfig::LIBAVDEVICE)
    endif()
    if(LIBSWRESAMPLE_FOUND)
        target_link_libraries(moq2ts-core PUBLIC PkgConfig::LIBSWRESAMPLE)
    endif()
    if(LIBSWSCALE_FOUND)
        target_link_libraries(moq2ts-core PUBLIC PkgConfig::LIBSWSCALE)
    endif()
endif()

if(OPUS_FOUND)
    target_link_libraries(moq2ts-core PUBLIC opus::opus)
endif()

# Opt out explicitly. Auto-detection alone cannot be tested on a host that has
# libsrt installed, and the no-SRT build is exactly the configuration CI runs, so it
# needs to be reachable on demand rather than only by accident of the environment.
option(MOQ2TS_ENABLE_SRT "Build SRT ingest when libsrt is available" ON)

# libsrt is resolved here rather than inside the moqxr-SDK branch, so the mock build
# links it too. Both the library and the POSIX socket headers SrtSource.cpp needs are
# optional, so the file itself is added conditionally below.
pkg_check_modules(SRT IMPORTED_TARGET srt)
# FFmpeg's libavformat is linked against libsrt-gnutls. If we additionally link
# the default (OpenSSL) libsrt, BOTH copies are loaded into the process; they
# export the same symbols and each runs its own exit-time destructors against
# the other's uninitialised state, which segfaults during shutdown. When the
# gnutls build is present, link that one so exactly one libsrt is loaded.
find_library(SRT_GNUTLS_LIB
    NAMES libsrt-gnutls.so.1.5 libsrt-gnutls.so srt-gnutls
    PATHS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE} /usr/lib /usr/local/lib
    NO_DEFAULT_PATH)
set(MOQ2TS_SRT_USABLE FALSE)
if(SRT_FOUND AND SRT_GNUTLS_LIB)
    message(STATUS "Linking libsrt-gnutls (${SRT_GNUTLS_LIB}) to match FFmpeg and avoid a duplicate libsrt")
    target_include_directories(moq2ts-core PUBLIC ${SRT_INCLUDE_DIRS})
    target_link_libraries(moq2ts-core PUBLIC ${SRT_GNUTLS_LIB})
    set(MOQ2TS_SRT_USABLE TRUE)
elseif(SRT_FOUND)
    target_link_libraries(moq2ts-core PUBLIC PkgConfig::SRT)
    set(MOQ2TS_SRT_USABLE TRUE)
else()
    find_library(SRT_LIB NAMES srt srt-gnutls srt-openssl)
    if(SRT_LIB)
        target_link_libraries(moq2ts-core PUBLIC ${SRT_LIB})
        set(MOQ2TS_SRT_USABLE TRUE)
    endif()
endif()

# SRT ingest is compiled only where it can be. SrtSource.cpp includes <srt/srt.h>
# and the POSIX socket headers (netdb.h, poll.h, sys/socket.h, unistd.h), none of
# which exist under MSVC, so listing it unconditionally broke the build on any host
# without libsrt and on Windows outright. The rest of the CLI is portable, so the
# file is added only when usable and MOQ2TS_HAS_SRT tells the CLI whether
# --srt-config can be honoured.
if(MOQ2TS_ENABLE_SRT AND MOQ2TS_SRT_USABLE AND NOT WIN32)
    target_sources(moq2ts-core PRIVATE src/media/SrtSource.cpp src/media/SrtSource.h)
    target_compile_definitions(moq2ts-core PUBLIC MOQ2TS_HAS_SRT)
    message(STATUS "SRT ingest enabled")
else()
    message(STATUS "SRT ingest disabled (MOQ2TS_ENABLE_SRT=OFF, no libsrt, or unsupported platform); --srt-config will be rejected")
endif()

# Per-program (MPTS) publishing, as SRT ingest above: MptsFanOut.cpp and
# MptsPipeline.cpp include POSIX headers (poll.h, unistd.h, fcntl.h, sys/stat.h)
# that MSVC lacks, so the four MPTS files are compiled only on UNIX, and
# MOQ2TS_HAS_MPTS tells the CLI whether --config can be honoured. The option
# makes the excluded build reachable on a UNIX host, to test it.
option(MOQ2TS_ENABLE_MPTS "Build per-program (MPTS) publishing, --config" ON)
set(MOQ2TS_MPTS_BUILT FALSE)
if(MOQ2TS_ENABLE_MPTS AND UNIX)
    target_sources(moq2ts-core PRIVATE
        src/media/PacketQueueDevice.cpp src/media/PacketQueueDevice.h
        src/media/MptsFanOut.cpp src/media/MptsFanOut.h
        src/media/MptsPipeline.cpp src/media/MptsPipeline.h
        src/app/PublisherSpec.cpp src/app/PublisherSpec.h
    )
    target_compile_definitions(moq2ts-core PUBLIC MOQ2TS_HAS_MPTS)
    set(MOQ2TS_MPTS_BUILT TRUE)
    message(STATUS "Per-program (MPTS) publishing enabled")
else()
    message(STATUS "Per-program (MPTS) publishing disabled (MOQ2TS_ENABLE_MPTS=OFF or unsupported platform); --config will be rejected")
endif()

if(MOQXR_PUBLISH_FOUND)
    target_include_directories(moq2ts-core PUBLIC ${MOQXR_PUBLISH_INCLUDE_DIR})
endif()

if(NOT MOQ2TS_BUILD_WITH_MOCK_MOQXR AND MOQXR_USE_PREBUILT_SDK)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # GNU ld/lld: --start-group lets the linker rescan archives and resolve the
        # circular picohttp/picoquic dependencies regardless of archive order.
        target_link_libraries(moq2ts-core PUBLIC
            -Wl,--start-group ${MOQXR_SDK_LIBS} -Wl,--end-group)
    else()
        # ld64 (macOS) and MSVC resolve cross-archive symbols without grouping.
        target_link_libraries(moq2ts-core PUBLIC ${MOQXR_SDK_LIBS})
    endif()
    target_link_libraries(moq2ts-core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    if(WIN32)
        target_link_libraries(moq2ts-core PUBLIC ws2_32 iphlpapi bcrypt)
    endif()
elseif(NOT MOQ2TS_BUILD_WITH_MOCK_MOQXR AND TARGET openmoq_publisher_lib)
    target_link_libraries(moq2ts-core PUBLIC openmoq_publisher_lib)
elseif(NOT MOQ2TS_BUILD_WITH_MOCK_MOQXR)
    message(FATAL_ERROR "MOQ2TS_BUILD_WITH_MOCK_MOQXR=OFF requires either a prebuilt SDK (set MOQXR_SDK_DIR) or a local moqxr checkout exposing openmoq_publisher_lib (set MOQXR_SOURCE_DIR).")
endif()

# ---------------- GUI executable (Qt Widgets) ----------------
set(GUI_SOURCES
    src/main.cpp
    src/app/MainWindow.cpp
    src/app/PreviewPanel.cpp
)
set(GUI_HEADERS
    src/app/MainWindow.h
    src/app/PreviewPanel.h
)

add_executable(moq2ts-publisher
    ${GUI_SOURCES}
    ${GUI_HEADERS}
)

if(APPLE)
    set_target_properties(moq2ts-publisher PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "moq2ts"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.moq2ts.publisher"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "Copyright (c) moq2ts contributors"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MacOSXBundleInfo.plist.in"
    )
endif()

set_target_properties(moq2ts-publisher PROPERTIES AUTOGEN_BUILD_DIR ${CMAKE_BINARY_DIR}/generated)

set_target_properties(moq2ts-publisher PROPERTIES
    AUTOMOC TRUE
    AUTOUIC TRUE
    AUTORCC TRUE
)

target_include_directories(moq2ts-publisher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(moq2ts-publisher PRIVATE moq2ts-core ${QT_LIBS})

install(TARGETS moq2ts-publisher
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION .
)

# ---------------- Headless CLI (no Widgets; Linux/server deployment) ----------------
add_executable(moq2ts-cli src/cli/main_cli.cpp)
set_target_properties(moq2ts-cli PROPERTIES
    AUTOMOC TRUE
    AUTOGEN_BUILD_DIR ${CMAKE_BINARY_DIR}/generated-cli
)
target_include_directories(moq2ts-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(moq2ts-cli PRIVATE moq2ts-core ${CORE_QT_LIBS})

install(TARGETS moq2ts-cli
    RUNTIME DESTINATION bin
)

if(MOQ2TS_BUILD_RECEIVER)
    add_subdirectory(src/receive)
endif()

# Publisher draft selection and Linux-only tests.
if(UNIX AND NOT APPLE)
    enable_testing()
    add_executable(moq2ts-moqt-draft-tests tests/moqt_draft_test.cpp)
    target_include_directories(moq2ts-moqt-draft-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(moq2ts-moqt-draft-tests PRIVATE moq2ts-core)
    add_test(NAME moq2ts-moqt-draft-tests COMMAND moq2ts-moqt-draft-tests)

    # Pure V4L2 mode-selection logic test (no Qt/libav deps).
    add_executable(moq2ts-v4l2-selection-tests
        tests/v4l2_selection_test.cpp
        src/media/V4l2Capabilities.cpp
    )
    target_include_directories(moq2ts-v4l2-selection-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME moq2ts-v4l2-selection-tests COMMAND moq2ts-v4l2-selection-tests)
endif()

# Pure egress-pacing delay logic test (no Qt/libav deps).
if(UNIX AND NOT APPLE)
    enable_testing()
    add_executable(moq2ts-pace-delay-tests
        tests/pace_delay_test.cpp
    )
    target_include_directories(moq2ts-pace-delay-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME moq2ts-pace-delay-tests COMMAND moq2ts-pace-delay-tests)
endif()

# Pure live-ingest ring-buffer logic test (no Qt/libav/SRT deps).
if(UNIX AND NOT APPLE)
    enable_testing()
    add_executable(moq2ts-ts-ring-tests
        tests/ts_ring_buffer_test.cpp
    )
    target_include_directories(moq2ts-ts-ring-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME moq2ts-ts-ring-tests COMMAND moq2ts-ts-ring-tests)
endif()

# SRT listener mode: loopback test, only where SRT ingest is compiled in.
if(UNIX AND NOT APPLE AND MOQ2TS_ENABLE_SRT AND MOQ2TS_SRT_USABLE)
    enable_testing()
    add_executable(moq2ts-srt-listener-tests tests/srt_listener_test.cpp)
    target_include_directories(moq2ts-srt-listener-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(moq2ts-srt-listener-tests PRIVATE moq2ts-core)
    add_test(NAME moq2ts-srt-listener-tests COMMAND moq2ts-srt-listener-tests)
    set_tests_properties(moq2ts-srt-listener-tests PROPERTIES TIMEOUT 60)
endif()

# MSFTS catalog test (Qt Core only), and packetizer test on synthetic TS
# files. libav is linked when present, because the packetizer's duration
# probe uses it.
if(UNIX AND NOT APPLE)
    enable_testing()
    add_executable(moq2ts-msfts-catalog-tests
        tests/msfts_catalog_test.cpp
        src/media/MsftsMuxer.cpp
    )
    target_include_directories(moq2ts-msfts-catalog-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(moq2ts-msfts-catalog-tests PRIVATE ${CORE_QT_LIBS})
    add_test(NAME moq2ts-msfts-catalog-tests COMMAND moq2ts-msfts-catalog-tests)

    add_executable(moq2ts-m2ts-packetizer-tests
        tests/m2ts_packetizer_test.cpp
        src/media/M2tsPacketizer.cpp
    )
    target_include_directories(moq2ts-m2ts-packetizer-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(moq2ts-m2ts-packetizer-tests PRIVATE ${CORE_QT_LIBS})
    if(TARGET PkgConfig::LIBAVFORMAT)
        target_link_libraries(moq2ts-m2ts-packetizer-tests PRIVATE PkgConfig::LIBAVFORMAT PkgConfig::LIBAVUTIL)
    endif()
    add_test(NAME moq2ts-m2ts-packetizer-tests COMMAND moq2ts-m2ts-packetizer-tests)

    add_executable(moq2ts-psi-assembler-tests
        tests/psi_assembler_test.cpp
        src/media/M2tsPacketizer.cpp
    )
    target_include_directories(moq2ts-psi-assembler-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(moq2ts-psi-assembler-tests PRIVATE ${CORE_QT_LIBS})
    if(TARGET PkgConfig::LIBAVFORMAT)
        target_link_libraries(moq2ts-psi-assembler-tests PRIVATE PkgConfig::LIBAVFORMAT PkgConfig::LIBAVUTIL)
    endif()
    add_test(NAME moq2ts-psi-assembler-tests COMMAND moq2ts-psi-assembler-tests)

    add_executable(moq2ts-object-lookahead-tests tests/object_lookahead_test.cpp)
    target_include_directories(moq2ts-object-lookahead-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(moq2ts-object-lookahead-tests PRIVATE moq2ts-core)
    add_test(NAME moq2ts-object-lookahead-tests COMMAND moq2ts-object-lookahead-tests)

    # LivePipeline on a FIFO with source gaps, as SRT ingest feeds it.
    add_executable(moq2ts-live-pipeline-gap-tests tests/live_pipeline_gap_test.cpp)
    target_include_directories(moq2ts-live-pipeline-gap-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(moq2ts-live-pipeline-gap-tests PRIVATE moq2ts-core)
    add_test(NAME moq2ts-live-pipeline-gap-tests COMMAND moq2ts-live-pipeline-gap-tests)
    set_tests_properties(moq2ts-live-pipeline-gap-tests PROPERTIES TIMEOUT 60)

    # The pipeline against the mock transport: how a live source ends.
    add_executable(moq2ts-live-pipeline-end-tests tests/live_pipeline_end_test.cpp)
    target_include_directories(moq2ts-live-pipeline-end-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(moq2ts-live-pipeline-end-tests PRIVATE moq2ts-core ${CORE_QT_LIBS})
    add_test(NAME moq2ts-live-pipeline-end-tests COMMAND moq2ts-live-pipeline-end-tests)
endif()

# MPTS tests, only where the MPTS sources are compiled in.
if(UNIX AND NOT APPLE AND MOQ2TS_MPTS_BUILT)
    enable_testing()
    add_executable(moq2ts-packet-queue-tests tests/packet_queue_device_test.cpp)
    target_include_directories(moq2ts-packet-queue-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(moq2ts-packet-queue-tests PRIVATE moq2ts-core)
    add_test(NAME moq2ts-packet-queue-tests COMMAND moq2ts-packet-queue-tests)

    add_executable(moq2ts-mpts-fanout-tests tests/mpts_fanout_test.cpp)
    target_include_directories(moq2ts-mpts-fanout-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(moq2ts-mpts-fanout-tests PRIVATE moq2ts-core)
    add_test(NAME moq2ts-mpts-fanout-tests COMMAND moq2ts-mpts-fanout-tests)
    set_tests_properties(moq2ts-mpts-fanout-tests PROPERTIES TIMEOUT 120)

    add_executable(moq2ts-mpts-pipeline-tests tests/mpts_pipeline_test.cpp)
    target_include_directories(moq2ts-mpts-pipeline-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(moq2ts-mpts-pipeline-tests PRIVATE moq2ts-core)
    add_test(NAME moq2ts-mpts-pipeline-tests COMMAND moq2ts-mpts-pipeline-tests)
    set_tests_properties(moq2ts-mpts-pipeline-tests PROPERTIES TIMEOUT 180)

    add_executable(moq2ts-publisher-spec-tests tests/publisher_spec_test.cpp)
    target_include_directories(moq2ts-publisher-spec-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(moq2ts-publisher-spec-tests PRIVATE moq2ts-core)
    add_test(NAME moq2ts-publisher-spec-tests COMMAND moq2ts-publisher-spec-tests)
endif()
