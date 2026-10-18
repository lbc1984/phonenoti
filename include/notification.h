#pragma once
#include <stdint.h>

// ANCS CategoryID
enum NotifCategory : uint8_t {
    CAT_OTHER = 0,
    CAT_INCOMING_CALL = 1,
    CAT_MISSED_CALL = 2,
    CAT_VOICEMAIL = 3,
    CAT_SOCIAL = 4,
    CAT_SCHEDULE = 5,
    CAT_EMAIL = 6,
    CAT_NEWS = 7,
    CAT_HEALTH = 8,
    CAT_FINANCE = 9,
    CAT_LOCATION = 10,
    CAT_ENTERTAINMENT = 11,
};

// ANCS EventFlags
enum NotifFlag : uint8_t {
    NOTIF_FLAG_SILENT = 1 << 0,
    NOTIF_FLAG_IMPORTANT = 1 << 1,
    NOTIF_FLAG_PRE_EXISTING = 1 << 2,
    NOTIF_FLAG_POSITIVE_ACTION = 1 << 3,  // có hành động "đồng ý" (vd. Nghe máy)
    NOTIF_FLAG_NEGATIVE_ACTION = 1 << 4,  // có hành động "từ chối/xoá"
};

// Một thông báo đã lấy đủ nội dung từ iPhone. Chuỗi đều là UTF-8, kết thúc bằng '\0'.
struct Notification {
    uint32_t uid;
    uint8_t category;
    uint8_t flags;
    char appId[64];
    char title[96];
    char subtitle[96];
    char message[400];
    char date[20];  // dạng yyyyMMdd'T'HHmmSS
};
