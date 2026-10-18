#pragma once
#include "ancs_client.h"

namespace ui {

void begin(const char* deviceName);

// true nếu đang chạm màn hình (dùng lúc khởi động để xoá ghép đôi).
bool isTouched();

void setLinkState(ancs::LinkState state);
void showPassKey(uint32_t key);
bool addNotification(const Notification& n);  // true nếu là tin mới, false nếu chỉ cập nhật tin cũ
void removeNotification(uint32_t uid);

// Gọi liên tục trong loop(): cảm ứng, đèn nền, đồng hồ, hiệu ứng nháy.
void loop();

}  // namespace ui
