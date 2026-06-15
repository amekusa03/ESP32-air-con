#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <sys/time.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include "secrets.h" // セキュリティ情報を別ファイルから読み込み

// --- NTP時刻同期設定 ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 9 * 3600; // 日本標準時(JST)
const int   daylightOffset_sec = 0;

Preferences preferences;
String airconId = "";

// --- センサー設定 ---
Adafruit_SHT31 shtSensor = Adafruit_SHT31();
const char* SENSOR_NAME = "SHT30/31";

bool hasSensor = false;

// SwitchBot API v1.1 の署名(sign)を生成する関数
String getSignature(String t, String nonce) {
    String data = token + t + nonce;
    unsigned char hmacResult[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
    
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)secret.c_str(), secret.length());
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)data.c_str(), data.length());
    mbedtls_md_hmac_finish(&ctx, hmacResult);
    mbedtls_md_free(&ctx);

    unsigned char base64Result[64];
    size_t out_len;
    mbedtls_base64_encode(base64Result, sizeof(base64Result), &out_len, hmacResult, 32);

    String sign = String((char *)base64Result);
    sign.toUpperCase(); // API仕様に基づき大文字化
    return sign;
}

// --- エアコン制御関数 ---
// mode: 1(自動), 2(冷房), 3(除湿), 4(送風), 5(暖房)
// fan: 1(自動), 2(弱), 3(中), 4(強)
// powerOn: true(ON), false(OFF)
void controlAircon(int temp, int mode, int fan, bool powerOn) {
    if (airconId == "") return;

    HTTPClient http;
    String url = "https://api.switch-bot.com/v1.1/devices/" + airconId + "/commands";
    http.begin(url);

    // エポックミリ秒の取得
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t time_ms = (int64_t)tv.tv_sec * 1000L + (int64_t)tv.tv_usec / 1000L;
    String t = String(time_ms);
    String nonce = String(esp_random());
    String sign = getSignature(t, nonce);

    // ヘッダーの設定
    http.addHeader("Authorization", token);
    http.addHeader("sign", sign);
    http.addHeader("t", t);
    http.addHeader("nonce", nonce);
    http.addHeader("Content-Type", "application/json");

    // パラメータ文字列の生成: "{temperature},{mode},{fanspeed},{power}"
    String powerStr = powerOn ? "on" : "off";
    String parameter = String(temp) + "," + String(mode) + "," + String(fan) + "," + powerStr;
    
    // JSONペイロード
    String payload = "{\"command\":\"setAll\",\"parameter\":\"" + parameter + "\",\"commandType\":\"command\"}";
    
    Serial.println("\n-> Sending API Request: " + payload);
    
    int httpCode = http.POST(payload);
    if (httpCode > 0) {
        String response = http.getString();
        Serial.printf("<- [OK] HTTP %d: %s\n", httpCode, response.c_str());
    } else {
        Serial.printf("<- [Error] HTTP POST failed: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
}

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);

    Serial.println("\n\n=== SwitchBot エアコン コントローラー ===");

    // NVS (ROM) の初期化とデバイスIDの読み込み
    preferences.begin("switchbot", false);
    airconId = preferences.getString("aircon_id", "");

    // 1. Wi-Fi接続
    Serial.printf("Connecting to %s ", ssid);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[OK] Wi-Fi Connected!");

    // 2. NTPによる時刻同期 (APIリクエストのタイムスタンプに必須)
    Serial.print("Synchronizing time via NTP ");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    struct tm timeinfo;
    while (!getLocalTime(&timeinfo)) {
        Serial.print(".");
        delay(1000);
    }
    Serial.println("\n[OK] Time synchronized!");

    // 3. ROMにデバイスIDがない場合のセットアップフロー
    if (airconId == "") {
        Serial.println("\n[Info] ROMにエアコンのデバイスIDが見つかりません。");
        Serial.println("SwitchBotからデバイス一覧を取得しています...");

        HTTPClient http;
        http.begin("https://api.switch-bot.com/v1.1/devices");

        // エポックミリ秒の取得
        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t time_ms = (int64_t)tv.tv_sec * 1000L + (int64_t)tv.tv_usec / 1000L;
        String t = String(time_ms);
        
        // Nonceの生成
        String nonce = String(esp_random());
        
        // 署名の生成
        String sign = getSignature(t, nonce);

        // ヘッダーの設定
        http.addHeader("Authorization", token);
        http.addHeader("sign", sign);
        http.addHeader("t", t);
        http.addHeader("nonce", nonce);
        http.addHeader("Content-Type", "application/json");

        // GETリクエストの送信
        int httpCode = http.GET();

        if (httpCode > 0) {
            String payload = http.getString();
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload);

            if (!error) {
                JsonArray deviceList = doc["body"]["deviceList"];
                JsonArray infraredRemoteList = doc["body"]["infraredRemoteList"];
                
                int index = 1;
                String ids[50]; // ID保存用配列 (最大50個と仮定)
                
                Serial.println("\n=== 登録されているデバイス一覧 ===");
                
                // 物理デバイス
                if (!deviceList.isNull()) {
                    for (JsonObject dev : deviceList) {
                        const char* dId = dev["deviceId"];
                        const char* dName = dev["deviceName"];
                        const char* dType = dev["deviceType"];
                        Serial.printf("[%d] %s (Type: %s) - ID: %s\n", index, dName, dType, dId);
                        if (index < 50) ids[index] = String(dId);
                        index++;
                    }
                }
                
                // 赤外線リモコン (エアコン等)
                if (!infraredRemoteList.isNull()) {
                    for (JsonObject dev : infraredRemoteList) {
                        const char* dId = dev["deviceId"];
                        const char* dName = dev["deviceName"];
                        const char* dType = dev["remoteType"]; // 赤外線リモコンの場合は remoteType
                        Serial.printf("[%d] %s (Type: %s) - ID: %s\n", index, dName, dType, dId);
                        if (index < 50) ids[index] = String(dId);
                        index++;
                    }
                }

                if (index == 1) {
                    Serial.println("デバイスが見つかりませんでした。");
                } else {
                    Serial.println("\n==================================");
                    Serial.println("制御したいエアコンの番号を入力し、送信(Enter)してください。");
                    
                    // シリアル入力待ち
                    while (!Serial.available()) {
                        delay(100);
                    }
                    
                    int selected = Serial.parseInt();
                    
                    // バッファに残っている改行コードなどをクリア
                    while (Serial.available()) Serial.read();

                    if (selected >= 1 && selected < index) {
                        String selectedId = ids[selected];
                        Serial.println("\n[OK] 選択されたID: " + selectedId);
                        
                        // ROMに保存
                        preferences.putString("aircon_id", selectedId);
                        Serial.println("[Info] デバイスIDをROMに保存しました。");
                        Serial.println("ESP32を再起動します...");
                        delay(2000);
                        ESP.restart();
                    } else {
                        Serial.println("\n[Error] 無効な番号です。再起動してやり直します...");
                        delay(3000);
                        ESP.restart();
                    }
                }
            } else {
                Serial.print("[Error] JSONパース失敗: ");
                Serial.println(error.c_str());
            }
        } else {
            Serial.printf("[Error] HTTP GET failed: %s\n", http.errorToString(httpCode).c_str());
        }
        http.end();
        
        // デバイス一覧取得後はここで無限ループ（再起動待ちまたはエラー停止）
        while(true) delay(1000);
        
    } else {
        Serial.println("\n[Info] ROMからエアコンのデバイスIDを読み込みました: " + airconId);
        Serial.println("=== 通常の処理を開始します ===");
        
        // センサーの初期化
        Wire.begin();
        if (!shtSensor.begin(0x44)) {
            Serial.printf("[Warning] %sセンサーが見つかりません。センサー処理をスキップします。\n", SENSOR_NAME);
            hasSensor = false;
        } else {
            Serial.printf("[OK] %sセンサーを検出しました。\n", SENSOR_NAME);
            hasSensor = true;
        }
    }
}

unsigned long lastSensorTime = 0;

void loop() {
  if (airconId != "") {
      // 1) 10秒ごとにセンサー読み取り (非同期処理)
      if (millis() - lastSensorTime >= 10000) {
          lastSensorTime = millis();
          if (hasSensor) {
              float t = shtSensor.readTemperature();
              float h = shtSensor.readHumidity();

              if (!isnan(t)) {
                  Serial.print("Temp *C = "); Serial.print(t); Serial.print("\t\t");
              } else {
                  Serial.println("Failed to read temperature");
              }
              
              if (!isnan(h)) {
                  Serial.print("Hum. % = "); Serial.println(h);
              } else {
                  Serial.println("Failed to read humidity");
              }
          }
      }
      
      // 2) シリアルモニタからのテストコマンド入力
      // '1' で ON (例: 26度, 冷房, 風量自動)
      // '0' で OFF
      if (Serial.available() > 0) {
          char c = Serial.read();
          if (c == '1') {
              Serial.println("\n[Test] エアコンを ON にします (26度, 冷房, 風量自動)");
              // temp=26, mode=2(冷房), fan=1(自動), powerOn=true
              controlAircon(26, 2, 1, true);
          } else if (c == '0') {
              Serial.println("\n[Test] エアコンを OFF にします");
              // temp, mode, fan の値はOFF時も形式上送信する必要があります
              controlAircon(26, 2, 1, false);
          }
      }
  }
}
