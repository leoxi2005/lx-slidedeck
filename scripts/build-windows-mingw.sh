#!/usr/bin/env bash
# Builds LXSlideDeck.dll for Windows x64 using the MinGW-w64 cross compiler.
#
#   brew install mingw-w64
#   ./scripts/build-windows-mingw.sh
#
# Why this exists: the shipping build is meant to be MSVC (see build/windows/), but MSVC
# only runs on Windows. This path makes it possible to build, link and verify the Windows
# plugin from a Mac, which is the difference between "the Windows code is written" and
# "the Windows code links and exports what the host needs".
#
# The result links the C++ runtime statically, so the .dll depends on nothing but Windows
# itself — no MSVC redistributable, no libstdc++, one file to copy.
set -euo pipefail

cd "$(dirname "$0")/.."

CXX=x86_64-w64-mingw32-g++
CC=x86_64-w64-mingw32-gcc
STRIP=x86_64-w64-mingw32-strip
OBJDUMP=x86_64-w64-mingw32-objdump

if ! command -v "$CXX" >/dev/null; then
    echo "MinGW-w64 not found. Install it with:  brew install mingw-w64"
    exit 1
fi

FFGL=third_party/ffgl
GLEW_VERSION=2.1.0
GLEW_SRC="third_party/glew-$GLEW_VERSION"
BUILD=out-windows
DIST=dist

# GLEW ships only headers in the FFGL SDK; the .c is needed to link without MSVC's .lib.
if [ ! -f "$GLEW_SRC/src/glew.c" ]; then
    echo "==> fetching GLEW $GLEW_VERSION source (needed once)"
    mkdir -p third_party
    curl -sL -o /tmp/glew.zip \
        "https://github.com/nigels-com/glew/releases/download/glew-$GLEW_VERSION/glew-$GLEW_VERSION.zip"
    unzip -oq /tmp/glew.zip -d third_party
    rm -f /tmp/glew.zip
fi

mkdir -p "$BUILD" "$DIST"

INCLUDES=( -Isrc -I"$FFGL/source/lib" -I"$GLEW_SRC/include" )
DEFINES=( -DGLEW_STATIC -DWIN32_LEAN_AND_MEAN )

SOURCES=(
    src/LXSlideDeck.cpp
    src/Worker.cpp
    src/Converter.cpp
    src/ComDispatch.cpp
    src/PowerPointRenderer.cpp
    src/PowerPointRendererStub.cpp
    src/Pptx.cpp
    src/XmlLite.cpp
    src/Zip.cpp
    src/DeckLogic.cpp
    src/ImageDecode.cpp
    src/LruPolicy.cpp
    src/Manifest.cpp
    src/Platform.cpp
    src/ScaleMode.cpp
    src/Sha1.cpp
    "$FFGL/source/lib/FFGLSDK.cpp"
)

echo "==> compiling"
OBJECTS=()
for source in "${SOURCES[@]}"; do
    object="$BUILD/$(basename "${source%.*}").o"
    "$CXX" -std=c++17 -O2 -c "$source" "${INCLUDES[@]}" "${DEFINES[@]}" -o "$object"
    OBJECTS+=( "$object" )
done
"$CC" -O2 -c src/miniz.c -o "$BUILD/miniz.o"
OBJECTS+=( "$BUILD/miniz.o" )
"$CC" -O2 -c "$GLEW_SRC/src/glew.c" -I"$GLEW_SRC/include" -DGLEW_STATIC -DGLEW_NO_GLU -o "$BUILD/glew.o"
OBJECTS+=( "$BUILD/glew.o" )

echo "==> linking"
"$CXX" -shared -o "$BUILD/LXSlideDeck.dll" "${OBJECTS[@]}" \
    -static -static-libgcc -static-libstdc++ \
    -lopengl32 -lole32 -loleaut32 -luuid -lshell32 -lgdi32 -luser32
"$STRIP" "$BUILD/LXSlideDeck.dll"

echo "==> verifying"
EXPORTS=$("$OBJDUMP" -p "$BUILD/LXSlideDeck.dll" | grep -oE '\b(plugMain|SetLogCallback)\b' | sort -u | tr '\n' ' ')
echo "    exports      : $EXPORTS"
case "$EXPORTS" in
    *plugMain*) ;;
    *) echo "    FAIL: plugMain is not exported — Resolume would ignore the file"; exit 1;;
esac

# Anything outside this list would have to be shipped alongside the .dll.
FOREIGN=$("$OBJDUMP" -p "$BUILD/LXSlideDeck.dll" | grep "DLL Name" | awk '{print tolower($3)}' \
          | grep -vE '^(kernel32|user32|gdi32|opengl32|ole32|oleaut32|shell32|advapi32|msvcrt|api-ms-win-)' || true)
if [ -n "$FOREIGN" ]; then
    echo "    FAIL: depends on something that is not part of Windows:"
    echo "$FOREIGN"
    exit 1
fi
echo "    dependencies : Windows system DLLs only"
echo "    size         : $(du -h "$BUILD/LXSlideDeck.dll" | cut -f1)"

cp "$BUILD/LXSlideDeck.dll" "$DIST/LXSlideDeck.dll"
echo "==> done"
echo "    $DIST/LXSlideDeck.dll"
echo
echo "    Copy it to, on the Windows machine:"
echo "      %USERPROFILE%\\Documents\\Resolume Arena\\Extra Effects\\"
echo
echo "    NOTE: built with MinGW-w64, not MSVC, and never yet loaded by Resolume on"
echo "    Windows. Test it before relying on it for a show."
