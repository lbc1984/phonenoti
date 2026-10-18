#pragma once
#include <string.h>

// Đổi bundle ID của app iOS sang tên dễ đọc.
struct AppName {
    const char* bundleId;
    const char* name;
};

static const AppName kAppNames[] = {
    {"com.apple.MobileSMS", "Messages"},
    {"com.apple.mobilephone", "Phone"},
    {"com.apple.mobilemail", "Mail"},
    {"com.apple.mobilecal", "Calendar"},
    {"com.apple.reminders", "Reminders"},
    {"com.apple.mobiletimer", "Clock"},
    {"com.apple.facetime", "FaceTime"},
    {"com.apple.Passbook", "Wallet"},
    {"com.apple.Health", "Health"},
    {"com.apple.findmy", "Find My"},
    {"vn.com.vng.zingalo", "Zalo"},
    {"com.facebook.Messenger", "Messenger"},
    {"com.facebook.Facebook", "Facebook"},
    {"net.whatsapp.WhatsApp", "WhatsApp"},
    {"ph.telegra.Telegraph", "Telegram"},
    {"com.viber", "Viber"},
    {"com.burbn.instagram", "Instagram"},
    {"com.zhiliaoapp.musically", "TikTok"},
    {"com.atebits.Tweetie2", "X"},
    {"com.google.ios.youtube", "YouTube"},
    {"com.google.Gmail", "Gmail"},
    {"com.microsoft.Office.Outlook", "Outlook"},
    {"com.microsoft.skype.teams", "Teams"},
    {"com.tinyspeck.chatlyio", "Slack"},
    {"com.hammerandchisel.discord", "Discord"},
    {"com.grabtaxi.passenger", "Grab"},
};

// Không có trong bảng -> lấy phần cuối của bundle ID ("com.abc.MyApp" -> "MyApp").
inline const char* appDisplayName(const char* bundleId) {
    for (const AppName& a : kAppNames) {
        if (strcmp(a.bundleId, bundleId) == 0) return a.name;
    }
    const char* dot = strrchr(bundleId, '.');
    return (dot && dot[1]) ? dot + 1 : bundleId;
}
