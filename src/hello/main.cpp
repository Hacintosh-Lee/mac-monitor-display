// 最小对照固件: 只打印心跳, 不碰任何 GPIO。
// 用来判断 boot loop 是硬件/分区问题还是扫描代码的问题。
#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("### HELLO ALIVE ###");
  Serial.printf("reset_reason=%d chip=%s rev=%d heap=%u\n",
                (int)esp_reset_reason(), ESP.getChipModel(),
                ESP.getChipRevision(), (unsigned)ESP.getFreeHeap());
}

void loop() {
  static uint32_t n = 0;
  Serial.printf("tick %u  uptime=%lums  heap=%u\n", n++, millis(),
                (unsigned)ESP.getFreeHeap());
  delay(1000);
}
