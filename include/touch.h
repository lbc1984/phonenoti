#pragma once

// Đọc cảm ứng, chọn loại trong platformio.ini:
//   UI_CTP_*   — cảm ứng điện dung FT6336 qua I2C (LCDWIKI ES3C28P)
//   UI_TOUCH_* — cảm ứng điện trở XPT2046 qua SPI (CYD)
// Toạ độ trả về theo màn hình ngang 320x240 (tft.setRotation(1)).
namespace touch {

bool begin();               // false nếu không có / không tìm thấy cảm ứng
bool read(int& x, int& y);  // true khi đang có ngón tay chạm

}  // namespace touch
