#include "ui.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

#include "app_names.h"
#include "fonts/font_body.h"
#include "fonts/font_digits.h"
#include "fonts/font_label.h"
#include "fonts/font_small.h"
#include "fonts/font_title.h"
#include "led.h"
#include "speech.h"
#include "touch.h"
#include "vlw_font.h"

namespace ui {
namespace {

constexpr int BOOT_PIN = 0;  // nút BOOT trên board
constexpr int BL_CHANNEL = 0;

// ---- Bố cục màn hình ngang 320x240 ----
constexpr int W = 320, H = 240;
constexpr int HDR_H = 26;   // dải trạng thái, đường kẻ ở y = HDR_H
constexpr int BAR_Y = 196;  // thanh nút dưới đáy, đường kẻ ở y = BAR_Y - 1
constexpr int BTN_Y = BAR_Y + 6, BTN_H = 32;
constexpr int PAD = 12;
constexpr int CHIP_Y = 36, CHIP_H = 22;  // nhãn tên app
constexpr int BODY_LINE_H = 23;
constexpr int TEXT_BOTTOM = BAR_Y - 9;  // baseline thấp nhất của nội dung
constexpr int SOUND_BTN_W = 30;         // vùng chạm nút loa trên thanh trạng thái
constexpr uint32_t SOUND_PANEL_MS = 5000;  // bảng âm lượng tự ẩn sau ngần này không chạm
// Nhãn iPhone gửi tin (từ 2 iPhone trở lên), bên phải nhãn tên app: chấm màu + tên ngắn
constexpr int TAG_DOT_D = 8, TAG_GAP = 5, TAG_TEXT_MAX = 84;

// ---- Màu (RGB565), tông tối ----
constexpr uint16_t C_BG = 0x0862;     // #0B0F14
constexpr uint16_t C_HDR = 0x10E4;    // #151C26
constexpr uint16_t C_SEP = 0x2188;    // #263243
constexpr uint16_t C_TEXT = 0xFFFF;
constexpr uint16_t C_BODY = 0xD6DC;   // #D5DAE1
constexpr uint16_t C_DIM = 0x8CB4;    // #8B95A5
constexpr uint16_t C_MUTED = 0x5B2E;  // #5B6576
constexpr uint16_t C_GREEN = 0x2E6E;
constexpr uint16_t C_YELLOW = 0xF623;
constexpr uint16_t C_BLUE = 0x2C7F;
constexpr uint16_t C_RED = 0xE8E4;
constexpr uint16_t C_ORANGE = 0xFC63;
constexpr uint16_t C_PURPLE = 0xA2FF;
// Màu riêng của từng iPhone, khác các màu loại thông báo (đỏ, xanh dương, vàng, cam, tím, xanh lá, xám)
constexpr uint16_t kPhoneColor[MAX_PHONES] = {0x269D /*#22D3EE xanh ngọc*/, 0xF396 /*#F472B6 hồng*/,
                                              0xA726 /*#A3E635 xanh nõn chuối*/};

// ---- Thời gian, cử chỉ ----
constexpr uint32_t DIM_AFTER_MS = 30000;
constexpr uint8_t BRIGHT_FULL = 255;
constexpr uint8_t BRIGHT_DIM = 25;
constexpr uint32_t LONG_PRESS_MS = 1200;
constexpr uint32_t TOUCH_POLL_MS = 10;
constexpr uint32_t RELEASE_MS = 60;  // mất tín hiệu chạm quá ngần này mới tính là nhả tay
constexpr int MOVE_SLOP = 10;        // dịch quá ngần này pixel là kéo/vuốt chứ không phải chạm
constexpr int SWIPE_MIN = 45;
constexpr int FLASH_TOGGLES = 4;
constexpr uint32_t FLASH_STEP_MS = 200;

constexpr int HISTORY_MAX = 20;

enum ButtonId : uint8_t {
    BTN_NONE,
    BTN_OLDER,
    BTN_NEWER,
    BTN_DISMISS,
    BTN_ANSWER,
    BTN_DECLINE,
    BTN_SOUND,       // loa trên thanh trạng thái: chạm mở bảng âm lượng, giữ để cài WiFi
    BTN_VOL_DOWN,    // bảng âm lượng: −
    BTN_VOL_UP,      // bảng âm lượng: +
    BTN_WIFI,        // bảng âm lượng: mở màn cài WiFi
    BTN_WIFI_LATER,  // "Later" ở màn cài WiFi
};

struct Button {
    int16_t x, y, w, h;
    ButtonId id;
};

struct TouchState {
    bool down = false;
    bool ignore = false;  // lần chạm này chỉ để bật sáng màn
    bool moved = false;
    bool longFired = false;
    uint32_t downAt = 0, lastSeen = 0;
    int startX = 0, startY = 0, x = 0, y = 0;
    int scrollAnchorY = 0;
};

TFT_eSPI tft;
TFT_eSprite canvas(&tft);  // vẽ cả màn hình vào đây rồi đẩy lên một lần -> không nhấp nháy
bool gUseCanvas = false;

VlwFont fSmall(font_small);    // Medium 14px: thanh trạng thái, giờ, gợi ý
VlwFont fLabel(font_label);    // SemiBold 14px: tên app, đồng hồ, chữ trên nút
VlwFont fBody(font_body);      // Regular 17px: nội dung
VlwFont fTitle(font_title);    // Bold 21px: tiêu đề
VlwFont fDigits(font_digits);  // Bold 44px: mã PIN

const char* gDeviceName = "";
ancs::LinkStatus gLink{};  // trạng thái kết nối tổng hợp của mọi iPhone
bool gShowPassKey = false;
uint32_t gPassKey = 0;

Notification gHistory[HISTORY_MAX];  // [0] là mới nhất
int gCount = 0;
int gView = 0;
int gScroll = 0, gMaxScroll = 0;  // cuộn nội dung tin đang xem, tính theo dòng

uint32_t gLastActivity = 0;
bool gDimmed = false;
int gFlashLeft = 0;
uint32_t gFlashNext = 0;
int gLastMinute = -1;

bool gHasTouch = false;
std::vector<Button> gButtons;  // nút đang hiện, dựng lại mỗi lần vẽ
ButtonId gPressed = BTN_NONE;  // nút đang bị nhấn, vẽ đậm hơn
TouchState gTouch;
uint32_t gTouchPollAt = 0;

bool gBootDown = false, gBootLong = false;
uint32_t gBootAt = 0;

uint8_t gSpeechDrawn = 0;  // trạng thái loa/WiFi lúc vẽ thanh trên lần gần nhất
bool gSoundPanel = false;  // đang mở bảng âm lượng ở thanh dưới đáy
uint32_t gSoundPanelAt = 0;

// ---------- Tiện ích ----------

void setBacklight(uint8_t level) {
#if TFT_BACKLIGHT_ON == LOW
    level = 255 - level;  // đèn nền bật ở mức thấp
#endif
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(TFT_BL, level);
#else
    ledcWrite(BL_CHANNEL, level);
#endif
}

void wake() {
    gLastActivity = millis();
    if (gDimmed) {
        gDimmed = false;
        setBacklight(BRIGHT_FULL);
        led::setDim(false);
    }
}

uint16_t categoryColor(uint8_t cat) {
    switch (cat) {
        case CAT_INCOMING_CALL:
        case CAT_MISSED_CALL:
        case CAT_VOICEMAIL: return C_RED;
        case CAT_SOCIAL: return C_BLUE;
        case CAT_EMAIL: return C_YELLOW;
        case CAT_SCHEDULE: return C_ORANGE;
        case CAT_NEWS:
        case CAT_ENTERTAINMENT: return C_PURPLE;
        case CAT_HEALTH:
        case CAT_FINANCE: return C_GREEN;
        default: return C_DIM;
    }
}

// Cuộc gọi đến có thể nghe/từ chối ngay trên màn hình
bool isCall(const Notification& n) {
    return n.category == CAT_INCOMING_CALL && (n.flags & NOTIF_FLAG_POSITIVE_ACTION) &&
           (n.flags & NOTIF_FLAG_NEGATIVE_ACTION);
}

bool clockValid(struct tm& now) {
    time_t t = time(nullptr);
    localtime_r(&t, &now);
    return now.tm_year + 1900 >= 2024;
}

int utf8CharLen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

TFT_eSPI& gfx() { return gUseCanvas ? static_cast<TFT_eSPI&>(canvas) : tft; }

void present() {
    if (gUseCanvas) canvas.pushSprite(0, 0);
}

int textWidth(const VlwFont& f, const std::string& s) { return f.width(s.c_str()); }

int drawText(const VlwFont& f, int x, int baseline, const std::string& s, uint16_t fg, uint16_t bg) {
    return f.draw(gfx(), x, baseline, s.c_str(), fg, bg);
}

void drawTextRight(const VlwFont& f, int right, int baseline, const std::string& s, uint16_t fg, uint16_t bg) {
    drawText(f, right - textWidth(f, s), baseline, s, fg, bg);
}

void drawTextCenter(const VlwFont& f, int baseline, const std::string& s, uint16_t fg, uint16_t bg) {
    drawText(f, (W - textWidth(f, s)) / 2, baseline, s, fg, bg);
}

// Baseline để chữ hoa nằm giữa dải [top, top + h)
int midBaseline(const VlwFont& f, int top, int h) { return top + (h + f.capHeight()) / 2; }

// Cắt chuỗi cho vừa maxW, thêm "…" nếu bị cắt.
std::string ellipsize(const VlwFont& f, std::string s, int maxW) {
    if (textWidth(f, s) <= maxW) return s;
    while (!s.empty()) {
        size_t cut = s.size() - 1;
        while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
        s.erase(cut);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        if (textWidth(f, s + "…") <= maxW) break;
    }
    return s + "…";
}

// Xuống dòng theo từ; từ quá dài thì bẻ theo ký tự. Quá maxLines thì dòng cuối có "…".
std::vector<std::string> wrapText(const VlwFont& f, const char* text, int maxW, int maxLines) {
    std::vector<std::string> lines;
    std::string line;
    bool truncated = false;

    auto pushLine = [&](const std::string& l) {
        if ((int)lines.size() >= maxLines) {
            truncated = true;
            return;
        }
        lines.push_back(l);
    };

    auto addWord = [&](std::string word) {
        if (word.empty()) return;
        std::string candidate = line.empty() ? word : line + " " + word;
        if (textWidth(f, candidate) <= maxW) {
            line = candidate;
            return;
        }
        if (!line.empty()) pushLine(line);
        line.clear();
        while (textWidth(f, word) > maxW) {
            size_t n = 0;
            while (n < word.size()) {
                size_t next = n + utf8CharLen((unsigned char)word[n]);
                if (textWidth(f, word.substr(0, next)) > maxW) break;
                n = next;
            }
            if (n == 0) n = utf8CharLen((unsigned char)word[0]);
            pushLine(word.substr(0, n));
            word.erase(0, n);
        }
        line = word;
    };

    std::string word;
    for (const char* p = text; *p && !truncated; ++p) {
        if (*p == ' ' || *p == '\t') {
            addWord(word);
            word.clear();
        } else if (*p == '\n' || *p == '\r') {
            addWord(word);
            word.clear();
            if (!line.empty()) pushLine(line);
            line.clear();
        } else {
            word += *p;
        }
    }
    if (!truncated) {
        addWord(word);
        if (!line.empty()) pushLine(line);
    }
    if (truncated && !lines.empty()) {
        std::string& last = lines.back();
        last = textWidth(f, last + "…") <= maxW ? last + "…" : ellipsize(f, last, maxW);
    }
    return lines;
}

// Bỏ ký tự font không vẽ được (emoji, ký tự điều hướng chữ...) và gộp khoảng trắng thừa.
void sanitize(char* s, size_t cap) {
    std::string out;
    bool pendingSpace = false;
    for (const char* p = s; *p;) {
        const char* start = p;
        uint32_t cp = VlwFont::nextCodepoint(p);
        if (cp == '\n') {
            if (!out.empty() && out.back() != '\n') out += '\n';
            pendingSpace = false;
        } else if (cp == ' ' || cp == '\t' || cp == 0xA0) {
            pendingSpace = !out.empty() && out.back() != '\n';
        } else if (fBody.has(cp)) {
            if (pendingSpace) out += ' ';
            pendingSpace = false;
            out.append(start, p - start);
        }
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    strlcpy(s, out.c_str(), cap);
}

// "20261018T104100" -> "10:41", hoặc "18 Oct 10:41" nếu không phải hôm nay.
std::string formatDate(const char* d) {
    if (strlen(d) < 13) return "";
    static const char* const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[20];
    struct tm now;
    bool sameDay = clockValid(now);
    if (sameDay) {
        char today[9];
        strftime(today, sizeof today, "%Y%m%d", &now);
        sameDay = strncmp(today, d, 8) == 0;
    }
    if (sameDay || !clockValid(now)) {
        snprintf(buf, sizeof buf, "%.2s:%.2s", d + 9, d + 11);
    } else {
        const int month = (d[4] - '0') * 10 + (d[5] - '0');
        const int day = (d[6] - '0') * 10 + (d[7] - '0');
        snprintf(buf, sizeof buf, "%d %s %.2s:%.2s", day, months[constrain(month, 1, 12) - 1], d + 9, d + 11);
    }
    return buf;
}

// Trạng thái loa/WiFi gói trong một byte để biết khi nào phải vẽ lại thanh trên
uint8_t speechState() {
    return (uint8_t)speech::net() | (uint8_t)speech::speaking() << 2 | (uint8_t)speech::available() << 3 |
           speech::volume() << 4;
}

// Tên ngắn của iPhone p, vd. "Cường"; chưa biết tên thì "iPhone 2"
std::string phoneLabel(int p) {
    if (p >= 0 && p < MAX_PHONES && gLink.phones[p].label[0]) return gLink.phones[p].label;
    char buf[12];
    snprintf(buf, sizeof buf, "iPhone %d", p >= 0 && p < MAX_PHONES ? p + 1 : 1);
    return buf;
}

// iPhone đầu tiên đã sẵn sàng, -1 nếu không có
int readyPhone() {
    for (int i = 0; i < MAX_PHONES; i++) {
        if (gLink.phones[i].ready) return i;
    }
    return -1;
}

// Chữ và màu chấm trạng thái trên thanh trên cùng
const char* linkLabel(uint16_t& dot, char* buf, size_t cap) {
    if (gShowPassKey) {
        dot = C_YELLOW;
        return "Pairing";
    }
    if (gLink.ready == 0) {
        if (gLink.connected > 0) {
            dot = C_YELLOW;
            return "Connecting…";
        }
        dot = C_MUTED;
        return "Waiting for iPhone…";
    }
    dot = C_GREEN;
    if (gLink.ready >= 2) snprintf(buf, cap, "%u iPhones", (unsigned)gLink.ready);
    else if (gLink.known >= 2) strlcpy(buf, phoneLabel(readyPhone()).c_str(), cap);  // 1 trong nhiều máy: tên máy đó
    else strlcpy(buf, "Connected", cap);
    if (gLink.connected > gLink.ready) {  // còn máy khác đang nối: "2 iPhones +1"
        char more[8];
        snprintf(more, sizeof more, " +%u", (unsigned)(gLink.connected - gLink.ready));
        strlcat(buf, more, cap);
    }
    return buf;
}

// ---------- Vẽ ----------

void addButton(int x, int y, int w, int h, ButtonId id) {
    const Button b = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, id};
    for (Button& old : gButtons) {  // thanh trên được vẽ lại mỗi phút mà không xoá danh sách nút
        if (old.id == id) {
            old = b;
            return;
        }
    }
    gButtons.push_back(b);
}

// Loa: số vòng sóng theo âm lượng, xanh dương = đang đọc, xám có dấu x đỏ = tắt tiếng
void drawSpeakerIcon(int cx, int cy, uint16_t bg) {
    TFT_eSPI& g = gfx();
    const uint8_t vol = speech::volume();
    const uint16_t c = vol == 0 ? C_MUTED : speech::speaking() ? C_BLUE : C_TEXT;
    const int x = cx - 11;  // mép trái thân loa
    g.fillRect(x, cy - 3, 4, 7, c);
    g.fillTriangle(x + 3, cy - 3, x + 8, cy - 7, x + 8, cy + 7, c);
    g.fillTriangle(x + 3, cy - 3, x + 8, cy + 7, x + 3, cy + 3, c);
    if (vol == 0) {
        g.drawWideLine(x + 12, cy - 3, x + 18, cy + 3, 2, C_RED, bg);
        g.drawWideLine(x + 12, cy + 3, x + 18, cy - 3, 2, C_RED, bg);
        return;
    }
    // Vòng sóng âm bên phải (góc 0 ở 6 giờ, quay theo chiều kim đồng hồ)
    const int waves = vol >= 8 ? 3 : vol >= 4 ? 2 : 1;
    for (int i = 0; i < waves; i++) g.drawSmoothArc(x + 8, cy, 6 + 4 * i, 4 + 4 * i, 225, 315, c, bg, true);
}

// WiFi: xanh lá = có mạng, vàng = đang nối, xanh dương = đang mở trang cài đặt, xám = chưa cài
void drawWifiIcon(int cx, int bottom) {
    TFT_eSPI& g = gfx();
    uint16_t c;
    switch (speech::net()) {
        case speech::Net::Online: c = C_GREEN; break;
        case speech::Net::Connecting: c = C_YELLOW; break;
        case speech::Net::Setup: c = C_BLUE; break;
        default: c = C_MUTED; break;
    }
    g.fillSmoothCircle(cx, bottom - 1, 2, c, C_HDR);
    g.drawSmoothArc(cx, bottom, 7, 5, 135, 225, c, C_HDR, true);
    g.drawSmoothArc(cx, bottom, 12, 10, 135, 225, c, C_HDR, true);
}

void drawHeader() {
    TFT_eSPI& g = gfx();
    g.fillRect(0, 0, W, HDR_H, C_HDR);
    g.drawFastHLine(0, HDR_H, W, C_SEP);

    uint16_t dot;
    char buf[48];
    const char* label = linkLabel(dot, buf, sizeof buf);
    const int base = midBaseline(fSmall, 0, HDR_H);
    g.fillSmoothCircle(14, HDR_H / 2, 4, dot, C_HDR);

    int right = W - PAD;
    struct tm now;
    if (clockValid(now)) {
        char clk[6];
        strftime(clk, sizeof clk, "%H:%M", &now);
        right -= textWidth(fLabel, clk);
        drawText(fLabel, right, base, clk, C_TEXT, C_HDR);
        right -= 10;
        gLastMinute = now.tm_min;
    }
    if (gCount > 0 && !gShowPassKey) {  // vị trí trong lịch sử, dạng viên thuốc
        char pos[12];
        snprintf(pos, sizeof pos, "%d/%d", gView + 1, gCount);
        const int pw = textWidth(fSmall, pos) + 14;
        g.fillSmoothRoundRect(right - pw, 4, pw, 18, 9, C_SEP, C_HDR);
        drawText(fSmall, right - pw + 7, base, pos, C_DIM, C_SEP);
        right -= pw + 6;
    }
    gSpeechDrawn = speechState();
    if (speech::available()) {
        right -= SOUND_BTN_W;
        uint16_t bg = C_HDR;
        if (gPressed == BTN_SOUND) {  // đang giữ nút loa
            bg = C_SEP;
            g.fillSmoothRoundRect(right + 1, 2, SOUND_BTN_W - 2, HDR_H - 4, 6, bg, C_HDR);
        }
        drawSpeakerIcon(right + SOUND_BTN_W / 2, HDR_H / 2, bg);
        // Thanh trên chỉ cao 26 px, ngón tay hay chạm lệch xuống dưới: nhận chạm tới y ~ 50
        addButton(right, 0, SOUND_BTN_W, HDR_H + 18, BTN_SOUND);
        drawWifiIcon(right - 12, HDR_H / 2 + 7);
        right -= 24;
    }
    // Chữ trạng thái vẽ sau cùng, rút gọn "…" nếu không đủ chỗ trước các biểu tượng bên phải
    drawText(fSmall, 24, base, ellipsize(fSmall, label, right - 6 - 24), C_TEXT, C_HDR);
}

// Nút chữ bo tròn trên thanh dưới đáy; đang nhấn thì dùng màu pressed.
void drawButton(int x, int w, ButtonId id, const char* label, uint16_t bg, uint16_t pressed, uint16_t fg) {
    const uint16_t c = gPressed == id ? pressed : bg;
    gfx().fillSmoothRoundRect(x, BTN_Y, w, BTN_H, 10, c, C_HDR);
    drawText(fLabel, x + (w - textWidth(fLabel, label)) / 2, midBaseline(fLabel, BTN_Y, BTN_H), label, fg, c);
    addButton(x, BTN_Y, w, BTN_H, id);
}

// Nút mũi tên ‹ ›; hết tin để chuyển thì vẽ mờ và không nhận chạm.
void drawArrowButton(int x, int w, ButtonId id, bool left, bool enabled) {
    TFT_eSPI& g = gfx();
    const uint16_t c = gPressed == id ? C_MUTED : C_SEP;
    g.fillSmoothRoundRect(x, BTN_Y, w, BTN_H, 10, c, C_HDR);
    const int cx = x + w / 2, cy = BTN_Y + BTN_H / 2;
    const int d = left ? 3 : -3;
    const uint16_t ic = enabled ? C_TEXT : C_MUTED;
    // Không truyền màu nền: hai nét gặp nhau ở mũi sẽ trộn với điểm ảnh đã vẽ
    g.drawWideLine(cx + d, cy - 7, cx - d, cy, 3, ic);
    g.drawWideLine(cx - d, cy, cx + d, cy + 7, 3, ic);
    if (enabled) addButton(x, BTN_Y, w, BTN_H, id);
}

void drawBarBackground() {
    gfx().drawFastHLine(0, BAR_Y - 1, W, C_SEP);
    gfx().fillRect(0, BAR_Y, W, H - BAR_Y, C_HDR);
}

void drawActionBar(const Notification& n) {
    TFT_eSPI& g = gfx();
    drawBarBackground();
    if (isCall(n)) {
        const int bw = (W - 2 * PAD - 12) / 2;
        drawButton(PAD, bw, BTN_DECLINE, "Decline", C_RED, g.alphaBlend(150, C_RED, C_BG), C_TEXT);
        drawButton(W - PAD - bw, bw, BTN_ANSWER, "Answer", C_GREEN, g.alphaBlend(150, C_GREEN, C_BG), C_TEXT);
        return;
    }
    const int aw = 60, gap = 12;
    drawArrowButton(PAD, aw, BTN_OLDER, true, gView < gCount - 1);
    drawButton(PAD + aw + gap, W - 2 * (PAD + aw + gap), BTN_DISMISS, "Dismiss", g.alphaBlend(70, C_RED, C_HDR),
               g.alphaBlend(150, C_RED, C_HDR), C_TEXT);
    drawArrowButton(W - PAD - aw, aw, BTN_NEWER, false, gView > 0);
}

// Nút − / + của bảng âm lượng; tới giới hạn thì vẽ mờ
void drawVolumeButton(int x, int w, ButtonId id, bool plus, bool enabled) {
    TFT_eSPI& g = gfx();
    g.fillSmoothRoundRect(x, BTN_Y, w, BTN_H, 10, gPressed == id ? C_MUTED : C_SEP, C_HDR);
    const int cx = x + w / 2, cy = BTN_Y + BTN_H / 2;
    const uint16_t ic = enabled ? C_TEXT : C_MUTED;
    g.fillRect(cx - 7, cy - 1, 15, 3, ic);
    if (plus) g.fillRect(cx - 1, cy - 7, 3, 15, ic);
    addButton(x, BTN_Y, w, BTN_H, id);
}

// Bảng âm lượng vẽ đè lên thanh dưới đáy: [−] vạch âm lượng [+] [WiFi]
void drawSoundPanel() {
    TFT_eSPI& g = gfx();
    gButtons.erase(std::remove_if(gButtons.begin(), gButtons.end(), [](const Button& b) { return b.y >= BAR_Y; }),
                   gButtons.end());  // nút của thanh dưới đáy đang bị che
    drawBarBackground();
    const int vol = speech::volume();
    const int bw = 52, wifiW = 64;
    const int plusX = W - PAD - wifiW - 10 - bw;
    drawVolumeButton(PAD, bw, BTN_VOL_DOWN, false, vol > 0);
    drawVolumeButton(plusX, bw, BTN_VOL_UP, true, vol < speech::MAX_VOLUME);
    drawButton(W - PAD - wifiW, wifiW, BTN_WIFI, "WiFi", C_SEP, C_MUTED, C_TEXT);
    // Vạch cao dần giữa hai nút, sáng tới mức hiện tại
    const int x0 = PAD + bw + 8;
    const int step = (plusX - 8 - x0 + 3) / speech::MAX_VOLUME;
    const int bottom = BTN_Y + BTN_H - 4;
    for (int i = 0; i < speech::MAX_VOLUME; i++) {
        const int h = 6 + 2 * i;
        g.fillSmoothRoundRect(x0 + i * step, bottom - h, step - 3, h, 2, i < vol ? C_BLUE : C_SEP, C_HDR);
    }
}

// Board không có cảm ứng: chỉ hiện gợi ý dùng nút BOOT
void drawHintBar() {
    drawBarBackground();
    const int base = midBaseline(fSmall, BAR_Y, H - BAR_Y);
    if (gCount > 1) drawText(fSmall, PAD, base, "BOOT: older", C_DIM, C_HDR);
    drawTextRight(fSmall, W - PAD, base, "Hold BOOT: clear all", C_MUTED, C_HDR);
}

void drawNotification(const Notification& n) {
    TFT_eSPI& g = gfx();
    g.fillRect(0, HDR_H + 1, W, BAR_Y - HDR_H - 2, C_BG);

    // Nhãn tên app: nền là màu loại thông báo pha tối, chữ cùng màu
    const uint16_t accent = categoryColor(n.category);
    const uint16_t chipBg = g.alphaBlend(56, accent, C_BG);
    const std::string when = formatDate(n.date);
    const int whenW = textWidth(fSmall, when);
    // Từ 2 iPhone trở lên: chấm màu + tên ngắn của iPhone gửi tin, trước giờ (1 iPhone thì không hiện gì)
    const bool showTag = gLink.known >= 2 && n.phone < MAX_PHONES;
    std::string tag;
    int tagW = 0;
    if (showTag) {
        tag = ellipsize(fSmall, phoneLabel(n.phone), TAG_TEXT_MAX);
        tagW = TAG_DOT_D + TAG_GAP + textWidth(fSmall, tag);
    }
    const std::string app =
        ellipsize(fLabel, appDisplayName(n.appId), W - 2 * PAD - whenW - 30 - (showTag ? tagW + 10 : 0));
    const int chipW = textWidth(fLabel, app) + 20;
    const int chipBase = midBaseline(fLabel, CHIP_Y, CHIP_H);
    g.fillSmoothRoundRect(PAD, CHIP_Y, chipW, CHIP_H, CHIP_H / 2, chipBg, C_BG);
    drawText(fLabel, PAD + 10, chipBase, app, accent, chipBg);
    drawTextRight(fSmall, W - PAD, chipBase, when, C_DIM, C_BG);
    if (showTag) {  // iPhone đang mất kết nối thì chấm và chữ mờ đi
        const int x = W - PAD - whenW - (whenW > 0 ? 10 : 0) - tagW;
        const bool online = gLink.phones[n.phone].connected;
        const uint16_t pc = kPhoneColor[n.phone];
        const uint16_t dotColor = online ? pc : g.alphaBlend(110, pc, C_BG);
        g.fillSmoothCircle(x + TAG_DOT_D / 2, CHIP_Y + CHIP_H / 2, TAG_DOT_D / 2, dotColor, C_BG);
        drawText(fSmall, x + TAG_DOT_D + TAG_GAP, chipBase, tag, online ? C_DIM : C_MUTED, C_BG);
    }

    int y = CHIP_Y + CHIP_H + 24;
    if (n.title[0]) {
        y += 2;
        drawText(fTitle, PAD, y, ellipsize(fTitle, n.title, W - 2 * PAD), C_TEXT, C_BG);
        y += 25;
    }
    if (n.subtitle[0]) {
        drawText(fSmall, PAD, y - 3, ellipsize(fSmall, n.subtitle, W - 2 * PAD), C_DIM, C_BG);
        y += 20;
    }

    // Nội dung: hiện được `visible` dòng, dài hơn thì kéo lên/xuống để cuộn
    const int visible = std::max(1, (TEXT_BOTTOM - y) / BODY_LINE_H + 1);
    const std::vector<std::string> lines = wrapText(fBody, n.message, W - 2 * PAD - 8, 200);
    gMaxScroll = std::max(0, (int)lines.size() - visible);
    gScroll = constrain(gScroll, 0, gMaxScroll);
    for (int i = 0; i < visible && gScroll + i < (int)lines.size(); i++) {
        drawText(fBody, PAD, y + i * BODY_LINE_H, lines[gScroll + i], C_BODY, C_BG);
    }
    if (gMaxScroll > 0) {  // thanh cuộn mảnh bên phải
        const int top = y - fBody.capHeight() - 2;
        const int height = (visible - 1) * BODY_LINE_H + fBody.capHeight() + 6;
        const int thumb = std::max(14, height * visible / (int)lines.size());
        g.fillSmoothRoundRect(W - 6, top, 3, height, 1, C_HDR, C_BG);
        g.fillSmoothRoundRect(W - 6, top + (height - thumb) * gScroll / gMaxScroll, 3, thumb, 1, C_DIM, C_HDR);
    }
}

void drawBluetoothIcon(int cx, int cy, uint16_t color) {
    TFT_eSPI& g = gfx();
    g.fillSmoothCircle(cx, cy, 28, color, C_BG);
    // Không truyền màu nền: các nét chồng lên nhau sẽ trộn với điểm ảnh đã vẽ
    const float w = 3;
    g.drawWideLine(cx - 2, cy - 18, cx - 2, cy + 18, w, C_TEXT);
    g.drawWideLine(cx - 2, cy - 18, cx + 9, cy - 8, w, C_TEXT);
    g.drawWideLine(cx + 9, cy - 8, cx - 11, cy + 10, w, C_TEXT);
    g.drawWideLine(cx - 2, cy + 18, cx + 9, cy + 8, w, C_TEXT);
    g.drawWideLine(cx + 9, cy + 8, cx - 11, cy - 10, w, C_TEXT);
}

void drawWaitingScreen() {
    gfx().fillRect(0, HDR_H + 1, W, H - HDR_H - 1, C_BG);
    switch (gLink.state) {
        case ancs::LinkState::Ready: {
            drawBluetoothIcon(W / 2, 84, C_GREEN);
            drawTextCenter(fTitle, 146, "Ready", C_TEXT, C_BG);
            char line[40];
            if (gLink.ready >= 2) snprintf(line, sizeof line, "Notifications from %u iPhones", (unsigned)gLink.ready);
            else strlcpy(line, "New iPhone notifications", sizeof line);
            drawTextCenter(fBody, 174, line, C_DIM, C_BG);
            drawTextCenter(fBody, 197, "will show up here", C_DIM, C_BG);
            if (gLink.advertising) {  // còn chỗ: iPhone khác vẫn thấy board
                drawTextCenter(fSmall, 224, ellipsize(fSmall, "Add another iPhone: Settings › Bluetooth", W - 2 * PAD),
                               C_MUTED, C_BG);
            }
            break;
        }
        case ancs::LinkState::Connected:
            drawBluetoothIcon(W / 2, 84, C_BLUE);
            drawTextCenter(fTitle, 146, "Connecting…", C_TEXT, C_BG);
            drawTextCenter(fBody, 174, "If your iPhone asks, turn on", C_DIM, C_BG);
            drawTextCenter(fBody, 197, "“Share System Notifications”", C_DIM, C_BG);
            break;
        default:
            drawBluetoothIcon(W / 2, 84, C_BLUE);
            drawTextCenter(fTitle, 146, "Connect your iPhone", C_TEXT, C_BG);
            drawTextCenter(fBody, 174, "Open Settings › Bluetooth", C_DIM, C_BG);
            drawTextCenter(fBody, 197, std::string("and tap “") + gDeviceName + "”", C_DIM, C_BG);
            break;
    }
}

void drawPassKeyScreen() {
    TFT_eSPI& g = gfx();
    g.fillRect(0, HDR_H + 1, W, H - HDR_H - 1, C_BG);
    drawTextCenter(fBody, 66, gLink.ready > 0 ? "Enter this code on the new iPhone" : "Enter this code on your iPhone",
                   C_TEXT, C_BG);

    const int boxX = 40, boxY = 84, boxW = 240, boxH = 72;
    g.fillSmoothRoundRect(boxX, boxY, boxW, boxH, 14, C_SEP, C_BG);
    g.fillSmoothRoundRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2, 13, C_HDR, C_SEP);

    // Các chữ số cách nhau thêm 6px cho dễ đọc
    char digits[8];
    snprintf(digits, sizeof digits, "%06lu", (unsigned long)gPassKey);
    const int gap = 6;
    int total = gap * (strlen(digits) - 1);
    for (const char* p = digits; *p; p++) total += textWidth(fDigits, std::string(1, *p));
    int x = (W - total) / 2;
    const int base = midBaseline(fDigits, boxY, boxH);
    for (const char* p = digits; *p; p++) x = drawText(fDigits, x, base, std::string(1, *p), C_YELLOW, C_HDR) + gap;

    drawTextCenter(fSmall, 190, "This code only works for this pairing", C_MUTED, C_BG);
}

// Board đang phát mạng "ESP32 Noti Setup": hướng dẫn cài WiFi bằng iPhone
void drawWifiSetupScreen() {
    gfx().fillRect(0, HDR_H + 1, W, H - HDR_H - 1, C_BG);
    drawText(fTitle, PAD, 58, "Set up WiFi to read aloud", C_TEXT, C_BG);
    static const char* const steps[] = {
        "1. On iPhone: Settings › Wi-Fi",
        "2. Join “ESP32 Noti Setup”",
        "3. Tap Configure WiFi, pick your network, enter the password and tap Save",
    };
    int y = 88;
    for (const char* step : steps) {
        for (const std::string& line : wrapText(fBody, step, W - 2 * PAD, 3)) {
            drawText(fBody, PAD, y, line, C_BODY, C_BG);
            y += BODY_LINE_H;
        }
    }
    drawBarBackground();
    const int bw = 140;
    drawButton((W - bw) / 2, bw, BTN_WIFI_LATER, "Later", C_SEP, C_MUTED, C_TEXT);
}

void render() {
    gButtons.clear();
    drawHeader();
    if (gShowPassKey) {
        drawPassKeyScreen();
    } else if (speech::net() == speech::Net::Setup) {
        drawWifiSetupScreen();
    } else if (gCount == 0) {
        drawWaitingScreen();
    } else {
        drawNotification(gHistory[gView]);
        if (gHasTouch) drawActionBar(gHistory[gView]);
        else drawHintBar();
    }
    if (gSoundPanel) drawSoundPanel();
    present();
}

void startFlash() {
    gFlashLeft = FLASH_TOGGLES;
    gFlashNext = millis();
}

// ---------- Thao tác trên danh sách (chỉ đổi trạng thái, người gọi tự render) ----------

// Đèn nháy cuộc gọi khi còn cuộc gọi đến trong danh sách; hết tin thì thôi nhắc
void updateLed() {
    bool call = false;
    for (int i = 0; i < gCount && !call; i++) call = isCall(gHistory[i]);
    led::setCall(call);
    if (gCount == 0) led::setUnread(false);
}

void showIndex(int i) {
    if (i < 0 || i >= gCount || i == gView) return;
    gView = i;
    gScroll = 0;
    gFlashLeft = 0;
}

void removeAt(int i) {
    memmove(&gHistory[i], &gHistory[i + 1], (gCount - i - 1) * sizeof(Notification));
    gCount--;
    if (i == gView) gScroll = 0;
    if (i < gView || gView >= gCount) gView = gView > 0 ? gView - 1 : 0;
    updateLed();
}

void clearAll() {
    gCount = 0;
    gView = 0;
    gScroll = 0;
    gFlashLeft = 0;
    updateLed();
}

void onButton(ButtonId id) {
    switch (id) {
        case BTN_SOUND:
            gSoundPanel = !gSoundPanel;
            return;
        case BTN_VOL_DOWN:
        case BTN_VOL_UP: {
            const int v = speech::volume() + (id == BTN_VOL_UP ? 1 : -1);
            if (v >= 0 && v <= speech::MAX_VOLUME) speech::setVolume(v);
            return;
        }
        case BTN_WIFI:
            gSoundPanel = false;
            speech::openWifiSetup();
            return;
        case BTN_WIFI_LATER:
            speech::closeWifiSetup();
            return;
        default:
            break;
    }
    if (gCount == 0) return;
    // Hành động gửi tới đúng iPhone đã gửi tin; iPhone đó đang mất kết nối thì chỉ xoá trên màn hình
    const uint8_t phone = gHistory[gView].phone;
    const uint32_t uid = gHistory[gView].uid;
    const bool canClear = gHistory[gView].flags & NOTIF_FLAG_NEGATIVE_ACTION;
    switch (id) {
        case BTN_OLDER: showIndex(gView + 1); break;
        case BTN_NEWER: showIndex(gView - 1); break;
        case BTN_DISMISS:
            if (canClear) ancs::performAction(phone, uid, false);  // xoá luôn trên iPhone
            removeAt(gView);
            break;
        case BTN_ANSWER:
            ancs::performAction(phone, uid, true);
            removeAt(gView);
            break;
        case BTN_DECLINE:
            ancs::performAction(phone, uid, false);
            removeAt(gView);
            break;
        default: break;
    }
}

// ---------- Cảm ứng ----------

ButtonId hitTest(int x, int y) {
    for (const Button& b : gButtons) {  // nới vùng nhận chạm ra vài pixel cho dễ bấm
        if (x >= b.x - 4 && x < b.x + b.w + 4 && y >= b.y - 6 && y < b.y + b.h + 6) return b.id;
    }
    return BTN_NONE;
}

void touchDown(int x, int y, uint32_t now) {
    gTouch = TouchState{};
    gTouch.down = true;
    gTouch.downAt = gTouch.lastSeen = now;
    gTouch.startX = gTouch.x = x;
    gTouch.startY = gTouch.y = gTouch.scrollAnchorY = y;
    Serial.printf("[Touch] chạm (%d, %d)\n", x, y);
    led::setUnread(false);  // đã chạm màn = đã thấy tin
    gSoundPanelAt = now;    // còn chạm thì bảng âm lượng chưa tự ẩn
    if (gDimmed) {  // chạm khi màn đang tối chỉ để bật sáng
        gTouch.ignore = true;
        wake();
        return;
    }
    wake();
    gPressed = hitTest(x, y);
    if (gPressed != BTN_NONE) render();
}

void touchMove(int x, int y, uint32_t now) {
    gTouch.x = x;
    gTouch.y = y;
    gTouch.lastSeen = now;
    if (gTouch.ignore || gTouch.longFired) return;
    const int dx = x - gTouch.startX, dy = y - gTouch.startY;
    if (!gTouch.moved && (abs(dx) > MOVE_SLOP || abs(dy) > MOVE_SLOP)) {
        gTouch.moved = true;
        if (gPressed != BTN_NONE) {  // kéo đi thì huỷ nhấn nút
            gPressed = BTN_NONE;
            render();
        }
    }
    // Kéo dọc: nội dung chạy theo ngón tay, mỗi BODY_LINE_H pixel một dòng
    if (gTouch.moved && abs(dy) > abs(dx) && gMaxScroll > 0) {
        const int lines = (gTouch.scrollAnchorY - y) / BODY_LINE_H;
        if (lines != 0) {
            gTouch.scrollAnchorY -= lines * BODY_LINE_H;
            const int s = constrain(gScroll + lines, 0, gMaxScroll);
            if (s != gScroll) {
                gScroll = s;
                render();
            }
        }
    }
    // Giữ nút Xoá: xoá hết danh sách trên màn hình (thông báo trên iPhone giữ nguyên).
    // Giữ nút loa: mở trang cài WiFi.
    if (!gTouch.moved && (gPressed == BTN_DISMISS || gPressed == BTN_SOUND) &&
        now - gTouch.downAt > LONG_PRESS_MS) {
        gTouch.longFired = true;
        const ButtonId held = gPressed;
        gPressed = BTN_NONE;
        if (held == BTN_DISMISS) clearAll();
        else speech::openWifiSetup();
        render();
    }
}

void touchUp() {
    gTouch.down = false;    if (gTouch.ignore || gTouch.longFired) return;
    const ButtonId pressed = gPressed;
    gPressed = BTN_NONE;
    if (pressed != BTN_NONE) {
        onButton(pressed);
        render();
        return;
    }
    // Vuốt ngang: sang trái xem tin cũ hơn, sang phải xem tin mới hơn
    const int dx = gTouch.x - gTouch.startX, dy = gTouch.y - gTouch.startY;
    if (gCount > 0 && !gShowPassKey && abs(dx) >= SWIPE_MIN && abs(dx) > 2 * abs(dy)) {
        const int before = gView;
        showIndex(dx < 0 ? gView + 1 : gView - 1);
        if (gView != before) render();
    }
}

// Nút BOOT: bấm xem tin cũ hơn (hết thì quay lại tin mới nhất), giữ để xoá hết
void pollBoot(uint32_t now) {
    const bool down = digitalRead(BOOT_PIN) == LOW;
    if (down && !gBootDown) {
        gBootDown = true;
        gBootLong = false;
        gBootAt = now;
    } else if (down && !gBootLong && now - gBootAt > LONG_PRESS_MS) {
        gBootLong = true;
        wake();
        clearAll();
        render();
    } else if (!down && gBootDown) {
        gBootDown = false;
        if (gBootLong || now - gBootAt < 30) return;  // giữ lâu đã xử lý, hoặc chỉ là rung tiếp điểm
        led::setUnread(false);
        if (gDimmed) {
            wake();
            return;
        }
        wake();
        if (gCount > 0 && !gShowPassKey) {
            gView = (gView + 1) % gCount;
            gScroll = 0;
            render();
        }
    }
}

void pollInput() {
    const uint32_t now = millis();
    pollBoot(now);
    if (!gHasTouch || now - gTouchPollAt < TOUCH_POLL_MS) return;
    gTouchPollAt = now;
    int x, y;
    if (touch::read(x, y)) {
        if (!gTouch.down) touchDown(x, y, now);
        else touchMove(x, y, now);
    } else if (gTouch.down && now - gTouch.lastSeen > RELEASE_MS) {
        touchUp();
    }
}

}  // namespace

// ---------- API ----------

void begin(const char* deviceName) {
    gDeviceName = deviceName;

    tft.init();
    // Đọc mã chip màn hình (ILI9341 trả về 9341) để kiểm tra dây SPI qua log Serial
    Serial.printf("[TFT] ID: %02X%02X\n", tft.readcommand8(0xD3, 2), tft.readcommand8(0xD3, 3));
    tft.setRotation(1);
    tft.fillScreen(C_BG);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(TFT_BL, 5000, 8);
#else
    ledcSetup(BL_CHANNEL, 5000, 8);
    ledcAttachPin(TFT_BL, BL_CHANNEL);
#endif
    setBacklight(BRIGHT_FULL);

    // Sprite 320x240 16-bit = 150KB, nằm trong PSRAM nếu có; không đủ bộ nhớ thì vẽ thẳng lên màn
    canvas.setColorDepth(16);
    gUseCanvas = canvas.createSprite(W, H) != nullptr;
    Serial.printf("[TFT] Sprite toàn màn hình: %s (PSRAM %lu KB)\n", gUseCanvas ? "có" : "không, vẽ trực tiếp",
                  (unsigned long)(ESP.getPsramSize() / 1024));

    fSmall.begin();
    fLabel.begin();
    fBody.begin();
    fTitle.begin();
    fDigits.begin();

    pinMode(BOOT_PIN, INPUT_PULLUP);
    gHasTouch = touch::begin();
    led::begin();

    gLastActivity = millis();
    render();
}

bool isTouched() {
    int x, y;
    return gHasTouch && touch::read(x, y);
}

void setLinkStatus(const ancs::LinkStatus& st) {
    const bool changed = st.state != gLink.state || st.ready != gLink.ready || st.connected != gLink.connected;
    const uint8_t prevReady = gLink.ready;
    gLink = st;
    for (auto& p : gLink.phones) sanitize(p.label, sizeof p.label);  // bỏ emoji/ký tự font không có
    if (st.connected == 0) gShowPassKey = false;  // KHÔNG ẩn mã PIN chỉ vì máy khác đổi trạng thái
    led::setLink(st.state);
    if (st.ready > prevReady && prevReady > 0) led::phoneJoined();  // thêm được một iPhone nữa
    led::setPairing(gShowPassKey);
    if (changed) wake();
    render();
}

void showPassKey(uint32_t key) {
    gPassKey = key;
    gShowPassKey = true;
    led::setPairing(true);
    wake();
    render();
}

void hidePassKey() {
    gShowPassKey = false;
    led::setPairing(false);
    render();
}

bool addNotification(const Notification& in, bool isNew) {
    Notification n = in;
    const bool hadMessage = n.message[0] != '\0';
    sanitize(n.title, sizeof n.title);
    sanitize(n.subtitle, sizeof n.subtitle);
    sanitize(n.message, sizeof n.message);
    if (hadMessage && !n.message[0]) strlcpy(n.message, "(emoji)", sizeof n.message);

    // Mỗi iPhone tự đánh số uid: thông báo được nhận diện bằng cặp (phone, uid)
    for (int i = 0; i < gCount; i++) {
        if (gHistory[i].phone != n.phone || gHistory[i].uid != n.uid) continue;
        if (!isNew) {  // thông báo được cập nhật nội dung
            gHistory[i] = n;
            updateLed();
            if (i == gView) render();
            return false;
        }
        removeAt(i);  // "Added" với uid đã có (iPhone dùng lại uid, vd. sau khi khởi động lại): coi như tin mới
        break;
    }
    int keep = gCount < HISTORY_MAX ? gCount : HISTORY_MAX - 1;
    memmove(&gHistory[1], &gHistory[0], keep * sizeof(Notification));
    gHistory[0] = n;
    gCount = keep + 1;
    gView = 0;
    gScroll = 0;
    led::newNotification(categoryColor(n.category));
    led::setUnread(true);
    updateLed();
    wake();
    render();
    startFlash();
    return true;
}

void removeNotification(uint8_t phone, uint32_t uid) {
    for (int i = 0; i < gCount; i++) {
        if (gHistory[i].phone != phone || gHistory[i].uid != uid) continue;
        removeAt(i);
        render();
        return;
    }
}

void forgetPhone(uint8_t phone) {
    for (int i = gCount - 1; i >= 0; i--) {
        if (phone == NO_PHONE || gHistory[i].phone == phone) removeAt(i);
    }
    render();
}

void loop() {
    pollInput();
    const uint32_t now = millis();  // lấy sau pollInput: wake() có thể vừa cập nhật gLastActivity

    if (gFlashLeft > 0 && (int32_t)(now - gFlashNext) >= 0) {
        if (gCount > 0) {
            if (gFlashLeft % 2 == 0) {
                const uint16_t c = categoryColor(gHistory[gView].category);
                gfx().drawRect(0, HDR_H + 1, W, BAR_Y - HDR_H - 2, c);
                gfx().drawRect(1, HDR_H + 2, W - 2, BAR_Y - HDR_H - 4, c);
            } else {
                drawNotification(gHistory[gView]);
            }
            present();
        }
        gFlashLeft--;
        gFlashNext = now + FLASH_STEP_MS;
    }

    // Không giảm sáng khi đang ghép đôi hoặc đang có cuộc gọi đến
    const bool keepBright = gShowPassKey || (gCount > 0 && isCall(gHistory[gView]));
    if (!gDimmed && !keepBright && now - gLastActivity > DIM_AFTER_MS) {
        gDimmed = true;
        setBacklight(BRIGHT_DIM);
        led::setDim(true);
    }
    led::loop();

    // Task đọc thông báo đổi trạng thái loa/WiFi (vd. vừa nối mạng, đang đọc) -> vẽ lại
    if (speechState() != gSpeechDrawn) render();
    if (gSoundPanel && !gTouch.down && now - gSoundPanelAt > SOUND_PANEL_MS) {
        gSoundPanel = false;
        render();
    }

    struct tm t;
    if (clockValid(t) && t.tm_min != gLastMinute) {
        drawHeader();
        present();
    }
}

}  // namespace ui
