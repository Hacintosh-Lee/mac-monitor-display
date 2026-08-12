// 屏幕硬件配置, 两版固件共用 —— 引脚是硬件事实, 只该有一处定义。
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ---------- 引脚 (focus 扫描实测, 编号 1/7/13/133 交叉验证) ----------
// 合宙 AirM2M CORE ESP32-C3 焊盘号 -> GPIO
#define PIN_SCK   2   // 焊盘19  SPI_CK
#define PIN_MOSI  3   // 焊盘20  SPI_MOSI
#define PIN_DC    4   // 焊盘28
#define PIN_CS    7   // 焊盘23  SPI_CS
#define PIN_RST   5   // 焊盘27
#define PIN_BL   -1   // 背光经载板 R2 电阻常亮, 没接 GPIO
// 这块屏在 40MHz 下不出图(pintest 实测), 20MHz 稳定
#define SPI_WRITE_HZ 20000000
// --------------------------------------------------------------------

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;

 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = SPI_WRITE_HZ;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_SCK;
      cfg.pin_mosi = PIN_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc = PIN_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_CS;
      cfg.pin_rst = PIN_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};
