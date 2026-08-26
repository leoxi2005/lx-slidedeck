# LX SlideDeck
#
#   macOS   : builds the universal .bundle Resolume loads, plus the unit tests
#   Windows : the shipping build is build/windows/LXSlideDeck.vcxproj (MSVC /MT).
#             This file still works there for the tests and for a quick local build.
#
#   cmake -B out -DCMAKE_BUILD_TYPE=Release && cmake --build out
cmake_minimum_required(VERSION 3.20)
project(LXSlideDeck VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    # Resolume 7.11+ runs natively on Apple Silicon and refuses x86_64-only plugins.
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()
if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "" FORCE)
endif()

set(FFGL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffgl)

# ---------------------------------------------------------------------------
# Everything that has no OpenGL and no FFGL in it. Shared by the plugin and the
# tests, which is the whole point: this is the part that can be tested anywhere.
# ---------------------------------------------------------------------------
add_library(lxsd_core STATIC
    src/DeckLogic.cpp
    src/ImageDecode.cpp
    src/LruPolicy.cpp
    src/Manifest.cpp
    src/Platform.cpp
    src/ScaleMode.cpp
    src/Sha1.cpp
    src/Converter.cpp
    src/XmlLite.cpp
    src/Zip.cpp
    src/Pptx.cpp
    src/PowerPointRendererStub.cpp
    src/miniz.c
)
set_source_files_properties(src/miniz.c PROPERTIES LANGUAGE C)
target_include_directories(lxsd_core PUBLIC src)
if(NOT APPLE AND NOT WIN32)
    target_link_libraries(lxsd_core PUBLIC pthread)
endif()

# ---------------------------------------------------------------------------
# The plugin
# ---------------------------------------------------------------------------
add_library(LXSlideDeck MODULE
    src/LXSlideDeck.cpp
    src/Worker.cpp
    ${FFGL_DIR}/source/lib/FFGLSDK.cpp
)
target_link_libraries(LXSlideDeck PRIVATE lxsd_core)
target_include_directories(LXSlideDeck PRIVATE src ${FFGL_DIR}/source/lib)

if(APPLE)
    target_compile_definitions(LXSlideDeck PRIVATE TARGET_OS_MAC=1)
    find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
    find_library(CARBON_FRAMEWORK Carbon REQUIRED)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    target_link_libraries(LXSlideDeck PRIVATE
        ${OPENGL_FRAMEWORK} ${CARBON_FRAMEWORK} ${APPKIT_FRAMEWORK})
    set_target_properties(LXSlideDeck PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/build/osx/Info.plist
        OUTPUT_NAME "LXSlideDeck")
    # Apple's OpenGL headers are deprecated with no replacement that Resolume uses.
    target_compile_options(LXSlideDeck PRIVATE -Wno-deprecated-declarations)
elseif(WIN32)
    find_package(GLEW QUIET)
    if(GLEW_FOUND)
        target_link_libraries(LXSlideDeck PRIVATE GLEW::GLEW)
    else()
        target_include_directories(LXSlideDeck PRIVATE ${FFGL_DIR}/deps/glew-2.1.0/include)
        target_compile_definitions(LXSlideDeck PRIVATE GLEW_STATIC)
        target_link_directories(LXSlideDeck PRIVATE ${FFGL_DIR}/deps/glew-2.1.0/lib/Release/x64)
        target_link_libraries(LXSlideDeck PRIVATE glew32s)
    endif()
    target_link_libraries(LXSlideDeck PRIVATE opengl32 ole32 oleaut32 uuid shell32)
    set_target_properties(LXSlideDeck PROPERTIES SUFFIX ".dll" PREFIX "")
endif()

# ---------------------------------------------------------------------------
# Unit tests — pure logic only, no GL context and no PowerPoint required
# ---------------------------------------------------------------------------
enable_testing()
add_executable(lxsd_tests tests/test_main.cpp)
add_executable(lxsd-convert tools/convert_main.cpp)
target_link_libraries(lxsd-convert PRIVATE lxsd_core)
target_link_libraries(lxsd_tests PRIVATE lxsd_core)
add_test(NAME lxsd_tests COMMAND lxsd_tests)

# ---------------------------------------------------------------------------
# Convenience: drop the built bundle where Resolume looks for it
# ---------------------------------------------------------------------------
if(APPLE)
    add_custom_target(install-plugin
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        COMMAND ${CMAKE_COMMAND} -E rm -rf
                "$ENV{HOME}/Documents/Resolume Arena/Extra Effects/LXSlideDeck.bundle"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:LXSlideDeck>"
                "$ENV{HOME}/Documents/Resolume Arena/Extra Effects/LXSlideDeck.bundle"
        DEPENDS LXSlideDeck
        COMMENT "Copying LXSlideDeck.bundle into Resolume's Extra Effects")
endif()
