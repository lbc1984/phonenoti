#include "speech.h"

#ifdef UI_SPEECH
#include <Arduino.h>
#include <HTTPClient.h>
#include <MP3DecoderHelix.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "app_names.h"
#include "audio.h"
#include "google_roots.h"
#include "vlw_font.h"
#endif

namespace speech {

#ifdef UI_SPEECH
namespace {

using namespace libhelix;

constexpr const char* SETUP_AP = "ESP32 Noti Setup";
constexpr const char* TTS_URL = "https://translate.google.com/translate_tts?ie=UTF-8&client=tw-ob&tl=";
constexpr size_t MAX_CHARS = 300;        // đọc tối đa ngần này ký tự mỗi thông báo
constexpr size_t MAX_CHUNK_CHARS = 180;  // Google nhận khoảng 200 ký tự mỗi lần
constexpr size_t MP3_BUF_SIZE = 512 * 1024;
constexpr size_t PREBUFFER = 8 * 1024;  // ~1 giây âm thanh tải trước rồi mới phát, đỡ ngắt khi mạng chậm
constexpr size_t QUEUE_MAX = 3;
constexpr size_t FEED_BYTES = 1024;          // đưa MP3 cho bộ giải mã từng đoạn để kịp dừng khi bị huỷ
constexpr uint32_t SYSTEM_UID = 0xFFFFFFFF;  // câu của chính board, không gắn với thông báo nào

// Một phần câu đọc bằng một giọng: câu của board bằng tiếng Anh, nội dung thông báo bằng tiếng Việt
struct Part {
    bool english;
    std::string text;
};

struct Item {
    uint32_t uid;
    bool chime;  // kêu "ting" trước khi đọc
    std::vector<Part> parts;
};

std::atomic<bool> gAvailable{false}, gSpeaking{false}, gAbort{false};
std::atomic<bool> gWantSetup{false}, gWantCloseSetup{false}, gWantTick{false};
std::atomic<uint8_t> gVolume{audio::MAX_VOLUME};
std::atomic<int> gPeak{0};  // mẫu lớn nhất của đoạn đang đọc, để biết còn tăng âm lượng được không
std::atomic<Net> gNet{Net::Off};

std::mutex gMutex;  // bảo vệ hàng đợi và thông báo đang đọc
std::vector<Item> gQueue;
bool gHasCurrent = false;
uint32_t gCurrentUid = 0;

Preferences gPrefs;  // "vol": âm lượng, "later": đã bấm "Later" ở màn cài WiFi

// ---------- Soạn câu ----------

// Giữ chữ (kể cả tiếng Việt) và dấu câu; bỏ emoji, ký hiệu lạ vì Google đọc ra tên ký hiệu hoặc báo lỗi
bool speakable(uint32_t cp) {
    return (cp >= 0x20 && cp < 0x2000 && cp != 0x7F) || (cp >= 0x2018 && cp <= 0x201D) || cp == 0x2026;
}

// Lọc ký tự, gộp khoảng trắng; xuống dòng thành chỗ ngắt câu cho giọng đọc ngừng một chút
std::string clean(const char* s) {
    std::string out;
    bool space = false, pause = false;
    for (const char* p = s; *p;) {
        const char* start = p;
        const uint32_t cp = VlwFont::nextCodepoint(p);
        if (cp == '\n' || cp == '\r') {
            pause = !out.empty();
        } else if (cp == ' ' || cp == '\t' || cp == 0xA0) {
            space = !out.empty();
        } else if (speakable(cp)) {
            if (pause) {
                if (!strchr(".,!?:;", out.back())) out += '.';
                out += ' ';
            } else if (space) {
                out += ' ';
            }
            pause = space = false;
            out.append(start, p - start);
        }
    }
    return out;
}

// Còn tối đa maxChars ký tự, cắt ở khoảng trắng gần nhất
std::string truncateChars(const std::string& s, size_t maxChars) {
    size_t chars = 0, lastSpace = 0;
    for (const char* p = s.c_str(); *p;) {
        if (*p == ' ') lastSpace = p - s.c_str();
        VlwFont::nextCodepoint(p);
        if (++chars == maxChars && *p) return s.substr(0, lastSpace ? lastSpace : p - s.c_str());
    }
    return s;
}

// Đường link đọc ra từng ký tự rất dài và vô nghĩa -> thay bằng chữ "link"
std::string dropLinks(const std::string& s) {
    std::string out;
    for (size_t i = 0; i <= s.size();) {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        const std::string word = s.substr(i, j - i);
        const bool link = word.find("://") != std::string::npos || word.compare(0, 4, "www.") == 0;
        if (!out.empty()) out += ' ';
        out += link ? "link" : word;
        i = j + 1;
    }
    return out;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return !prefix.empty() && s.compare(0, prefix.size(), prefix) == 0;
}

// "Zalo. Ngọc Hà: tối nay đi ăn không" (giọng Việt) / "Incoming call from" (giọng Anh) + "Ngọc Hà" (giọng Việt)
std::vector<Part> compose(const Notification& n) {
    const std::string app = clean(appDisplayName(n.appId));
    std::string title = clean(n.title);
    if (n.category == CAT_INCOMING_CALL || n.category == CAT_MISSED_CALL) {
        return {{true, n.category == CAT_INCOMING_CALL ? "Incoming call from" : "Missed call from"},
                {false, truncateChars(title.empty() ? app : title, MAX_CHARS)}};
    }
    std::string subtitle = clean(n.subtitle);
    const std::string message = dropLinks(clean(n.message));
    // Nhiều app (vd. Gmail) lặp lại tiêu đề ở đầu nội dung: chỉ đọc một lần
    if (startsWith(message, subtitle)) subtitle.clear();
    if (startsWith(message, title)) title.clear();
    std::string who = title;
    if (!subtitle.empty()) who += who.empty() ? subtitle : ", " + subtitle;
    std::string text = app;
    if (!who.empty() && who != app) text += ". " + who;
    if (!message.empty()) text += (text.size() > app.size() ? ": " : ". ") + message;
    return {{false, truncateChars(text, MAX_CHARS)}};
}

// Chia thành đoạn ≤ MAX_CHUNK_CHARS ký tự: ưu tiên ngắt sau dấu chấm câu, rồi dấu phẩy, rồi khoảng trắng
std::vector<std::string> splitChunks(const std::string& s) {
    std::vector<std::string> out;
    const char* p = s.c_str();
    while (*p) {
        while (*p == ' ') p++;
        const char* start = p;
        const char *sentence = nullptr, *comma = nullptr, *space = nullptr;
        for (size_t chars = 0; *p && chars < MAX_CHUNK_CHARS; chars++) {
            if (*p == ' ' && p > start) {
                if (strchr(".!?;:", p[-1])) sentence = p;
                else if (p[-1] == ',') comma = p;
                else space = p;
            }
            VlwFont::nextCodepoint(p);
        }
        if (*p) {  // còn chữ phía sau: lùi về chỗ ngắt tự nhiên nhất
            const char* cut = sentence ? sentence : comma ? comma : space;
            if (cut) p = cut;
        }
        if (p > start) out.emplace_back(start, p - start);
    }
    return out;
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

// ---------- Hàng đợi ----------

void enqueue(Item item) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gQueue.size() >= QUEUE_MAX) gQueue.erase(gQueue.begin());  // đầy thì bỏ câu cũ nhất
    gQueue.push_back(std::move(item));
}

bool takeNext(Item& out) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gQueue.empty()) return false;
    out = std::move(gQueue.front());
    gQueue.erase(gQueue.begin());
    gHasCurrent = true;
    gCurrentUid = out.uid;
    gAbort = false;
    return true;
}

void finishCurrent() {
    std::lock_guard<std::mutex> lock(gMutex);
    gHasCurrent = false;
}

// ---------- Tải và phát giọng đọc ----------

void onPcm(MP3FrameInfo& info, short* pcm, size_t len, void*) {
    if (gAbort) return;
    size_t samples = len;
    if (info.nChans == 2) {  // trộn stereo thành mono
        samples = len / 2;
        for (size_t i = 0; i < samples; i++) pcm[i] = (pcm[2 * i] + pcm[2 * i + 1]) / 2;
    }
    int peak = gPeak;
    for (size_t i = 0; i < samples; i++) peak = std::max(peak, std::abs((int)pcm[i]));
    gPeak = peak;
    audio::start(info.samprate);
    audio::write(pcm, samples);
}

MP3DecoderHelix gDecoder(onPcm);

// Nhận MP3 từ HTTPClient (đã bỏ phần đóng gói "chunked" của Google) và phát dần ngay trong lúc tải.
// Luôn giữ PREBUFFER byte chưa phát để mạng chậm thoáng chốc không làm giọng đọc bị ngắt.
class Mp3Player : public Stream {
  public:
    bool begin() {
        data_ = (uint8_t*)ps_malloc(MP3_BUF_SIZE);
        return data_ != nullptr;
    }
    void start() {
        size_ = played_ = 0;
        gDecoder.begin();
    }
    // Tải xong: phát nốt phần còn lại
    void finish() {
        feed(size_);
        if (!gAbort && size_ > 0) {
            // Khung cuối chỉ được giải mã khi thấy đầu khung kế tiếp: thêm một đầu khung giả rồi xả hết
            static const uint8_t nextHeader[] = {0xFF, 0xF3, 0x84, 0xC4};
            gDecoder.write(nextHeader, sizeof nextHeader);
            gDecoder.flush();
        }
        gDecoder.end();
    }
    size_t size() const { return size_; }

    size_t write(uint8_t b) override { return write(&b, 1); }
    size_t write(const uint8_t* buf, size_t n) override {
        if (gAbort) return 0;  // bị huỷ: báo lỗi ghi để HTTPClient thôi tải
        n = std::min(n, MP3_BUF_SIZE - size_);
        memcpy(data_ + size_, buf, n);
        size_ += n;
        if (size_ > PREBUFFER) feed(size_ - PREBUFFER);
        return n;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

  private:
    // Giải mã và phát tới vị trí upto; chờ theo tốc độ phát của loa
    void feed(size_t upto) {
        while (played_ < upto && !gAbort) {
            const size_t n = std::min(FEED_BYTES, upto - played_);
            gDecoder.write(data_ + played_, n);
            played_ += n;
        }
    }

    uint8_t* data_ = nullptr;
    size_t size_ = 0, played_ = 0;
};

Mp3Player gPlayer;

// Tải giọng đọc một đoạn và phát luôn. Kết nối TLS được giữ lại cho đoạn sau (setReuse).
bool speakChunk(HTTPClient& http, WiFiClientSecure& tls, const std::string& text, bool english) {
    const uint32_t t0 = millis();
    const std::string url = TTS_URL + std::string(english ? "en" : "vi") + "&q=" + urlEncode(text);
    gPlayer.start();
    gPeak = 0;
    int code = -1, result = 0;
    uint32_t headMs = 0;
    for (int attempt = 0; attempt < 2 && code < 0 && !gAbort; attempt++) {
        // Lỗi kết nối (vd. vừa nối mạng xong, DNS chưa sẵn sàng): chờ một chút rồi thử lại một lần
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(1000));
        if (!http.begin(tls, url.c_str())) break;
        code = http.GET();
        headMs = millis() - t0;
        if (code == HTTP_CODE_OK) result = http.writeToStream(&gPlayer);
        http.end();
    }
    const uint32_t loadMs = millis() - t0;
    gPlayer.finish();
    Serial.printf("[TTS] \"%s\" -> HTTP %d sau %lu ms, tải xong %u KB sau %lu ms, đọc xong sau %lu ms%s%s, "
                  "đỉnh %d%%, RAM trong còn %u KB (thấp nhất %u KB)\n",
                  text.c_str(), code, (unsigned long)headMs, (unsigned)(gPlayer.size() / 1024),
                  (unsigned long)loadMs, (unsigned long)(millis() - t0), result < 0 ? ", lỗi: " : "",
                  result < 0 ? HTTPClient::errorToString(result).c_str() : "", gPeak * 100 / 32768,
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return code == HTTP_CODE_OK && result >= 0;
}

void speak(HTTPClient& http, WiFiClientSecure& tls, const Item& item) {
    gSpeaking = true;
    if (item.chime) audio::chime();
    if (gNet == Net::Online) {
        bool ok = true;
        for (const Part& part : item.parts) {
            for (const std::string& chunk : splitChunks(part.text)) {
                ok = !gAbort && speakChunk(http, tls, chunk, part.english);
                if (!ok) break;
            }
            if (!ok) break;
        }
    }
    audio::stop();
    gSpeaking = false;
}

// ---------- WiFi ----------

bool wifiSaved() {
    wifi_config_t conf = {};
    return esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0] != 0;
}

const char* netName(Net n) {
    switch (n) {
        case Net::Online: return "đã có mạng";
        case Net::Connecting: return "đang nối WiFi đã lưu";
        case Net::Setup: return "đang mở trang cài đặt";
        default: return "chưa cài WiFi";
    }
}

void task(void*) {
    static WiFiClientSecure tls;
    static HTTPClient http;
    static WiFiManager wm;

    tls.setCACert(kGoogleRootCAs);  // chỉ gửi nội dung thông báo cho đúng máy chủ Google
    tls.setHandshakeTimeout(10);
    http.setReuse(true);
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setUserAgent("Mozilla/5.0");

    wm.setDebugOutput(false);
    wm.setConfigPortalBlocking(false);  // trang cài đặt chạy song song, gọi wm.process() trong vòng lặp
    wm.setTitle("ESP32 Noti");
    std::vector<const char*> menu = {"wifi", "exit"};
    wm.setMenu(menu);
    wm.setDarkMode(true);
    wm.setSaveConnectTimeout(20);
    wm.setHostname("esp32-noti");
    static bool savedNew = false;  // trang cài đặt vừa lưu WiFi mới và nối được
    wm.setSaveConfigCallback([] { savedNew = true; });

    WiFi.mode(WIFI_STA);
    if (wifiSaved()) WiFi.begin();
    else if (!gPrefs.getBool("later", false)) gWantSetup = true;

    Net last = Net::Off;
    Item item;
    for (;;) {
        if (gWantSetup.exchange(false) && !wm.getConfigPortalActive()) wm.startConfigPortal(SETUP_AP);
        if (gWantCloseSetup.exchange(false) && wm.getConfigPortalActive()) {
            wm.stopConfigPortal();
            gPrefs.putBool("later", true);
            WiFi.mode(WIFI_STA);
            if (wifiSaved()) WiFi.begin();
        }
        if (wm.getConfigPortalActive()) wm.process();

        const Net now = wm.getConfigPortalActive()   ? Net::Setup
                        : WiFi.status() == WL_CONNECTED ? Net::Online
                        : wifiSaved()                   ? Net::Connecting
                                                        : Net::Off;
        if (now != last) {
            Serial.printf("[WiFi] %s%s%s\n", netName(now), now == Net::Setup ? ": mạng " : "",
                          now == Net::Setup ? SETUP_AP : "");
            if (now == Net::Online && savedNew) {
                savedNew = false;
                if (gVolume > 0) enqueue({SYSTEM_UID, false, {{true, "WiFi connected"}}});  // báo cài xong, thử loa
            }
            if (last == Net::Setup && !wifiSaved()) gPrefs.putBool("later", true);  // thoát trang mà chưa cài
            last = now;
            gNet = now;
        }

        if (takeNext(item)) {
            speak(http, tls, item);
            finishCurrent();
        } else if (gWantTick.exchange(false)) {  // vừa chỉnh âm lượng: kêu thử ở mức mới
            gSpeaking = true;
            audio::tick();
            audio::stop();
            gSpeaking = false;
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

}  // namespace

void begin() {
    gPrefs.begin("speech", false);
    gVolume = std::min(gPrefs.getUChar("vol", audio::MAX_VOLUME), audio::MAX_VOLUME);
    if (!audio::begin()) return;
    audio::setVolume(gVolume);
    if (!gPlayer.begin()) {
        Serial.println("[TTS] Không đủ PSRAM cho bộ đệm giọng đọc");
        return;
    }
    gAvailable = true;
    // Core 0 cùng WiFi/BLE; giao diện chạy ở core 1 nên không bị giật khi tải và giải mã
    xTaskCreatePinnedToCore(task, "speech", 16384, nullptr, 2, nullptr, 0);
}

bool available() { return gAvailable; }

void announce(const Notification& n) {
    if (!gAvailable || gVolume == 0 || (n.flags & NOTIF_FLAG_SILENT)) return;
    Item item{n.uid, true, compose(n)};
    for (const Part& p : item.parts) Serial.printf("[TTS] Sẽ đọc (%s): %s\n", p.english ? "en" : "vi", p.text.c_str());
    enqueue(std::move(item));
}

void say(const char* text) {
    if (!gAvailable) return;
    const std::string s = truncateChars(clean(text), MAX_CHARS);
    // Câu thử toàn chữ không dấu thì đọc giọng Anh, có dấu thì giọng Việt
    const bool english = std::all_of(s.begin(), s.end(), [](char c) { return (unsigned char)c < 0x80; });
    enqueue({SYSTEM_UID, true, {{english, s}}});
}

void cancel(uint32_t uid) {
    std::lock_guard<std::mutex> lock(gMutex);
    gQueue.erase(std::remove_if(gQueue.begin(), gQueue.end(), [uid](const Item& i) { return i.uid == uid; }),
                 gQueue.end());
    if (gHasCurrent && gCurrentUid == uid) gAbort = true;
}

void setVolume(uint8_t level) {
    if (!gAvailable) return;
    level = std::min(level, audio::MAX_VOLUME);
    gVolume = level;
    gPrefs.putUChar("vol", level);
    audio::setVolume(level);
    Serial.printf("[TTS] Âm lượng %u/%u\n", level, audio::MAX_VOLUME);
    if (level > 0) {
        gWantTick = true;
        return;
    }
    std::lock_guard<std::mutex> lock(gMutex);  // tắt tiếng: bỏ hết câu đang chờ và dừng câu đang đọc
    gQueue.clear();
    if (gHasCurrent) gAbort = true;
}

uint8_t volume() { return gVolume; }
bool muted() { return gVolume == 0; }
bool speaking() { return gSpeaking; }
Net net() { return gNet; }

void openWifiSetup() {
    if (gAvailable) gWantSetup = true;
}

void closeWifiSetup() { gWantCloseSetup = true; }

#else
void begin() {}
bool available() { return false; }
void announce(const Notification&) {}
void say(const char*) {}
void cancel(uint32_t) {}
void setVolume(uint8_t) {}
uint8_t volume() { return 0; }
bool muted() { return false; }
bool speaking() { return false; }
Net net() { return Net::Off; }
void openWifiSetup() {}
void closeWifiSetup() {}
#endif

}  // namespace speech
