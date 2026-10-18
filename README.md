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

1. Màn hình hiện "Đang chờ iPhone...".
2. Trên iPhone: **Cài đặt → Bluetooth**, trong mục *Thiết bị khác* chọn **ESP32 Noti**.
3. Màn hình ESP32 hiện **mã 6 số** → nhập mã đó trên iPhone.
4. iPhone hỏi có cho phép nhận thông báo không → bấm **Cho phép**.
   (Kiểm tra lại: Cài đặt → Bluetooth → (i) cạnh ESP32 Noti → bật **Chia sẻ thông báo hệ thống**.)
5. Trạng thái chuyển thành "Đã kết nối" là xong. Lần sau cắm điện, iPhone tự kết nối lại.

## Sử dụng

Cảm ứng:

| Thao tác | Kết quả |
|---|---|
| Vuốt sang trái / nút `‹` | Xem thông báo cũ hơn |
| Vuốt sang phải / nút `›` | Xem thông báo mới hơn |
| Kéo lên / xuống trên nội dung | Cuộn tin dài (có thanh cuộn ở cạnh phải) |
| Nút **Xoá** | Xoá thông báo này, **xoá luôn trên iPhone** |
| Giữ nút **Xoá** 1,2 giây | Xoá hết danh sách trên màn hình (trên iPhone giữ nguyên) |
| Cuộc gọi đến: **Nghe** / **Từ chối** | Nghe máy hoặc từ chối cuộc gọi trên iPhone |
| Chạm khi màn đang tối | Chỉ bật sáng lại |
| Giữ tay trên màn lúc cắm điện | Xoá các iPhone đã ghép đôi |

Nút **BOOT**: bấm để xem tin cũ hơn (hết thì quay lại tin mới nhất), giữ để xoá hết danh sách.

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
- **Chạm không đúng vị trí**: mở Serial Monitor, mỗi lần chạm sẽ in `[Touch] chạm (x, y)` theo màn
  ngang 320x240 (góc trên trái là 0,0). Nếu bị ngược, sửa phép đổi toạ độ trong `touch::read()`
  ở `src/touch.cpp`.

## Cấu trúc mã nguồn

- `src/ancs_client.cpp` — Bluetooth: quảng bá, ghép đôi, đọc ANCS, ghép các mảnh dữ liệu, lấy giờ từ iPhone.
- `src/ui.cpp` — vẽ giao diện vào sprite toàn màn hình (không nhấp nháy), xuống dòng, lịch sử, nút bấm, đèn nền.
- `src/vlw_font.cpp` — vẽ chữ khử răng cưa từ font VLW; ký tự font không có (emoji) được bỏ qua.
- `src/touch.cpp` — đọc cảm ứng FT6336 (I2C) hoặc XPT2046 (SPI), đổi toạ độ sang màn ngang.
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
