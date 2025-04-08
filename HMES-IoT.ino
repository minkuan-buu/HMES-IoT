#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h> 
#include <OneWire.h>
#include <DallasTemperature.h>

#define TDS_PIN 32
#define VREF 3.3
#define RESOLUTION 4095.0
#define ONE_WIRE_BUS 4  // GPIO4
#define SCOUNT  30               // số mẫu lấy trung bình

int analogBuffer[SCOUNT];       
int analogBufferTemp[SCOUNT];
int analogBufferIndex = 0, copyIndex = 0;

// Khởi tạo đối tượng OneWire và DallasTemperature
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

float averageVoltage = 0;
float temperature = 25;
float adcValue = 0;
float voltage = 0;
float tdsValue = 0;

const char* ap_ssid = "HMES-Kit";
const char* ap_password = "12345678";

String user_ssid;
String user_password;
const char* mqtt_server = "14.225.210.123"; // Hoặc IP Mosquitto
const int mqtt_port = 1883;
const char* mqtt_topic = "esp32/status";
const char* mqtt_subscribe_topic = "esp32/refresh/";
String mqtt_subscribe_update_refresh_cycle;

unsigned long lastTdsSentTime = 0;

WebServer server(80);
WiFiClient espClient;
PubSubClient client(espClient);
Preferences preferences;

void handleRoot() {
  server.send(200, "application/json", scanNetworks());
}

String scanNetworks() {
    DynamicJsonDocument doc(1024); // Kích thước buffer đủ lớn để chứa dữ liệu JSON
    JsonArray networks = doc.createNestedArray("networks");

    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; ++i) {
        JsonObject network = networks.createNestedObject();
        network["ssid"] = WiFi.SSID(i);
        network["rssi"] = WiFi.RSSI(i); // Độ mạnh tín hiệu Wi-Fi (RSSI)
    }

    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}

void calculateTemp() {
  // Yêu cầu DS18B20 đo nhiệt độ
  sensors.requestTemperatures();

  // Đọc nhiệt độ (đơn vị: độ C)
  float temperatureC = sensors.getTempCByIndex(0);

  // Kiểm tra nếu cảm biến trả về giá trị hợp lệ
  if (temperatureC != DEVICE_DISCONNECTED_C) {
    Serial.print("Nhiệt độ: ");
    Serial.print(temperatureC);
    Serial.println(" °C");
    temperature = temperatureC;
  } else {
    Serial.println("Không đọc được dữ liệu từ DS18B20!");
  }
}

void handleConnect() {
    user_ssid = server.arg("ssid");
    user_password = server.arg("password");

    // 🔹 ESP giải mã dữ liệu từ Mobile
    String user_token = server.arg("token");
    String user_deviceId = server.arg("deviceId");
    String user_refreshToken = server.arg("refreshToken");
    String newAccessToken;

    WiFi.begin(user_ssid.c_str(), user_password.c_str());

    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 30) {
        delay(500);
        Serial.print(".");
        retry++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✅ Kết nối thành công!");

        HTTPClient http;
        http.begin("https://api.hmes.site/api/device/active");
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Authorization", "Bearer " + user_token);
        http.addHeader("Cookie", "RefreshToken=" + user_refreshToken + "; DeviceId=" + user_deviceId);

        preferences.begin("device_info", true);
        String deviceId = preferences.getString("deviceId", "Unknown");

        String payload = "\"" + deviceId + "\"";
        int httpResponseCode = http.POST(payload);

        if (httpResponseCode > 0) {
            newAccessToken = http.header("new-access-token");
            // if (newAccessToken.length() > 0) {
            Serial.println("✔ API Response: " + http.getString());
        } else {
            Serial.println("❌ Lỗi gọi API: " + String(httpResponseCode));
        }

        http.end();
        preferences.end();

        // 🔹 Lưu WiFi vào bộ nhớ ESP
        preferences.begin("wifi", false);
        preferences.putString("ssid", user_ssid);
        preferences.putString("password", user_password);
        preferences.end();

        String response = "{\"status\":\"Connected\"}";
        server.sendHeader("new-access-token", newAccessToken); // Thêm vào header
        server.send(200, "application/json", response);
        delay(1000);
        WiFi.softAPdisconnect(true);
    } else {
        Serial.println("\n❌ Kết nối thất bại!");
        server.send(400, "text/html", "<h1>Failed to connect. Please try again.</h1>");
    }
}



void handleClearWiFi() {
    preferences.begin("wifi", false);
    preferences.remove("ssid");
    preferences.remove("password");
    preferences.end();
    server.send(200, "text/html", "<h1>WiFi credentials cleared. Restarting...</h1>");
    delay(2000);
    ESP.restart();
}

bool connectToSavedWiFi() {
    preferences.begin("wifi", true);
    String saved_ssid = preferences.getString("ssid", "");
    String saved_password = preferences.getString("password", "");
    Serial.println(saved_ssid);
    Serial.println(saved_password);
    preferences.end();

    if (saved_ssid.length() > 0 && saved_password.length() > 0) {
        WiFi.begin(saved_ssid.c_str(), saved_password.c_str());
        int retry = 0;
        while (WiFi.status() != WL_CONNECTED && retry < 40) {
            delay(500);
            Serial.print(".");
            retry++;
        }
        return WiFi.status() == WL_CONNECTED;
    }
    return false;
}

// void sendTDSDataToAPI() {
//     calculateTemp();
//     // adcValue = analogRead(TDS_PIN);
//     // voltage = (adcValue / RESOLUTION) * VREF;

//     // float compensationCoefficient = 1.0 + 0.02 * (temperature - 25.0);
//     // float compensationVoltage = voltage / compensationCoefficient;

//     // tdsValue = (133.42 * compensationVoltage * compensationVoltage * compensationVoltage -
//     //         255.86 * compensationVoltage * compensationVoltage +
//     //         857.39 * compensationVoltage) * 0.25;

//     // Serial.print("Nhiệt độ: "); Serial.println(temperature);
//     // Serial.print("ADC: "); Serial.println(adcValue);
//     // Serial.print("Voltage: "); Serial.println(voltage, 3);
//     // Serial.print("Comp Voltage: "); Serial.println(compensationVoltage, 3);
//     // Serial.print("TDS Value: "); Serial.println(tdsValue);
//   static unsigned long analogSampleTimepoint = millis();
//   if (millis() - analogSampleTimepoint > 40U) {
//     analogSampleTimepoint = millis();
//     analogBuffer[analogBufferIndex] = analogRead(TDS_PIN);
//     analogBufferIndex++;
//     if (analogBufferIndex == SCOUNT)
//       analogBufferIndex = 0;
//   }

//   static unsigned long printTimepoint = millis();
//   if (millis() - printTimepoint > 800U) {
//     printTimepoint = millis();

//     for (copyIndex = 0; copyIndex < SCOUNT; copyIndex++)
//       analogBufferTemp[copyIndex] = analogBuffer[copyIndex];

//     int median = getMedianNum(analogBufferTemp, SCOUNT);

//     averageVoltage = (float)median * VREF / RESOLUTION;  // 4095 cho độ phân giải 12-bit
//     float compensationCoefficient = 1.0 + 0.02 * (temperature - 25.0);
//     float compensationVoltage = averageVoltage / compensationCoefficient;

//     tdsValue = (133.42 * compensationVoltage * compensationVoltage * compensationVoltage
//                 - 255.86 * compensationVoltage * compensationVoltage
//                 + 857.39 * compensationVoltage) * 0.5;

//     Serial.print("TDS Value: ");
//     Serial.print(tdsValue, 0);
//     Serial.println(" ppm");
//   }
// }

void sendTDSDataToAPI() {
  calculateTemp(); // Hàm này bạn tự định nghĩa để cập nhật biến 'temperature'

  // Đọc ADC SCOUNT lần, lưu vào buffer
  for (int i = 0; i < SCOUNT; i++) {
    analogBuffer[i] = analogRead(TDS_PIN);
    delay(5); // Delay nhỏ để ổn định ADC nếu cần, có thể bỏ nếu không cần
  }

  // Sao chép mảng sang mảng tạm
  for (int i = 0; i < SCOUNT; i++) {
    analogBufferTemp[i] = analogBuffer[i];
  }

  // Lọc trung vị
  int median = getMedianNum(analogBufferTemp, SCOUNT);

  // Tính toán voltage và TDS
  averageVoltage = (float)median * VREF / RESOLUTION;
  float compensationCoefficient = 1.0 + 0.02 * (temperature - 25.0);
  float compensationVoltage = averageVoltage / compensationCoefficient;

  tdsValue = (133.42 * compensationVoltage * compensationVoltage * compensationVoltage
              - 255.86 * compensationVoltage * compensationVoltage
              + 857.39 * compensationVoltage) * 0.5;

  // Gửi hoặc in dữ liệu
  Serial.print("TDS Value: ");
  Serial.print(tdsValue, 0);
  Serial.println(" ppm");
}

int getMedianNum(int bArray[], int iFilterLen) 
{
      int bTab[iFilterLen];
      for (byte i = 0; i<iFilterLen; i++)
      bTab[i] = bArray[i];
      int i, j, bTemp;
      for (j = 0; j < iFilterLen - 1; j++) 
      {
      for (i = 0; i < iFilterLen - j - 1; i++) 
          {
        if (bTab[i] > bTab[i + 1]) 
            {
        bTemp = bTab[i];
            bTab[i] = bTab[i + 1];
        bTab[i + 1] = bTemp;
         }
      }
      }
      if ((iFilterLen & 1) > 0)
    bTemp = bTab[(iFilterLen - 1) / 2];
      else
    bTemp = (bTab[iFilterLen / 2] + bTab[iFilterLen / 2 - 1]) / 2;
      return bTemp;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  
  // Convert the payload to a string
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.println(message);
  
  // You can add your logic to handle the received message here
  if (String(topic) == mqtt_subscribe_topic) {
    if (message == "turn_on") {
      Serial.println("Turning on the device...");
      // Add code to turn on the device or perform some action
    } else if (message == "turn_off") {
      Serial.println("Turning off the device...");
      // Add code to turn off the device or perform some action
    }
  } else if (String(topic) == mqtt_subscribe_update_refresh_cycle) {
    Serial.println("🕒 Nhận yêu cầu cập nhật refresh cycle: " + message);
    // Xử lý logic cập nhật thời gian refresh tại đây
  }
}

// void setup() {
//     Serial.begin(115200);
//     analogReadResolution(12);
//     // preferences.begin("wifi", false);
//     // preferences.remove("ssid");
//     // preferences.remove("password");
//     // preferences.end();
//     preferences.begin("device_info", false); 

//     // Kiểm tra xem deviceId đã tồn tại chưa
//     if (!preferences.isKey("deviceId")) {
//         preferences.putString("deviceId", "B61A4675-8D10-4597-8702-42702D16F48F");
//         Serial.println("✅ Ghi deviceId vào bộ nhớ");
//     } else {
//         Serial.println("🔄 deviceId đã tồn tại, không cần ghi lại");
//     }

//     preferences.end(); // Đóng Preferences để giải phóng bộ nhớ

//     if (!connectToSavedWiFi()) {
//         WiFi.softAP(ap_ssid, ap_password);
//         IPAddress apIP(192, 168, 2, 30);
//         WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
//         Serial.println("ESP32 started as Access Point.");
//     }
    
//     server.on("/scan", HTTP_GET, handleRoot);
//     server.on("/connect", HTTP_POST, handleConnect);

//     client.setServer(mqtt_server, mqtt_port);
//     client.setCallback(mqttCallback);

//     preferences.begin("device_info", true);
//       String deviceId = preferences.getString("deviceId", "Unknown");
//       while (!client.connected()) {
//           Serial.println("🔄 Đang kết nối MQTT...");
//           String clientId = "ESP32_Client" + deviceId;
//           if (client.connect(clientId.c_str())) {
//               Serial.println("✅ Đã kết nối MQTT!");
//               client.subscribe((mqtt_subscribe_topic + deviceId).c_str());
//           } else {
//               Serial.print("Lỗi: ");
//               Serial.println(client.state());
//               delay(5000);
//           }
//       }
//     preferences.end();

//     // server.on("/clear_wifi", HTTP_GET, handleClearWiFi);
//     server.begin();
// }

void setup() {
    Serial.begin(115200);
    analogReadResolution(12);
    pinMode(TDS_PIN, INPUT);
    // Khởi động cảm biến DS18B20
    sensors.begin();
    // preferences.begin("wifi", false);
    // preferences.remove("ssid");
    // preferences.remove("password");
    // preferences.end();
    preferences.begin("device_info", false);

    // Kiểm tra xem deviceId đã tồn tại chưa
    if (!preferences.isKey("deviceId")) {
        preferences.putString("deviceId", "B61A4675-8D10-4597-8702-42702D16F48F");
        Serial.println("✅ Ghi deviceId vào bộ nhớ");
    } else {
        Serial.println("🔄 deviceId đã tồn tại, không cần ghi lại");
    }

    preferences.end();

    if (connectToSavedWiFi() || WiFi.status() == WL_CONNECTED) {
        Serial.println("✅ Kết nối Wi-Fi đã lưu thành công!");
        client.setServer(mqtt_server, mqtt_port);
        client.setCallback(mqttCallback);

        preferences.begin("device_info", true);
        String deviceId = preferences.getString("deviceId", "Unknown");
        mqtt_subscribe_update_refresh_cycle = "esp32/" + deviceId + "/refreshCycleHours/";
        while (!client.connected()) {
            Serial.println("🔄 Đang kết nối MQTT...");
            String clientId = "ESP32_Client" + deviceId;
            if (client.connect(clientId.c_str())) {
                Serial.println("✅ Đã kết nối MQTT!");
                client.subscribe((mqtt_subscribe_topic + deviceId).c_str());
                client.subscribe(mqtt_subscribe_update_refresh_cycle.c_str());
            } else {
                Serial.print("Lỗi: ");
                Serial.println(client.state());
                delay(5000);
            }
        }
        preferences.end();
    } else {
        Serial.println("❌ Không thể kết nối Wi-Fi đã lưu, khởi động AP.");
        WiFi.softAP(ap_ssid, ap_password);
        IPAddress apIP(192, 168, 2, 30);
        WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
        Serial.println("ESP32 started as Access Point.");

        server.on("/scan", HTTP_GET, handleRoot);
        server.on("/connect", HTTP_POST, handleConnect);
        server.begin();
    }
}

void sendDeviceStatus(const char* status) {
    preferences.begin("device_info", true);
    String deviceId = preferences.getString("deviceId", "Unknown");
    preferences.end();
    StaticJsonDocument<200> doc;
    doc["deviceId"] = deviceId;
    doc["status"] = status;

    char buffer[256];
    serializeJson(doc, buffer);

    client.publish("esp32/status", buffer);
    Serial.println("📤 Gửi JSON: " + String(buffer));
}

void loop() {
    if (WiFi.status() == WL_CONNECTED) {
        client.loop();
        sendDeviceStatus("online");
        sendTDSDataToAPI();
    } else {
        server.handleClient();
    }
    delay(5000);
}