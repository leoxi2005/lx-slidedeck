# LX SlideDeck — mang sang máy khác

Chọn một trong hai đường. Đường B ít thứ hỏng hơn, dùng cho ngày diễn.

---

## Đường A — mang cả file .pptx (máy đích tự convert)

Dùng khi: máy đích có LibreOffice (macOS) hoặc PowerPoint (Windows), và anh còn muốn
sửa deck trên máy đó.

### Trên máy nguồn

1. `./scripts/package-macos.sh` → `dist/LXSlideDeck-macOS-1.0.zip`
2. Chép sang máy đích: file zip đó + file `.pptx`

### Trên máy đích

3. Giải nén, chép `LXSlideDeck.bundle` vào `~/Documents/Resolume Arena/Extra Effects/`
4. Gỡ cờ quarantine — **bước hay quên nhất, quên là plugin không hiện ra và không báo lỗi gì**:

       xattr -dr com.apple.quarantine ~/Documents/"Resolume Arena"/"Extra Effects"/LXSlideDeck.bundle

5. Cài LibreOffice nếu chưa có (libreoffice.org, miễn phí)
6. Thoát hẳn Resolume Arena rồi mở lại
7. Sources → gõ `LX` → kéo **LX SlideDeck** vào một ô clip
8. **Đọc ô Status trước khi làm gì khác.** Phải thấy:

       Idle — renderer: LibreOffice 26.x

   Nếu thấy `LibreOffice not installed` hoặc `would not start` thì dừng lại xử lý,
   đừng trỏ deck vào.
9. `Deck File` → chọn file `.pptx` → chờ convert (thanh tiến độ chạy giữa màn hình)
10. Status hiện `Ready — N steps` là xong

---

## Đường B — mang thư mục ảnh (máy đích không cần gì cả)

Dùng cho ngày diễn. Máy đích **không cần PowerPoint, không cần LibreOffice**, không phải
chờ convert, và không còn gì để hỏng.

### Trên máy nguồn

1. Nạp deck vào plugin, chờ `Ready — N steps`
2. Kiểm tra bằng mắt vài bước quan trọng
3. Bấm **Export Deck** → Status báo `Exported N steps to <tên deck>_LXSD`
4. Thư mục `<tên deck>_LXSD` nằm ngay cạnh file `.pptx`
5. `./scripts/package-macos.sh`
6. Chép sang máy đích: file zip + thư mục `_LXSD`

### Trên máy đích

7. Chép bundle + gỡ quarantine (bước 3–4 của đường A)
8. Mở lại Arena, kéo plugin vào clip
9. `Deck File` → chọn `step_0001.png` **bên trong thư mục `_LXSD`**
10. Status hiện `Ready — N steps` ngay lập tức, không convert

> Có thể tự xuất ảnh bằng PowerPoint (File → Export → PNG → Save Every Slide) thay cho
> bước Export Deck. Khác biệt: PowerPoint xuất **một ảnh mỗi slide**, còn Export Deck xuất
> **một ảnh mỗi bước animation**. Deck có bullet hiện từng dòng thì phải dùng Export Deck,
> không thì mất hiệu ứng.

---

## Kiểm tra trên máy đích trước khi diễn

- [ ] Status ghi `Ready — N steps`, và N đúng bằng số bước mong đợi
- [ ] Next / Prev crossfade mượt, không nháy đen
- [ ] Gõ số vào ô Step nhảy đúng bước
- [ ] Scale Mode đúng với tỉ lệ màn hình thật của sân khấu
- [ ] Đã gán MIDI / phím tắt cho Next và Prev, bấm thử bằng chính clicker sẽ dùng
- [ ] Chạy thử 10 phút, xem RAM trong Activity Monitor có phình không
- [ ] Có bản dự phòng: thư mục `_LXSD` nằm ở layer dưới, sẵn sàng kéo lên nếu layer chính có sự cố

---

## Những thứ KHÔNG mang theo được

**Cache** nằm ở `~/Library/Caches/LXSlideDeck/<mã băm>/`, mã băm tính từ **đường dẫn tuyệt
đối** của file `.pptx` cộng thời điểm sửa file. Chép cache sang máy khác sẽ không bao giờ
khớp — dùng **Export Deck** thay vì chép cache.

**Đường dẫn trong composition**: Resolume lưu đường dẫn tuyệt đối của Deck File vào file
`.avc`. Copy project sang máy khác mà đường dẫn đổi thì phải trỏ lại Deck File một lần.
Cách tránh: để deck và composition trong cùng một thư mục, và đặt thư mục đó ở vị trí
giống nhau trên cả hai máy.

---

## Máy show Windows

1. Build bằng một trong hai cách trong [README](README.md#build-cho-windows)
2. Chép `LXSlideDeck.dll` vào:

       %USERPROFILE%\Documents\Resolume Arena\Extra Effects\

   Windows **không** có cơ chế quarantine như macOS, nên không cần lệnh gỡ cờ nào.
3. Thoát hẳn Resolume Arena rồi mở lại
4. Sources → gõ `LX` → kéo vào một ô clip
5. Đọc ô Status. Trên Windows có PowerPoint phải thấy:

       Idle — renderer: PowerPoint

   Nếu thấy `LibreOffice not installed` thì PowerPoint chưa đăng ký COM trên máy đó —
   mở PowerPoint lên một lần rồi bấm **Reload** trong plugin để dò lại.

Cả hai đường A và B ở trên đều dùng được y hệt trên Windows. Đường B (mang thư mục ảnh)
không cần PowerPoint lẫn LibreOffice, và là cấu hình tôi khuyên dùng cho ngày diễn.
