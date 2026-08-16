// 时间来源管理, 三层优先级:
//
//   1. 主机 $T 帧      —— 最准。Mac 在线时以它为准
//   2. 本地 RTC 外推    —— 主机一断开就接管, 短时断连无缝, 不用等 WiFi
//   3. WiFi + NTP      —— 断电重启且没有主机时的兜底
//
// 第 2 层是关键: 屏幕多数时候是被 Mac 供电的, Mac 睡眠后采集端停了但屏还亮着,
// 这时候只要靠 millis() 从最后一次同步往后推就够了, 完全不需要网络。
//
// WiFi 密码不写在代码里。首次开机没有保存的凭据时会起一个热点, 用手机连上去
// 在网页里填 —— 这个项目是公开仓库, 密码写死既不安全, 别人也没法直接用。
#pragma once
#include <Arduino.h>
#include <time.h>

#ifndef TK_ENABLE_WIFI
#define TK_ENABLE_WIFI 1
#endif

#if TK_ENABLE_WIFI
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#endif

static time_t   g_tkBase   = 0;      // 最后一次同步到的 epoch(UTC)
static uint32_t g_tkBaseMs = 0;      // 同步时的 millis()
static long     g_tkTz     = 0;      // 本地时区偏移(秒)
static bool     g_tkHas    = false;  // 有没有拿到过时间
static bool     g_tkFromNet = false; // 当前时间是不是 NTP 来的

#if TK_ENABLE_WIFI
static Preferences g_tkPrefs;
static WebServer   g_tkWeb(80);
static DNSServer   g_tkDns;
static bool  g_tkPortal   = false;   // 配网热点是否开着
static bool  g_tkNtpDone  = false;
static uint32_t g_tkWifiT = 0;
#endif

static inline time_t tkEpoch() {
  // 无符号相减天然处理 millis() 的 49 天回绕
  return g_tkBase + (time_t)((millis() - g_tkBaseMs) / 1000UL);
}

static inline bool tkHasTime() { return g_tkHas; }

static struct tm tkLocalTime() {
  time_t t = tkEpoch() + g_tkTz;
  struct tm out;
  gmtime_r(&t, &out);   // 已经加过时区偏移, 所以这里用 gmtime 而不是 localtime
  return out;
}

// 收到主机的 $T
static void tkSetFromHost(time_t epoch, long tz) {
  g_tkBase = epoch;
  g_tkBaseMs = millis();
  g_tkTz = tz;
  g_tkHas = true;
  g_tkFromNet = false;
#if TK_ENABLE_WIFI
  // 时区存下来: 下次断电重启且没有主机时, NTP 只给 UTC, 时区得靠自己记住
  static long lastSaved = 0x7FFFFFFF;
  if (tz != lastSaved) {
    g_tkPrefs.begin("macmon", false);
    g_tkPrefs.putLong("tz", tz);
    g_tkPrefs.end();
    lastSaved = tz;
  }
#endif
}

#if TK_ENABLE_WIFI
// ---------------- 配网热点 ----------------
static const char *TK_FORM =
    "<!doctype html><meta charset=\"utf-8\">"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:-apple-system,sans-serif;background:#0A0D14;color:#E6EDF3;"
    "display:flex;min-height:100vh;align-items:center;justify-content:center;margin:0}"
    "form{width:300px}h2{font-weight:600;margin:0 0 6px}p{color:#6B7A8F;font-size:13px;margin:0 0 18px}"
    "input{width:100%;box-sizing:border-box;padding:12px;margin:6px 0;border-radius:10px;"
    "border:1px solid #1E2836;background:#121A26;color:#E6EDF3;font-size:15px}"
    "button{width:100%;padding:12px;margin-top:12px;border:0;border-radius:10px;"
    "background:#00E5FF;color:#06202B;font-size:15px;font-weight:600}</style>"
    "<form method=POST action=/save><h2>Mac Monitor</h2>"
    "<p>连接 WiFi 后，屏幕在主机离线时可作为时钟使用<br>"
    "Connect WiFi so the display works as a clock when the host is offline</p>"
    "<input name=s placeholder='WiFi 名称 / SSID' autocapitalize=off>"
    "<input name=p type=password placeholder='WiFi 密码 / Password'>"
    "<button>保存并重启 / Save &amp; Reboot</button></form>";

static void tkStartPortal(const char *apName) {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName);
  g_tkDns.start(53, "*", WiFi.softAPIP());
  g_tkWeb.onNotFound([]() { g_tkWeb.send(200, "text/html; charset=utf-8", TK_FORM); });
  g_tkWeb.on("/", []() { g_tkWeb.send(200, "text/html; charset=utf-8", TK_FORM); });
  g_tkWeb.on("/save", HTTP_POST, []() {
    g_tkPrefs.begin("macmon", false);
    g_tkPrefs.putString("ssid", g_tkWeb.arg("s"));
    g_tkPrefs.putString("pass", g_tkWeb.arg("p"));
    g_tkPrefs.end();
    g_tkWeb.send(200, "text/html; charset=utf-8",
                 "<meta charset=\"utf-8\"><body style='font-family:sans-serif'>"
                 "已保存，正在重启… / Saved, rebooting…</body>");
    delay(500);
    ESP.restart();
  });
  g_tkWeb.begin();
  g_tkPortal = true;
  Serial.printf("[tk] 配网热点已开: %s -> http://%s\n", apName,
                WiFi.softAPIP().toString().c_str());
}
#endif

static void tkBegin(const char *apName) {
#if TK_ENABLE_WIFI
  g_tkPrefs.begin("macmon", true);
  String ssid = g_tkPrefs.getString("ssid", "");
  String pass = g_tkPrefs.getString("pass", "");
  g_tkTz = g_tkPrefs.getLong("tz", 0);
  g_tkPrefs.end();

  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);              // 省点电, 我们对延迟没要求
    WiFi.begin(ssid.c_str(), pass.c_str());   // 非阻塞, 不能卡住仪表盘
    g_tkWifiT = millis();
    Serial.printf("[tk] 连接 WiFi: %s\n", ssid.c_str());
  } else {
    tkStartPortal(apName);
  }
#endif
}

static void tkLoop() {
#if TK_ENABLE_WIFI
  if (g_tkPortal) {
    g_tkDns.processNextRequest();
    g_tkWeb.handleClient();
    return;
  }
  if (!g_tkNtpDone && WiFi.status() == WL_CONNECTED) {
    // 只取 UTC, 时区用主机给过的/存下来的那个
    configTime(0, 0, "pool.ntp.org", "ntp.aliyun.com", "time.apple.com");
    g_tkNtpDone = true;
    Serial.printf("[tk] WiFi 已连接 %s, 开始 NTP\n", WiFi.localIP().toString().c_str());
  }
  // 主机时间优先; 只有从没拿到过主机时间时才采纳 NTP 的结果
  if (g_tkNtpDone && !g_tkHas) {
    time_t now = time(nullptr);
    if (now > 1700000000) {           // 2023-11 之后才算同步成功
      g_tkBase = now;
      g_tkBaseMs = millis();
      g_tkHas = true;
      g_tkFromNet = true;
      Serial.println("[tk] NTP 同步完成");
    }
  }
#endif
}

static inline bool tkNetOk() {
#if TK_ENABLE_WIFI
  return g_tkHas;
#else
  return g_tkHas;
#endif
}
