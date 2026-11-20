#include "network_api.h"
#include "config.h"
#include "storage.h"
#include "certs.h"
#include "audio.h"
#include "epd_driver.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <LittleFS.h>
#include <mbedtls/base64.h>
#include <time.h>

// ── Time state ──────────────────────────────────────────────
int curHour, curMin, curSec;
static unsigned long lastHeartbeatAt = 0;
bool g_userAborted = false;
bool g_suppressAbortCheck = false;

static bool checkAbort() {
    if (g_suppressAbortCheck) return false;
    if (digitalRead(PIN_CFG_BTN) == LOW) {
        delay(50);
        if (digitalRead(PIN_CFG_BTN) == LOW) {
            g_userAborted = true;
            return true;
        }
    }
    return false;
}

static bool beginHttpForUrl(HTTPClient &http, WiFiClient &plainClient, WiFiClientSecure &secClient, const String &url);
static bool recoverDeviceTokenIfUnauthorized(int code);
static String extractJsonStringField(const String &body, const char *key);
static String extractJsonBoolField(const String &body, const char *key);
static int extractJsonIntField(const String &body, const char *key, int defaultValue = 0);
static bool parseWsServerUrl(const String &baseUrl, bool &useSSL, String &hostOut, uint16_t &portOut, String &pathOut);
static bool decodeBase64ToHeap(const String &encoded, uint8_t **dataOut, size_t *dataLenOut);
static String encodeBase64(const uint8_t *data, size_t len);
static void handleVoiceWsEvent(VoiceWsEvent &event);
static void voiceWsSocketEvent(WStype_t type, uint8_t *payload, size_t length);

static WebSocketsClient gVoiceWsClient;
static bool gVoiceWsConnected = false;
static unsigned long gVoiceWsOpenStartedAt = 0;
static unsigned long gVoiceWsConnectedAt = 0;
static bool gVoiceWsDisconnected = false;
static bool gVoiceWsConfigured = false;
static bool gVoiceWsBinaryAudioEnabled = false;
static bool gVoiceWsOpusEnabled = false;
static bool gVoiceWsServerVadEnabled = false;
static QueueHandle_t gVoiceWsEventQueue = nullptr;

// ── WiFi connection ─────────────────────────────────────────

// Associate with a single AP and wait up to WIFI_TIMEOUT. Returns true on
// success. Returns false on timeout/abort (g_userAborted set on abort).
static bool tryAssociate(const String &ssid, const String &pass) {
    Serial.printf("WiFi: %s ", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.begin(ssid.c_str(), pass.c_str());

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (checkAbort()) return false;
        if (millis() - t0 > (unsigned long)WIFI_TIMEOUT) {
            Serial.println("TIMEOUT");
            return false;
        }
        delay(300);
        Serial.print(".");
    }
    Serial.printf(" OK  IP=%s\n", WiFi.localIP().toString().c_str());
    return true;
}

bool connectWiFi() {
    g_userAborted = false;

    // Try each saved network in order; first success wins.
    int count = getWiFiCount();
    if (count <= 0) {
        // No saved list (shouldn't normally happen post-loadConfig); fall back
        // to the legacy primary credentials.
        if (cfgSSID.length() == 0) return false;
        if (!tryAssociate(cfgSSID, cfgPass)) return false;
    } else {
        bool associated = false;
        for (int i = 0; i < count; i++) {
            String ssid, pass;
            if (!getWiFiAt(i, ssid, pass)) continue;
            if (g_userAborted) return false;
            Serial.printf("[WIFI] Trying network %d/%d\n", i + 1, count);
            if (tryAssociate(ssid, pass)) {
                // Promote the connected network to primary so cfgSSID/cfgPass
                // (used for display, pairing, heartbeat) reflect it.
                cfgSSID = ssid;
                cfgPass = pass;
                associated = true;
                break;
            }
            if (g_userAborted) return false;
        }
        if (!associated) {
            Serial.println("[WIFI] All saved networks failed");
            return false;
        }
    }

    if (!ensureDeviceToken()) return false;
    if (cfgPendingPairCode.length() > 0) {
        String mac = WiFi.macAddress();
        String url = cfgServer + "/api/device/" + mac + "/claim-token";
        String body = String("{\"pair_code\":\"") + cfgPendingPairCode + "\"}";
        for (int attempt = 0; attempt < 3; attempt++) {
            if (checkAbort()) return false;
            Serial.printf("[PAIR] POST %s (attempt %d/3)\n", url.c_str(), attempt + 1);
            WiFiClient plainClient;
            WiFiClientSecure secClient;
            HTTPClient http;
            if (!beginHttpForUrl(http, plainClient, secClient, url)) {
                Serial.println("[PAIR] begin failed");
                delay(800);
                continue;
            }
            http.addHeader("Content-Type", "application/json");
            if (cfgDeviceToken.length() > 0) {
                http.addHeader("X-Device-Token", cfgDeviceToken);
            }
            http.setTimeout(HTTP_TIMEOUT);

            int code = http.POST(body);
            Serial.printf("[PAIR] HTTP code: %d\n", code);
            if (code >= 200 && code < 300) {
                String resp = http.getString();
                String savedPairCode = extractJsonStringField(resp, "pair_code");
                http.end();
                if (savedPairCode == cfgPendingPairCode) {
                    clearPendingPairCode();
                    Serial.println("[PAIR] pair code registered");
                    break;
                }
                Serial.printf(
                    "[PAIR] pair code mismatch: local=%s remote=%s\n",
                    cfgPendingPairCode.c_str(),
                    savedPairCode.length() > 0 ? savedPairCode.c_str() : "empty"
                );
                delay(800);
                continue;
            }
            if (code < 0) {
                Serial.printf("[PAIR] error: %s\n", http.errorToString(code).c_str());
            } else {
                String resp = http.getString();
                Serial.printf("[PAIR] response: %s\n", resp.substring(0, 300).c_str());
            }
            http.end();
            if (!recoverDeviceTokenIfUnauthorized(code)) {
                delay(800);
            }
        }
    }
    postHeartbeat(true);
    return true;
}

// ── Battery voltage ─────────────────────────────────────────
// 用 analogReadMilliVolts() 取芯片 eFuse 里的出厂校准结果，不再自己 esp_adc_cal，
// 也不再假设「满量程 = 3.3V」：C3 在 11dB 档的实际满量程只有 ~3.1V，
// 按 3.3V 折算会把 4.2V 读成 4.4V 以上。
float readBatteryVoltage() {
    static bool adcReady = false;
    if (!adcReady) {
        analogReadResolution(12);
        analogSetAttenuation(ADC_11db);
        adcReady = true;
    }

    const int SAMPLES = 16;
    const int DISCARD = 2;  // 去掉最大/最小各 2 个
    uint16_t mv[SAMPLES];

    for (int i = 0; i < SAMPLES; i++) {
        mv[i] = (uint16_t)analogReadMilliVolts(PIN_BAT_ADC);
        delayMicroseconds(200);
    }

    // 排序后取中间段平均，滤掉单次毛刺
    for (int i = 0; i < SAMPLES - 1; i++)
        for (int j = i + 1; j < SAMPLES; j++)
            if (mv[i] > mv[j]) {
                uint16_t tmp = mv[i];
                mv[i] = mv[j];
                mv[j] = tmp;
            }

    uint32_t sum = 0;
    for (int i = DISCARD; i < SAMPLES - DISCARD; i++) sum += mv[i];

    float pinMv = (float)sum / (SAMPLES - 2 * DISCARD);

    // 分压电阻没接时引脚悬空，读数会在几百毫伏乱飘（实测 540~610mV）。
    // 这种值上报出去会被算成 0% 并写进后台电量曲线，不如直接当没电池。
    if (pinMv < BAT_ADC_MIN_PIN_MV) {
        Serial.printf("[BAT] pin=%.0fmV floating (no divider), report 0.00V\n", pinMv);
        return 0.0f;
    }

    float realBatteryVoltage = (pinMv / 1000.0f) * BAT_ADC_DIVIDER;
    Serial.printf("[BAT] pin=%.0fmV vbat=%.2fV\n", pinMv, realBatteryVoltage);
    return realBatteryVoltage;
}

// ── Stream helper ───────────────────────────────────────────

static bool readExact(WiFiClient *s, uint8_t *buf, int len) {
    int got = 0;
    unsigned long t0 = millis();
    while (got < len) {
        if (millis() - t0 > 10000) {
            Serial.printf(
                "readExact: timeout %d/%d connected=%d available=%d wifi=%d\n",
                got,
                len,
                s->connected() ? 1 : 0,
                s->available(),
                WiFi.status()
            );
            return false;
        }
        int avail = s->available();
        if (avail > 0) {
            int r = s->readBytes(buf + got, min(avail, len - got));
            got += r;
            t0 = millis();  // Reset timeout on progress
        } else {
            delay(1);
        }
    }
    return true;
}

static bool beginHttpForUrl(HTTPClient &http, WiFiClient &plainClient, WiFiClientSecure &secClient, const String &url) {
    if (url.startsWith("https://")) {
        secClient.setCACert(ROOT_CA);
        return http.begin(secClient, url);
    }
    return http.begin(plainClient, url);
}

static String extractJsonStringField(const String &body, const char *key) {
    String needle = String("\"") + key + "\"";
    int start = body.indexOf(needle);
    if (start < 0) return "";
    start += needle.length();
    while (start < body.length() && (body[start] == ' ' || body[start] == '\t' || body[start] == '\r' || body[start] == '\n')) {
        start++;
    }
    if (start >= body.length() || body[start] != ':') return "";
    start++;
    while (start < body.length() && (body[start] == ' ' || body[start] == '\t' || body[start] == '\r' || body[start] == '\n')) {
        start++;
    }
    if (start >= body.length() || body[start] != '"') return "";
    start++;
    int end = body.indexOf('"', start);
    if (end < 0) return "";
    return body.substring(start, end);
}

static String extractJsonBoolField(const String &body, const char *key) {
    String needle = String("\"") + key + "\":";
    int start = body.indexOf(needle);
    if (start < 0) return "";
    start += needle.length();
    while (start < body.length() && body[start] == ' ') {
        start++;
    }
    if (body.startsWith("true", start)) return "true";
    if (body.startsWith("false", start)) return "false";
    if (body.startsWith("1", start)) return "1";
    if (body.startsWith("0", start)) return "0";
    return "";
}

static int extractJsonIntField(const String &body, const char *key, int defaultValue) {
    String needle = String("\"") + key + "\":";
    int start = body.indexOf(needle);
    if (start < 0) return defaultValue;
    start += needle.length();
    while (start < body.length() && body[start] == ' ') {
        start++;
    }
    int end = start;
    while (end < body.length() && (body[end] == '-' || (body[end] >= '0' && body[end] <= '9'))) {
        end++;
    }
    if (end <= start) return defaultValue;
    return body.substring(start, end).toInt();
}

static bool parseWsServerUrl(const String &baseUrl, bool &useSSL, String &hostOut, uint16_t &portOut, String &pathOut) {
    String url = baseUrl;
    useSSL = false;
    if (url.startsWith("https://")) {
        useSSL = true;
        url = url.substring(8);
        portOut = 443;
    } else if (url.startsWith("http://")) {
        url = url.substring(7);
        portOut = 80;
    } else {
        return false;
    }

    int slash = url.indexOf('/');
    String hostPort = slash >= 0 ? url.substring(0, slash) : url;
    pathOut = slash >= 0 ? url.substring(slash) : "";
    if (pathOut.length() == 0) {
        pathOut = "";
    }

    int colon = hostPort.indexOf(':');
    if (colon >= 0) {
        hostOut = hostPort.substring(0, colon);
        portOut = (uint16_t)hostPort.substring(colon + 1).toInt();
    } else {
        hostOut = hostPort;
    }
    if (hostOut.length() == 0) {
        return false;
    }
    return true;
}

static bool decodeBase64ToHeap(const String &encoded, uint8_t **dataOut, size_t *dataLenOut) {
    if (dataOut == nullptr || dataLenOut == nullptr) return false;
    *dataOut = nullptr;
    *dataLenOut = 0;
    if (encoded.length() == 0) {
        return true;
    }
    size_t maxLen = ((encoded.length() + 3) / 4) * 3 + 4;
    uint8_t *buffer = (uint8_t *)malloc(maxLen);
    if (buffer == nullptr) {
        return false;
    }
    size_t outputLen = 0;
    int rc = mbedtls_base64_decode(buffer, maxLen, &outputLen, (const unsigned char *)encoded.c_str(), encoded.length());
    if (rc != 0) {
        free(buffer);
        return false;
    }
    *dataOut = buffer;
    *dataLenOut = outputLen;
    return true;
}

static String encodeBase64(const uint8_t *data, size_t len) {
    if (data == nullptr || len == 0) return "";
    size_t outputLen = 0;
    size_t capacity = ((len + 2) / 3) * 4 + 4;
    unsigned char *buffer = (unsigned char *)malloc(capacity);
    if (buffer == nullptr) {
        return "";
    }
    int rc = mbedtls_base64_encode(buffer, capacity, &outputLen, data, len);
    if (rc != 0) {
        free(buffer);
        return "";
    }
    buffer[outputLen] = '\0';
    String encoded = String((const char *)buffer);
    free(buffer);
    return encoded;
}

static bool recoverDeviceTokenIfUnauthorized(int code) {
    if (code != 401 || cfgDeviceToken.length() == 0) return false;
    Serial.println("[AUTH] 401 unauthorized, resetting cached device token");
    clearDeviceToken();
    return ensureDeviceToken();
}

bool postHeartbeat(bool force) {
    if (WiFi.status() != WL_CONNECTED) return false;
    unsigned long now = millis();
    if (!force && lastHeartbeatAt != 0 && now - lastHeartbeatAt < HEARTBEAT_INTERVAL_MS) {
        return true;
    }
    if (!ensureDeviceToken()) return false;

    float v = readBatteryVoltage();
    int rssi = WiFi.RSSI();
    String mac = WiFi.macAddress();
    String url = cfgServer + "/api/device/" + mac + "/heartbeat";
    String body = String("{\"battery_voltage\":") + String(v, 2) + ",\"wifi_rssi\":" + String(rssi) + "}";
    for (int attempt = 0; attempt < 2; attempt++) {
        WiFiClient plainClient;
        WiFiClientSecure secClient;
        HTTPClient http;
        if (!beginHttpForUrl(http, plainClient, secClient, url)) return false;
        http.addHeader("Content-Type", "application/json");
        if (cfgDeviceToken.length() > 0) {
            http.addHeader("X-Device-Token", cfgDeviceToken);
        }
        http.setTimeout(HTTP_TIMEOUT);

        int code = http.POST(body);
        if (code >= 200 && code < 300) {
            Serial.printf("[HEARTBEAT] POST -> %d\n", code);
            http.end();
            lastHeartbeatAt = now;
            return true;
        }
        if (code < 0) {
            Serial.printf("[HEARTBEAT] error: %s\n", http.errorToString(code).c_str());
        } else {
            Serial.printf("[HEARTBEAT] POST -> %d\n", code);
        }
        http.end();
        if (!recoverDeviceTokenIfUnauthorized(code)) {
            return false;
        }
    }
    return false;
}

bool ensureDeviceToken() {
    if (cfgDeviceToken.length() > 0) return true;
    if (WiFi.status() != WL_CONNECTED) return false;

    String mac = WiFi.macAddress();
    String url = cfgServer + "/api/device/" + mac + "/token";
    delay(1200);
    for (int attempt = 0; attempt < 3; attempt++) {
        if (checkAbort()) return false;
        Serial.printf("[TOKEN] POST %s (attempt %d/3)\n", url.c_str(), attempt + 1);
        WiFiClient plainClient;
        WiFiClientSecure secClient;
        HTTPClient http;
        if (!beginHttpForUrl(http, plainClient, secClient, url)) {
            Serial.println("[TOKEN] begin failed");
            delay(800);
            continue;
        }
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(HTTP_TIMEOUT);

        int code = http.POST("{}");
        Serial.printf("[TOKEN] HTTP code: %d\n", code);
        if (code >= 200 && code < 300) {
            String body = http.getString();
            http.end();
            String token = extractJsonStringField(body, "token");
            if (token.length() == 0) {
                Serial.println("[TOKEN] token field empty");
                delay(800);
                continue;
            }
            saveDeviceToken(token);
            Serial.println("[TOKEN] token saved");
            return true;
        }
        if (code < 0) {
            Serial.printf("[TOKEN] error: %s\n", http.errorToString(code).c_str());
        } else {
            String body = http.getString();
            Serial.printf("[TOKEN] response: %s\n", body.substring(0, 300).c_str());
        }
        http.end();
        delay(800);
    }
    Serial.println("[TOKEN] failed to obtain device token");
    return false;
}

bool fetchFocusListeningFlag(bool *outEnabled, bool *outAlwaysActive) {
    if (!outEnabled) return false;
    *outEnabled = false;
    if (outAlwaysActive) *outAlwaysActive = false;
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!ensureDeviceToken()) return false;

    String mac = WiFi.macAddress();
    String url = cfgServer + "/api/config/" + mac;
    bool useSSL = cfgServer.startsWith("https://");

    for (int attempt = 0; attempt < 2; attempt++) {
        WiFiClient plainClient;
        WiFiClientSecure secClient;
        HTTPClient http;
        if (useSSL) {
            secClient.setCACert(ROOT_CA);
            http.begin(secClient, url);
        } else {
            http.begin(plainClient, url);
        }
        http.setTimeout(HTTP_TIMEOUT);
        if (cfgDeviceToken.length() > 0) {
            http.addHeader("X-Device-Token", cfgDeviceToken);
        }

        int code = http.GET();
        if (code != 200) {
            http.end();
            if (!recoverDeviceTokenIfUnauthorized(code)) return false;
            continue;
        }

        String body = http.getString();
        http.end();
        bool enabled =
            body.indexOf("\"is_focus_listening\":true") >= 0 ||
            body.indexOf("\"is_focus_listening\": true") >= 0 ||
            body.indexOf("\"focus_listening\":1") >= 0 ||
            body.indexOf("\"focus_listening\": 1") >= 0;
        bool alwaysActive =
            body.indexOf("\"is_always_active\":true") >= 0 ||
            body.indexOf("\"is_always_active\": true") >= 0 ||
            body.indexOf("\"always_active\":1") >= 0 ||
            body.indexOf("\"always_active\": 1") >= 0 ||
            body.indexOf("\"always_active\":true") >= 0 ||
            body.indexOf("\"always_active\": true") >= 0;
        *outEnabled = enabled;
        if (outAlwaysActive) *outAlwaysActive = alwaysActive;
        Serial.printf("[CONFIG] is_focus_listening=%s always_active=%s\n",
                      enabled ? "true" : "false",
                      alwaysActive ? "true" : "false");
        return true;
    }
    return false;
}

bool fetchFocusAlertBMP() {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!ensureDeviceToken()) return false;
    String mac = WiFi.macAddress();
    String url = cfgServer + "/api/device/" + mac + "/alert-bmp"
               + "?w=" + String(W) + "&h=" + String(H);
    bool useSSL = cfgServer.startsWith("https://");

    for (int attempt = 0; attempt < 2; attempt++) {
        WiFiClient plainClient;
        WiFiClientSecure secClient;
        HTTPClient http;
        if (useSSL) {
            secClient.setCACert(ROOT_CA);
            http.begin(secClient, url);
        } else {
            http.begin(plainClient, url);
        }
        http.setTimeout(HTTP_TIMEOUT);
        if (cfgDeviceToken.length() > 0) {
            http.addHeader("X-Device-Token", cfgDeviceToken);
        }

        int code = http.GET();
        Serial.printf("[FOCUS] alert-bmp HTTP code: %d\n", code);
        if (code == 204) {
            http.end();
            return false;
        }
        if (code != 200) {
            http.end();
            if (!recoverDeviceTokenIfUnauthorized(code)) return false;
            continue;
        }

        WiFiClient *stream = http.getStreamPtr();
        uint8_t fileHeader[14];
        if (!readExact(stream, fileHeader, 14)) {
            http.end();
            return false;
        }
        uint32_t pixelOffset = fileHeader[10]
                             | ((uint32_t)fileHeader[11] << 8)
                             | ((uint32_t)fileHeader[12] << 16)
                             | ((uint32_t)fileHeader[13] << 24);
        int toSkip = (int)pixelOffset - 14;
        while (toSkip > 0 && stream->connected()) {
            if (stream->available()) { stream->read(); toSkip--; }
        }

        uint8_t rowBuf[ROW_STRIDE];
        for (int bmpY = 0; bmpY < H; bmpY++) {
            if (!readExact(stream, rowBuf, ROW_STRIDE)) {
                http.end();
                return false;
            }
            int dispY = H - 1 - bmpY;
            memcpy(imgBuf + dispY * ROW_BYTES, rowBuf, ROW_BYTES);
        }
        http.end();
        return true;
    }
    return false;
}

// ── Fetch BMP from backend ──────────────────────────────────

#if EPD_BPP >= 2 && defined(EPD_PANEL_398_NZ07)
// 原始 2bpp 帧直接流进面板 RAM：每读 192 字节（一行＝768px @2bpp）就下发一行，
// MCU 侧只需要一个栈上的行缓冲。原来的做法是先 malloc 105,984 字节再读，
// 而这一步发生在 HTTPS 会话还开着的时候，TLS 收发缓冲把堆切碎后必然失败。
static bool stream2bppToPanel(WiFiClient *stream, uint32_t *checksumOut) {
    if (!epdStream2bppBegin()) {
        Serial.println("[RENDER] panel stream begin failed");
        return false;
    }
    uint8_t row[EPD_WIDTH / 4];
    uint32_t sum = 0;
    for (int y = 0; y < EPD_HEIGHT; y++) {
        if (!readExact(stream, row, sizeof(row))) {
            Serial.printf("[RENDER] panel stream read failed at row %d/%d\n", y, EPD_HEIGHT);
            epdStream2bppAbort();
            return false;
        }
        for (size_t i = 0; i < sizeof(row); i++) sum += row[i];
        epdStream2bppRow(y, row);
    }
    *checksumOut = sum;
    return true;
}
#endif

bool fetchBMP(bool nextMode, bool *isFallback, String *renderedModeIdOut) {
    if (isFallback) *isFallback = false;
    if (renderedModeIdOut) *renderedModeIdOut = "";
#if EPD_BPP >= 2
    // 上一帧如果没走完（拉到了图但没显示就退出），标志会残留。
    // 不清掉的话下一帧 BMP 会被当成「已写进面板」跳过绘制，屏幕停在旧内容上。
    if (useStreamedPanel) {
#if defined(EPD_PANEL_398_NZ07)
        epdStream2bppAbort();
#endif
        useStreamedPanel = false;
    }
    useColorBuf = false;
#endif
    if (!ensureDeviceToken()) return false;
    float v = readBatteryVoltage();
    String mac = WiFi.macAddress();
    int rssi = WiFi.RSSI();
#if DEBUG_MODE
    int effectiveRefreshMin = DEBUG_REFRESH_MIN;
#else
    int effectiveRefreshMin = cfgSleepMin;
#endif
#if EPD_BPP >= 2
    const int colorCapability = 4;
#else
    const int colorCapability = 2;
#endif
    // 读数不可信时（引脚悬空 / 没装电池）不带 v，让后端用默认值，
    // 否则会被算成 0% 并画成红色空电池，看起来像快没电了。
    String vParam = "";
    if (v > 0.0f) vParam = "v=" + String(v, 2) + "&";
    String url = cfgServer + "/api/render?" + vParam
               + "mac=" + mac + "&rssi=" + String(rssi)
               + "&refresh_min=" + String(effectiveRefreshMin)
               + "&w=" + String(W) + "&h=" + String(H)
               + "&bpp=" + String(EPD_BPP)
               + "&colors=" + String(colorCapability);
    if (nextMode) {
        url += "&next=1";
    }
    Serial.printf("GET %s (RSSI=%d)\n", url.c_str(), rssi);

    bool useSSL = cfgServer.startsWith("https://");
    for (int attempt = 0; attempt < 2; attempt++) {
        if (checkAbort()) {
            Serial.println("[RENDER] fetchBMP aborted before HTTP");
            return false;
        }
        WiFiClient plainClient;
        WiFiClientSecure secClient;
        HTTPClient http;
        bool begun = false;
        if (useSSL) {
            secClient.setCACert(ROOT_CA);
            begun = http.begin(secClient, url);
        } else {
            begun = http.begin(plainClient, url);
        }
        if (!begun) {
            Serial.println("[RENDER] http.begin failed");
            http.end();
            return false;
        }
        http.setReuse(false);
        http.setTimeout(HTTP_TIMEOUT);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        const char *headerKeys[] = {"X-Content-Fallback", "X-Refresh-Minutes", "X-Mode-Id"};
        http.collectHeaders(headerKeys, 3);

        http.addHeader("Accept-Encoding", "identity");
        http.addHeader("Connection", "close");
        if (cfgDeviceToken.length() > 0) {
            http.addHeader("X-Device-Token", cfgDeviceToken);
        }

        Serial.printf("Free heap: %d\n", ESP.getFreeHeap());
        int code = http.GET();
        Serial.printf("HTTP code: %d\n", code);
        if (renderedModeIdOut && code >= 200 && code < 300) {
            *renderedModeIdOut = http.header("X-Mode-Id");
        }
        if (isFallback) {
            String fallbackHeader = http.header("X-Content-Fallback");
            *isFallback = (fallbackHeader == "1" || fallbackHeader == "true");
            if (*isFallback) {
                Serial.println("[RENDER] Received fallback content");
            }
        }
        String refreshHeader = http.header("X-Refresh-Minutes");
        int serverRefreshMin = refreshHeader.toInt();
        if (serverRefreshMin >= 10 && serverRefreshMin <= 1440 && serverRefreshMin != cfgSleepMin) {
            saveSleepMin(serverRefreshMin);
            Serial.printf("[RENDER] Applied refresh interval: %d min\n", serverRefreshMin);
        }

        if (code != 200) {
            if (code < 0) {
                Serial.printf("HTTP error: %s\n", http.errorToString(code).c_str());
            } else {
                String body = http.getString();
                Serial.printf("Response: %s\n", body.substring(0, 500).c_str());
            }
            http.end();
            if (!recoverDeviceTokenIfUnauthorized(code)) {
                return false;
            }
            continue;
        }

        int contentLen = http.getSize();
        Serial.printf("Content-Length: %d\n", contentLen);

        WiFiClient *stream = http.getStreamPtr();

#if EPD_BPP >= 2
        if (contentLen == COLOR_BUF_LEN) {
#if defined(EPD_PANEL_398_NZ07)
            uint32_t streamSum = 0;
            if (!stream2bppToPanel(stream, &streamSum)) {
                Serial.println("2bpp streaming to panel failed");
                http.end();
                return false;
            }
            useColorBuf = false;
            useStreamedPanel = true;
            streamedPanelChecksum = streamSum;
            http.end();
            Serial.printf("2BPP streamed to panel  %d bytes\n", COLOR_BUF_LEN);
            lastHeartbeatAt = millis();
            return true;
#else
            if (!ensureColorBuf()) {
                Serial.println("colorBuf alloc failed, skip 2bpp");
                http.end();
                return false;
            }
            if (!readExact(stream, colorBuf, COLOR_BUF_LEN)) {
                Serial.println("Failed to read 2bpp data");
                http.end();
                return false;
            }
            useColorBuf = true;
            http.end();
            Serial.printf("2BPP OK  %d bytes\n", COLOR_BUF_LEN);
            lastHeartbeatAt = millis();
            return true;
#endif
        }
        Serial.printf("Not raw 2bpp (%d bytes), fallback to BMP\n", contentLen);
        useColorBuf = false;
#endif

        uint8_t fileHeader[14];
        if (!readExact(stream, fileHeader, 14)) {
            Serial.println("Failed to read BMP header");
            http.end();
            return false;
        }

        uint32_t pixelOffset = fileHeader[10]
                             | ((uint32_t)fileHeader[11] << 8)
                             | ((uint32_t)fileHeader[12] << 16)
                             | ((uint32_t)fileHeader[13] << 24);
        Serial.printf("BMP pixel offset: %u\n", pixelOffset);

        // Read info header to get bit count (biBitCount at offset 28 = 14+14)
        uint8_t infoHeader[16];
        if (!readExact(stream, infoHeader, 16)) {
            Serial.println("Failed to read BMP info header");
            http.end();
            return false;
        }
        int bmpBits = infoHeader[14] | ((int)infoHeader[15] << 8);
        Serial.printf("BMP bit count: %d\n", bmpBits);

        int toSkip = pixelOffset - 14 - 16;  // skip remaining header+palette
        while (toSkip > 0 && stream->connected()) {
            if (stream->available()) { stream->read(); toSkip--; }
        }

        memset(imgBuf, 0xFF, IMG_BUF_LEN);

        if (bmpBits <= 1) {
            // 1-bit BMP: each row is ROW_STRIDE bytes (padded)
            uint8_t rowBuf[ROW_STRIDE];
            for (int bmpY = 0; bmpY < H; bmpY++) {
                if (!readExact(stream, rowBuf, ROW_STRIDE)) {
                    Serial.printf("Failed to read row %d\n", bmpY);
                    http.end();
                    return false;
                }
                int dispY = H - 1 - bmpY;
                memcpy(imgBuf + dispY * ROW_BYTES, rowBuf, ROW_BYTES);
            }
        } else {
            // 8-bit (or 24-bit) BMP: convert each pixel to 1 bit
            int srcRowBytes = (W * bmpBits + 31) / 32 * 4;
            uint8_t *srcRow = (uint8_t *)malloc(srcRowBytes);
            if (!srcRow) {
                Serial.println("Failed to alloc srcRow");
                http.end();
                return false;
            }
            for (int bmpY = 0; bmpY < H; bmpY++) {
                if (!readExact(stream, srcRow, srcRowBytes)) {
                    Serial.printf("Failed to read row %d\n", bmpY);
                    free(srcRow);
                    http.end();
                    return false;
                }
                int dispY = H - 1 - bmpY;
                for (int x = 0; x < W; x++) {
                    uint8_t pixel;
                    if (bmpBits == 8) {
                        pixel = srcRow[x];
                    } else {
                        pixel = srcRow[x * 3];  // 24-bit: use blue channel
                    }
                    if (pixel < 128) {
                        imgBuf[dispY * ROW_BYTES + x / 8] &= ~(0x80 >> (x % 8));
                    }
                }
            }
            free(srcRow);
        }

        http.end();
        Serial.printf("BMP OK  %d bytes\n", IMG_BUF_LEN);
        lastHeartbeatAt = millis();
        return true;
    }
    return false;
}

bool hasPendingRemoteAction(bool *shouldExitLive) {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!ensureDeviceToken()) return false;

    String mac = WiFi.macAddress();
    String url = cfgServer + "/api/device/" + mac + "/state";

    bool useSSL = cfgServer.startsWith("https://");
    for (int attempt = 0; attempt < 2; attempt++) {
        if (checkAbort()) return false;
        WiFiClient plainClient;
        WiFiClientSecure secClient;
        HTTPClient http;
        if (useSSL) {
            secClient.setCACert(ROOT_CA);
            http.begin(secClient, url);
        } else {
            http.begin(plainClient, url);
        }
        http.setTimeout(HTTP_TIMEOUT);
        if (cfgDeviceToken.length() > 0) {
            http.addHeader("X-Device-Token", cfgDeviceToken);
        }

        int code = http.GET();
        if (code != 200) {
            http.end();
            if (!recoverDeviceTokenIfUnauthorized(code)) {
                return false;
            }
            continue;
        }

        String body = http.getString();
        http.end();

        // Do NOT treat runtime_mode=="interval" as "leave live mode". That field
        // is a status echo the backend derives from last_state_poll_at, so a
        // device that has just woken reads its own stale "interval" and puts
        // itself straight back to sleep — a self-sustaining loop that kept the
        // device offline (and the config page stuck on "interval").
        // Sleep timing is owned locally by the online window instead.
        (void)shouldExitLive;

        bool pendingRefresh =
            body.indexOf("\"pending_refresh\":1") >= 0 ||
            body.indexOf("\"pending_refresh\": 1") >= 0 ||
            body.indexOf("\"pending_refresh\":true") >= 0 ||
            body.indexOf("\"pending_refresh\": true") >= 0;

        bool pendingMode =
            (body.indexOf("\"pending_mode\":\"") >= 0 || body.indexOf("\"pending_mode\": \"") >= 0) &&
            body.indexOf("\"pending_mode\":\"\"") < 0 &&
            body.indexOf("\"pending_mode\": \"\"") < 0;

        return pendingRefresh || pendingMode;
    }
    return false;
}

bool peekPendingMode(String &pendingModeOut) {
    pendingModeOut = "";
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!ensureDeviceToken()) return false;

    String mac = WiFi.macAddress();
    String url = cfgServer + "/api/device/" + mac + "/state";
    bool useSSL = cfgServer.startsWith("https://");

    for (int attempt = 0; attempt < 2; attempt++) {
        WiFiClient plainClient;
        WiFiClientSecure secClient;
        HTTPClient http;
        if (useSSL) {
            secClient.setCACert(ROOT_CA);
            http.begin(secClient, url);
        } else {
            http.begin(plainClient, url);
        }

        http.setTimeout(HTTP_TIMEOUT);
        if (cfgDeviceToken.length() > 0) {
            http.addHeader("X-Device-Token", cfgDeviceToken);
        }

        int code = http.GET();
        if (code != 200) {
            http.end();
            if (!recoverDeviceTokenIfUnauthorized(code)) return false;
            continue;
        }

        String body = http.getString();
        http.end();
        pendingModeOut = extractJsonStringField(body, "pending_mode");
        return pendingModeOut.length() > 0;
    }

    return false;
}

// ── Post config to backend ──────────────────────────────────

void postConfigToBackend() {
    if (cfgConfigJson.length() == 0) return;
    if (!ensureDeviceToken()) return;

    // Inject MAC address into the config JSON
    String mac = WiFi.macAddress();
    String body = cfgConfigJson;
    if (body.startsWith("{")) {
        body = "{\"mac\":\"" + mac + "\"," + body.substring(1);
    }

    String url = cfgServer + "/api/config";
    bool useSSL = cfgServer.startsWith("https://");
    for (int attempt = 0; attempt < 2; attempt++) {
        if (checkAbort()) return;
        WiFiClient plainClient;
        WiFiClientSecure secClient;
        HTTPClient http;
        if (useSSL) {
            secClient.setCACert(ROOT_CA);
            http.begin(secClient, url);
        } else {
            http.begin(plainClient, url);
        }
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(HTTP_TIMEOUT);
        if (cfgDeviceToken.length() > 0) {
            http.addHeader("X-Device-Token", cfgDeviceToken);
        }

        int code = http.POST(body);
        Serial.printf("POST /api/config -> %d\n", code);
        http.end();
        if (!recoverDeviceTokenIfUnauthorized(code)) {
            return;
        }
    }
}

// ── Post runtime mode to backend ────────────────────────────

bool postRuntimeMode(const char *mode) {
    if (!ensureDeviceToken()) return false;
    String mac = WiFi.macAddress();
    String url = cfgServer + "/api/device/" + mac + "/runtime";
    bool useSSL = cfgServer.startsWith("https://");
    String body = String("{\"mode\":\"") + mode + "\"}";
    for (int attempt = 0; attempt < 2; attempt++) {
        WiFiClient plainClient;
        WiFiClientSecure secClient;
        HTTPClient http;
        if (useSSL) {
            secClient.setCACert(ROOT_CA);
            http.begin(secClient, url);
        } else {
            http.begin(plainClient, url);
        }
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(HTTP_TIMEOUT);
        if (cfgDeviceToken.length() > 0) {
            http.addHeader("X-Device-Token", cfgDeviceToken);
        }

        int code = http.POST(body);
        http.end();

        if (code == 404) {
            return true;
        }
        if (code >= 200 && code < 300) {
            return true;
        }
        if (!recoverDeviceTokenIfUnauthorized(code)) {
            return false;
        }
    }
    return false;
}

bool postVocabEvent(const char *action, const char *rating) {
    if (!ensureDeviceToken()) return false;
    String mac = WiFi.macAddress();
    String url = cfgServer + "/api/device/" + mac + "/vocab/event";
    bool useSSL = cfgServer.startsWith("https://");
    String body = String("{\"action\":\"") + (action ? action : "") + "\"";
    if (rating && strlen(rating) > 0) {
        body += String(",\"rating\":\"") + rating + "\"";
    }
    body += "}";

    for (int attempt = 0; attempt < 2; attempt++) {
        WiFiClient plainClient;
        WiFiClientSecure secClient;
        HTTPClient http;
        if (useSSL) {
            secClient.setCACert(ROOT_CA);
            http.begin(secClient, url);
        } else {
            http.begin(plainClient, url);
        }
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(HTTP_TIMEOUT);
        if (cfgDeviceToken.length() > 0) {
            http.addHeader("X-Device-Token", cfgDeviceToken);
        }

        int code = http.POST(body);
        http.end();
        Serial.printf("[VOCAB] POST %s -> %d\n", action ? action : "", code);
        if (code >= 200 && code < 300) {
            return true;
        }
        if (!recoverDeviceTokenIfUnauthorized(code)) {
            return false;
        }
    }
    return false;
}

static uint16_t readLe16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t readLe32(const uint8_t *p) {
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

bool fetchVocabReviewPack(uint8_t *ratingParts, size_t partLen, int yStart, int yEnd) {
    if (!ratingParts || partLen == 0) return false;
    if (!ensureDeviceToken()) return false;

    float v = readBatteryVoltage();
    String mac = WiFi.macAddress();
    String url = cfgServer + "/api/device/" + mac + "/vocab/review-pack"
               + "?v=" + String(v, 2)
               + "&w=" + String(W)
               + "&h=" + String(H)
               + "&y_start=" + String(yStart)
               + "&y_end=" + String(yEnd);
    bool useSSL = cfgServer.startsWith("https://");

    for (int attempt = 0; attempt < 2; attempt++) {
        WiFiClient plainClient;
        WiFiClientSecure secClient;
        HTTPClient http;
        if (useSSL) {
            secClient.setCACert(ROOT_CA);
            http.begin(secClient, url);
        } else {
            http.begin(plainClient, url);
        }
        http.setTimeout(HTTP_TIMEOUT);
        http.addHeader("Accept-Encoding", "identity");
        if (cfgDeviceToken.length() > 0) {
            http.addHeader("X-Device-Token", cfgDeviceToken);
        }

        int code = http.GET();
        Serial.printf("[VOCAB] GET review-pack -> %d\n", code);
        if (code != 200) {
            if (code < 0) {
                Serial.printf("[VOCAB] review-pack error: %s\n", http.errorToString(code).c_str());
            } else {
                String body = http.getString();
                Serial.printf("[VOCAB] review-pack response: %s\n", body.substring(0, 200).c_str());
            }
            http.end();
            if (!recoverDeviceTokenIfUnauthorized(code)) return false;
            continue;
        }

        WiFiClient *stream = http.getStreamPtr();
        uint8_t header[24];
        if (!readExact(stream, header, sizeof(header))) {
            http.end();
            return false;
        }
        if (memcmp(header, "IVP1", 4) != 0) {
            Serial.println("[VOCAB] review-pack bad magic");
            http.end();
            return false;
        }
        uint16_t packW = readLe16(header + 4);
        uint16_t packH = readLe16(header + 6);
        uint16_t packYStart = readLe16(header + 8);
        uint16_t packYEnd = readLe16(header + 10);
        uint32_t fullLen = readLe32(header + 12);
        uint32_t packPartLen = readLe32(header + 16);
        uint8_t ratingCount = header[20];
        if (packW != W || packH != H || packYStart != yStart || packYEnd != yEnd ||
            fullLen != IMG_BUF_LEN || packPartLen != partLen || ratingCount != 3) {
            Serial.printf("[VOCAB] review-pack mismatch w=%u h=%u y=%u-%u full=%u part=%u count=%u\n",
                          packW, packH, packYStart, packYEnd, fullLen, packPartLen, ratingCount);
            http.end();
            return false;
        }
        if (!readExact(stream, imgBuf, IMG_BUF_LEN)) {
            http.end();
            return false;
        }
        if (!readExact(stream, ratingParts, partLen * 3)) {
            http.end();
            return false;
        }
        http.end();
        Serial.printf("[VOCAB] review-pack OK front=%d parts=%u\n", IMG_BUF_LEN, (unsigned)(partLen * 3));
        lastHeartbeatAt = millis();
        return true;
    }
    return false;
}

bool fetchVocabAudio(AudioChunkCallback onChunk, void *userData) {
    if (!onChunk) return false;
    if (!ensureDeviceToken()) return false;
    String mac = WiFi.macAddress();
    String url = cfgServer + "/api/device/" + mac + "/vocab/audio";
    bool useSSL = cfgServer.startsWith("https://");
    for (int attempt = 0; attempt < 2; attempt++) {
        WiFiClient plainClient;
        WiFiClientSecure secClient;
        HTTPClient http;
        if (useSSL) {
            secClient.setCACert(ROOT_CA);
            http.begin(secClient, url);
        } else {
            http.begin(plainClient, url);
        }
        http.setTimeout(60000);
        if (cfgDeviceToken.length() > 0) {
            http.addHeader("X-Device-Token", cfgDeviceToken);
        }

        int code = http.GET();
        int contentLen = http.getSize();
        if (code == 204) {
            Serial.println("[VOCAB] audio -> 204 no content");
            http.end();
            return true;
        }
        if (code == 200) {
            WiFiClient *stream = http.getStreamPtr();
            uint8_t buffer[1024];
            size_t totalRead = 0;
            unsigned long lastDataAt = millis();
            while (http.connected() || stream->available()) {
                int available = stream->available();
                if (available <= 0) {
                    if (contentLen >= 0 && totalRead >= (size_t)contentLen) {
                        break;
                    }
                    if (millis() - lastDataAt > 3000) {
                        Serial.printf("[VOCAB] audio read timeout total=%u expected=%d connected=%d\n",
                                      (unsigned int)totalRead, contentLen, http.connected() ? 1 : 0);
                        break;
                    }
                    delay(1);
                    continue;
                }
                int readLen = stream->readBytes(buffer, min(available, (int)sizeof(buffer)));
                if (readLen > 0) {
                    totalRead += (size_t)readLen;
                    lastDataAt = millis();
                    onChunk(buffer, (size_t)readLen, userData);
                }
                if (contentLen >= 0 && totalRead >= (size_t)contentLen) {
                    break;
                }
            }
            http.end();
            return true;
        }
        if (code < 0) {
            Serial.printf("[VOCAB] audio error: %s\n", http.errorToString(code).c_str());
        } else {
            String body = http.getString();
            Serial.printf("[VOCAB] audio -> %d %s\n", code, body.substring(0, 200).c_str());
        }
        http.end();
        if (!recoverDeviceTokenIfUnauthorized(code)) {
            return false;
        }
    }
    return false;
}

static bool parseVoiceTurnResponse(const String &body, String &turnId, String &replyText, String &transcript, bool &exitConversation) {
    turnId = extractJsonStringField(body, "turn_id");
    replyText = extractJsonStringField(body, "reply_text");
    transcript = extractJsonStringField(body, "transcript");
    String exitStr = extractJsonStringField(body, "exit_conversation");
    exitConversation = false;
    if (exitStr == "true" || exitStr == "1") {
        exitConversation = true;
    } else if (body.indexOf("\"exit_conversation\":true") >= 0) {
        exitConversation = true;
    }
    return turnId.length() > 0;
}

bool submitVoiceTurn(const char *pcmPath, int sampleRate, int screenW, int screenH, String &turnId, String &replyText, String &transcript, bool &exitConversation) {
    turnId = "";
    replyText = "";
    transcript = "";
    exitConversation = false;
    if (!ensureDeviceToken()) return false;

    File file = LittleFS.open(pcmPath, "r");
    if (!file) {
        Serial.println("[VOICE] failed to open PCM file");
        return false;
    }

    String mac = WiFi.macAddress();
    String url = cfgServer + "/api/device/" + mac + "/voice/turn"
               + "?sample_rate=" + String(sampleRate)
               + "&w=" + String(screenW)
               + "&h=" + String(screenH);
    bool useSSL = cfgServer.startsWith("https://");

    for (int attempt = 0; attempt < 2; attempt++) {
        file.seek(0);
        WiFiClient plainClient;
        WiFiClientSecure secClient;
        HTTPClient http;
        if (useSSL) {
            secClient.setCACert(ROOT_CA);
            http.begin(secClient, url);
        } else {
            http.begin(plainClient, url);
        }
        http.addHeader("Content-Type", "application/octet-stream");
        http.setTimeout(60000);
        if (cfgDeviceToken.length() > 0) {
            http.addHeader("X-Device-Token", cfgDeviceToken);
        }

        int code = http.sendRequest("POST", &file, file.size());
        if (code >= 200 && code < 300) {
            String body = http.getString();
            http.end();
            file.close();
            return parseVoiceTurnResponse(body, turnId, replyText, transcript, exitConversation);
        }
        if (code < 0) {
            Serial.printf("[VOICE] submit error: %s\n", http.errorToString(code).c_str());
        } else {
            String body = http.getString();
            Serial.printf("[VOICE] submit -> %d %s\n", code, body.substring(0, 300).c_str());
        }
        http.end();
        if (!recoverDeviceTokenIfUnauthorized(code)) {
            file.close();
            return false;
        }
    }

    file.close();
    return false;
}

bool submitVoiceTurnBytes(const uint8_t *pcmBytes, size_t pcmSize, int sampleRate, int screenW, int screenH, String &turnId, String &replyText, String &transcript, bool &exitConversation) {
    turnId = "";
    replyText = "";
    transcript = "";
    exitConversation = false;
    if (!ensureDeviceToken()) return false;
    if (pcmBytes == NULL || pcmSize == 0) {
        Serial.println("[VOICE] empty PCM payload");
        return false;
    }

    String mac = WiFi.macAddress();
    String url = cfgServer + "/api/device/" + mac + "/voice/turn"
               + "?sample_rate=" + String(sampleRate)
               + "&w=" + String(screenW)
               + "&h=" + String(screenH);
    bool useSSL = cfgServer.startsWith("https://");

    for (int attempt = 0; attempt < 2; attempt++) {
        WiFiClient plainClient;
        WiFiClientSecure secClient;
        HTTPClient http;
        if (useSSL) {
            secClient.setCACert(ROOT_CA);
            http.begin(secClient, url);
        } else {
            http.begin(plainClient, url);
        }
        http.addHeader("Content-Type", "application/octet-stream");
        http.setTimeout(60000);
        if (cfgDeviceToken.length() > 0) {
            http.addHeader("X-Device-Token", cfgDeviceToken);
        }

        int code = http.sendRequest("POST", (uint8_t *)pcmBytes, pcmSize);
        if (code >= 200 && code < 300) {
            String body = http.getString();
            http.end();
            return parseVoiceTurnResponse(body, turnId, replyText, transcript, exitConversation);
        }
        if (code < 0) {
            Serial.printf("[VOICE] submit-bytes error: %s\n", http.errorToString(code).c_str());
        } else {
            String body = http.getString();
            Serial.printf("[VOICE] submit-bytes -> %d %s\n", code, body.substring(0, 300).c_str());
        }
        http.end();
        if (!recoverDeviceTokenIfUnauthorized(code)) {
            return false;
        }
    }

    return false;
}

bool fetchVoiceAudio(const String &turnId, const char *path) {
    if (!ensureDeviceToken()) return false;
    String mac = WiFi.macAddress();
    String url = cfgServer + "/api/device/" + mac + "/voice/" + turnId + "/audio";
    bool useSSL = cfgServer.startsWith("https://");
    for (int attempt = 0; attempt < 2; attempt++) {
        File file = LittleFS.open(path, "w");
        if (!file) {
            Serial.println("[VOICE] failed to open audio output file");
            return false;
        }
        WiFiClient plainClient;
        WiFiClientSecure secClient;
        HTTPClient http;
        if (useSSL) {
            secClient.setCACert(ROOT_CA);
            http.begin(secClient, url);
        } else {
            http.begin(plainClient, url);
        }
        http.setTimeout(60000);
        if (cfgDeviceToken.length() > 0) {
            http.addHeader("X-Device-Token", cfgDeviceToken);
        }

        int code = http.GET();
        if (code == 200) {
            WiFiClient *stream = http.getStreamPtr();
            uint8_t buffer[1024];
            while (http.connected() || stream->available()) {
                int available = stream->available();
                if (available <= 0) {
                    delay(1);
                    continue;
                }
                int readLen = stream->readBytes(buffer, min(available, (int)sizeof(buffer)));
                if (readLen > 0) {
                    file.write(buffer, readLen);
                }
            }
            http.end();
            file.close();
            return true;
        }
        if (code < 0) {
            Serial.printf("[VOICE] audio error: %s\n", http.errorToString(code).c_str());
        } else {
            String body = http.getString();
            Serial.printf("[VOICE] audio -> %d %s\n", code, body.substring(0, 200).c_str());
        }
        http.end();
        if (!recoverDeviceTokenIfUnauthorized(code)) {
            file.close();
            return false;
        }
        file.close();
    }
    return false;
}

bool fetchVoiceImage(const String &turnId) {
    if (!ensureDeviceToken()) return false;

    String mac = WiFi.macAddress();
    String url = cfgServer + "/api/device/" + mac + "/voice/" + turnId + "/image";
    bool useSSL = cfgServer.startsWith("https://");
    for (int attempt = 0; attempt < 2; attempt++) {
        WiFiClient plainClient;
        WiFiClientSecure secClient;
        HTTPClient http;
        if (useSSL) {
            secClient.setCACert(ROOT_CA);
            http.begin(secClient, url);
        } else {
            http.begin(plainClient, url);
        }
        http.setTimeout(HTTP_TIMEOUT);
        if (cfgDeviceToken.length() > 0) {
            http.addHeader("X-Device-Token", cfgDeviceToken);
        }

        int code = http.GET();
        if (code != 200) {
            if (code < 0) {
                Serial.printf("[VOICE] image error: %s\n", http.errorToString(code).c_str());
            } else {
                String body = http.getString();
                Serial.printf("[VOICE] image -> %d %s\n", code, body.substring(0, 200).c_str());
            }
            http.end();
            if (!recoverDeviceTokenIfUnauthorized(code)) {
                return false;
            }
            continue;
        }

        WiFiClient *stream = http.getStreamPtr();
        uint8_t fileHeader[14];
        if (!readExact(stream, fileHeader, 14)) {
            http.end();
            return false;
        }

        uint32_t pixelOffset = fileHeader[10]
                             | ((uint32_t)fileHeader[11] << 8)
                             | ((uint32_t)fileHeader[12] << 16)
                             | ((uint32_t)fileHeader[13] << 24);
        int toSkip = (int)pixelOffset - 14;
        while (toSkip > 0 && stream->connected()) {
            if (stream->available()) {
                stream->read();
                toSkip--;
            }
        }

        uint8_t rowBuf[ROW_STRIDE];
        memset(imgBuf, 0xFF, IMG_BUF_LEN);
        for (int bmpY = 0; bmpY < H; bmpY++) {
            if (!readExact(stream, rowBuf, ROW_STRIDE)) {
                http.end();
                return false;
            }
            int dispY = H - 1 - bmpY;
            memcpy(imgBuf + dispY * ROW_BYTES, rowBuf, ROW_BYTES);
        }

        http.end();
        return true;
    }
    return false;
}

bool fetchVoiceIntroImage(int screenW, int screenH) {
    if (!ensureDeviceToken()) return false;

    String mac = WiFi.macAddress();
    String url = cfgServer + "/api/device/" + mac + "/voice/intro/image?w=" + String(screenW) + "&h=" + String(screenH);
    bool useSSL = cfgServer.startsWith("https://");
    for (int attempt = 0; attempt < 2; attempt++) {
        WiFiClient plainClient;
        WiFiClientSecure secClient;
        HTTPClient http;
        if (useSSL) {
            secClient.setCACert(ROOT_CA);
            http.begin(secClient, url);
        } else {
            http.begin(plainClient, url);
        }
        http.setTimeout(HTTP_TIMEOUT);
        if (cfgDeviceToken.length() > 0) {
            http.addHeader("X-Device-Token", cfgDeviceToken);
        }

        int code = http.GET();
        if (code != 200) {
            if (code < 0) {
                Serial.printf("[VOICE] intro image error: %s\n", http.errorToString(code).c_str());
            } else {
                String body = http.getString();
                Serial.printf("[VOICE] intro image -> %d %s\n", code, body.substring(0, 200).c_str());
            }
            http.end();
            if (!recoverDeviceTokenIfUnauthorized(code)) {
                return false;
            }
            continue;
        }

        WiFiClient *stream = http.getStreamPtr();
        uint8_t fileHeader[14];
        if (!readExact(stream, fileHeader, 14)) {
            http.end();
            return false;
        }

        uint32_t pixelOffset = fileHeader[10]
                             | ((uint32_t)fileHeader[11] << 8)
                             | ((uint32_t)fileHeader[12] << 16)
                             | ((uint32_t)fileHeader[13] << 24);
        int toSkip = (int)pixelOffset - 14;
        while (toSkip > 0 && stream->connected()) {
            if (stream->available()) {
                stream->read();
                toSkip--;
            }
        }

        uint8_t rowBuf[ROW_STRIDE];
        memset(imgBuf, 0xFF, IMG_BUF_LEN);
        for (int bmpY = 0; bmpY < H; bmpY++) {
            if (!readExact(stream, rowBuf, ROW_STRIDE)) {
                http.end();
                return false;
            }
            int dispY = H - 1 - bmpY;
            memcpy(imgBuf + dispY * ROW_BYTES, rowBuf, ROW_BYTES);
        }

        http.end();
        return true;
    }
    return false;
}

static void handleVoiceWsEvent(VoiceWsEvent &event) {
    if (gVoiceWsEventQueue == nullptr) {
        voiceWsReleaseEvent(event);
        return;
    }
    VoiceWsEvent *heapEvent = new VoiceWsEvent(event);
    if (heapEvent == nullptr) {
        voiceWsReleaseEvent(event);
        return;
    }
    if (xQueueSend(gVoiceWsEventQueue, &heapEvent, 0) != pdTRUE) {
        delete heapEvent;
        voiceWsReleaseEvent(event);
    }
}

static void voiceWsSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            gVoiceWsConnected = true;
            gVoiceWsDisconnected = false;
            gVoiceWsConnectedAt = millis();
            Serial.printf("[VOICE_WS] connected (heap=%u)\n", ESP.getFreeHeap());
            return;
        case WStype_DISCONNECTED: {
            bool wasConnected = gVoiceWsConnected;
            gVoiceWsConnected = false;
            gVoiceWsDisconnected = true;
            if (gVoiceWsConnectedAt > 0) {
                Serial.printf("[VOICE_WS] disconnected after %lu ms\n", millis() - gVoiceWsConnectedAt);
            } else if (gVoiceWsOpenStartedAt > 0) {
                Serial.printf("[VOICE_WS] disconnected before connected after %lu ms\n", millis() - gVoiceWsOpenStartedAt);
            } else {
                Serial.println("[VOICE_WS] disconnected before connected");
            }
            if (payload != nullptr && length > 0) {
                Serial.printf("[VOICE_WS] disconnected payload len=%u: ", (unsigned)length);
                for (size_t i = 0; i < length; i++) Serial.print((char)payload[i]);
                Serial.println();
            } else {
                Serial.println("[VOICE_WS] disconnected (no payload)");
            }
            if (wasConnected) {
                gVoiceWsConnectedAt = 0;
            }
            return;
        }
        case WStype_BIN: {
            if (payload == nullptr || length == 0) return;
            VoiceWsEvent event;
            event.type = VoiceWsEventType::TtsAudioChunk;
            event.sampleRate = 16000;
            event.needsDecode = gVoiceWsOpusEnabled;
            event.data = (uint8_t *)malloc(length);
            if (event.data == nullptr) return;
            memcpy(event.data, payload, length);
            event.dataLen = length;
            handleVoiceWsEvent(event);
            return;
        }
        case WStype_TEXT:
            Serial.printf("[VOICE_WS] recv text len=%u: %.120s\n", (unsigned)length, payload ? (char *)payload : "");
            break;
        case WStype_ERROR:
            if (payload && length > 0) {
                Serial.printf("[VOICE_WS] socket error: %.*s\n", (int)length, (char *)payload);
            } else {
                Serial.println("[VOICE_WS] socket error (no payload)");
            }
            return;
        default:
            return;
    }

    String body;
    body.reserve(length + 1);
    for (size_t i = 0; i < length; i++) {
        body += (char)payload[i];
    }
    String eventName = extractJsonStringField(body, "event");
    if (eventName.length() == 0) {
        return;
    }

    VoiceWsEvent event;
    if (eventName == "session.ready") {
        event.type = VoiceWsEventType::SessionReady;
        String binaryAudio = extractJsonBoolField(body, "binary_audio");
        gVoiceWsBinaryAudioEnabled = (binaryAudio == "true" || binaryAudio == "1");
        String codec = extractJsonStringField(body, "audio_codec");
        gVoiceWsOpusEnabled = (codec == "opus");
        String serverVad = extractJsonBoolField(body, "server_vad");
        gVoiceWsServerVadEnabled = (serverVad == "true" || serverVad == "1");
    } else if (eventName == "asr.partial") {
        event.type = VoiceWsEventType::AsrPartial;
        event.text = extractJsonStringField(body, "text");
    } else if (eventName == "asr.final") {
        event.type = VoiceWsEventType::AsrFinal;
        event.transcript = extractJsonStringField(body, "transcript");
    } else if (eventName == "llm.delta") {
        event.type = VoiceWsEventType::LlmDelta;
        event.text = extractJsonStringField(body, "delta");
        event.generationId = extractJsonIntField(body, "generation_id", 0);
    } else if (eventName == "tts.text_chunk") {
        event.type = VoiceWsEventType::TtsTextChunk;
        event.text = extractJsonStringField(body, "text");
        event.generationId = extractJsonIntField(body, "generation_id", 0);
        event.chunkId = extractJsonIntField(body, "chunk_id", 0);
    } else if (eventName == "tts.audio_chunk") {
        event.type = VoiceWsEventType::TtsAudioChunk;
        event.generationId = extractJsonIntField(body, "generation_id", 0);
        event.chunkId = extractJsonIntField(body, "chunk_id", 0);
        event.sampleRate = extractJsonIntField(body, "sample_rate", 16000);
        if (!decodeBase64ToHeap(extractJsonStringField(body, "audio"), &event.data, &event.dataLen)) {
            Serial.println("[VOICE_WS] failed to decode audio chunk");
            return;
        }
    } else if (eventName == "turn.done") {
        event.type = VoiceWsEventType::TurnDone;
        event.turnId = extractJsonStringField(body, "turn_id");
        event.transcript = extractJsonStringField(body, "transcript");
        event.text = extractJsonStringField(body, "reply_text");
        event.generationId = extractJsonIntField(body, "generation_id", 0);
        String exitFlag = extractJsonBoolField(body, "exit_conversation");
        event.exitConversation = (exitFlag == "true" || exitFlag == "1");
        event.switchToMode = extractJsonStringField(body, "switch_to_mode");
        if (!decodeBase64ToHeap(extractJsonStringField(body, "image"), &event.data, &event.dataLen)) {
            Serial.println("[VOICE_WS] failed to decode image payload");
            return;
        }
    } else if (eventName == "turn.interrupted") {
        event.type = VoiceWsEventType::TurnInterrupted;
        event.generationId = extractJsonIntField(body, "generation_id", 0);
    } else if (eventName == "error") {
        event.type = VoiceWsEventType::Error;
        event.text = extractJsonStringField(body, "message");
    } else {
        return;
    }
    handleVoiceWsEvent(event);
}

bool voiceWsOpen(int sampleRate, int screenW, int screenH, bool includeImage) {
    Serial.printf("[VOICE_WS] free heap before open: %u\n", ESP.getFreeHeap());
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!ensureDeviceToken()) return false;

    if (gVoiceWsEventQueue == nullptr) {
        gVoiceWsEventQueue = xQueueCreate(12, sizeof(VoiceWsEvent *));
        if (gVoiceWsEventQueue == nullptr) {
            return false;
        }
    }

    bool useSSL = false;
    String host;
    uint16_t port = 0;
    String basePath;
    if (!parseWsServerUrl(cfgServer, useSSL, host, port, basePath)) {
        Serial.println("[VOICE_WS] invalid server url");
        return false;
    }

    String mac = WiFi.macAddress();
    String path = basePath + "/api/device/" + mac + "/voice/ws?token=" + cfgDeviceToken;
    String extraHeaders = String("X-Device-Token: ") + cfgDeviceToken;
    String startMsg = String("{\"type\":\"session.start\",\"sample_rate\":") + sampleRate
                    + ",\"w\":" + screenW
                    + ",\"h\":" + screenH
                    + ",\"include_image\":" + (includeImage ? "true" : "false")
                    + ",\"protocol\":2"
#if ENABLE_OPUS
                    + ",\"audio_codec\":\"opus\""
#else
                    + ",\"audio_codec\":\"pcm\""
#endif
                    + "}";

    for (int attempt = 1; attempt <= 3; attempt++) {
        voiceWsClose();
        gVoiceWsConnected = false;
        gVoiceWsDisconnected = false;
        gVoiceWsBinaryAudioEnabled = false;
        gVoiceWsOpusEnabled = false;
        gVoiceWsServerVadEnabled = false;
        gVoiceWsOpenStartedAt = millis();
        gVoiceWsConnectedAt = 0;

        gVoiceWsClient.onEvent(voiceWsSocketEvent);
        gVoiceWsClient.setReconnectInterval(60000);
        gVoiceWsClient.setExtraHeaders(extraHeaders.c_str());

        Serial.printf("[VOICE_WS] attempt %d/3 %s://%s:%u%s\n",
            attempt, useSSL ? "wss" : "ws", host.c_str(), port, path.c_str());

        if (useSSL) {
            gVoiceWsClient.beginSSL(host.c_str(), port, path.c_str(), nullptr, "");
        } else {
            gVoiceWsClient.begin(host.c_str(), port, path.c_str(), "");
        }
        gVoiceWsConfigured = true;

        unsigned long startAt = millis();
        while (!gVoiceWsConnected && !gVoiceWsDisconnected && millis() - startAt < 15000UL) {
            gVoiceWsClient.loop();
            delay(10);
        }
        if (!gVoiceWsConnected) {
            Serial.printf("[VOICE_WS] attempt %d failed: %s\n", attempt,
                gVoiceWsDisconnected ? "disconnected before connected" : "connect timeout");
            voiceWsClose();
            delay(500);
            continue;
        }

        // Stabilize: loop for 500ms, bail if connection drops.
        bool stable = true;
        unsigned long stableStart = millis();
        while (millis() - stableStart < 500UL) {
            gVoiceWsClient.loop();
            if (!gVoiceWsConnected) {
                Serial.printf("[VOICE_WS] attempt %d failed: dropped during stabilization\n", attempt);
                stable = false;
                break;
            }
            delay(10);
        }
        if (!stable) {
            voiceWsClose();
            delay(500);
            continue;
        }

        Serial.printf("[VOICE_WS] sending session.start len=%u\n", (unsigned)startMsg.length());
        if (!gVoiceWsClient.sendTXT(startMsg)) {
            Serial.printf("[VOICE_WS] attempt %d failed: sendTXT returned false\n", attempt);
            voiceWsClose();
            delay(500);
            continue;
        }
        Serial.println("[VOICE_WS] session.start sent OK");

        // Flush: loop for 500ms to ensure the frame is sent.
        bool flushed = true;
        unsigned long flushStart = millis();
        while (millis() - flushStart < 500UL) {
            gVoiceWsClient.loop();
            if (!gVoiceWsConnected) {
                Serial.printf("[VOICE_WS] attempt %d failed: dropped after session.start\n", attempt);
                flushed = false;
                break;
            }
            delay(10);
        }
        if (!flushed) {
            voiceWsClose();
            delay(500);
            continue;
        }

        gVoiceWsClient.enableHeartbeat(15000, 3000, 2);
        Serial.printf("[VOICE_WS] attempt %d connected successfully\n", attempt);
        return true;
    }

    Serial.println("[VOICE_WS] all 3 attempts failed");
    return false;
}

bool voiceWsConnected() {
    return gVoiceWsConnected;
}

void voiceWsLoop() {
    if (gVoiceWsConfigured) {
        gVoiceWsClient.loop();
    }
}

bool voiceWsSendAudioBin(const int16_t *samples, size_t sampleCount) {
    if (!gVoiceWsConnected || samples == nullptr || sampleCount == 0) return false;
#if ENABLE_OPUS
    if (gVoiceWsOpusEnabled) {
        uint8_t opusBuf[OPUS_MAX_PACKET_BYTES];
        int encoded = opusEncode(samples, sampleCount, opusBuf, sizeof(opusBuf));
        if (encoded > 0) {
            return gVoiceWsClient.sendBIN(opusBuf, encoded);
        }
        return false;
    }
#endif
    return gVoiceWsClient.sendBIN((const uint8_t *)samples, sampleCount * sizeof(int16_t));
}

bool voiceWsSendAudioChunk(const int16_t *samples, size_t sampleCount) {
    if (!gVoiceWsConnected || samples == nullptr || sampleCount == 0) return false;
    if (gVoiceWsBinaryAudioEnabled) {
        return voiceWsSendAudioBin(samples, sampleCount);
    }
    String audio = encodeBase64((const uint8_t *)samples, sampleCount * sizeof(int16_t));
    if (audio.length() == 0) {
        return false;
    }
    String msg = String("{\"type\":\"audio.append\",\"audio\":\"") + audio + "\"}";
    return gVoiceWsClient.sendTXT(msg);
}

bool voiceWsSendRawPacket(const uint8_t *data, size_t len) {
    if (!gVoiceWsConnected || data == nullptr || len == 0) return false;
    if (gVoiceWsBinaryAudioEnabled) {
        return gVoiceWsClient.sendBIN(data, len);
    }
    String audio = encodeBase64(data, len);
    if (audio.length() == 0) return false;
    String msg = String("{\"type\":\"audio.append\",\"audio\":\"") + audio + "\"}";
    return gVoiceWsClient.sendTXT(msg);
}

bool voiceWsBinaryAudio() {
    return gVoiceWsBinaryAudioEnabled;
}

bool voiceWsServerVad() {
    return gVoiceWsServerVadEnabled;
}

bool voiceWsCommitTurn() {
    if (!gVoiceWsConnected) return false;
    return gVoiceWsClient.sendTXT("{\"type\":\"audio.commit\"}");
}

bool voiceWsInterrupt() {
    if (!gVoiceWsConnected) return false;
    return gVoiceWsClient.sendTXT("{\"type\":\"interrupt\"}");
}

bool voiceWsPollEvent(VoiceWsEvent &eventOut) {
    if (gVoiceWsEventQueue == nullptr) return false;
    VoiceWsEvent *heapEvent = nullptr;
    if (xQueueReceive(gVoiceWsEventQueue, &heapEvent, 0) != pdTRUE || heapEvent == nullptr) {
        return false;
    }
    eventOut = *heapEvent;
    delete heapEvent;
    return true;
}

void voiceWsReleaseEvent(VoiceWsEvent &event) {
    if (event.data != nullptr) {
        free(event.data);
        event.data = nullptr;
    }
    event.dataLen = 0;
    event.text = "";
    event.transcript = "";
    event.turnId = "";
    event.type = VoiceWsEventType::None;
}

void voiceWsClose() {
    if (gVoiceWsConfigured) {
        if (gVoiceWsConnected) {
            gVoiceWsClient.sendTXT("{\"type\":\"close\"}");
        }
        gVoiceWsClient.disconnect();
    }
    gVoiceWsConfigured = false;
    gVoiceWsConnected = false;
    gVoiceWsDisconnected = false;
    gVoiceWsOpenStartedAt = 0;
    gVoiceWsConnectedAt = 0;
    gVoiceWsBinaryAudioEnabled = false;
    gVoiceWsOpusEnabled = false;
    gVoiceWsServerVadEnabled = false;
    if (gVoiceWsEventQueue != nullptr) {
        VoiceWsEvent *heapEvent = nullptr;
        while (xQueueReceive(gVoiceWsEventQueue, &heapEvent, 0) == pdTRUE) {
            if (heapEvent != nullptr) {
                voiceWsReleaseEvent(*heapEvent);
                delete heapEvent;
            }
        }
    }
}

// ── NTP time sync ───────────────────────────────────────────

void syncNTP() {
    configTime(NTP_UTC_OFFSET, 0, "ntp.aliyun.com", "pool.ntp.org");
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) {
        curHour = timeinfo.tm_hour;
        curMin  = timeinfo.tm_min;
        curSec  = timeinfo.tm_sec;
        Serial.printf("NTP synced: %02d:%02d:%02d\n", curHour, curMin, curSec);
    } else {
        curHour = 0; curMin = 0; curSec = 0;
        Serial.println("NTP failed, using 00:00:00");
    }
}

// ── Software clock tick ─────────────────────────────────────

void tickTime() {
    curSec++;
    if (curSec >= 60) { curSec = 0; curMin++; }
    if (curMin >= 60) { curMin = 0; curHour++; }
    if (curHour >= 24) { curHour = 0; }
}
