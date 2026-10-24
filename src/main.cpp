// Hiển thị thông báo iPhone lên màn hình ESP32 qua Bluetooth LE (ANCS).
#include <Arduino.h>

#include "ancs_client.h"
#include "speech.h"
#include "ui.h"

static const char* kDeviceName = "ESP32 Noti";
static ancs::Event gEvent;  // khá lớn (~800 byte) nên để static
static String gSerialLine;

void setup() {
    Serial.setRxBufferSize(1024);  // câu thử gõ vào Serial Monitor có thể dài vài trăm byte
    Serial.begin(115200);
    ui::begin(kDeviceName);

    // Giữ tay trên màn hình lúc cắm điện -> xoá các iPhone đã ghép đôi
    bool forget = false;
    for (int i = 0; i < 10 && !forget; i++) {
        forget = ui::isTouched();
        delay(50);
    }

    ancs::begin(kDeviceName);
    if (forget) ancs::forgetBonds();  // xoá được cả khi đang quảng bá
    speech::begin();
}

void loop() {
    while (ancs::nextEvent(gEvent)) {
        switch (gEvent.type) {
            case ancs::EventType::Link: ui::setLinkStatus(gEvent.link); break;
            case ancs::EventType::PassKey: ui::showPassKey(gEvent.passKey); break;
            case ancs::EventType::PassKeyDone: ui::hidePassKey(); break;
            case ancs::EventType::Added:
                if (ui::addNotification(gEvent.notif, gEvent.isNew)) speech::announce(gEvent.notif);
                break;
            case ancs::EventType::Removed:
                ui::removeNotification(gEvent.phone, gEvent.uid);
                speech::cancel(gEvent.phone, gEvent.uid);
                break;
            case ancs::EventType::PhoneForgotten:
                ui::forgetPhone(gEvent.phone);
                speech::cancelPhone(gEvent.phone);
                break;
        }
    }
    // Gõ một câu vào Serial Monitor rồi Enter để nghe thử loa
    while (Serial.available()) {
        const char c = Serial.read();
        if (c == '\n') {
            if (gSerialLine.length()) speech::say(gSerialLine.c_str());
            gSerialLine = "";
        } else if (c != '\r' && gSerialLine.length() < 600) {
            gSerialLine += c;
        }
    }
    ui::loop();
    delay(5);
}
