#include "ancs_client.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>

// API host NimBLE (ble_gattc_*, ble_gap_*, ble_store_*, ble_hs_mbuf_*, ble_uuid_*); kéo theo syscfg -> MYNEWT_VAL()
#include "nimble/nimble/host/include/host/ble_hs.h"

namespace ancs {
namespace {

// ---- UUID của Apple ANCS ----
const NimBLEUUID kAncsService("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
const NimBLEUUID kNotificationSource("9FBF120D-6301-42D9-8C58-25E699A21DBD");
const NimBLEUUID kControlPoint("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
const NimBLEUUID kDataSource("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");
// Current Time trên iPhone (chỉnh đồng hồ) và tên máy trong GAP (đặt nhãn ngắn cho từng iPhone)
const NimBLEUUID kCtsCurrentTime((uint16_t)0x2A2B);
const NimBLEUUID kGapDeviceName((uint16_t)0x2A00);

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
const uint8_t kNotifyOn[2] = {0x01, 0x00};  // giá trị CCCD: bật notify

constexpr int kSlots = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
static_assert(kSlots == MAX_PHONES && kSlots <= 4, "mỗi kết nối một iPhone; chỉ số slot gói trong 2 bit");
static_assert(MYNEWT_VAL(BLE_STORE_MAX_BONDS) == MAX_PHONES, "bảng iPhone phải bằng số ghép đôi");
static_assert(MYNEWT_VAL(BLE_SM_MAX_PROCS) >= kSlots, "thiếu -DMYNEWT_VAL_BLE_SM_MAX_PROCS=3 trong platformio.ini");
static_assert(MYNEWT_VAL(BLE_GATT_MAX_PROCS) >= 2 * kSlots, "thiếu -DMYNEWT_VAL_BLE_GATT_MAX_PROCS=8");
static_assert(MYNEWT_VAL(MSYS_1_BLOCK_COUNT) >= 24, "thiếu -DCONFIG_BT_NIMBLE_MSYS1_BLOCK_COUNT=24");

constexpr uint16_t NONE = BLE_HS_CONN_HANDLE_NONE;
constexpr size_t kDsCap = 1024;
constexpr size_t kOpBufCap = 100;  // đủ cho tên iPhone dài; Read By Type trả tối đa MTU-4 byte
constexpr int kMaxChrs = 8, kPendingDepth = 32, kActionDepth = 4, kEventDepth = 8;
constexpr uint32_t kSlotStack = 6144;
constexpr UBaseType_t kSlotPrio = 3;  // trên task đọc thông báo (2): giảm rủi ro ble_gattc gửi rồi mới chèn thủ tục
constexpr uint32_t kSecureTimeoutMs = 60000, kNoAncsWaitMs = 30000, kOpTimeoutMs = 35000;  // 35 s > 30 s ATT
constexpr uint32_t kDataTimeoutMs = 3000, kIdleMs = 2000, kSecurePollMs = 250;
constexpr int kDiscoverTries = 3;           // dò dịch vụ/characteristic ANCS tối đa ngần này lần
constexpr uint32_t kDiscoverRetryMs = 500;  // nghỉ giữa các lần dò lại
constexpr bool kRequestConnParams = true;
constexpr uint16_t kItvlMin = 24, kItvlMax = 40, kLatency = 0, kSupTimeout = 500;  // 30-50 ms, 5 s

constexpr EventBits_t BIT_CONN = 1 << 0;  // slot vừa được gán kết nối mới
constexpr EventBits_t BIT_DISC = 1 << 1;  // mất kết nối (chỉ để đánh thức; connHandle mới là sự thật)
constexpr EventBits_t BIT_ENC = 1 << 2;   // có ENC_CHANGE
constexpr EventBits_t BIT_OP = 1 << 3;    // thao tác GATT đang chờ đã xong
constexpr EventBits_t BIT_DATA = 1 << 4;  // Data Source đã đủ
constexpr EventBits_t BIT_WORK = 1 << 5;  // có sự kiện Notification Source hoặc hành động mới

// ---------- Kiểu dữ liệu ----------

// Một sự kiện từ Notification Source, chờ slot task xử lý.
struct Pending {
    uint8_t eventId;
    uint8_t flags;
    uint8_t category;
    uint32_t uid;
};

// Hành động người dùng chọn trên màn hình, chờ slot task gửi sang iPhone.
struct ActionReq {
    uint32_t uid;
    uint8_t actionId;  // 0 = positive, 1 = negative
    uint32_t gen;      // phiên kết nối nhận lệnh: slot đã đổi sang kết nối khác thì bỏ, không gửi nhầm iPhone
};

struct QueueItem {  // phần tử hàng đợi gEvents: chỉ sự kiện dữ liệu
    EventType type;  // Added / Removed / PhoneForgotten
    uint8_t phone;
    bool isNew;
    uint32_t uid;
    Notification notif;
};

enum class Stage : uint8_t { Idle, Securing, Discovering, Ready };
enum : uint8_t { CHR_OTHER, CHR_NS, CHR_CP, CHR_DS };
struct ChrInfo {
    uint16_t def, val;
    uint8_t kind;
};

struct Slot {
    uint8_t index;
    EventGroupHandle_t bits;
    QueueHandle_t pending;  // Pending x32: listener -> slot task
    QueueHandle_t actions;  // ActionReq x4: UI -> slot task
    uint8_t* dsBuf;         // kDsCap byte (PSRAM nếu có)
    QueueItem* out;         // bộ đệm gửi sự kiện của riêng slot task (PSRAM nếu có)
    // ---- dùng chung, chỉ đọc/ghi trong gMux ----
    uint16_t connHandle;  // NONE = trống
    uint32_t gen;         // tăng mỗi lần gán kết nối (khác 0)
    Stage stage;
    uint8_t phone;   // NO_PHONE cho tới khi biết danh tính
    bool running;    // slot task đang chạy một phiên
    bool encrypted;  // từ onAuthenticationComplete / secure()
    bool pairing;    // ứng viên của mã PIN đang hiện
    int encStatus;   // status ENC_CHANGE gần nhất (listener)
    uint32_t connectedAt;
    uint16_t nsVal, dsVal;  // handle định tuyến thông báo (0 = chưa)
    bool collecting, dsDone;
    uint16_t dsLen;
    uint32_t wantUid;
    uint16_t nsDropped;
    uint32_t opSeq;  // thao tác GATT đang chờ
    bool opActive, opFound;
    int opStatus;
    uint16_t opH1, opH2, opLen;
    uint8_t opBuf[kOpBufCap];
    ChrInfo chrs[kMaxChrs];
    uint8_t nChrs;
    // ---- chỉ slot task ----
    uint16_t cpVal;
};

struct PhoneRec {  // 32 byte, lưu NVS
    uint8_t used, addrType;
    uint8_t addr[6];
    char label[24];
};
static_assert(sizeof(PhoneRec) == 32, "định dạng NVS");
static_assert(sizeof(PhoneRec::label) == sizeof(PhoneInfo::label), "nhãn chép thẳng sang LinkStatus");

// ---------- Biến dùng chung ----------
//
// Thứ bậc khoá: chỉ được lấy khoá bậc CAO hơn mọi khoá đang giữ.
//   1 gRegMutex (mutex) < 2 gGattMutex (mutex) < 3 gAdvMutex (recursive) < 4 gMux (spinlock, trong cùng)
// Trong gMux: không gọi hàm chặn, Serial, snprintf, malloc, xQueueSend, xEventGroupSetBits hay hàm NimBLE nào
// (ngoại lệ duy nhất: ble_hs_mbuf_to_flat trong onNotifyRx/onReadUuid, chỉ chép dữ liệu từ mbuf, không khoá gì).
// Chép trường ra, thoát khoá rồi mới làm.
// Host task (callback) chỉ lấy gMux, gGattMutex (onDisconnect, nhả ngay) và gAdvMutex (ensureAdvertising,
// unpairPeer). gGattMutex chỉ giữ quanh một lệnh NimBLE không chặn; ACK lệnh HCI và báo gửi xong ACL đến từ
// ngữ cảnh controller/transport chứ không phải host task, nên host chờ ngắn trên các khoá này không thể kẹt.

Slot gSlots[kSlots];
QueueHandle_t gEvents;  // QueueItem x8 -> loop()
NimBLEServer* gServer = nullptr;
portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;  // trường dùng chung của Slot, gReg, mã PIN, gGenCounter
SemaphoreHandle_t gGattMutex;  // [kiểm tra còn kết nối + gọi NimBLE theo conn handle] so với onDisconnect
SemaphoreHandle_t gAdvMutex;   // recursive: bật/tắt quảng bá, xoá ghép đôi
SemaphoreHandle_t gRegMutex;   // sửa bảng iPhone + ghi NVS
uint32_t gGenCounter = 0;      // gMux
int gAdvPause = 0;             // gAdvMutex
std::atomic<bool> gLinkDirty{true};
uint32_t gPkKey = 0, gPkGen = 0;  // gMux
bool gPkActive = false;           // gMux
uint32_t gPkSeen = 0;             // chỉ main task
PhoneRec gReg[MAX_PHONES];        // gMux (RAM); ghi NVS khi giữ gRegMutex
Preferences gPrefs;               // namespace "ancs", key "phones"
QueueItem* gMainItem;             // chỉ main task: nhận trong nextEvent, gửi trong forgetBonds

// ---------- Tiện ích ----------

uint32_t readLe32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

// Độ dài sau khi bỏ một ký tự UTF-8 dở dang ở cuối (chuỗi nguồn đã bị cắt ngang lúc đọc).
size_t utf8Complete(const uint8_t* s, size_t n) {
    if (n == 0) return 0;
    size_t i = n - 1;
    for (int back = 0; i > 0 && back < 3 && (s[i] & 0xC0) == 0x80; back++) i--;
    const uint8_t c = s[i];
    const size_t need = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    return i + need > n ? i : n;
}

// Bộ đệm lớn: ưu tiên PSRAM, không có thì RAM trong. Hết bộ nhớ lúc khởi động thì dừng hẳn.
void* allocBuf(size_t n) {
    void* p = psramFound() ? heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) {
        Serial.printf("[ANCS] Hết bộ nhớ (cần %u byte)\n", (unsigned)n);
        abort();
    }
    memset(p, 0, n);
    return p;
}

// Phản hồi Data Source đã đủ chưa (chỉ duyệt, không chép): 1 = đủ, 0 = chờ mảnh tiếp, -1 = dữ liệu của yêu cầu cũ.
int dsScan(const uint8_t* b, size_t n, uint32_t want) {
    if (n < 5) return 0;
    if (b[0] != kCmdGetNotificationAttributes || readLe32(b + 1) != want) return -1;
    size_t p = 5;
    int got = 0;
    while (p + 3 <= n) {
        const size_t len = b[p + 1] | (b[p + 2] << 8);
        if (p + 3 + len > n) return 0;
        p += 3 + len;
        got++;
    }
    return got >= kAttrCount ? 1 : 0;
}

// Tách các attribute (TLV) của một phản hồi Data Source đã đủ.
void parseAttributes(const uint8_t* b, size_t n, Notification& out) {
    size_t p = 5;
    while (p + 3 <= n) {
        const uint8_t id = b[p];
        const uint16_t len = b[p + 1] | (b[p + 2] << 8);
        if (p + 3 + len > n) break;
        const uint8_t* v = b + p + 3;
        switch (id) {
            case ATTR_APP_ID: copyUtf8(out.appId, sizeof out.appId, v, len); break;
            case ATTR_TITLE: copyUtf8(out.title, sizeof out.title, v, len); break;
            case ATTR_SUBTITLE: copyUtf8(out.subtitle, sizeof out.subtitle, v, len); break;
            case ATTR_MESSAGE: copyUtf8(out.message, sizeof out.message, v, len); break;
            case ATTR_DATE: copyUtf8(out.date, sizeof out.date, v, len); break;
        }
        p += 3 + len;
    }
}

// Giờ hiện tại từ iPhone (giá trị thô của characteristic 0x2A2B) để đồng hồ trên màn hình chạy đúng.
void syncClock(const uint8_t* d, size_t n) {
    if (n < 7) return;
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

// ---------- Slot: kiểm tra kết nối, chờ ----------

bool alive(const Slot& s, uint16_t h, uint32_t gen) {
    portENTER_CRITICAL(&gMux);
    const bool a = s.connHandle == h && s.gen == gen;
    portEXIT_CRITICAL(&gMux);
    return a;
}

void setStage(Slot& s, uint32_t gen, Stage st) {
    portENTER_CRITICAL(&gMux);
    if (s.gen == gen && s.connHandle != NONE) s.stage = st;
    portEXIT_CRITICAL(&gMux);
    gLinkDirty = true;
}

// Gọi NimBLE theo conn handle chỉ khi kết nối (h, gen) còn: handle không thể bị dùng lại giữa chừng
template <typename F>
bool withConn(Slot& s, uint16_t h, uint32_t gen, F f) {
    xSemaphoreTake(gGattMutex, portMAX_DELAY);
    const bool ok = alive(s, h, gen);
    if (ok) f();
    xSemaphoreGive(gGattMutex);
    return ok;
}

void disconnectIfAlive(Slot& s, uint16_t h, uint32_t gen) {
    withConn(s, h, gen, [&] { gServer->disconnect(h); });
}

// Chờ một trong các bit mask (bit được xoá khi trả về); thoát sớm khi (h, gen) không còn.
// mask = 0: chỉ chờ mất kết nối hoặc hết giờ. Bit báo nhầm không hại: nơi gọi luôn kiểm tra lại trạng thái.
EventBits_t waitBits(Slot& s, uint16_t h, uint32_t gen, EventBits_t mask, uint32_t ms) {
    const uint32_t t0 = millis();
    while (alive(s, h, gen)) {
        const uint32_t used = millis() - t0;
        if (used >= ms) return 0;
        const EventBits_t b = xEventGroupWaitBits(s.bits, mask | BIT_DISC, pdTRUE, pdFALSE,
                                                  pdMS_TO_TICKS(std::min<uint32_t>(ms - used, 250)));
        if (b & mask) return b & mask;
    }
    return 0;
}

// ---------- Thao tác GATT (client) trên iPhone, tối đa một thao tác mỗi slot ----------
//
// arg của callback = (opSeq << 2) | chỉ số slot. Callback chạy trong host task, đọc err->status ngay (đối tượng
// lỗi là biến static dùng chung) và luôn trả 0: trả khác 0 sẽ kết thúc thủ tục mà không gọi callback cuối.
// Callback trễ của thao tác cũ (hoặc lần "kết thúc" thứ hai của read_by_uuid khi hết mbuf) bị opOwner bỏ qua.

Slot* opOwner(void* arg, uint16_t conn) {  // gọi trong gMux
    const uintptr_t v = (uintptr_t)arg;
    if ((v & 3) >= (uintptr_t)kSlots) return nullptr;
    Slot& s = gSlots[v & 3];
    return (s.opActive && s.opSeq == (uint32_t)(v >> 2) && s.connHandle == conn) ? &s : nullptr;
}

void opFinishLocked(Slot& s, int st) {
    s.opActive = false;
    s.opStatus = st == BLE_HS_EDONE ? 0 : st;
}

// Callback chỉ có lần gọi cuối (MTU, ghi)
int finishOp(uint16_t conn, int st, void* arg) {
    portENTER_CRITICAL(&gMux);
    Slot* s = opOwner(arg, conn);
    if (s) opFinishLocked(*s, st);
    portEXIT_CRITICAL(&gMux);
    if (s) xEventGroupSetBits(s->bits, BIT_OP);
    return 0;
}

int onMtu(uint16_t conn, const ble_gatt_error* err, uint16_t /*mtu*/, void* arg) {
    return finishOp(conn, err->status, arg);
}

int onWrite(uint16_t conn, const ble_gatt_error* err, ble_gatt_attr* /*attr*/, void* arg) {
    return finishOp(conn, err->status, arg);
}

// Các callback dưới đây: status 0 = một mục, khác 0 (EDONE hoặc lỗi) = lần gọi cuối
int onSvc(uint16_t conn, const ble_gatt_error* err, const ble_gatt_svc* svc, void* arg) {
    const int st = err->status;
    bool fin = false;
    portENTER_CRITICAL(&gMux);
    Slot* s = opOwner(arg, conn);
    if (s) {
        if (st == 0 && svc) {
            if (!s->opFound) {
                s->opFound = true;
                s->opH1 = svc->start_handle;
                s->opH2 = svc->end_handle;
            }
        } else {
            opFinishLocked(*s, st);
            fin = true;
        }
    }
    portEXIT_CRITICAL(&gMux);
    if (fin) xEventGroupSetBits(s->bits, BIT_OP);
    return 0;
}

int onChr(uint16_t conn, const ble_gatt_error* err, const ble_gatt_chr* chr, void* arg) {
    const int st = err->status;
    uint8_t kind = CHR_OTHER;  // phân loại trước khi vào gMux (ble_uuid_cmp là hàm NimBLE)
    if (st == 0 && chr) {
        if (ble_uuid_cmp(&chr->uuid.u, kNotificationSource.getBase()) == 0) kind = CHR_NS;
        else if (ble_uuid_cmp(&chr->uuid.u, kControlPoint.getBase()) == 0) kind = CHR_CP;
        else if (ble_uuid_cmp(&chr->uuid.u, kDataSource.getBase()) == 0) kind = CHR_DS;
    }
    bool fin = false;
    portENTER_CRITICAL(&gMux);
    Slot* s = opOwner(arg, conn);
    if (s) {
        if (st == 0 && chr) {
            if (s->nChrs < kMaxChrs) s->chrs[s->nChrs++] = ChrInfo{chr->def_handle, chr->val_handle, kind};
        } else {
            opFinishLocked(*s, st);
            fin = true;
        }
    }
    portEXIT_CRITICAL(&gMux);
    if (fin) xEventGroupSetBits(s->bits, BIT_OP);
    return 0;
}

int onDsc(uint16_t conn, const ble_gatt_error* err, uint16_t /*chrVal*/, const ble_gatt_dsc* dsc, void* arg) {
    const int st = err->status;
    const bool cccd = st == 0 && dsc && dsc->uuid.u.type == BLE_UUID_TYPE_16 &&
                      ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16;
    bool fin = false;
    portENTER_CRITICAL(&gMux);
    Slot* s = opOwner(arg, conn);
    if (s) {
        if (st == 0 && dsc) {
            if (cccd && !s->opFound) {
                s->opFound = true;
                s->opH1 = dsc->handle;
            }
        } else {
            opFinishLocked(*s, st);
            fin = true;
        }
    }
    portEXIT_CRITICAL(&gMux);
    if (fin) xEventGroupSetBits(s->bits, BIT_OP);
    return 0;
}

int onReadUuid(uint16_t conn, const ble_gatt_error* err, ble_gatt_attr* attr, void* arg) {
    const int st = err->status;
    bool fin = false;
    portENTER_CRITICAL(&gMux);
    Slot* s = opOwner(arg, conn);
    if (s) {
        if (st == 0 && attr) {
            if (!s->opFound && attr->om) {
                uint16_t n = 0;
                ble_hs_mbuf_to_flat(attr->om, s->opBuf, sizeof s->opBuf, &n);  // chỉ chép; host tự giải phóng om
                s->opLen = n;
                s->opFound = true;
            }
        } else {
            opFinishLocked(*s, st);
            fin = true;
        }
    }
    portEXIT_CRITICAL(&gMux);
    if (fin) xEventGroupSetBits(s->bits, BIT_OP);
    return 0;
}

// Chạy một thủ tục GATT rồi chờ callback cuối. start(arg) gọi ble_gattc_*(..., cb, arg).
template <typename F>
int runOp(Slot& s, uint16_t h, uint32_t gen, F start) {
    int rc = BLE_HS_ENOMEM;
    for (int i = 0; i < 50 && rc == BLE_HS_ENOMEM; i++) {  // hết proc/mbuf: thử lại tối đa ~1 s
        if (i) vTaskDelay(pdMS_TO_TICKS(20));
        xSemaphoreTake(gGattMutex, portMAX_DELAY);
        void* arg = nullptr;
        portENTER_CRITICAL(&gMux);
        const bool ok = s.connHandle == h && s.gen == gen;
        if (ok) {
            s.opSeq = (s.opSeq + 1) & 0x3FFFFFFF;
            s.opActive = true;
            s.opFound = false;
            s.opStatus = BLE_HS_EUNKNOWN;
            s.opLen = 0;
            s.nChrs = 0;
            arg = (void*)(uintptr_t)((s.opSeq << 2) | s.index);
        }
        portEXIT_CRITICAL(&gMux);
        if (!ok) {
            xSemaphoreGive(gGattMutex);
            return BLE_HS_ENOTCONN;
        }
        xEventGroupClearBits(s.bits, BIT_OP);
        rc = start(arg);
        xSemaphoreGive(gGattMutex);
        if (rc != 0) {
            portENTER_CRITICAL(&gMux);
            s.opActive = false;
            portEXIT_CRITICAL(&gMux);
        }
    }
    if (rc != 0) return rc;  // lỗi ngay khi gửi: NimBLE sẽ không gọi callback
    const uint32_t t0 = millis();
    for (;;) {
        bool active;
        portENTER_CRITICAL(&gMux);
        active = s.opActive;
        rc = s.opStatus;
        portEXIT_CRITICAL(&gMux);
        if (!active) return rc;  // kể cả mất kết nối: callback ENOTCONN tới trước sự kiện DISCONNECT
        if (!alive(s, h, gen) || millis() - t0 > kOpTimeoutMs) break;
        xEventGroupWaitBits(s.bits, BIT_OP | BIT_DISC, pdTRUE, pdFALSE, pdMS_TO_TICKS(250));
    }
    portENTER_CRITICAL(&gMux);
    if (s.opActive) {
        s.opActive = false;
        rc = BLE_HS_ETIMEOUT;
    } else {
        rc = s.opStatus;
    }
    portEXIT_CRITICAL(&gMux);
    return rc;
}

// Đọc giá trị đầu tiên của characteristic có UUID này trên iPhone (Read By Type, không cần tìm dịch vụ).
// out phải chứa được kOpBufCap byte.
bool readByUuid(Slot& s, uint16_t h, uint32_t gen, const NimBLEUUID& uuid, uint8_t* out, uint16_t& n) {
    n = 0;
    const int rc = runOp(s, h, gen, [&](void* a) {
        return ble_gattc_read_by_uuid(h, 1, 0xFFFF, uuid.getBase(), onReadUuid, a);
    });
    bool found = false;
    portENTER_CRITICAL(&gMux);
    if (rc == 0 && s.opFound) {
        n = s.opLen;
        memcpy(out, s.opBuf, n);
        found = true;
    }
    portEXIT_CRITICAL(&gMux);
    return found;
}

// Tìm CCCD của characteristic có value handle val (descriptor nằm trong (val, end]). 0 = không có.
uint16_t findCccd(Slot& s, uint16_t h, uint32_t gen, uint16_t val, uint16_t end) {
    if (val >= end) return 0;
    const int rc = runOp(s, h, gen, [&](void* a) { return ble_gattc_disc_all_dscs(h, val, end, onDsc, a); });
    uint16_t found = 0;
    portENTER_CRITICAL(&gMux);
    if (rc == 0 && s.opFound) found = s.opH1;
    portEXIT_CRITICAL(&gMux);
    return found;
}

// ---------- Quảng bá và ghép đôi ----------

// Quảng bá khi còn chỗ (< 3 kết nối). Mọi lần bật quảng bá của ta đều qua đây (advertiseOnDisconnect = false).
// Thư viện vẫn có thể tự bật lại ngoài khoá này (CONNECT lỗi trong NimBLEServer, onSync sau khi host reset),
// nên chỗ nào cần chắc chắn đã tắt (unpairPeer) đều tự tắt lại và thử lại.
void ensureAdvertising() {
    xSemaphoreTakeRecursive(gAdvMutex, portMAX_DELAY);
    if (gAdvPause == 0 && gServer && gServer->getConnectedCount() < kSlots &&
        !NimBLEDevice::getAdvertising()->isAdvertising() && NimBLEDevice::startAdvertising())
        Serial.println("[BLE] Quảng bá lại: iPhone khác vẫn thấy board");
    xSemaphoreGiveRecursive(gAdvMutex);
}

// Xoá ghép đôi (ngắt luôn nếu đang kết nối). iPhone có IRK: NimBLE trả EBUSY và KHÔNG xoá gì nếu
// đang quảng bá (ble_gap.c:8317-8324) -> tạm dừng quảng bá trong lúc xoá.
bool unpairPeer(const ble_addr_t& addr) {
    xSemaphoreTakeRecursive(gAdvMutex, portMAX_DELAY);
    gAdvPause++;
    int rc = BLE_HS_EBUSY;
    for (int i = 0; i < 3 && rc == BLE_HS_EBUSY; i++) {  // thư viện có thể tự bật lại quảng bá (onSync)
        if (ble_gap_adv_active()) ble_gap_adv_stop();
        rc = ble_gap_unpair(&addr);
    }
    gAdvPause--;
    xSemaphoreGiveRecursive(gAdvMutex);
    ensureAdvertising();
    Serial.printf("[BLE] Xoá ghép đôi %s: %s (rc=%d)\n", NimBLEAddress(addr).toString().c_str(), rc == 0 ? "ok" : "lỗi",
                  rc);
    return rc == 0;
}

class DeviceCallbacks : public NimBLEDeviceCallbacks {
    // Host task; ble_hs_lock KHÔNG bị giữ (ble_store.c). FULL -> cứ ghi; OVERFLOW -> bỏ máy cũ nhất KHÔNG đang
    // kết nối (mặc định ble_store_util_status_rr có thể ngắt máy đang dùng). Mọi loại bản ghi gắn với một iPhone
    // (khoá, CCCD, CSFC, địa chỉ) đều đi đường này, không bao giờ rơi xuống ble_store_util_status_rr.
    int onStoreStatus(ble_store_status_event* ev, void* arg) override {
        if (ev->event_code != BLE_STORE_EVENT_OVERFLOW) return ble_store_util_status_rr(ev, arg);
        const ble_addr_t* keep;
        switch (ev->overflow.obj_type) {
            case BLE_STORE_OBJ_TYPE_OUR_SEC:
            case BLE_STORE_OBJ_TYPE_PEER_SEC: keep = &ev->overflow.value->sec.peer_addr; break;
            case BLE_STORE_OBJ_TYPE_CCCD: keep = &ev->overflow.value->cccd.peer_addr; break;
            case BLE_STORE_OBJ_TYPE_CSFC: keep = &ev->overflow.value->csfc.peer_addr; break;
            case BLE_STORE_OBJ_TYPE_PEER_ADDR: keep = &ev->overflow.value->rpa_rec.peer_addr; break;
            default: return ble_store_util_status_rr(ev, arg);  // loại không gắn với iPhone nào
        }
        ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
        int n = 0;
        if (ble_store_util_bonded_peers(peers, &n, MYNEWT_VAL(BLE_STORE_MAX_BONDS)) != 0) return BLE_HS_ESTORE_CAP;
        for (int i = 0; i < n; i++) {  // [0] = cũ nhất
            if (ble_addr_cmp(&peers[i], keep) == 0) continue;
            if (ble_gap_conn_find_by_addr(&peers[i], nullptr) == 0) continue;  // đang kết nối: không đụng tới
            Serial.printf("[BLE] Bộ nhớ ghép đôi đầy (loại %d): bỏ iPhone cũ nhất không kết nối\n",
                          ev->overflow.obj_type);
            return unpairPeer(peers[i]) ? 0 : BLE_HS_ESTORE_CAP;  // 0 = NimBLE ghi lại lần nữa
        }
        Serial.println("[BLE] Bộ nhớ ghép đôi đầy, mọi iPhone đều đang kết nối: không lưu khoá mới");
        return BLE_HS_ESTORE_CAP;  // không lưu khoá mới còn hơn ngắt máy đang dùng
    }
};

// Bỏ ứng viên mã PIN của slot; hết ứng viên -> PassKeyDone. Gọi trong gMux.
void clearPairingLocked(Slot& s) {
    s.pairing = false;
    bool any = false;
    for (const Slot& t : gSlots) any = any || t.pairing;
    if (!any && gPkActive) {
        gPkActive = false;
        gPkGen++;
    }
}

void clearPairing(Slot& s) {
    portENTER_CRITICAL(&gMux);
    clearPairingLocked(s);
    portEXIT_CRITICAL(&gMux);
}

// Slot task thấy kết nối đã mã hoá (kể cả khi ENC_CHANGE tới trước CONNECT nên host không ghi được vào slot):
// đánh dấu encrypted để onPassKeyDisplay không coi nó là máy đang ghép đôi, và bỏ cờ ứng viên nếu lỡ bị đặt.
void markSecured(Slot& s, uint32_t gen) {
    portENTER_CRITICAL(&gMux);
    if (s.gen == gen && s.connHandle != NONE) {
        s.encrypted = true;
        if (s.pairing) clearPairingLocked(s);
    }
    portEXIT_CRITICAL(&gMux);
}

// ---------- Callback của host (NimBLEServerCallbacks) ----------

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
        const uint16_t h = info.getConnHandle();
        const uint32_t now = millis();
        const bool enc = info.isEncrypted();  // iPhone đã ghép đôi có thể mã hoá xong trước CONNECT
        Slot* s = nullptr;
        portENTER_CRITICAL(&gMux);
        for (Slot& t : gSlots) {
            if (t.connHandle == NONE && !t.running) {
                s = &t;
                break;
            }
        }
        if (!s) {  // task của slot trống còn đang dọn phiên cũ: nó sẽ nhận BIT_CONN ngay sau đó
            for (Slot& t : gSlots) {
                if (t.connHandle == NONE) {
                    s = &t;
                    break;
                }
            }
        }
        if (s) {
            if (++gGenCounter == 0) gGenCounter = 1;
            s->connHandle = h;
            s->gen = gGenCounter;
            s->stage = Stage::Securing;
            s->phone = NO_PHONE;
            // encrypted chỉ do onAuthenticationComplete/markSecured đặt (secure() dựa vào đó, xem secured())
            s->encrypted = false;
            // CONNECT của slave chờ đọc phiên bản/tính năng từ xa nên PASSKEY_ACTION có thể tới trước: mã PIN đang hiện
            // thì kết nối mới chưa mã hoá cũng là ứng viên, để máy khác rớt/mã hoá lại không ẩn mất mã đang nhập
            s->pairing = gPkActive && !enc;
            s->encStatus = 0;
            s->connectedAt = now;
            s->nsVal = s->dsVal = 0;
            s->collecting = s->dsDone = false;
            s->dsLen = 0;
            s->opActive = false;
        }
        portEXIT_CRITICAL(&gMux);
        if (!s) {  // không xảy ra với 3 slot
            Serial.printf("[BLE] Hết chỗ cho kết nối mới (handle %u), ngắt\n", h);
            server->disconnect(h);
            return;
        }
        Serial.printf("[BLE%u] Kết nối từ %s (handle %u)\n", s->index, info.getAddress().toString().c_str(), h);
        xEventGroupClearBits(s->bits, BIT_DISC | BIT_ENC | BIT_OP | BIT_DATA | BIT_WORK);
        xEventGroupSetBits(s->bits, BIT_CONN);
        gLinkDirty = true;
        ensureAdvertising();  // slave: quảng bá đã tự dừng khi có kết nối; còn chỗ thì bật lại
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
        const uint16_t h = info.getConnHandle();
        const uint32_t now = millis();
        Slot* s = nullptr;
        uint32_t since = 0;
        xSemaphoreTake(gGattMutex, portMAX_DELAY);  // chờ slot task đang gọi NimBLE theo handle này
        portENTER_CRITICAL(&gMux);
        for (Slot& t : gSlots) {
            if (t.connHandle != h) continue;
            s = &t;
            t.connHandle = NONE;
            t.nsVal = t.dsVal = 0;
            t.collecting = t.dsDone = false;
            since = now - t.connectedAt;
            break;
        }
        portEXIT_CRITICAL(&gMux);
        xSemaphoreGive(gGattMutex);
        if (s) {
            Serial.printf("[BLE%u] Mất kết nối (handle %u) sau %lu s, reason=0x%X\n", s->index, h,
                          (unsigned long)(since / 1000), (unsigned)reason);
            clearPairing(*s);
            xEventGroupSetBits(s->bits, BIT_DISC | BIT_ENC | BIT_OP | BIT_DATA);
        } else {
            Serial.printf("[BLE] Mất kết nối (handle %u), reason=0x%X\n", h, (unsigned)reason);
        }
        gLinkDirty = true;
        ensureAdvertising();
    }

    // Không có thông tin kết nối ở đây (NimBLEServer.cpp), và PASSKEY_ACTION không tới listener:
    // đánh dấu mọi kết nối chưa mã hoá là ứng viên. Mỗi lần xin một mã mới; màn hình luôn hiện mã mới nhất.
    uint32_t onPassKeyDisplay() override {
        const uint32_t key = esp_random() % 1000000;
        portENTER_CRITICAL(&gMux);
        for (Slot& t : gSlots) {
            if (t.connHandle != NONE && !t.encrypted) t.pairing = true;
        }
        gPkKey = key;
        gPkActive = true;
        gPkGen++;
        portEXIT_CRITICAL(&gMux);
        Serial.printf("[BLE] Mã PIN: %06lu\n", (unsigned long)key);
        return key;
    }

    // Gọi cho mọi ENC_CHANGE (thành công hay lỗi), sau khi listener đã ghi status.
    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        const uint16_t h = info.getConnHandle();
        const bool enc = info.isEncrypted();
        Slot* s = nullptr;
        portENTER_CRITICAL(&gMux);
        for (Slot& t : gSlots) {
            if (t.connHandle != h) continue;
            s = &t;
            t.encrypted = enc;
            break;
        }
        portEXIT_CRITICAL(&gMux);
        if (s) {
            Serial.printf("[BLE%u] Bảo mật xong: encrypted=%d bonded=%d\n", s->index, enc, info.isBonded());
            clearPairing(*s);
            xEventGroupSetBits(s->bits, BIT_ENC);
        } else {
            Serial.printf("[BLE] Bảo mật xong (handle %u, chưa có CONNECT): encrypted=%d bonded=%d\n", h, enc,
                          info.isBonded());
        }
        gLinkDirty = true;
    }
};

// ---------- Listener GAP: định tuyến notify theo (conn_handle, attr_handle) ----------

// Host task; mbuf bị giải phóng ngay sau khi listener trả về. Không in Serial ở đây (slot task in khi xử lý).
// Sau khi collecting = false, listener không bao giờ ghi dsBuf nữa: slot task đọc dsBuf ngoài khoá.
void onNotifyRx(uint16_t conn, uint16_t attr, const os_mbuf* om) {
    const uint16_t len = OS_MBUF_PKTLEN(om);
    Slot* hit = nullptr;
    bool ns = false, done = false;
    uint8_t d[8] = {};
    portENTER_CRITICAL(&gMux);
    for (Slot& s : gSlots) {
        if (s.connHandle != conn || attr == 0) continue;
        if (attr == s.nsVal) {
            hit = &s;
            ns = true;
            if (len >= 8) ble_hs_mbuf_to_flat(om, d, 8, nullptr);  // chỉ chép
        } else if (attr == s.dsVal && s.collecting) {
            hit = &s;
            if (s.dsLen + len > kDsCap) {
                s.dsLen = 0;  // tràn: bỏ, chờ phản hồi đúng
            } else {
                uint16_t n = 0;
                ble_hs_mbuf_to_flat(om, s.dsBuf + s.dsLen, len, &n);  // chỉ chép
                s.dsLen += n;
                const int r = dsScan(s.dsBuf, s.dsLen, s.wantUid);
                if (r < 0) {
                    s.dsLen = 0;
                } else if (r > 0) {
                    s.collecting = false;
                    s.dsDone = true;
                    done = true;
                }
            }
        }
        break;
    }
    portEXIT_CRITICAL(&gMux);
    if (!hit) return;
    if (ns) {
        if (len < 8) return;
        Pending p{d[0], d[1], d[2], readLe32(d + 4)};
        // Bỏ qua thông báo cũ iPhone gửi lại ngay khi vừa kết nối
        if (p.eventId != EVT_REMOVED && (p.flags & NOTIF_FLAG_PRE_EXISTING)) return;
        if (xQueueSend(hit->pending, &p, 0) == pdTRUE) {
            xEventGroupSetBits(hit->bits, BIT_WORK);
        } else {
            portENTER_CRITICAL(&gMux);
            hit->nsDropped++;
            portEXIT_CRITICAL(&gMux);
        }
    } else if (done) {
        xEventGroupSetBits(hit->bits, BIT_DATA);
    }
}

int gapListener(ble_gap_event* ev, void*) {
    if (ev->type == BLE_GAP_EVENT_NOTIFY_RX) {
        onNotifyRx(ev->notify_rx.conn_handle, ev->notify_rx.attr_handle, ev->notify_rx.om);
    } else if (ev->type == BLE_GAP_EVENT_ENC_CHANGE) {  // listener chạy TRƯỚC onAuthenticationComplete
        portENTER_CRITICAL(&gMux);
        for (Slot& s : gSlots) {
            if (s.connHandle == ev->enc_change.conn_handle) s.encStatus = ev->enc_change.status;
        }
        portEXIT_CRITICAL(&gMux);
    }
    return 0;  // NimBLE bỏ qua giá trị trả về
}

// ---------- Gửi sự kiện cho loop() ----------

void postItem(const QueueItem& q, uint32_t ms) {
    if (xQueueSend(gEvents, &q, pdMS_TO_TICKS(ms)) != pdTRUE)
        Serial.printf("[ANCS] Hàng đợi sự kiện đầy, bỏ loại %u\n", (unsigned)q.type);
}

void postRemoved(Slot& s, uint8_t phone, uint32_t uid) {
    memset(s.out, 0, sizeof *s.out);
    s.out->type = EventType::Removed;
    s.out->phone = phone;
    s.out->uid = uid;
    postItem(*s.out, 1000);
}

void postForgotten(QueueItem* q, uint8_t phone, uint32_t ms) {
    memset(q, 0, sizeof *q);
    q->type = EventType::PhoneForgotten;
    q->phone = phone;
    postItem(*q, ms);
}

// ---------- Bảng iPhone (NVS "ancs"/"phones", 96 byte) ----------
//
// Chỉ số 0..2 gắn với địa chỉ định danh của iPhone đã ghép đôi: giữ nguyên màu và tên ngắn qua các lần kết nối lại,
// kể cả khi iPhone vào slot khác.

bool sameAddr(const PhoneRec& r, const ble_addr_t& a) {
    return r.used && r.addrType == a.type && memcmp(r.addr, a.val, sizeof r.addr) == 0;
}

bool inList(const PhoneRec& r, const ble_addr_t* list, int n) {
    for (int i = 0; i < n; i++) {
        if (sameAddr(r, list[i])) return true;
    }
    return false;
}

bool phoneConnectedLocked(uint8_t idx, const Slot* except) {  // gọi trong gMux
    for (const Slot& t : gSlots) {
        if (&t != except && t.connHandle != NONE && t.phone == idx) return true;
    }
    return false;
}

void registryPersist() {  // người gọi giữ gRegMutex
    PhoneRec copy[MAX_PHONES];
    portENTER_CRITICAL(&gMux);
    memcpy(copy, gReg, sizeof copy);
    portEXIT_CRITICAL(&gMux);
    gPrefs.putBytes("phones", copy, sizeof copy);
}

// Gọi trong begin() sau NimBLEDevice::init (khoá ghép đôi đã nạp), trước khi có task nào khác.
void registryLoad() {
    PhoneRec tmp[MAX_PHONES] = {};
    if (gPrefs.isKey("phones") && gPrefs.getBytesLength("phones") == sizeof tmp) gPrefs.getBytes("phones", tmp, sizeof tmp);
    ble_addr_t bonded[MAX_PHONES];
    int nb = 0;
    const bool bondedOk = ble_store_util_bonded_peers(bonded, &nb, MAX_PHONES) == 0;
    bool changed = false;
    for (PhoneRec& r : tmp) {
        if (r.used && bondedOk && !inList(r, bonded, nb)) {  // khoá đã bị xoá: quên luôn chỉ số này
            memset(&r, 0, sizeof r);
            changed = true;
        }
        r.label[sizeof r.label - 1] = '\0';
    }
    portENTER_CRITICAL(&gMux);
    memcpy(gReg, tmp, sizeof gReg);
    portEXIT_CRITICAL(&gMux);
    if (changed) {
        xSemaphoreTake(gRegMutex, portMAX_DELAY);
        registryPersist();
        xSemaphoreGive(gRegMutex);
    }
    for (int i = 0; i < MAX_PHONES; i++) {
        if (tmp[i].used) Serial.printf("[BLE] iPhone %d: \"%s\"\n", i + 1, tmp[i].label);
    }
}

// Slot task, sau khi mã hoá: chỉ số của iPhone có địa chỉ định danh id. iPhone mới lấy chỗ trống (hoặc chỗ của máy
// đã bị xoá ghép đôi: báo PhoneForgotten trước, nên luôn tới trước mọi Added của máy mới).
uint8_t registryAcquire(Slot& s, uint32_t gen, const ble_addr_t& id) {
    ble_addr_t bonded[MAX_PHONES];
    int nb = 0;
    const bool bondedOk = ble_store_util_bonded_peers(bonded, &nb, MAX_PHONES) == 0;
    char defLabel[MAX_PHONES][sizeof(PhoneRec::label)] = {};  // cả phần sau '\0' cũng sạch (lưu NVS nguyên khối)
    for (int i = 0; i < MAX_PHONES; i++) snprintf(defLabel[i], sizeof defLabel[i], "iPhone %d", i + 1);

    struct Dup {
        Slot* slot;
        uint16_t h;
        uint32_t gen;
    };
    Dup dups[kSlots];
    int nd = 0;
    uint8_t forgotten[MAX_PHONES];
    int nf = 0;
    uint8_t idx = NO_PHONE;
    bool changed = false;

    xSemaphoreTake(gRegMutex, portMAX_DELAY);
    portENTER_CRITICAL(&gMux);
    for (int i = 0; i < MAX_PHONES && idx == NO_PHONE; i++) {
        if (sameAddr(gReg[i], id)) idx = i;
    }
    if (idx == NO_PHONE && bondedOk) {  // máy mới: dọn chỗ của máy không còn ghép đôi (bị thay chỗ / xoá)
        for (int i = 0; i < MAX_PHONES; i++) {
            if (gReg[i].used && !inList(gReg[i], bonded, nb) && !phoneConnectedLocked(i, &s)) {
                memset(&gReg[i], 0, sizeof gReg[i]);
                forgotten[nf++] = i;
                changed = true;
            }
        }
    }
    for (int i = 0; i < MAX_PHONES && idx == NO_PHONE; i++) {
        if (!gReg[i].used) idx = i;
    }
    for (int i = 0; i < MAX_PHONES && idx == NO_PHONE; i++) {  // dự phòng: lấy chỗ của máy không đang kết nối
        if (!phoneConnectedLocked(i, &s)) {
            idx = i;
            forgotten[nf++] = i;
        }
    }
    if (idx != NO_PHONE) {
        PhoneRec& r = gReg[idx];
        if (!sameAddr(r, id)) {
            r.used = 1;
            r.addrType = id.type;
            memcpy(r.addr, id.val, sizeof r.addr);
            memcpy(r.label, defLabel[idx], sizeof r.label);
            changed = true;
        }
        if (s.gen == gen) {
            s.phone = idx;
            for (Slot& t : gSlots) {  // kết nối cũ của cùng iPhone còn treo (chưa hết supervision timeout)
                if (&t != &s && t.connHandle != NONE && t.phone == idx) dups[nd++] = Dup{&t, t.connHandle, t.gen};
            }
        }
    }
    portEXIT_CRITICAL(&gMux);
    if (changed) registryPersist();
    xSemaphoreGive(gRegMutex);

    for (int i = 0; i < nf; i++) {
        Serial.printf("[ANCS%u] Quên iPhone %d (không còn ghép đôi)\n", s.index, forgotten[i] + 1);
        postForgotten(s.out, forgotten[i], 5000);
    }
    for (int i = 0; i < nd; i++) {
        Serial.printf("[ANCS%u] Ngắt kết nối cũ của cùng iPhone (slot %u)\n", s.index, dups[i].slot->index);
        disconnectIfAlive(*dups[i].slot, dups[i].h, dups[i].gen);
    }
    gLinkDirty = true;
    return idx;
}

// Tên máy -> nhãn ngắn: "Cường's iPhone" / "iPhone của Cường" -> "Cường"; không còn gì thì "iPhone N".
void shortLabel(const uint8_t* raw, size_t len, uint8_t idx, char* out, size_t cap) {
    char t[kOpBufCap + 1];
    copyUtf8(t, sizeof t, raw, utf8Complete(raw, len));  // bỏ ký tự dở dang nếu tên bị cắt lúc đọc
    auto trim = [](char* s) -> char* {
        while (*s == ' ' || *s == '\t') s++;
        size_t n = strlen(s);
        while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
        return s;
    };
    char* p = trim(t);
    static const char* const kSuffixes[] = {"'s iPhone", "\xE2\x80\x99s iPhone"};
    for (const char* suf : kSuffixes) {
        const size_t n = strlen(p), k = strlen(suf);
        if (n > k && strncasecmp(p + n - k, suf, k) == 0) {
            p[n - k] = '\0';
            break;
        }
    }
    static const char kPrefix[] = "iPhone c\xE1\xBB\xA7" "a ";  // "iPhone của "
    const size_t k = sizeof kPrefix - 1;
    if (strlen(p) > k && strncasecmp(p, kPrefix, k) == 0) p += k;
    p = trim(p);
    if (!*p || strcasecmp(p, "iPhone") == 0) snprintf(out, cap, "iPhone %d", idx + 1);
    else copyUtf8(out, cap, (const uint8_t*)p, strlen(p));
}

// Đặt nhãn cho iPhone idx từ tên máy (GAP Device Name). Trùng nhãn máy khác thì thêm " N".
void registrySetLabel(uint8_t idx, const ble_addr_t& id, const uint8_t* raw, size_t len) {
    if (idx >= MAX_PHONES) return;
    char label[sizeof(PhoneRec::label)] = {};  // cả phần sau '\0' cũng sạch (lưu NVS nguyên khối)
    shortLabel(raw, len, idx, label, sizeof label);

    xSemaphoreTake(gRegMutex, portMAX_DELAY);
    char others[MAX_PHONES][sizeof(PhoneRec::label)];
    bool used[MAX_PHONES];
    portENTER_CRITICAL(&gMux);
    for (int i = 0; i < MAX_PHONES; i++) {
        used[i] = gReg[i].used;
        memcpy(others[i], gReg[i].label, sizeof others[i]);
    }
    portEXIT_CRITICAL(&gMux);
    bool clash = false;
    for (int i = 0; i < MAX_PHONES; i++) clash = clash || (i != idx && used[i] && strcmp(others[i], label) == 0);
    if (clash) {
        char suffix[6];
        const int sl = snprintf(suffix, sizeof suffix, " %d", idx + 1);
        size_t n = strlen(label);
        if (n + sl > sizeof label - 1) {  // cắt ở ranh giới ký tự UTF-8 để vừa 23 byte
            n = sizeof label - 1 - sl;
            while (n > 0 && ((uint8_t)label[n] & 0xC0) == 0x80) n--;
        }
        memcpy(label + n, suffix, sl + 1);
    }
    bool changed = false;
    portENTER_CRITICAL(&gMux);
    PhoneRec& r = gReg[idx];
    if (sameAddr(r, id) && strncmp(r.label, label, sizeof r.label) != 0) {  // chỉ số vẫn thuộc iPhone này
        memcpy(r.label, label, sizeof r.label);
        changed = true;
    }
    portEXIT_CRITICAL(&gMux);
    if (changed) {
        registryPersist();
        gLinkDirty = true;
        Serial.printf("[BLE] iPhone %d: \"%s\"\n", idx + 1, label);
    }
    xSemaphoreGive(gRegMutex);
}

// ---------- Phiên làm việc của một slot ----------

// Lỗi tạm thời (hết giờ, gõ sai mã, mất kết nối...): không xoá khoá ghép đôi.
bool transientSecError(int st) {
    switch (st) {
        case 0:  // không có ENC_CHANGE nào
        case BLE_HS_ETIMEOUT:
        case BLE_HS_ENOTCONN:
        case BLE_HS_ENOMEM:
        case BLE_HS_EBUSY:
        case BLE_HS_EALREADY:
        case BLE_HS_SM_US_ERR(BLE_SM_ERR_PASSKEY):
        case BLE_HS_SM_PEER_ERR(BLE_SM_ERR_PASSKEY):
        case BLE_HS_SM_US_ERR(BLE_SM_ERR_CONFIRM_MISMATCH):
        case BLE_HS_SM_PEER_ERR(BLE_SM_ERR_CONFIRM_MISMATCH): return true;
        default: return false;
    }
}

// Đã mã hoá VÀ danh tính iPhone đã chốt. Ghép đôi mới: controller báo mã hoá xong là sec_state.encrypted = 1 ngay
// (ble_sm.c:1267) trong khi iPhone còn đang gửi IRK + địa chỉ định danh; tới ble_sm_persist_keys (ble_sm.c:548)
// peer_id_addr mới đổi từ địa chỉ ngẫu nhiên (RPA) sang địa chỉ định danh và khoá mới được lưu (kể cả bỏ máy cũ khi
// bộ nhớ đầy), rồi mới có ENC_CHANGE. Nhận sớm thì bảng iPhone ghi theo RPA, lần kết nối sau iPhone bị "quên".
// Khôi phục khoá cũ (ENC_RESTORE) đặt bonded = 1 cùng lúc với encrypted, địa chỉ đã phân giải sẵn.
// encOk: slot đã nhận ENC_CHANGE thành công (onAuthenticationComplete chạy SAU persist_keys).
bool secured(const ble_gap_conn_desc& d, bool encOk) {
    if (!d.sec_state.encrypted) return false;
    return encOk || (d.sec_state.bonded && NimBLEDevice::isBonded(NimBLEAddress(d.peer_id_addr)));
}

// Chờ kết nối được mã hoá (ghép đôi mới hoặc khôi phục khoá cũ).
bool secure(Slot& s, uint16_t h, uint32_t gen) {
    ble_gap_conn_desc d;
    // Xem sec_state của chính kết nối TRƯỚC: iPhone đã ghép đôi có thể mã hoá lại ngay, ENC_CHANGE tới trước CONNECT
    // (CONNECT của slave chờ đọc phiên bản/tính năng từ xa) nên encStatus/encrypted của slot có thể không bao giờ
    // được ghi cho lần mã hoá đó.
    if (ble_gap_conn_find(h, &d) != 0) return false;
    if (secured(d, false)) {
        markSecured(s, gen);
        return true;
    }
    withConn(s, h, gen, [&] { NimBLEDevice::startSecurity(h); });  // EALREADY (iPhone tự bắt đầu) cũng được
    const uint32_t t0 = millis();
    int st = 0;
    for (;;) {
        const uint32_t used = millis() - t0;
        if (used >= kSecureTimeoutMs) break;
        // Mỗi 250 ms xem lại sec_state (khôi phục khoá mà ENC_CHANGE không tới slot), không chỉ trông vào ENC_CHANGE;
        // riêng sec_state.encrypted thì chưa đủ (đang trao khoá, xem secured())
        waitBits(s, h, gen, BIT_ENC, std::min<uint32_t>(kSecureTimeoutMs - used, kSecurePollMs));
        if (!alive(s, h, gen) || ble_gap_conn_find(h, &d) != 0) return false;
        bool enc;
        portENTER_CRITICAL(&gMux);
        enc = s.encrypted;  // trước khi secure() trả về, chỉ onAuthenticationComplete đặt true
        st = s.encStatus;
        portEXIT_CRITICAL(&gMux);
        if (secured(d, enc && st == 0)) {
            markSecured(s, gen);
            return true;
        }
        if (st != 0) break;  // ENC_CHANGE báo lỗi
    }
    // Xoá khoá chỉ khi lỗi khoá thật với máy đã ghép đôi (vd. iPhone đã "Quên thiết bị này" rồi ghép với khoá khác).
    // iPhone quên board nhưng board còn khoá (ghép đôi lặp lại) thì thư viện tự xử lý (REPEAT_PAIRING).
    if (!transientSecError(st) && NimBLEDevice::isBonded(NimBLEAddress(d.peer_id_addr))) {
        Serial.printf("[BLE%u] Bảo mật thất bại (0x%X), xoá khoá cũ để ghép đôi lại\n", s.index, (unsigned)st);
        unpairPeer(d.peer_id_addr);  // ngắt luôn kết nối
    } else {
        if (st == 0) Serial.printf("[BLE%u] Hết thời gian chờ mã hoá, ngắt kết nối, giữ ghép đôi\n", s.index);
        else Serial.printf("[BLE%u] Bảo mật thất bại (0x%X), ngắt kết nối, giữ ghép đôi\n", s.index, (unsigned)st);
        disconnectIfAlive(s, h, gen);
    }
    return false;
}

// Hỏi iPhone nội dung của một thông báo rồi chờ Data Source trả về đủ.
void fetch(Slot& s, uint16_t h, uint32_t gen, uint8_t phone, const Pending& p) {
    const uint16_t maxTitle = sizeof(Notification::title) - 1;
    const uint16_t maxSub = sizeof(Notification::subtitle) - 1;
    const uint16_t maxMsg = sizeof(Notification::message) - 1;
    const uint8_t cmd[] = {
        kCmdGetNotificationAttributes,
        (uint8_t)p.uid, (uint8_t)(p.uid >> 8), (uint8_t)(p.uid >> 16), (uint8_t)(p.uid >> 24),
        ATTR_APP_ID,
        ATTR_TITLE, (uint8_t)maxTitle, (uint8_t)(maxTitle >> 8),
        ATTR_SUBTITLE, (uint8_t)maxSub, (uint8_t)(maxSub >> 8),
        ATTR_MESSAGE, (uint8_t)maxMsg, (uint8_t)(maxMsg >> 8),
        ATTR_DATE,
    };

    xEventGroupClearBits(s.bits, BIT_DATA);
    bool armed = false;
    portENTER_CRITICAL(&gMux);
    if (s.gen == gen && s.connHandle == h) {
        s.dsLen = 0;
        s.wantUid = p.uid;
        s.dsDone = false;
        s.collecting = true;
        armed = true;
    }
    portEXIT_CRITICAL(&gMux);
    if (!armed) return;

    const int rc = runOp(s, h, gen, [&](void* a) {
        return ble_gattc_write_flat(h, s.cpVal, cmd, sizeof cmd, onWrite, a);
    });
    if (rc == 0) waitBits(s, h, gen, BIT_DATA, kDataTimeoutMs);

    bool done;
    size_t n;
    portENTER_CRITICAL(&gMux);
    done = s.dsDone;
    n = s.dsLen;
    if (s.gen == gen) {
        s.collecting = false;
        s.dsDone = false;
    }
    portEXIT_CRITICAL(&gMux);
    if (rc != 0) {  // thường do thông báo đã bị xoá trước khi kịp hỏi (ATT 0xA2)
        Serial.printf("[ANCS%u] Không lấy được uid=%lu (rc=0x%X)\n", s.index, (unsigned long)p.uid, (unsigned)rc);
        return;
    }
    if (!done) {
        Serial.printf("[ANCS%u] Hết thời gian chờ uid=%lu\n", s.index, (unsigned long)p.uid);
        return;
    }

    QueueItem& q = *s.out;
    memset(&q, 0, sizeof q);
    q.type = EventType::Added;
    q.phone = phone;
    q.isNew = p.eventId == EVT_ADDED;
    q.uid = p.uid;
    q.notif.uid = p.uid;
    q.notif.category = p.category;
    q.notif.flags = p.flags;
    q.notif.phone = phone;
    parseAttributes(s.dsBuf, n, q.notif);
    Serial.printf("[ANCS%u] iPhone %d: %s | %s | %s\n", s.index, phone + 1, q.notif.appId, q.notif.title,
                  q.notif.message);
    postItem(q, 1000);
}

// Gửi các hành động người dùng đã chọn (Nghe máy / Từ chối / Xoá) sang iPhone.
void doActions(Slot& s, uint16_t h, uint32_t gen) {
    ActionReq a;
    while (alive(s, h, gen) && xQueueReceive(s.actions, &a, 0) == pdTRUE) {
        if (a.gen != gen) {  // lệnh cho kết nối trước của slot này (có thể đã là iPhone khác)
            Serial.printf("[ANCS%u] Bỏ hành động uid=%lu của kết nối trước\n", s.index, (unsigned long)a.uid);
            continue;
        }
        const uint8_t cmd[] = {
            kCmdPerformNotificationAction,
            (uint8_t)a.uid, (uint8_t)(a.uid >> 8), (uint8_t)(a.uid >> 16), (uint8_t)(a.uid >> 24),
            a.actionId,
        };
        const int rc = runOp(s, h, gen, [&](void* arg) {
            return ble_gattc_write_flat(h, s.cpVal, cmd, sizeof cmd, onWrite, arg);
        });
        if (rc == 0)
            Serial.printf("[ANCS%u] Hành động %s uid=%lu: ok\n", s.index, a.actionId == 0 ? "đồng ý" : "từ chối/xoá",
                          (unsigned long)a.uid);
        else
            Serial.printf("[ANCS%u] Hành động %s uid=%lu: lỗi (rc=0x%X)\n", s.index,
                          a.actionId == 0 ? "đồng ý" : "từ chối/xoá", (unsigned long)a.uid, (unsigned)rc);
    }
}

// Stack còn trống ít nhất của host task NimBLE (byte), để kiểm tra CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE
// (onStoreStatus -> unpairPeer chạy lồng trong lúc lưu khoá là đường sâu nhất)
unsigned hostStackLeft() {
    TaskHandle_t t = xTaskGetHandle("nimble_host");
    return t ? (unsigned)uxTaskGetStackHighWaterMark(t) : 0;
}

// Một phiên với một iPhone; dừng ở lỗi đầu tiên (finishSession dọn dẹp).
void runSession(Slot& s, uint16_t h, uint32_t gen) {
    xQueueReset(s.pending);
    xQueueReset(s.actions);
    portENTER_CRITICAL(&gMux);
    s.nsDropped = 0;
    portEXIT_CRITICAL(&gMux);
    Serial.printf("[ANCS%u] Bắt đầu phiên (handle %u)\n", s.index, h);

    if (!secure(s, h, gen)) return;
    ensureAdvertising();  // ghép đôi máy mới (thêm IRK) làm NimBLE ngầm dừng quảng bá

    // Danh tính: peer_id_addr đã là địa chỉ định danh (type 0/1) sau ENC_CHANGE (trao khoá xong), xem secured()
    ble_gap_conn_desc d;
    if (ble_gap_conn_find(h, &d) != 0) return;
    const uint8_t phone = registryAcquire(s, gen, d.peer_id_addr);
    Serial.printf("[ANCS%u] iPhone %d (%s)\n", s.index, phone + 1, NimBLEAddress(d.peer_id_addr).toString().c_str());
    setStage(s, gen, Stage::Discovering);

    int rc = runOp(s, h, gen, [&](void* a) { return ble_gattc_exchange_mtu(h, onMtu, a); });
    if (rc != 0 && rc != BLE_HS_EALREADY) Serial.printf("[ANCS%u] Đổi MTU lỗi rc=0x%X\n", s.index, (unsigned)rc);

    uint8_t buf[kOpBufCap];
    uint16_t n = 0;
    if (readByUuid(s, h, gen, kCtsCurrentTime, buf, n) && n >= 7) syncClock(buf, n);
    if (readByUuid(s, h, gen, kGapDeviceName, buf, n) && n) registrySetLabel(phone, d.peer_id_addr, buf, n);

    // Dịch vụ ANCS + characteristic. Ngay sau khi mã hoá, iOS có thể đang đổi bảng thuộc tính (ANCS chỉ hiện khi
    // đã mã hoá) nên lần dò đầu đôi khi nhận phản hồi lệch (BLE_HS_EBADDATA): dò lại vài lần trước khi bỏ cuộc.
    bool found = false;
    uint16_t svcStart = 0, svcEnd = 0;
    ChrInfo chrs[kMaxChrs];
    int nc = 0, ins = -1, icp = -1, ids = -1;
    for (int attempt = 0; attempt < kDiscoverTries; attempt++) {
        if (attempt > 0) {
            Serial.printf("[ANCS%u] Dò lại dịch vụ ANCS (lần %d)\n", s.index, attempt + 1);
            waitBits(s, h, gen, 0, kDiscoverRetryMs);
            if (!alive(s, h, gen)) return;
        }
        rc = runOp(s, h, gen,
                   [&](void* a) { return ble_gattc_disc_svc_by_uuid(h, kAncsService.getBase(), onSvc, a); });
        portENTER_CRITICAL(&gMux);
        found = rc == 0 && s.opFound;
        svcStart = s.opH1;
        svcEnd = s.opH2;
        portEXIT_CRITICAL(&gMux);
        if (!found) {
            if (rc != 0 && alive(s, h, gen)) {
                Serial.printf("[ANCS%u] Tìm dịch vụ ANCS lỗi rc=0x%X\n", s.index, (unsigned)rc);
                continue;
            }
            break;  // không có dịch vụ ANCS (chưa cho phép) hoặc mất kết nối
        }

        // Characteristic (trả về theo thứ tự handle tăng dần)
        rc = runOp(s, h, gen, [&](void* a) { return ble_gattc_disc_all_chrs(h, svcStart, svcEnd, onChr, a); });
        portENTER_CRITICAL(&gMux);
        nc = s.nChrs;
        memcpy(chrs, s.chrs, sizeof chrs);
        portEXIT_CRITICAL(&gMux);
        ins = icp = ids = -1;
        for (int i = 0; i < nc; i++) {
            if (chrs[i].kind == CHR_NS) ins = i;
            else if (chrs[i].kind == CHR_CP) icp = i;
            else if (chrs[i].kind == CHR_DS) ids = i;
        }
        if (rc == 0 && ins >= 0 && icp >= 0 && ids >= 0) break;
        Serial.printf("[ANCS%u] Thiếu characteristic (rc=0x%X)\n", s.index, (unsigned)rc);
        found = false;
        if (!alive(s, h, gen)) return;
    }
    if (!found) {
        if (!alive(s, h, gen)) return;
        if (nc > 0 || rc != 0) return;  // dò lỗi nhiều lần: finishSession ngắt, iPhone tự kết nối lại
        Serial.printf("[ANCS%u] Không thấy dịch vụ ANCS. Hãy bật 'Chia sẻ thông báo hệ thống' trên iPhone.\n",
                      s.index);
        waitBits(s, h, gen, 0, kNoAncsWaitMs);
        return;  // finishSession ngắt kết nối để iPhone hỏi quyền lần nữa
    }
    auto endOf = [&](int i) -> uint16_t { return i + 1 < nc ? chrs[i + 1].def - 1 : svcEnd; };

    // CCCD: tìm thật, không đoán val+1
    const uint16_t dsCccd = findCccd(s, h, gen, chrs[ids].val, endOf(ids));
    const uint16_t nsCccd = dsCccd ? findCccd(s, h, gen, chrs[ins].val, endOf(ins)) : 0;
    if (!dsCccd || !nsCccd) {
        Serial.printf("[ANCS%u] Không thấy CCCD (NS %u, DS %u)\n", s.index, nsCccd, dsCccd);
        return;
    }

    // Định tuyến trước khi đăng ký để notify đến sớm không bị bỏ
    s.cpVal = chrs[icp].val;
    portENTER_CRITICAL(&gMux);
    if (s.gen == gen && s.connHandle == h) {
        s.nsVal = chrs[ins].val;
        s.dsVal = chrs[ids].val;
    }
    portEXIT_CRITICAL(&gMux);

    // Apple khuyến nghị đăng ký Data Source trước Notification Source
    rc = runOp(s, h, gen, [&](void* a) {
        return ble_gattc_write_flat(h, dsCccd, kNotifyOn, sizeof kNotifyOn, onWrite, a);
    });
    if (rc == 0) {
        rc = runOp(s, h, gen, [&](void* a) {
            return ble_gattc_write_flat(h, nsCccd, kNotifyOn, sizeof kNotifyOn, onWrite, a);
        });
    }
    if (rc != 0) {
        Serial.printf("[ANCS%u] Đăng ký nhận thông báo thất bại rc=0x%X\n", s.index, (unsigned)rc);
        return;
    }

    setStage(s, gen, Stage::Ready);
    // Trong giới hạn Apple cho phụ kiện; iOS có thể từ chối, không sao
    if (kRequestConnParams) {
        withConn(s, h, gen, [&] { gServer->updateConnParams(h, kItvlMin, kItvlMax, kLatency, kSupTimeout); });
    }
    ensureAdvertising();
    Serial.printf("[ANCS%u] Sẵn sàng (iPhone %d), stack còn %u, nimble_host còn %u\n", s.index, phone + 1,
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr), hostStackLeft());

    while (alive(s, h, gen)) {
        const EventBits_t b = waitBits(s, h, gen, BIT_WORK, kIdleMs);
        if (!alive(s, h, gen)) break;
        if (!b) ensureAdvertising();  // rảnh 2 s: quảng bá có thể đã bị dừng ngầm (thêm IRK, rớt kết nối trước CONNECT)
        doActions(s, h, gen);
        Pending p;
        while (alive(s, h, gen) && xQueueReceive(s.pending, &p, 0) == pdTRUE) {
            Serial.printf("[ANCS%u] event=%u flags=0x%02X cat=%u uid=%lu\n", s.index, p.eventId, p.flags, p.category,
                          (unsigned long)p.uid);
            if (p.eventId == EVT_REMOVED) postRemoved(s, phone, p.uid);
            else fetch(s, h, gen, phone, p);
            doActions(s, h, gen);  // Dismiss không phải chờ hết loạt tin
        }
        uint16_t dropped;
        portENTER_CRITICAL(&gMux);
        dropped = s.nsDropped;
        s.nsDropped = 0;
        portEXIT_CRITICAL(&gMux);
        if (dropped) Serial.printf("[ANCS%u] Bỏ %u sự kiện vì hàng đợi đầy\n", s.index, dropped);
    }
}

void finishSession(Slot& s, uint16_t h, uint32_t gen) {
    while (alive(s, h, gen)) {  // phiên kết thúc khi còn nối: ngắt hẳn
        disconnectIfAlive(s, h, gen);
        waitBits(s, h, gen, 0, 2000);
    }
    portENTER_CRITICAL(&gMux);
    if (s.gen == gen) {
        s.stage = Stage::Idle;
        s.phone = NO_PHONE;
        s.collecting = false;
        s.nsVal = s.dsVal = 0;
    }
    s.running = false;
    portEXIT_CRITICAL(&gMux);
    gLinkDirty = true;
    Serial.printf("[ANCS%u] Hết phiên, stack còn %u byte, nimble_host còn %u\n", s.index,
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr), hostStackLeft());
}

void slotTask(void* arg) {
    Slot& s = *static_cast<Slot*>(arg);
    for (;;) {
        if (!(xEventGroupWaitBits(s.bits, BIT_CONN, pdTRUE, pdFALSE, pdMS_TO_TICKS(kIdleMs)) & BIT_CONN)) {
            ensureAdvertising();
            continue;
        }
        uint16_t h;
        uint32_t gen;
        portENTER_CRITICAL(&gMux);
        h = s.connHandle;
        gen = s.gen;
        if (h != NONE) s.running = true;
        portEXIT_CRITICAL(&gMux);
        if (h == NONE) continue;
        runSession(s, h, gen);
        finishSession(s, h, gen);
    }
}

// Trạng thái kết nối tổng hợp, dựng từ trạng thái thật (không qua hàng đợi nên không bao giờ mất).
void buildLink(LinkStatus& st) {
    memset(&st, 0, sizeof st);
    portENTER_CRITICAL(&gMux);
    for (const Slot& s : gSlots) {
        if (s.connHandle == NONE) continue;
        st.connected++;
        if (s.phone < MAX_PHONES) {
            st.phones[s.phone].connected = true;
            if (s.stage == Stage::Ready) st.phones[s.phone].ready = true;
        }
    }
    for (int i = 0; i < MAX_PHONES; i++) {
        st.phones[i].known = gReg[i].used;
        memcpy(st.phones[i].label, gReg[i].label, sizeof st.phones[i].label);
    }
    portEXIT_CRITICAL(&gMux);
    for (const PhoneInfo& p : st.phones) {
        st.ready += p.ready;
        st.known += p.known;
    }
    st.advertising = st.connected < kSlots;
    st.state = st.ready ? LinkState::Ready : st.connected ? LinkState::Connected : LinkState::Advertising;
}

}  // namespace

void begin(const char* deviceName) {
    gEvents = xQueueCreate(kEventDepth, sizeof(QueueItem));
    gGattMutex = xSemaphoreCreateMutex();
    gAdvMutex = xSemaphoreCreateRecursiveMutex();
    gRegMutex = xSemaphoreCreateMutex();
    gMainItem = (QueueItem*)allocBuf(sizeof(QueueItem));
    setenv("TZ", "UTC0", 1);
    tzset();
    for (int i = 0; i < kSlots; i++) {
        Slot& s = gSlots[i];
        s.index = i;
        s.connHandle = NONE;
        s.phone = NO_PHONE;
        s.bits = xEventGroupCreate();
        s.pending = xQueueCreate(kPendingDepth, sizeof(Pending));
        s.actions = xQueueCreate(kActionDepth, sizeof(ActionReq));
        s.dsBuf = (uint8_t*)allocBuf(kDsCap);
        s.out = (QueueItem*)allocBuf(sizeof(QueueItem));
    }

    NimBLEDevice::init(deviceName);                          // chờ tới khi host đồng bộ với controller
    NimBLEDevice::setSecurityAuth(true, true, true);         // bonding + MITM + Secure Connections
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);  // ESP32 hiện PIN, iPhone nhập
    static DeviceCallbacks devCb;
    NimBLEDevice::setDeviceCallbacks(&devCb);
    NimBLEDevice::setCustomGapHandler(gapListener);  // trước khi quảng bá

    // Chỉ dùng NimBLEServer cho quảng bá, bảo mật và callback kết nối; không dùng client của server (m_pClient luôn
    // null), mọi thao tác GATT với iPhone đi thẳng qua ble_gattc_*
    gServer = NimBLEDevice::createServer();
    gServer->setCallbacks(new ServerCallbacks());
    gServer->advertiseOnDisconnect(false);  // mọi lần bật quảng bá đều qua ensureAdvertising()

    gPrefs.begin("ancs", false);
    registryLoad();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData advData;
    advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    advData.addData(kAdvSolicitAncs, sizeof kAdvSolicitAncs);
    NimBLEAdvertisementData scanData;
    scanData.setName(deviceName);
    adv->enableScanResponse(true);
    adv->setAdvertisementData(advData);
    adv->setScanResponseData(scanData);

    static const char* const kNames[kSlots] = {"ancs0", "ancs1", "ancs2"};
    for (int i = 0; i < kSlots; i++) xTaskCreate(slotTask, kNames[i], kSlotStack, &gSlots[i], kSlotPrio, nullptr);

    ensureAdvertising();
    gLinkDirty = true;
    Serial.printf("[BLE] Đang quảng bá với tên \"%s\", số thiết bị đã ghép đôi: %d\n", deviceName,
                  NimBLEDevice::getNumBonds());
}

// Hàng đợi trước, rồi mã PIN, rồi trạng thái kết nối (hai loại sau dựng từ trạng thái thật, không bao giờ mất).
bool nextEvent(Event& out) {
    if (xQueueReceive(gEvents, gMainItem, 0) == pdTRUE) {
        out.type = gMainItem->type;
        out.phone = gMainItem->phone;
        out.isNew = gMainItem->isNew;
        out.uid = gMainItem->uid;
        if (out.type == EventType::Added) out.notif = gMainItem->notif;
        return true;
    }
    uint32_t gen, key;
    bool active;
    portENTER_CRITICAL(&gMux);
    gen = gPkGen;
    key = gPkKey;
    active = gPkActive;
    portEXIT_CRITICAL(&gMux);
    if (gen != gPkSeen) {
        gPkSeen = gen;
        out.type = active ? EventType::PassKey : EventType::PassKeyDone;
        out.passKey = key;
        return true;
    }
    if (gLinkDirty.exchange(false)) {
        buildLink(out.link);
        out.type = EventType::Link;
        return true;
    }
    return false;
}

bool performAction(uint8_t phone, uint32_t uid, bool positive) {
    Slot* best = nullptr;
    uint32_t gen = 0;
    portENTER_CRITICAL(&gMux);
    for (Slot& s : gSlots) {
        // Không cần Ready: s.phone chỉ có sau khi biết là iPhone nào (registryAcquire); lệnh chờ trong hàng đợi, tới
        // Ready mới gửi (runSession xoá hàng đợi trước secure(), ActionReq.gen bỏ lệnh của kết nối trước)
        if (s.connHandle == NONE || s.phone != phone) continue;
        if (!best || s.gen > gen) {
            best = &s;
            gen = s.gen;
        }
    }
    portEXIT_CRITICAL(&gMux);
    if (!best) {
        Serial.printf("[ANCS] iPhone %d không kết nối, bỏ qua hành động uid=%lu\n", phone + 1, (unsigned long)uid);
        return false;
    }
    ActionReq a{uid, (uint8_t)(positive ? 0 : 1), gen};
    if (xQueueSend(best->actions, &a, 0) != pdTRUE) return false;
    xEventGroupSetBits(best->bits, BIT_WORK);
    return true;
}

void forgetBonds() {
    // Xoá từng máy qua unpairPeer (dừng quảng bá, thử lại khi EBUSY) thay cho deleteAllBonds(): hàm đó dừng ở lỗi
    // đầu tiên, mà iPhone có IRK thì ble_gap_unpair trả EBUSY khi đang quảng bá, và thư viện có thể tự bật lại
    // quảng bá ngoài gAdvMutex (CONNECT lỗi trong NimBLEServer, onSync).
    ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int n = 0;
    bool ok = ble_store_util_bonded_peers(peers, &n, MYNEWT_VAL(BLE_STORE_MAX_BONDS)) == 0;
    for (int i = 0; i < n; i++) ok = unpairPeer(peers[i]) && ok;  // ngắt luôn iPhone đó nếu đang kết nối

    // Ngắt nốt các kết nối chưa ghép đôi
    for (Slot& s : gSlots) {
        uint16_t h;
        uint32_t gen;
        portENTER_CRITICAL(&gMux);
        h = s.connHandle;
        gen = s.gen;
        portEXIT_CRITICAL(&gMux);
        if (h != NONE) disconnectIfAlive(s, h, gen);
    }

    xSemaphoreTake(gRegMutex, portMAX_DELAY);
    portENTER_CRITICAL(&gMux);
    memset(gReg, 0, sizeof gReg);
    portEXIT_CRITICAL(&gMux);
    registryPersist();
    xSemaphoreGive(gRegMutex);

    postForgotten(gMainItem, NO_PHONE, 1000);
    gLinkDirty = true;
    ensureAdvertising();
    const int left = NimBLEDevice::getNumBonds();
    if (ok && left == 0) Serial.println("[BLE] Đã xoá toàn bộ ghép đôi");
    else Serial.printf("[BLE] Đã xoá toàn bộ ghép đôi (có lỗi, còn %d)\n", left);
}

}  // namespace ancs
