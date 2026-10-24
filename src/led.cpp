#include "led.h"

#include <Arduino.h>
#include <math.h>

namespace led {

#ifdef UI_RGB_LED
namespace {

constexpr uint8_t MAX_LEVEL = 40;  // độ sáng tối đa, đèn WS2812 rất chói
constexpr uint32_t RED = 0xFF0000, GREEN = 0x00FF00, BLUE = 0x0050FF, YELLOW = 0xFFB000;
constexpr uint32_t UPDATE_MS = 20;

ancs::LinkState gLink = ancs::LinkState::Advertising;
bool gPairing = false, gCall = false, gUnread = false, gDim = false;
uint32_t gNotifColor = BLUE;
uint32_t gNotifAt = 0;
bool gNotifFlash = false;
uint32_t gConnectedAt = 0;
bool gJustConnected = false;
uint32_t gLastWrite = 0;
uint32_t gShown = 0xFFFFFFFF;

uint32_t rgb565to888(uint16_t c) {
    const uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

// Màu (chưa tính độ sáng) và cường độ 0..255 theo trạng thái ưu tiên cao nhất
uint32_t effect(uint32_t now, uint8_t& level) {
    level = 255;
    if (gCall) return (now / 160) % 2 ? RED : GREEN;  // cuộc gọi đến: đỏ/xanh nháy nhanh
    if (gPairing) return now % 1000 < 500 ? YELLOW : 0;
    const uint32_t sinceNotif = now - gNotifAt;
    if (gNotifFlash) {
        if (sinceNotif < 3 * 240) return sinceNotif % 240 < 120 ? gNotifColor : 0;
        gNotifFlash = false;
    }
    if (gJustConnected) {
        if (now - gConnectedAt < 1000) return GREEN;
        gJustConnected = false;
    }
    if (gUnread) return sinceNotif % 4000 < 200 ? gNotifColor : 0;  // chớp nhắc mỗi 4 giây
    if (gLink != ancs::LinkState::Ready) {  // chờ iPhone: xanh dương "thở" chu kỳ 3 giây
        level = (uint8_t)(255 * (1 - cosf(2 * PI * (now % 3000) / 3000.0f)) / 2);
        return BLUE;
    }
    return 0;
}

}  // namespace

void begin() { neopixelWrite(UI_RGB_LED, 0, 0, 0); }

void loop() {
    const uint32_t now = millis();
    if (now - gLastWrite < UPDATE_MS) return;
    gLastWrite = now;
    uint8_t level;
    const uint32_t rgb = effect(now, level);
    const uint32_t scale = (uint32_t)level * MAX_LEVEL / (gDim ? 2 : 1);  // 0..255*MAX_LEVEL
    const uint8_t r = ((rgb >> 16) & 0xFF) * scale / (255 * 255);
    const uint8_t g = ((rgb >> 8) & 0xFF) * scale / (255 * 255);
    const uint8_t b = (rgb & 0xFF) * scale / (255 * 255);
    const uint32_t shown = (uint32_t)r << 16 | g << 8 | b;
    if (shown == gShown) return;
    gShown = shown;
    neopixelWrite(UI_RGB_LED, r, g, b);
}

void setLink(ancs::LinkState state) {
    if (state == ancs::LinkState::Ready && gLink != ancs::LinkState::Ready) {
        gJustConnected = true;
        gConnectedAt = millis();
    }
    gLink = state;
}

void phoneJoined() {
    gJustConnected = true;
    gConnectedAt = millis();
}

void setPairing(bool on) { gPairing = on; }
void setCall(bool on) { gCall = on; }

void newNotification(uint16_t color565) {
    gNotifColor = rgb565to888(color565);
    gNotifAt = millis();
    gNotifFlash = true;
}

void setUnread(bool on) { gUnread = on; }
void setDim(bool on) { gDim = on; }

#else
void begin() {}
void loop() {}
void setLink(ancs::LinkState) {}
void phoneJoined() {}
void setPairing(bool) {}
void setCall(bool) {}
void newNotification(uint16_t) {}
void setUnread(bool) {}
void setDim(bool) {}
#endif

}  // namespace led
