#!/usr/bin/env bash
# Builds and packages LXSlideDeck.bundle for another Mac.
#
#   ./scripts/package-macos.sh            → dist/LXSlideDeck-macOS-<version>.zip
#
# The bundle is ad-hoc signed. That is enough for Resolume to load it, but macOS puts a
# quarantine flag on anything that arrives by download or AirDrop, and a quarantined bundle
# is refused silently — hence the note in INSTALL.txt and the ditto below, which is the one
# copy method that preserves the bundle's structure.
set -euo pipefail

cd "$(dirname "$0")/.."
VERSION="${1:-1.0}"
BUILD_DIR="out-release"
DIST_DIR="dist"
STAGE="$DIST_DIR/LXSlideDeck-$VERSION"

echo "==> configuring"
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" >/dev/null

echo "==> building"
cmake --build "$BUILD_DIR" --target LXSlideDeck -j"$(sysctl -n hw.ncpu)" >/dev/null

BUNDLE="$BUILD_DIR/LXSlideDeck.bundle"
BINARY="$BUNDLE/Contents/MacOS/LXSlideDeck"

echo "==> verifying"
ARCHS=$(lipo -archs "$BINARY")
echo "    architectures : $ARCHS"
case "$ARCHS" in
    *arm64*) ;;
    *) echo "    FAIL: no arm64 slice — Resolume runs natively on Apple Silicon"; exit 1;;
esac
case "$ARCHS" in
    *x86_64*) ;;
    *) echo "    FAIL: no x86_64 slice — Intel Macs could not load it"; exit 1;;
esac

if ! nm -gU "$BINARY" | grep -q "_plugMain"; then
    echo "    FAIL: plugMain is not exported"; exit 1
fi
echo "    exports       : plugMain"

# A universal binary's otool output repeats a header line per slice, so keep only the
# indented dependency lines.
NON_SYSTEM=$(otool -L "$BINARY" | grep '^\s' | awk '{print $1}' | sort -u \
             | grep -v '^/usr/lib/' | grep -v '^/System/Library/' || true)
if [ -n "$NON_SYSTEM" ]; then
    echo "    FAIL: links against something that is not on every Mac:"
    echo "$NON_SYSTEM"
    exit 1
fi
echo "    dependencies  : system frameworks only"

echo "==> signing (ad-hoc)"
codesign --force --deep --sign - "$BUNDLE"
codesign --verify --verbose=1 "$BUNDLE" 2>&1 | sed 's/^/    /'

echo "==> packaging"
rm -rf "$STAGE"
mkdir -p "$STAGE"
ditto "$BUNDLE" "$STAGE/LXSlideDeck.bundle"

cat > "$STAGE/INSTALL.txt" <<'TXT'
LX SlideDeck — cài trên macOS
=============================

1. Chép LXSlideDeck.bundle vào:

       ~/Documents/Resolume Arena/Extra Effects/

2. QUAN TRỌNG — gỡ cờ quarantine.
   File tải về / AirDrop bị macOS gắn cờ, Resolume sẽ lặng lẽ không nạp plugin.
   Mở Terminal, chạy:

       xattr -dr com.apple.quarantine ~/Documents/"Resolume Arena"/"Extra Effects"/LXSlideDeck.bundle

3. Thoát hẳn Resolume Arena rồi mở lại (Arena chỉ quét plugin lúc khởi động).

4. Tab Sources → gõ "LX" → kéo LX SlideDeck vào một ô clip.


Yêu cầu trên máy đích
---------------------
* Resolume Arena 7.3.1 trở lên (đã thử trên 7.23.2)
* macOS 10.15 trở lên, Intel hoặc Apple Silicon đều chạy
* LibreOffice — CHỈ cần nếu muốn nạp thẳng file .pptx.
  Tải miễn phí ở libreoffice.org. Không có LibreOffice thì plugin vẫn
  phát được thư mục PNG dựng sẵn (xem dưới).


Mang deck sang máy khác
-----------------------
Cache ảnh nằm ở ~/Library/Caches/LXSlideDeck/<mã băm>/ và mã băm tính từ
ĐƯỜNG DẪN TUYỆT ĐỐI của file .pptx, nên chép cache sang máy khác sẽ không khớp.

Có hai cách:

  a) Chép file .pptx sang, trỏ Deck File vào nó, để plugin convert lại.
     Cần LibreOffice trên máy đó.

  b) Không cần LibreOffice, và đây là cách an toàn nhất cho ngày diễn:
     chép cả thư mục cache (step_0001.png, step_0002.png, …) sang máy đích,
     rồi trỏ Deck File vào step_0001.png trong thư mục đó.
     Plugin hiểu là "phát cả thư mục này".
     Cách này không phụ thuộc PowerPoint lẫn LibreOffice.
TXT

ZIP="$DIST_DIR/LXSlideDeck-macOS-$VERSION.zip"
rm -f "$ZIP"
# --keepParent so it unpacks into a named folder rather than spilling into Downloads.
( cd "$DIST_DIR" && ditto -c -k --sequesterRsrc --keepParent "LXSlideDeck-$VERSION" "$(basename "$ZIP")" )
rm -rf "$STAGE"

echo "==> done"
echo "    $ZIP  ($(du -h "$ZIP" | cut -f1))"
