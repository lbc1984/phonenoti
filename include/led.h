#pragma once
#include <stdint.h>

#include "ancs_client.h"

// Đèn RGB WS2812 trên board (chân UI_RGB_LED trong platformio.ini) báo trạng thái.
// Không khai báo UI_RGB_LED thì mọi hàm không làm gì.
namespace led {

void begin();
void loop();  // gọi liên tục, tự tính hiệu ứng theo thời gian

void setLink(ancs::LinkState state);
void phoneJoined();  // thêm một iPhone sẵn sàng: xanh lá 1 giây
void setPairing(bool on);
void setCall(bool on);                   // đang có cuộc gọi đến
void newNotification(uint16_t color565);  // nháy 3 lần theo màu loại thông báo
void setUnread(bool on);                 // nhịp nhắc còn tin chưa xem
void setDim(bool on);                    // màn đang tối -> đèn dịu hơn

}  // namespace led
