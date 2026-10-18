#pragma once
#include <stddef.h>
#include <stdint.h>

// Loa trên board: codec ES8311 (I2C chung với cảm ứng) nhận âm thanh qua I2S, ampli bật bằng chân
// AUDIO_PA. Chỉ có khi khai báo UI_SPEECH; các hàm phát chỉ gọi từ một task (task đọc thông báo).
namespace audio {

constexpr uint8_t MAX_VOLUME = 10;

// Gọi trong setup() sau touch::begin() (Wire đã chạy). Trả về false nếu không thấy codec.
bool begin();
void setVolume(uint8_t level);  // 0..MAX_VOLUME (0 = im lặng); dùng I2C nên gọi từ loop()

void start(uint32_t sampleRate);                // bật ampli, đổi tần số lấy mẫu nếu cần
void write(const int16_t* mono, size_t count);  // phát mẫu mono 16-bit, chờ tới khi vào được DMA
void chime();                                   // tiếng "ting" báo có tin
void tick();                                    // tiếng "tíc" ngắn khi chỉnh âm lượng
void stop();                                    // phát nốt phần đang chờ rồi tắt ampli

}  // namespace audio
