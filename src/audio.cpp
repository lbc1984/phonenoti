#include "audio.h"

#ifdef UI_SPEECH
#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <math.h>

#include <algorithm>

#ifndef AUDIO_PA_ON
#define AUDIO_PA_ON LOW  // mức logic bật ampli trên chân AUDIO_PA
#endif

namespace audio {
namespace {

constexpr uint8_t ES8311_ADDR = 0x18;
constexpr i2s_port_t PORT = I2S_NUM_0;
constexpr uint32_t DEFAULT_RATE = 24000;  // giọng Google trả về MP3 24 kHz
// Thanh ghi âm lượng DAC (0x32) cho từng mức: 0xBF = 0 dB, mỗi đơn vị 0,5 dB.
// Mức 1..10 = -24, -20, -16, -12, -9, -6, -3, 0, +3, +6 dB.
constexpr uint8_t VOLUME_REG[MAX_VOLUME + 1] = {0x00, 0x8F, 0x97, 0x9F, 0xA7, 0xAD, 0xB3, 0xB9, 0xBF, 0xC5, 0xCB};
constexpr int DMA_BUFS = 8, DMA_FRAMES = 256;
constexpr size_t BLOCK = 128;  // số mẫu mỗi lần đẩy xuống DMA

bool gReady = false;
bool gPaOn = false;
uint32_t gRate = DEFAULT_RATE;

bool writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

uint8_t readReg(uint8_t reg) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(ES8311_ADDR, (uint8_t)1) != 1) return 0;
    return Wire.read();
}

// Theo driver es8311 của Espressif (esp-bsp): ES8311 làm slave, xung nhịp lấy từ chân MCLK = 256 x fs.
// Với tỉ lệ này hệ số chia giống nhau ở mọi tần số 16..48 kHz, nên đổi tần số chỉ cần đổi I2S.
bool initCodec() {
    if (!writeReg(0x00, 0x1F)) return false;  // reset
    delay(20);
    writeReg(0x00, 0x00);
    writeReg(0x00, 0x80);  // bật nguồn, chế độ slave
    writeReg(0x01, 0x3F);  // MCLK lấy từ chân MCLK, bật mọi xung nhịp
    // Hệ số chia cho MCLK = 256 x fs: pre_div 1, pre_multi 1x, adc/dac_div 1, OSR 0x10, bclk_div 4
    writeReg(0x02, readReg(0x02) & 0x07);
    writeReg(0x03, 0x10);
    writeReg(0x04, 0x10);
    writeReg(0x05, 0x00);
    writeReg(0x06, (readReg(0x06) & 0xC0) | (4 - 1));  // bit 5 = 0: không đảo SCLK
    writeReg(0x07, readReg(0x07) & 0xC0);
    writeReg(0x08, 0xFF);
    // Khung I2S chuẩn, 16 bit cho cả chiều vào và ra
    writeReg(0x09, 0x0C);
    writeReg(0x0A, 0x0C);
    // Bật mạch analog và DAC
    writeReg(0x0D, 0x01);
    writeReg(0x0E, 0x02);
    writeReg(0x12, 0x00);
    writeReg(0x13, 0x10);
    writeReg(0x1C, 0x6A);
    writeReg(0x37, 0x08);
    writeReg(0x32, 0x00);                   // im lặng tới khi setVolume()
    writeReg(0x31, readReg(0x31) & ~0x60);  // bỏ tắt tiếng
    return true;
}

bool initI2s() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = DEFAULT_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.dma_buf_count = DMA_BUFS;
    cfg.dma_buf_len = DMA_FRAMES;
    cfg.tx_desc_auto_clear = true;  // hết dữ liệu thì phát im lặng chứ không lặp lại đoạn cũ
    cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if (i2s_driver_install(PORT, &cfg, 0, nullptr) != ESP_OK) return false;

    i2s_pin_config_t pins = {};
    pins.mck_io_num = AUDIO_MCLK;
    pins.bck_io_num = AUDIO_BCLK;
    pins.ws_io_num = AUDIO_LRCK;
    pins.data_out_num = AUDIO_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;
    return i2s_set_pin(PORT, &pins) == ESP_OK;
}

void setAmp(bool on) {
    digitalWrite(AUDIO_PA, on == (AUDIO_PA_ON == HIGH) ? HIGH : LOW);
    gPaOn = on;
}

// Một nốt chuông: lên trong 4 ms (khỏi lụp bụp) rồi tắt dần
float bell(float t, float freq) {
    const float attack = t < 0.004f ? t / 0.004f : 1.0f;
    return attack * expf(-9.0f * t) * sinf(2 * PI * freq * t);
}

}  // namespace

bool begin() {
    pinMode(AUDIO_PA, OUTPUT);
    setAmp(false);
    // Codec cần MCLK chạy trước khi cấu hình; Wire đã được touch::begin() khởi động
    if (!initI2s()) {
        Serial.println("[Audio] Không khởi tạo được I2S");
        return false;
    }
    gReady = initCodec();
    Serial.printf("[Audio] ES8311: %s\n", gReady ? "có" : "không tìm thấy");
    return gReady;
}

void setVolume(uint8_t level) {
    if (gReady) writeReg(0x32, VOLUME_REG[std::min(level, MAX_VOLUME)]);
}

void start(uint32_t sampleRate) {
    if (!gReady) return;
    if (sampleRate != gRate && sampleRate >= 8000) {
        i2s_set_clk(PORT, sampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
        gRate = sampleRate;
    }
    if (!gPaOn) {
        i2s_zero_dma_buffer(PORT);
        setAmp(true);
        delay(30);  // chờ ampli ổn định
    }
}

void write(const int16_t* mono, size_t count) {
    if (!gReady) return;
    int16_t stereo[2 * BLOCK];
    while (count > 0) {
        const size_t n = std::min(count, BLOCK);
        for (size_t i = 0; i < n; i++) stereo[2 * i] = stereo[2 * i + 1] = mono[i];
        size_t written;
        i2s_write(PORT, stereo, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        mono += n;
        count -= n;
    }
}

// Hai nốt Rê 6 rồi Sol 6 chồng lên nhau, kiểu chuông báo tin
void chime() {
    if (!gReady) return;
    start(gRate);
    const uint32_t total = gRate * 600 / 1000, second = gRate * 110 / 1000;
    int16_t buf[BLOCK];
    for (uint32_t i = 0; i < total;) {
        const size_t n = std::min<uint32_t>(BLOCK, total - i);
        for (size_t k = 0; k < n; k++, i++) {
            float s = bell((float)i / gRate, 1174.7f);
            if (i >= second) s += bell((float)(i - second) / gRate, 1568.0f);
            buf[k] = (int16_t)(s * 7000);
        }
        write(buf, n);
    }
}

void tick() {
    if (!gReady) return;
    start(gRate);
    const uint32_t total = gRate * 400 / 1000;
    int16_t buf[BLOCK];
    for (uint32_t i = 0; i < total;) {
        const size_t n = std::min<uint32_t>(BLOCK, total - i);
        for (size_t k = 0; k < n; k++, i++) buf[k] = (int16_t)(bell((float)i / gRate, 1568.0f) * 7000);
        write(buf, n);
    }
}

void stop() {
    if (!gReady || !gPaOn) return;
    // Đẩy im lặng đủ lấp đầy DMA để phần âm thanh còn lại phát hết rồi mới tắt ampli
    static const int16_t silence[BLOCK] = {};
    for (int i = 0; i < DMA_BUFS * DMA_FRAMES / (int)BLOCK + 2; i++) write(silence, BLOCK);
    setAmp(false);
}

}  // namespace audio
#endif  // UI_SPEECH
