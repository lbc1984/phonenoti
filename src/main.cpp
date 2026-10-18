// Hiển thị thông báo iPhone lên màn hình ESP32 qua Bluetooth LE (ANCS).
#include <Arduino.h>

#include "ancs_client.h"
#include "ui.h"

static const char* kDeviceName = "ESP32 Noti";
static ancs::Event gEvent;  // khá lớn (~700 byte) nên để static

void setup() {
    Serial.begin(115200);
    ui::begin(kDeviceName);

    // Giữ tay trên màn hình lúc cắm điện -> xoá các iPhone đã ghép đôi
    bool forget = false;
    for (int i = 0; i < 10 && !forget; i++) {
        forget = ui::isTouched();
        delay(50);
    }

    ancs::begin(kDeviceName);
    if (forget) ancs::forgetBonds();
}

void loop() {
    while (ancs::nextEvent(gEvent)) {
        switch (gEvent.type) {
            case ancs::EventType::State: ui::setLinkState(gEvent.state); break;
            case ancs::EventType::PassKey: ui::showPassKey(gEvent.passKey); break;
            case ancs::EventType::Added: ui::addNotification(gEvent.notif); break;
            case ancs::EventType::Removed: ui::removeNotification(gEvent.uid); break;
        }
    }
    ui::loop();
    delay(5);
}
