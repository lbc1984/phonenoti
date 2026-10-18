# ESP32 Noti — hiện thông báo iPhone lên màn hình 2.8"

ESP32 nhận thông báo từ iPhone qua Bluetooth LE (dịch vụ ANCS có sẵn trong iOS) và hiển thị
tên app, tiêu đề, nội dung tiếng Việt có dấu. Không cần cài app trên iPhone, không cần WiFi.

Hỗ trợ 2 loại mạch (chọn trong `platformio.ini`):

| Môi trường | Mạch | Điều khiển |
|---|---|---|
| `s3` (mặc định) | LCDWIKI ES3C28P / ES3N28P: ESP32-S3 N16R8 + màn IPS 2.8" ILI9341V | cảm ứng điện dung FT6336 + nút **BOOT** |
| `cyd` | ESP32-2432S028R "Cheap Yellow Display" | cảm ứng điện trở XPT2046 + nút **BOOT** |

Sơ đồ chân màn của bản `s3`: CS=10, DC=46, MOSI=11, SCK=12, MISO=13, đèn nền=45 (mức cao = sáng),
RESET chung với chip. Mạch khác thì sửa các dòng `-DTFT_...` trong `[env:s3]`.

## Nạp chương trình

1. Cài VS Code + extension **PlatformIO IDE**, mở thư mục này.
2. Cắm board qua cổng USB.
3. Bấm **Upload** (mũi tên → ở thanh dưới) hoặc chạy:
   ```
   pio run -e s3 -t upload
   pio device monitor
   ```
   Nếu nạp báo lỗi "Failed to connect", giữ nút **BOOT**, bấm **RST**, thả BOOT rồi nạp lại.

## Ghép đôi với iPhone (chỉ làm 1 lần)

Giao diện trên màn hình bằng tiếng Anh; tài liệu này ghi kèm chữ hiện trên màn hình để dễ tìm.

1. Màn hình hiện "Waiting for iPhone…" và "Connect your iPhone".
2. Trên iPhone: **Cài đặt → Bluetooth**, trong mục *Thiết bị khác* chọn **ESP32 Noti**.
3. Màn hình ESP32 hiện **mã 6 số** → nhập mã đó trên iPhone.
4. iPhone hỏi có cho phép nhận thông báo không → bấm **Cho phép**.
   (Kiểm tra lại: Cài đặt → Bluetooth → (i) cạnh ESP32 Noti → bật **Chia sẻ thông báo hệ thống** /
   *Share System Notifications*.)
5. Trạng thái chuyển thành "Connected" là xong. Lần sau cắm điện, iPhone tự kết nối lại.

## Sử dụng

Cảm ứng:

| Thao tác | Kết quả |
|---|---|
| Vuốt sang trái / nút `‹` | Xem thông báo cũ hơn |
| Vuốt sang phải / nút `›` | Xem thông báo mới hơn |
| Kéo lên / xuống trên nội dung | Cuộn tin dài (có thanh cuộn ở cạnh phải) |
| Nút **Dismiss** | Xoá thông báo này, **xoá luôn trên iPhone** |
| Giữ nút **Dismiss** 1,2 giây | Xoá hết danh sách trên màn hình (trên iPhone giữ nguyên) |
| Cuộc gọi đến: **Answer** / **Decline** | Nghe máy hoặc từ chối cuộc gọi trên iPhone |
| Chạm khi màn đang tối | Chỉ bật sáng lại |
| Giữ tay trên màn lúc cắm điện | Xoá các iPhone đã ghép đôi |

Nút **BOOT**: bấm để xem tin cũ hơn (hết thì quay lại tin mới nhất), giữ để xoá hết danh sách.

Đèn RGB trên board (bản `s3`):

| Đèn | Ý nghĩa |
|---|---|
| Xanh dương "thở" chậm | Đang chờ iPhone / mất kết nối |
| Xanh lá sáng 1 giây | Vừa kết nối xong |
| Vàng nhấp nháy | Đang ghép đôi (nhập mã PIN) |
| Nháy 3 lần theo màu loại tin | Có thông báo mới (màu giống nhãn tên app) |
| Chớp nhẹ mỗi 4 giây | Còn tin chưa xem — chạm màn hoặc bấm BOOT là tắt |
| Đỏ ↔ xanh lá nháy nhanh | Cuộc gọi đến |
| Tắt | Đã kết nối, không có gì mới |

### Đọc to thông báo (bản `s3`)

Có tin mới thì board kêu "ting" rồi đọc bằng giọng Google Dịch, ví dụ *"Zalo. Ngọc Hà: tối nay đi ăn
không"*. Nội dung thông báo đọc bằng **giọng tiếng Việt**; câu do board tự nói thì bằng **giọng tiếng Anh**: cuộc gọi
đọc *"Incoming call from"* / *"Missed call from"* rồi tới tên người gọi (giọng Việt). Tin dài chỉ đọc khoảng 300 ký tự
đầu, đường link được đọc thành "link".

- Cần **cắm loa** (4–8 Ω) vào cổng loa trên board và **có WiFi** để lấy giọng đọc. Không có mạng thì chỉ kêu "ting".
- **Cài WiFi**: khi chưa có WiFi, màn hình hiện *"Set up WiFi to read aloud"*. Trên iPhone vào **Cài đặt › Wi-Fi**,
  chọn mạng **ESP32 Noti Setup**, trang cài đặt sẽ tự mở. Bấm *Configure WiFi*, chọn WiFi nhà bạn, nhập mật khẩu rồi
  bấm *Save*. Board nói *"WiFi connected"* là xong. Bấm **Later** nếu chưa muốn cài (lần sau không tự hiện nữa).
- Hai biểu tượng trên thanh trên cùng:

  | Biểu tượng | Ý nghĩa |
  |---|---|
  | WiFi xanh lá / vàng / xanh dương / xám | Có mạng / đang nối / đang mở trang cài đặt / chưa cài |
  | Loa có 1–3 vòng sóng / xanh dương / xám có dấu x đỏ | Âm lượng nhỏ–to / đang đọc / tắt tiếng |

- **Chạm loa** mở bảng âm thanh ở thanh dưới đáy (tự ẩn sau 5 giây):
  - **−** / **+**: chỉnh âm lượng 10 mức, mỗi lần bấm kêu "tíc" ở mức mới. Về 0 là tắt tiếng (dừng đọc ngay).
    Mặc định to nhất; mức đã chọn được nhớ cả khi rút điện.
  - **WiFi**: mở màn cài WiFi để đổi mạng khác (giữ biểu tượng loa 1,2 giây cũng được).
- Thông báo bị xoá (trên iPhone, nút Dismiss, Answer/Decline cuộc gọi) trong lúc đang đọc thì board ngừng đọc.
  Tin iOS đánh dấu im lặng thì không đọc.
- Thử loa: mở Serial Monitor (115200), gõ một câu rồi Enter.

> Nội dung thông báo được gửi tới máy chủ Google (qua HTTPS, có kiểm tra chứng chỉ của Google) để chuyển thành
> giọng nói. Đây là dịch vụ miễn phí không chính thức nên Google có thể đổi hoặc chặn bất cứ lúc nào; khi đó board
> chỉ kêu "ting". Không muốn gửi nội dung đi thì để âm lượng về 0 (tắt tiếng).

### Ghi chú khác

- Lưu 20 thông báo gần nhất. Xoá thông báo trên iPhone thì nó cũng biến mất khỏi màn hình.
- Các thông báo đã có sẵn trên iPhone lúc kết nối không được hiện, chỉ hiện thông báo mới.
- Sau 30 giây không có gì mới, màn hình tự giảm sáng (trừ khi đang có cuộc gọi đến).
- Màu nhãn tên app: xanh dương = tin nhắn/mạng xã hội, đỏ = cuộc gọi, vàng = email,
  cam = lịch, tím = tin tức/giải trí, xanh lá = sức khỏe/tài chính, xám = khác.
- Nội dung thông báo phụ thuộc cài đặt iPhone: nếu bật "Hiển thị bản xem trước: Khi được mở khóa",
  lúc iPhone khoá sẽ chỉ thấy tên app chứ không thấy nội dung.

## Xử lý sự cố

- **Bản `s3`: đỏ và xanh dương bị đổi chỗ** (biểu tượng Bluetooth không phải màu xanh dương):
  bỏ dấu `;` ở dòng `-DTFT_RGB_ORDER=1` trong `[env:s3]` rồi nạp lại.
  **Màu bị âm bản** (nền trắng): xoá dòng `-DTFT_INVERSION_ON=1`.
- **Bản `cyd`: màn hình trắng, sai màu hoặc bị lật**: bản CYD có 2 cổng USB dùng chip ST7789.
  Trong `[env:cyd]`, xoá dòng `-DILI9341_2_DRIVER=1` và bỏ dấu `;` ở 2 dòng
  `ST7789_DRIVER` / `TFT_INVERSION_ON`, rồi nạp lại.
- **Không thấy "ESP32 Noti" trong Bluetooth iPhone**: thử tắt/bật Bluetooth trên iPhone, hoặc mở
  Serial Monitor (115200) xem dòng `[BLE] Đang quảng bá...`.
- **Đã ghép đôi nhưng không kết nối được / iPhone báo lỗi**: trên iPhone chọn (i) → *Quên thiết bị này*,
  rồi ghép đôi lại. ESP32 tự xoá khoá cũ khi thấy iPhone không còn nhận ra nó.
- **Kết nối rồi nhưng không có thông báo**: kiểm tra mục *Chia sẻ thông báo hệ thống* (bước 4 ở trên)
  và chắc chắn app đó được phép gửi thông báo trên iPhone.
- **Không nghe đọc**: kiểm tra loa đã cắm, âm lượng khác 0 và biểu tượng WiFi màu xanh lá.
  Trong Serial Monitor: `[Audio] ES8311: có` là thấy mạch âm thanh; mỗi lần đọc có dòng `[TTS] … -> HTTP 200`.
  HTTP khác 200 nghĩa là Google từ chối hoặc mất mạng.
- **Chạm không đúng vị trí**: mở Serial Monitor, mỗi lần chạm sẽ in `[Touch] chạm (x, y)` theo màn
  ngang 320x240 (góc trên trái là 0,0). Nếu bị ngược, sửa phép đổi toạ độ trong `touch::read()`
  ở `src/touch.cpp`.

## Cấu trúc mã nguồn

- `src/ancs_client.cpp` — Bluetooth: quảng bá, ghép đôi, đọc ANCS, ghép các mảnh dữ liệu, lấy giờ từ iPhone.
- `src/ui.cpp` — vẽ giao diện vào sprite toàn màn hình (không nhấp nháy), xuống dòng, lịch sử, nút bấm, đèn nền.
- `src/vlw_font.cpp` — vẽ chữ khử răng cưa từ font VLW; ký tự font không có (emoji) được bỏ qua.
- `src/touch.cpp` — đọc cảm ứng FT6336 (I2C) hoặc XPT2046 (SPI), đổi toạ độ sang màn ngang.
- `src/speech.cpp` — đọc to thông báo: WiFi (trang cài đặt WiFiManager), ghép câu, tải giọng Google, giải mã MP3.
- `src/audio.cpp` — loa: cấu hình codec ES8311, I2S, ampli, tiếng "ting".
- `src/led.cpp` — đèn RGB báo trạng thái.
- `include/fonts/` — font Be Vietnam Pro đã chuyển sẵn (14/17/21px và chữ số 44px).
- `include/app_names.h` — bảng đổi bundle ID → tên app; thêm app mới vào đây.

## Đổi font / cỡ chữ

Font gốc nằm trong `tools/fonts/` (Be Vietnam Pro, giấy phép SIL OFL 1.1). Muốn đổi cỡ chữ hoặc dùng
font TTF khác, sửa bảng `FONTS` trong `tools/make_fonts.py` rồi chạy:

```
pip install freetype-py
python tools/make_fonts.py tools/fonts include/fonts
```
- `src/main.cpp` — nối hai phần trên.
