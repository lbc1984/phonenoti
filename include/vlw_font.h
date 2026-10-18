#pragma once
#include <TFT_eSPI.h>
#include <stdint.h>

#include <vector>

// Font khử răng cưa định dạng VLW (tạo bởi tools/make_fonts.py), vẽ được lên màn hình hoặc sprite.
// Ký tự không có trong font (emoji...) được bỏ qua.
class VlwFont {
  public:
    explicit VlwFont(const uint8_t* data) : data_(data) {}

    void begin();  // đọc bảng ký tự, gọi một lần trước khi dùng

    int ascent() const { return ascent_; }  // cao nhất trên baseline, kể cả dấu tiếng Việt
    int descent() const { return descent_; }
    int capHeight() const { return capHeight_; }  // chiều cao chữ hoa, dùng để căn giữa theo chiều dọc
    bool has(uint32_t cp) const { return find(cp) != nullptr; }

    int width(const char* utf8) const;
    // Vẽ chuỗi với baseline tại y; viền chữ được trộn với màu nền bg. Trả về x sau ký tự cuối.
    int draw(TFT_eSPI& gfx, int x, int baseline, const char* utf8, uint16_t fg, uint16_t bg) const;

    // Đọc một ký tự UTF-8 và tiến con trỏ; byte lỗi trả về U+FFFD.
    static uint32_t nextCodepoint(const char*& p);

  private:
    struct Glyph {
        uint32_t cp;
        const uint8_t* bitmap;  // độ phủ 0..255, w*h byte
        uint8_t w, h, advance;
        int8_t dx;   // lệch trái so với con trỏ
        int16_t dy;  // từ baseline lên đỉnh bitmap
    };
    const Glyph* find(uint32_t cp) const;

    const uint8_t* data_;
    std::vector<Glyph> glyphs_;  // sắp theo cp để tìm nhị phân
    int ascent_ = 0, descent_ = 0, capHeight_ = 0;
};
