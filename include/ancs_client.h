#pragma once
#include "notification.h"

// Nhận thông báo từ tối đa MAX_PHONES iPhone cùng lúc qua Apple Notification Center Service (BLE).
// Mỗi kết nối có task riêng; loop() chỉ cần gọi ancs::nextEvent().
// Mỗi iPhone tự đánh số uid từ 0, nên thông báo được nhận diện bằng cặp (phone, uid).
namespace ancs {

enum class LinkState : uint8_t {
    Advertising,  // chưa có kết nối nào
    Connected,    // có kết nối nhưng chưa iPhone nào sẵn sàng
    Pairing,      // (giữ cho led.cpp; trạng thái tổng hợp không dùng giá trị này)
    Ready,        // ít nhất một iPhone đã đăng ký nhận thông báo
};

struct PhoneInfo {
    bool known;      // chỉ số này đang gắn với một iPhone đã ghép đôi
    bool connected;  // iPhone này đang có kết nối (kể cả đang tìm dịch vụ)
    bool ready;      // đã đăng ký ANCS
    char label[24];  // tên ngắn (UTF-8), vd. "Cường" hoặc "iPhone 2"
};

struct LinkStatus {
    LinkState state;    // Ready nếu ready > 0; Connected nếu connected > 0; còn lại Advertising
    uint8_t connected;  // số kết nối BLE đang mở
    uint8_t ready;      // số iPhone (khác nhau) đã sẵn sàng
    uint8_t known;      // số iPhone đã ghép đôi (có trong bảng)
    bool advertising;   // connected < MAX_PHONES: iPhone khác còn thấy board
    PhoneInfo phones[MAX_PHONES];
};

enum class EventType : uint8_t {
    Link,            // trạng thái kết nối tổng hợp -> link
    PassKey,         // cần hiện mã PIN -> passKey
    PassKeyDone,     // mọi lần ghép đôi đang chờ mã đã xong/huỷ -> ẩn màn mã PIN
    Added,           // thông báo mới hoặc vừa đổi nội dung -> notif (notif.phone đã điền), isNew
    Removed,         // thông báo bị xoá trên iPhone -> phone, uid
    PhoneForgotten,  // iPhone bị quên (bị thay chỗ / xoá ghép đôi) -> phone (NO_PHONE = tất cả)
};

// Không dùng default member initializer (C++11). Trường nào hợp lệ tuỳ theo type.
struct Event {
    EventType type;
    uint8_t phone;
    bool isNew;  // Added: true = EventID Added (tin mới), false = Modified (cập nhật nội dung)
    uint32_t passKey;
    uint32_t uid;
    LinkStatus link;
    Notification notif;
};

void begin(const char* deviceName);
bool nextEvent(Event& out);  // không chờ; false nếu chưa có gì
void forgetBonds();          // xoá mọi ghép đôi; chỉ gọi từ main task (setup/loop)
// Đồng ý (Nghe máy) / từ chối-xoá trên đúng iPhone. Chạy nền; false nếu iPhone đó đang không kết nối.
bool performAction(uint8_t phone, uint32_t uid, bool positive);

}  // namespace ancs
