#pragma once
#include "notification.h"

// Nhận thông báo iPhone qua Apple Notification Center Service (BLE).
// Toàn bộ BLE chạy ở task riêng; loop() chỉ cần gọi ancs::nextEvent().
namespace ancs {

enum class LinkState : uint8_t {
    Advertising,  // chờ iPhone kết nối
    Connected,    // đã kết nối, đang bảo mật/tìm dịch vụ ANCS
    Pairing,      // iPhone đang yêu cầu nhập mã PIN
    Ready,        // đã đăng ký nhận thông báo
};

enum class EventType : uint8_t {
    State,    // trạng thái kết nối thay đổi -> state
    PassKey,  // cần hiển thị mã PIN -> passKey
    Added,    // thông báo mới hoặc vừa cập nhật -> notif
    Removed,  // thông báo bị xoá trên iPhone -> uid
};

struct Event {
    EventType type;
    LinkState state;
    uint32_t passKey;
    uint32_t uid;
    Notification notif;
};

void begin(const char* deviceName);

// Lấy sự kiện kế tiếp, không chờ. Trả về false nếu chưa có gì.
bool nextEvent(Event& out);

// Xoá mọi thiết bị đã ghép đôi (dùng khi iPhone đã "Quên thiết bị này").
void forgetBonds();

// Thực hiện hành động của thông báo trên iPhone: positive = đồng ý (Nghe máy),
// negative = từ chối cuộc gọi / xoá thông báo. Chạy nền, không chờ kết quả.
void performAction(uint32_t uid, bool positive);

}  // namespace ancs
