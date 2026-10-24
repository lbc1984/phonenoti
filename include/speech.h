#pragma once
#include <stdint.h>

#include "audio.h"
#include "notification.h"

// Đọc to thông báo: kêu "ting" rồi đọc bằng giọng Google Dịch qua loa trên board (nội dung giọng Việt,
// câu của board như "Incoming call from" giọng Anh).
// Chạy ở task nền (WiFi, tải giọng đọc, phát). Không khai báo UI_SPEECH thì mọi hàm không làm gì.
namespace speech {

enum class Net : uint8_t {
    Off,         // chưa cài WiFi
    Connecting,  // đang nối WiFi đã lưu
    Online,      // có mạng
    Setup,       // đang phát mạng "ESP32 Noti Setup" để cài WiFi
};

void begin();      // gọi trong setup() sau ui::begin()
bool available();  // có loa để phát

void announce(const Notification& n);  // bỏ qua nếu đang tắt tiếng hoặc tin im lặng
void say(const char* text);            // đọc thử một câu (gõ vào Serial Monitor)
void cancel(uint8_t phone, uint32_t uid);  // thông báo đã bị xoá -> thôi đọc
void cancelPhone(uint8_t phone);           // NO_PHONE = mọi thông báo (không đụng câu của board)

// Âm lượng 0..MAX_VOLUME, 0 = tắt tiếng (dừng đọc ngay). Nhớ cả sau khi rút điện; mặc định to nhất.
// Gọi từ loop() (ghi thanh ghi codec qua I2C chung với cảm ứng).
constexpr uint8_t MAX_VOLUME = audio::MAX_VOLUME;
void setVolume(uint8_t level);
uint8_t volume();
bool muted();  // volume() == 0
bool speaking();

Net net();
void openWifiSetup();
void closeWifiSetup();  // "Later": đóng trang cài đặt, lần khởi động sau không tự mở nữa

}  // namespace speech
