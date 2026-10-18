#include "ancs_client.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <string.h>
#include <sys/time.h>

namespace ancs {
namespace {

// ---- UUID của Apple ANCS ----
const NimBLEUUID kAncsService("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
const NimBLEUUID kNotificationSource("9FBF120D-6301-42D9-8C58-25E699A21DBD");
const NimBLEUUID kControlPoint("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
const NimBLEUUID kDataSource("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");
// Current Time Service trên iPhone, dùng để chỉnh đồng hồ
const NimBLEUUID kCtsService((uint16_t)0x1805);
const NimBLEUUID kCtsCurrentTime((uint16_t)0x2A2B);

// Quảng bá "solicitation" UUID ANCS (128-bit, little-endian) để iOS hiện thiết bị
// trong Cài đặt → Bluetooth và cho phép dùng ANCS.
const uint8_t kAdvSolicitAncs[] = {
    17, 0x15,  // độ dài, AD type: List of 128-bit Service Solicitation UUIDs
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,
    0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79,
};

enum : uint8_t { EVT_ADDED = 0, EVT_MODIFIED = 1, EVT_REMOVED = 2 };
enum : uint8_t {
    ATTR_APP_ID = 0,
    ATTR_TITLE = 1,
    ATTR_SUBTITLE = 2,
    ATTR_MESSAGE = 3,
    ATTR_DATE = 5,
};
constexpr uint8_t kCmdGetNotificationAttributes = 0;
constexpr uint8_t kCmdPerformNotificationAction = 2;
constexpr int kAttrCount = 5;

constexpr EventBits_t BIT_CONNECTED = 1 << 0;
constexpr EventBits_t BIT_DISCONNECTED = 1 << 1;
constexpr EventBits_t BIT_AUTH_DONE = 1 << 2;
constexpr EventBits_t BIT_DATA_DONE = 1 << 3;

// Một sự kiện từ Notification Source, chờ worker xử lý.
struct Pending {
    uint8_t eventId;
    uint8_t flags;
    uint8_t category;
    uint32_t uid;
};

// Hành động người dùng chọn trên màn hình, chờ worker gửi sang iPhone.
struct ActionReq {
    uint32_t uid;
    uint8_t actionId;  // 0 = positive, 1 = negative
};

QueueHandle_t gEvents;   // -> UI
QueueHandle_t gPending;  // Notification Source -> worker
QueueHandle_t gActions;  // UI -> worker
EventGroupHandle_t gBits;
NimBLEServer* gServer = nullptr;
volatile uint16_t gConnHandle = BLE_HS_CONN_HANDLE_NONE;
volatile uint32_t gConnSeq = 0;  // tăng mỗi lần có kết nối mới
volatile bool gAuthOk = false;

// Ghép các mảnh phản hồi từ Data Source (một phản hồi có thể dài hơn MTU).
uint8_t gBuf[1024];
size_t gLen = 0;
volatile bool gCollecting = false;
uint32_t gWantUid = 0;
Notification gCur;

uint32_t readLe32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void postEvent(const Event& e) { xQueueSend(gEvents, &e, 0); }

void postState(LinkState s) {
    static Event e;
    e = Event{};
    e.type = EventType::State;
    e.state = s;
    postEvent(e);
}

// Chép chuỗi UTF-8, cắt bớt nếu quá dài nhưng không cắt đôi một ký tự nhiều byte.
void copyUtf8(char* dst, size_t cap, const uint8_t* src, size_t len) {
    size_t n = len < cap - 1 ? len : cap - 1;
    if (n < len) {
        while (n > 0 && (src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// Trả về true khi đã nhận đủ mọi attribute đã xin.
bool parseAttributes() {
    if (gLen < 5) return false;
    if (gBuf[0] != kCmdGetNotificationAttributes || readLe32(gBuf + 1) != gWantUid) {
        gLen = 0;  // dữ liệu thừa của yêu cầu cũ
        return false;
    }
    size_t p = 5;
    int got = 0;
    while (p + 3 <= gLen) {
        uint8_t id = gBuf[p];
        uint16_t len = gBuf[p + 1] | (gBuf[p + 2] << 8);
        if (p + 3 + len > gLen) return false;  // chờ mảnh tiếp theo
        const uint8_t* v = gBuf + p + 3;
        switch (id) {
            case ATTR_APP_ID: copyUtf8(gCur.appId, sizeof gCur.appId, v, len); break;
            case ATTR_TITLE: copyUtf8(gCur.title, sizeof gCur.title, v, len); break;
            case ATTR_SUBTITLE: copyUtf8(gCur.subtitle, sizeof gCur.subtitle, v, len); break;
            case ATTR_MESSAGE: copyUtf8(gCur.message, sizeof gCur.message, v, len); break;
            case ATTR_DATE: copyUtf8(gCur.date, sizeof gCur.date, v, len); break;
        }
        p += 3 + len;
        got++;
    }
    return got >= kAttrCount;
}

// ---- Callback BLE (chạy trong task NimBLE host: không được chặn lâu) ----

void onNotificationSource(NimBLERemoteCharacteristic*, uint8_t* d, size_t len, bool) {
    if (len < 8) return;
    Pending p{d[0], d[1], d[2], readLe32(d + 4)};
    Serial.printf("[ANCS] event=%u flags=0x%02X cat=%u uid=%lu\n", p.eventId, p.flags, p.category,
                  (unsigned long)p.uid);
    // Bỏ qua thông báo cũ iPhone gửi lại ngay khi vừa kết nối
    if (p.eventId != EVT_REMOVED && (p.flags & NOTIF_FLAG_PRE_EXISTING)) return;
    xQueueSend(gPending, &p, 0);
}

void onDataSource(NimBLERemoteCharacteristic*, uint8_t* d, size_t len, bool) {
    if (!gCollecting) return;
    if (gLen + len > sizeof gBuf) {
        gLen = 0;
        return;
    }
    memcpy(gBuf + gLen, d, len);
    gLen += len;
    if (parseAttributes()) {
        gCollecting = false;
        xEventGroupSetBits(gBits, BIT_DATA_DONE);
    }
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
        Serial.printf("[BLE] Kết nối từ %s\n", info.getAddress().toString().c_str());
        gConnHandle = info.getConnHandle();
        gConnSeq++;
        gAuthOk = false;
        xEventGroupClearBits(gBits, BIT_DISCONNECTED | BIT_AUTH_DONE | BIT_DATA_DONE);
        xEventGroupSetBits(gBits, BIT_CONNECTED);
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
        Serial.printf("[BLE] Mất kết nối, reason=0x%X\n", reason);
        gConnHandle = BLE_HS_CONN_HANDLE_NONE;
        gCollecting = false;
        xEventGroupClearBits(gBits, BIT_CONNECTED);
        // Đánh thức worker nếu nó đang chờ bất cứ thứ gì
        xEventGroupSetBits(gBits, BIT_DISCONNECTED | BIT_AUTH_DONE | BIT_DATA_DONE);
    }

    uint32_t onPassKeyDisplay() override {
        uint32_t key = esp_random() % 1000000;
        Serial.printf("[BLE] Mã PIN: %06lu\n", (unsigned long)key);
        static Event e;
        e = Event{};
        e.type = EventType::PassKey;
        e.passKey = key;
        postEvent(e);
        return key;
    }

    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        gAuthOk = info.isEncrypted();
        Serial.printf("[BLE] Bảo mật xong: encrypted=%d bonded=%d\n", info.isEncrypted(), info.isBonded());
        xEventGroupSetBits(gBits, BIT_AUTH_DONE);
    }
};

bool isConnected() { return gConnHandle != BLE_HS_CONN_HANDLE_NONE; }

// Đọc giờ hiện tại từ iPhone (CTS) để đồng hồ trên màn hình chạy đúng.
void syncClock(NimBLEClient* client) {
    NimBLERemoteService* cts = client->getService(kCtsService);
    if (!cts) return;
    NimBLERemoteCharacteristic* ch = cts->getCharacteristic(kCtsCurrentTime);
    if (!ch) return;
    NimBLEAttValue v = ch->readValue();
    if (v.size() < 7) return;
    const uint8_t* d = v.data();
    struct tm t = {};
    t.tm_year = (d[0] | (d[1] << 8)) - 1900;
    t.tm_mon = d[2] - 1;
    t.tm_mday = d[3];
    t.tm_hour = d[4];
    t.tm_min = d[5];
    t.tm_sec = d[6];
    // Giờ CTS đã là giờ địa phương, nên giữ TZ=UTC để localtime() trả lại đúng giá trị này.
    timeval tv = {mktime(&t), 0};
    settimeofday(&tv, nullptr);
    Serial.printf("[CTS] Giờ iPhone: %02d:%02d:%02d %02d/%02d/%04d\n", t.tm_hour, t.tm_min, t.tm_sec,
                  t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
}

// Hỏi iPhone nội dung của một thông báo rồi chờ Data Source trả về đủ.
void fetchNotification(NimBLERemoteCharacteristic* cp, const Pending& p) {
    const uint16_t maxTitle = sizeof gCur.title - 1;
    const uint16_t maxSub = sizeof gCur.subtitle - 1;
    const uint16_t maxMsg = sizeof gCur.message - 1;
    const uint8_t cmd[] = {
        kCmdGetNotificationAttributes,
        (uint8_t)p.uid, (uint8_t)(p.uid >> 8), (uint8_t)(p.uid >> 16), (uint8_t)(p.uid >> 24),
        ATTR_APP_ID,
        ATTR_TITLE, (uint8_t)maxTitle, (uint8_t)(maxTitle >> 8),
        ATTR_SUBTITLE, (uint8_t)maxSub, (uint8_t)(maxSub >> 8),
        ATTR_MESSAGE, (uint8_t)maxMsg, (uint8_t)(maxMsg >> 8),
        ATTR_DATE,
    };

    memset(&gCur, 0, sizeof gCur);
    gCur.uid = p.uid;
    gCur.category = p.category;
    gCur.flags = p.flags;
    gLen = 0;
    gWantUid = p.uid;
    xEventGroupClearBits(gBits, BIT_DATA_DONE);
    gCollecting = true;

    if (!cp->writeValue(cmd, sizeof cmd, true)) {
        // Thường do thông báo đã bị xoá trước khi kịp hỏi
        gCollecting = false;
        Serial.printf("[ANCS] Không lấy được uid=%lu\n", (unsigned long)p.uid);
        return;
    }
    EventBits_t bits = xEventGroupWaitBits(gBits, BIT_DATA_DONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
    gCollecting = false;
    if (!(bits & BIT_DATA_DONE) || !isConnected()) {
        Serial.printf("[ANCS] Hết thời gian chờ uid=%lu\n", (unsigned long)p.uid);
        return;
    }

    Serial.printf("[ANCS] %s | %s | %s\n", gCur.appId, gCur.title, gCur.message);
    static Event e;
    e = Event{};
    e.type = EventType::Added;
    e.notif = gCur;
    postEvent(e);
}

// Một phiên làm việc với iPhone, trả về khi mất kết nối hoặc lỗi.
void runSession() {
    uint16_t handle = gConnHandle;
    NimBLEConnInfo info = gServer->getPeerInfoByHandle(handle);

    if (!info.isEncrypted()) {
        NimBLEDevice::startSecurity(handle);  // iPhone có thể đã tự bắt đầu, lỗi "đang chạy" thì bỏ qua
        xEventGroupWaitBits(gBits, BIT_AUTH_DONE, pdFALSE, pdFALSE, pdMS_TO_TICKS(60000));
        if (!isConnected()) return;
        info = gServer->getPeerInfoByHandle(handle);
        if (!gAuthOk || !info.isEncrypted()) {
            // Thường gặp khi iPhone đã "Quên thiết bị" nhưng ESP32 còn giữ khoá cũ
            Serial.println("[BLE] Bảo mật thất bại, xoá khoá cũ để ghép đôi lại");
            NimBLEDevice::deleteBond(info.getIdAddress());
            gServer->disconnect(handle);
            return;
        }
    }

    NimBLEClient* client = gServer->getClient(info);
    if (!client) return;
    client->exchangeMTU();
    syncClock(client);

    NimBLERemoteService* svc = client->getService(kAncsService);
    if (!svc) {
        Serial.println("[ANCS] Không thấy dịch vụ ANCS. Hãy bật 'Chia sẻ thông báo hệ thống' trên iPhone.");
        xEventGroupWaitBits(gBits, BIT_DISCONNECTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
        if (isConnected()) gServer->disconnect(handle);  // kết nối lại để iPhone hỏi quyền lần nữa
        return;
    }
    NimBLERemoteCharacteristic* ns = svc->getCharacteristic(kNotificationSource);
    NimBLERemoteCharacteristic* cp = svc->getCharacteristic(kControlPoint);
    NimBLERemoteCharacteristic* ds = svc->getCharacteristic(kDataSource);
    if (!ns || !cp || !ds) {
        Serial.println("[ANCS] Thiếu characteristic");
        gServer->disconnect(handle);
        return;
    }
    // Apple khuyến nghị đăng ký Data Source trước Notification Source
    if (!ds->subscribe(true, onDataSource) || !ns->subscribe(true, onNotificationSource)) {
        Serial.println("[ANCS] Đăng ký nhận thông báo thất bại");
        gServer->disconnect(handle);
        return;
    }
    Serial.println("[ANCS] Sẵn sàng");
    postState(LinkState::Ready);

    while (isConnected()) {
        ActionReq a;
        while (xQueueReceive(gActions, &a, 0) == pdTRUE) {
            const uint8_t cmd[] = {
                kCmdPerformNotificationAction,
                (uint8_t)a.uid, (uint8_t)(a.uid >> 8), (uint8_t)(a.uid >> 16), (uint8_t)(a.uid >> 24),
                a.actionId,
            };
            bool ok = cp->writeValue(cmd, sizeof cmd, true);
            Serial.printf("[ANCS] Hành động %s uid=%lu: %s\n", a.actionId == 0 ? "đồng ý" : "từ chối/xoá",
                          (unsigned long)a.uid, ok ? "ok" : "lỗi");
        }
        Pending p;
        if (xQueueReceive(gPending, &p, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (p.eventId == EVT_REMOVED) {
            static Event e;
            e = Event{};
            e.type = EventType::Removed;
            e.uid = p.uid;
            postEvent(e);
        } else {
            fetchNotification(cp, p);
        }
    }
}

void workerTask(void*) {
    for (;;) {
        xEventGroupWaitBits(gBits, BIT_CONNECTED, pdFALSE, pdFALSE, portMAX_DELAY);
        uint32_t seq = gConnSeq;
        xQueueReset(gPending);
        xQueueReset(gActions);
        postState(LinkState::Connected);
        runSession();
        // Chờ phiên này kết thúc hẳn (hoặc iPhone đã kết nối lại thành phiên mới)
        while (gConnSeq == seq && isConnected()) vTaskDelay(pdMS_TO_TICKS(100));
        if (!isConnected()) postState(LinkState::Advertising);
    }
}

}  // namespace

void begin(const char* deviceName) {
    gEvents = xQueueCreate(6, sizeof(Event));
    gPending = xQueueCreate(16, sizeof(Pending));
    gActions = xQueueCreate(4, sizeof(ActionReq));
    gBits = xEventGroupCreate();
    setenv("TZ", "UTC0", 1);
    tzset();

    NimBLEDevice::init(deviceName);
    NimBLEDevice::setSecurityAuth(true, true, true);  // bonding + MITM + Secure Connections
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);  // ESP32 hiện PIN, iPhone nhập

    gServer = NimBLEDevice::createServer();
    gServer->setCallbacks(new ServerCallbacks());
    gServer->advertiseOnDisconnect(true);

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData advData;
    advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    advData.addData(kAdvSolicitAncs, sizeof kAdvSolicitAncs);
    NimBLEAdvertisementData scanData;
    scanData.setName(deviceName);
    adv->enableScanResponse(true);
    adv->setAdvertisementData(advData);
    adv->setScanResponseData(scanData);
    adv->start();

    xTaskCreate(workerTask, "ancs", 8192, nullptr, 3, nullptr);
    postState(LinkState::Advertising);
    Serial.printf("[BLE] Đang quảng bá với tên \"%s\", số thiết bị đã ghép đôi: %d\n", deviceName,
                  NimBLEDevice::getNumBonds());
}

bool nextEvent(Event& out) { return xQueueReceive(gEvents, &out, 0) == pdTRUE; }

void forgetBonds() {
    NimBLEDevice::deleteAllBonds();
    uint16_t h = gConnHandle;
    if (h != BLE_HS_CONN_HANDLE_NONE) gServer->disconnect(h);
    Serial.println("[BLE] Đã xoá toàn bộ ghép đôi");
}

void performAction(uint32_t uid, bool positive) {
    ActionReq a{uid, (uint8_t)(positive ? 0 : 1)};
    xQueueSend(gActions, &a, 0);
}

}  // namespace ancs
