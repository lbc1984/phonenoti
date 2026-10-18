#include "vlw_font.h"

#include <algorithm>

namespace {

int32_t readBe32(const uint8_t* p) {
    return (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
}

}  // namespace

void VlwFont::begin() {
    if (!glyphs_.empty()) return;
    // Đầu file: 6 số int32 big-endian, số đầu là số ký tự. Sau đó mỗi ký tự 7 số int32,
    // cuối cùng là bitmap của từng ký tự theo đúng thứ tự đó.
    const uint32_t count = readBe32(data_);
    const uint8_t* rec = data_ + 24;
    const uint8_t* bitmap = rec + count * 28;
    glyphs_.reserve(count);
    for (uint32_t i = 0; i < count; i++, rec += 28) {
        Glyph g;
        g.cp = readBe32(rec);
        g.h = readBe32(rec + 4);
        g.w = readBe32(rec + 8);
        g.advance = readBe32(rec + 12);
        g.dy = readBe32(rec + 16);
        g.dx = readBe32(rec + 20);
        g.bitmap = bitmap;
        bitmap += g.w * g.h;
        glyphs_.push_back(g);
        ascent_ = std::max(ascent_, (int)g.dy);
        descent_ = std::max(descent_, g.h - g.dy);
    }
    std::sort(glyphs_.begin(), glyphs_.end(), [](const Glyph& a, const Glyph& b) { return a.cp < b.cp; });
    const Glyph* cap = find('H');
    if (!cap) cap = find('0');
    capHeight_ = cap ? cap->dy : ascent_;
}

const VlwFont::Glyph* VlwFont::find(uint32_t cp) const {
    auto it = std::lower_bound(glyphs_.begin(), glyphs_.end(), cp,
                               [](const Glyph& g, uint32_t c) { return g.cp < c; });
    return (it != glyphs_.end() && it->cp == cp) ? &*it : nullptr;
}

uint32_t VlwFont::nextCodepoint(const char*& p) {
    uint8_t c = *p++;
    if (c < 0x80) return c;
    int extra;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
        extra = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        cp = c & 0x07;
    } else {
        return 0xFFFD;
    }
    while (extra-- > 0) {
        if ((*p & 0xC0) != 0x80) return 0xFFFD;  // chuỗi bị cắt ngang giữa ký tự
        cp = (cp << 6) | (*p++ & 0x3F);
    }
    return cp;
}

int VlwFont::width(const char* utf8) const {
    int w = 0;
    for (const char* p = utf8; *p;) {
        const Glyph* g = find(nextCodepoint(p));
        if (g) w += g->advance;
    }
    return w;
}

int VlwFont::draw(TFT_eSPI& gfx, int x, int baseline, const char* utf8, uint16_t fg, uint16_t bg) const {
    for (const char* p = utf8; *p;) {
        const Glyph* g = find(nextCodepoint(p));
        if (!g) continue;
        const int gx = x + g->dx, gy = baseline - g->dy;
        const uint8_t* a = g->bitmap;
        for (int row = 0; row < g->h; row++) {
            for (int col = 0; col < g->w; col++, a++) {
                if (*a == 0) continue;
                gfx.drawPixel(gx + col, gy + row, *a == 255 ? fg : gfx.alphaBlend(*a, fg, bg));
            }
        }
        x += g->advance;
    }
    return x;
}
