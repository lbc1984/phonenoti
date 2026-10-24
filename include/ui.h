#pragma once
#include "ancs_client.h"

namespace ui {

void begin(const char* deviceName);

// true nếu đang chạm màn hình (dùng lúc khởi động để xoá ghép đôi).
bool isTouched();

void setLinkStatus(const ancs::LinkStatus& st);
void showPassKey(uint32_t key);
void hidePassKey();
// isNew: EventID Added (tin mới) / Modified (đổi nội dung). Trả về true nếu là tin mới, false nếu chỉ cập nhật tin cũ.
bool addNotification(const Notification& n, bool isNew);
void removeNotification(uint8_t phone, uint32_t uid);
void forgetPhone(uint8_t phone);  // NO_PHONE = tất cả

// Gọi liên tục trong loop(): cảm ứng, đèn nền, đồng hồ, hiệu ứng nháy.
void loop();

}  // namespace ui
