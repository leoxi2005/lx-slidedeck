# LX SlideDeck

Plugin FFGL cho **Resolume Arena**: chạy thẳng file `.pptx` trong Arena, tách từng bước
animation của mỗi slide thành ảnh tĩnh rồi phát lại bằng crossfade trên GPU.

Chạy trên **macOS** (đã kiểm chứng trong Arena 7.23.2, universal arm64 + x86_64) và
**Windows x64** (đã build và kiểm tra được symbol/phụ thuộc, chưa nạp thử trong Arena).

---

## Plugin làm gì

Trỏ vào một file `.pptx` → plugin đọc thẳng cấu trúc XML bên trong file, biết slide nào có
mấy bước animation, dựng lại từng bước thành một slide tĩnh, nhờ LibreOffice (macOS) hoặc
PowerPoint (Windows) render ra PNG, cache lại, rồi phát bằng crossfade.

Slide có 4 bullet hiện lần lượt → **5 bước** (0, 1, 2, 3, 4 dòng) → 5 ảnh → crossfade giữa
chúng cho ra đúng cảm giác chữ hiện dần, chạy trên GPU.

Cũng phát được **thư mục ảnh PNG** dựng sẵn, không cần phần mềm nào khác — đây là chế độ
an toàn nhất cho ngày diễn.

---

## Bảng thông số trong Arena

| Tên | Kiểu | Ghi chú |
|---|---|---|
| Deck File | File | `.pptx` / `.pptm`, hoặc một file `.png` bất kỳ trong thư mục ảnh |
| Reload | Event | Convert lại; cũng dò lại xem đã cài LibreOffice chưa |
| Step | Integer | Bước hiện tại, map được vào timeline / MIDI |
| Next / Prev | Event | Sang bước sau / về bước trước |
| Fade Time | 0…5 s | Thời gian crossfade |
| Fade Curve | Option | Linear / Smooth / Ease Out |
| Autopilot + Interval | Bool + 0.5…60 s | Tự chạy |
| Loop | Bool | Hết deck quay lại đầu |
| Scale Mode | Option | Native / Fit / Fill / Stretch |
| Export Width | 1280…4096 | Độ phân giải convert |
| Preview | Option | Off / Next Only / Split / Corner — xem trước bước kế tiếp |
| Sync | Option | Off / Group A / B / C — cho hai bản plugin bám nhau |
| Export Deck | Event | Chép ảnh đã render ra thư mục cạnh file `.pptx`, để mang đi máy khác |
| Status | Text | Trạng thái, tiến độ, lỗi, và tên renderer đang dùng |

---

## Build trên macOS

```bash
cmake -B out -DCMAKE_BUILD_TYPE=Release
cmake --build out -j8
cmake --build out --target install-plugin   # chép vào Extra Effects của Resolume
```

Đóng gói để mang đi máy khác:

```bash
./scripts/package-macos.sh          # → dist/LXSlideDeck-macOS-1.0.zip
```

Script tự kiểm tra: có đủ arm64 + x86_64, có xuất `plugMain`, chỉ link framework hệ thống.
Thiếu cái nào là dừng, không đóng gói.

Xem [DEPLOY.md](DEPLOY.md) cho quy trình mang sang máy khác.

## Build cho Windows

Hai đường, cùng một source.

**A. MSVC — bản chính thức theo spec**

Mở `build/windows/LXSlideDeck.sln` bằng Visual Studio 2022, chọn `Release | x64`, Build.
Ra `dist/windows/Release/LXSlideDeck.dll`. Dùng toolset v143, C++17, `/MT` (runtime tĩnh,
máy show không cần cài VC redistributable).

**B. Cross-compile từ macOS bằng MinGW-w64**

```bash
brew install mingw-w64
./scripts/build-windows-mingw.sh      # → dist/LXSlideDeck.dll
```

Đường này tồn tại vì MSVC chỉ chạy trên Windows, mà máy phát triển là Mac. Nó là khác biệt
giữa "code Windows đã viết" và "code Windows link được và xuất đúng symbol host cần".
Script tự kiểm tra sau khi link: có xuất `plugMain` không, và có phụ thuộc DLL nào ngoài
Windows không. Thiếu là dừng.

Kết quả: **2.0 MB, một file duy nhất**, phụ thuộc đúng `KERNEL32 / USER32 / GDI32 /
OPENGL32 / OLE32 / OLEAUT32 / api-ms-win-crt-*` — toàn bộ là DLL có sẵn của Windows. C++
runtime được link tĩnh nên không cần chép kèm gì.

**Chưa kiểm chứng:** chưa có Windows để nạp thử trong Arena, và backend PowerPoint COM chưa
chạy thật lần nào. Xem phần "Còn nợ" ở cuối.

---

## Công cụ dòng lệnh

Chạy converter không cần Resolume — dùng để thử, đo thời gian, và tìm lỗi:

```bash
./out/lxsd-convert --analyze deck.pptx          # in ra từng bước và những gì bị ẩn
./out/lxsd-convert deck.pptx /tmp/out 3840      # convert thật
./out/lxsd-convert --build-step deck.pptx 3 s3.pptx   # xuất 1 bước ra .pptx để mổ
```

Tạo bộ ảnh test và deck có animation để thử:

```bash
python3 tools/make-test-deck.py ~/Desktop/Test 8 1920 1080
python3 tools/make-fixture-deck.py tests/fixtures/anim.pptx   # cần python-pptx
```

---

## Kiểm thử

```bash
./out/lxsd_tests          # 77 phép kiểm tra, không cần GL context hay PowerPoint
glslangValidator shaders/slide.vert shaders/slide.frag
```

Phần logic thuần được tách hẳn khỏi GL và khỏi hệ điều hành để test được ở bất kỳ đâu:
tách bước từ cây timing, bản đồ hiển thị, 4 chế độ scale, LRU, SHA-1, manifest.

---

## Kiến trúc

```
RENDER THREAD (Resolume gọi)          WORKER THREAD (plugin tự tạo)
├─ ProcessOpenGL                      ├─ đọc .pptx, phân tích XML
│  ├─ nạp tối đa 2 texture/khung      ├─ dựng .pptx một-slide cho từng bước
│  ├─ cập nhật crossfade              ├─ gọi LibreOffice / PowerPoint render PNG
│  ├─ vẽ quad mix(A, B, t)            ├─ giải mã PNG → buffer RGBA
│  └─ KHÔNG BAO GIỜ block             └─ đẩy vào hàng đợi (tối đa 4)
```

Hai luật cứng: không có lời gọi hệ thống hay giải mã ảnh nào trong `ProcessOpenGL`, và
không có lời gọi OpenGL nào trên worker thread.

| File | Việc |
|---|---|
| `src/LXSlideDeck.cpp` | Plugin FFGL: tham số, texture, crossfade, preview, sync |
| `src/Worker.cpp` | Thread nền, hàng đợi lệnh và kết quả, cache |
| `src/Pptx.cpp` | Đọc cấu trúc animation trong OOXML, dựng gói `.pptx` cho từng bước |
| `src/XmlLite.cpp` | Parser XML giữ nguyên byte offset, sửa file bằng splice |
| `src/Converter.cpp` | Chọn renderer, dò xem nó có chạy được không, điều phối convert |
| `src/DeckLogic.cpp` | Logic thuần: tách bước, bản đồ hiển thị |
| `src/Zip.cpp` | Đọc/ghi zip (`.pptx` là file zip) |

---

## Vì sao đọc thẳng XML thay vì hỏi PowerPoint qua COM

Spec ban đầu định dùng PowerPoint COM để đọc animation. Đọc thẳng OOXML hơn ở 4 điểm:

1. **Chạy được trên cả Mac lẫn Windows** — COM chỉ có trên Windows.
2. **`Effect.Paragraph` không có tài liệu** (0-based hay 1-based?) — trong XML nó là
   `<p:pRg st="0"/>`, ECMA-376 ghi rõ là 0-based.
3. **Không phải đoán entrance hay emphasis** — XML ghi thẳng `presetClass="entr"|"exit"|"emph"`.
4. **Ẩn chữ bằng alpha=0 luôn được**, kể cả chữ tô gradient — cách cũ qua COM thì không.

Bốn cái bẫy COM mà spec cảnh báo đều biến mất theo, vì ta không còn đọc animation qua COM.

---

## Còn nợ

* **Chưa nạp thử `.dll` trong Resolume trên Windows.** Đã kiểm tra được symbol và phụ
  thuộc, nhưng chưa có máy Windows để chạy.
* **Backend PowerPoint COM chưa chạy thật lần nào.** Code compile sạch cho Windows nhưng
  chưa có PowerPoint để gọi. Nếu nó hỏng, vẫn còn đường lui: cài LibreOffice trên máy
  Windows, plugin tự dùng nó.
* **Ô Status còn phần chẩn đoán** dạng `[b8 scale=2 vp=1920x1080 img=3840x2160]`. Hữu ích
  lúc dò lỗi từ xa, nên tạm để lại; gỡ trong `UpdateDiagnostics()` khi thấy không cần nữa.

---

## Phụ thuộc

Nhúng thẳng trong source, không phải cài gì:

* [FFGL SDK](https://github.com/resolume/ffgl) — `third_party/ffgl`, giấy phép BSD
* `stb_image.h` — public domain
* `miniz` — public domain

Cần trên máy chạy: **LibreOffice** (macOS) hoặc **PowerPoint** (Windows), và chỉ khi muốn
nạp thẳng `.pptx`. Chế độ thư mục ảnh không cần gì cả.
