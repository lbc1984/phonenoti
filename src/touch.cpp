#include "touch.h"

#include <Arduino.h>

#if defined(UI_CTP_SDA)
#include <Wire.h>
#elif defined(UI_TOUCH_CS)
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#endif

namespace touch {
namespace {

constexpr int W = 320, H = 240;
bool gReady = false;

#if defined(UI_CTP_SDA)
// FT6336 (FocalTech) ở địa chỉ I2C 0x38. Toạ độ gốc theo màn dọc 240x320, cùng hướng với
// tft.setRotation(0) (theo BSP của board: không đảo trục, không lật).
constexpr uint8_t FT_ADDR = 0x38;
constexpr uint8_t REG_TD_STATUS = 0x02;  // số điểm chạm, tiếp theo là XH, XL, YH, YL của điểm 1
constexpr uint8_t REG_CHIP_ID = 0xA3;
constexpr uint8_t EVENT_LIFT_UP = 1;

bool readRegs(uint8_t reg, uint8_t* buf, uint8_t n) {
    Wire.beginTransmission(FT_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(FT_ADDR, n) != n) return false;
    for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
    return true;
}
#elif defined(UI_TOUCH_CS)
#if CONFIG_IDF_TARGET_ESP32
SPIClass touchSpi(VSPI);
#else
SPIClass touchSpi(FSPI);  // ESP32-S3: màn hình đã dùng SPI3 (USE_HSPI_PORT)
#endif
XPT2046_Touchscreen xpt(UI_TOUCH_CS, UI_TOUCH_IRQ);
#endif

}  // namespace

bool begin() {
#if defined(UI_CTP_SDA)
    pinMode(UI_CTP_INT, INPUT_PULLUP);
    pinMode(UI_CTP_RST, OUTPUT);
    digitalWrite(UI_CTP_RST, LOW);
    delay(10);
    digitalWrite(UI_CTP_RST, HIGH);
    delay(300);  // FT6336 cần khoảng 300ms sau khi reset
    Wire.begin(UI_CTP_SDA, UI_CTP_SCL, 400000);
    uint8_t id = 0;
    gReady = readRegs(REG_CHIP_ID, &id, 1);
    Serial.printf("[Touch] FT6336: %s (chip ID 0x%02X)\n", gReady ? "có" : "không tìm thấy", id);
#elif defined(UI_TOUCH_CS)
    touchSpi.begin(UI_TOUCH_CLK, UI_TOUCH_MISO, UI_TOUCH_MOSI, UI_TOUCH_CS);
    xpt.begin(touchSpi);
    xpt.setRotation(1);
    gReady = true;
#endif
    return gReady;
}

bool read(int& x, int& y) {
    if (!gReady) return false;
#if defined(UI_CTP_SDA)
    uint8_t d[5];
    if (!readRegs(REG_TD_STATUS, d, sizeof d)) return false;
    const uint8_t points = d[0] & 0x0F;
    if (points == 0 || points > 2 || (d[1] >> 6) == EVENT_LIFT_UP) return false;
    const int tx = ((d[1] & 0x0F) << 8) | d[2];  // 0..239 theo màn dọc
    const int ty = ((d[3] & 0x0F) << 8) | d[4];  // 0..319
    // Đổi sang màn ngang setRotation(1): trục dọc thành trục ngang, trục ngang bị lật
    x = constrain(ty, 0, W - 1);
    y = constrain(239 - tx, 0, H - 1);
    return true;
#elif defined(UI_TOUCH_CS)
    if (!(xpt.tirqTouched() && xpt.touched())) return false;
    TS_Point p = xpt.getPoint();
    x = constrain(map(p.x, 200, 3700, 0, W), 0, W - 1);
    y = constrain(map(p.y, 240, 3800, 0, H), 0, H - 1);
    return true;
#else
    return false;
#endif
}

}  // namespace touch
