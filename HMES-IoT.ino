#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h> 
#include <OneWire.h>
#include <DallasTemperature.h>

#define RELAY_PIN 13
#define TDS_PIN 32
#define pH_PIN 34
#define VREF 3.3
#define RESOLUTION 4095.0
#define ONE_WIRE_BUS 4  // GPIO4
#define SCOUNT  30               // số mẫu lấy trung bình
#define POWER_PIN 33
#define SIGNAL_PIN 36
#define THRESHOLD 1580

int analogBuffer[SCOUNT];       
int analogBufferTemp[SCOUNT];
int analogBufferIndex = 0, copyIndex = 0;
bool isRealtime = false;

// Khởi tạo đối tượng OneWire và DallasTemperature
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

float averageVoltage = 0.0;
float temperature = 25.0;
float adcValue = 0.0;
float voltage = 0.0;
float tdsValue = 0.0;
float pH = 0.0;
float waterLevel = 0;

const char* ap_ssid = "HMES-Kit";
const char* ap_password = "12345678";

String user_ssid;
String user_password;
const char* mqtt_server = "14.225.210.123"; // Hoặc IP Mosquitto
const int mqtt_port = 1883;
const char* mqtt_topic = "esp32/status";
const char* mqtt_subscribe_topic = "esp32/refresh/";
const char* mqtt_set_refresh_realtime = "esp32/refresh/realtime";
const char* mqtt_get_refresh_realtime = "esp32/refresh/realtime/status";
const char* mqtt_subscribe_update_refresh_cycle = "esp32/";

unsigned long lastSendTime = 0;
unsigned long interval = 0;  

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

void calculatepH(){
  int adcValue = analogRead(pH_PIN);
  float voltage = adcValue * (VREF / RESOLUTION);  // ESP32 có ADC 12-bit
  pH = 7 + ((2.5 - voltage) / 0.18);    // Công thức tạm, cần hiệu chuẩn

  Serial.print("Điện áp: ");
  Serial.print(voltage, 2);
  Serial.print(" V | pH = ");
  Serial.println(pH, 2);
}

int readAverage(int pin, int samples) {
  long total = 0;
  for (int i = 0; i < samples; i++) {
    total += analogRead(pin);
    delay(10); // Tránh đọc quá nhanh
  }
  return total / samples;
}

void getWaterLever(){
  digitalWrite(POWER_PIN, HIGH);
  delay(10);
  int value = analogRead(SIGNAL_PIN);
  waterLevel = value;
  digitalWrite(POWER_PIN, LOW);
  Serial.print("Water level value: ");
  Serial.println(value);
  if(value < THRESHOLD)
  {
    Serial.print("The water is detected");
    digitalWrite(RELAY_PIN, LOW);
  } else {
    digitalWrite(RELAY_PIN, HIGH);
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
        http.begin("https://api.hmes.site/api/user/me/devices/active/");
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Authorization", "Bearer " + user_token);
        http.addHeader("Cookie", "RefreshToken=" + user_refreshToken + "; DeviceId=" + user_deviceId);

        preferences.begin("device_info", false);
        String deviceId = preferences.getString("deviceId", "Unknown");

        String payload = "\"" + deviceId + "\"";
        int httpResponseCode = http.POST(payload);

        String apiResponse = "";

        if (httpResponseCode > 0) {

            newAccessToken = http.header("new-access-token");
            apiResponse = http.getString();
            DynamicJsonDocument doc(1024);
            deserializeJson(doc, apiResponse);

            String token = doc["response"]["token"];
            preferences.putString("token", token);
            // if (newAccessToken.length() > 0) {
            Serial.println("✔ API Response: " + http.getString());
            getInitData();
            Serial.println("✅ Kết nối Wi-Fi thành công!");
            
            // 🔹 Lưu WiFi vào bộ nhớ ESP
            preferences.begin("wifi", false);
            preferences.putString("ssid", user_ssid);
            preferences.putString("password", user_password);
            preferences.end();
            client.setServer(mqtt_server, mqtt_port);
            client.setCallback(mqttCallback);
            while (!client.connected()) {
                Serial.println("🔄 Đang kết nối MQTT...");
                String clientId = "ESP32_Client" + deviceId;
                if (client.connect(clientId.c_str())) {
                    Serial.println("✅ Đã kết nối MQTT!");
                    client.subscribe((mqtt_subscribe_topic + deviceId).c_str());
                    client.subscribe((mqtt_subscribe_update_refresh_cycle + deviceId + "/refreshCycleHours").c_str());
                } else {
                    Serial.print("Lỗi: ");
                    Serial.println(client.state());
                    delay(5000);
                }
            }
        } else {
            Serial.println("❌ Lỗi gọi API: " + String(httpResponseCode));
        }

        http.end();
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

void updateLog(float temperature, float soluteConcentration, float ph, float waterLevel) {
  HTTPClient http;
  preferences.begin("device_info", true);
  String deviceId = preferences.getString("deviceId", "Unknown");
  String token = preferences.getString("token", "Unknown");
  String url = "https://api.hmes.site/api/iot/" + deviceId;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Token", token);
  http.addHeader("X-DeviceItemId", deviceId);
  String payload = "{";
    payload += "\"temperature\":" + String(temperature, 2) + ",";
    payload += "\"soluteConcentration\":" + String(soluteConcentration, 2) + ",";
    payload += "\"ph\":" + String(ph, 2) + ",";
    payload += "\"waterLevel\":" + String(waterLevel, 2);
    payload += "}";
  int httpResponseCode = http.POST(payload);
  String apiResponse = "";
  if (httpResponseCode > 0) {
      apiResponse = http.getString();
      DynamicJsonDocument doc(1024);
      deserializeJson(doc, apiResponse);

      String message = doc["response"]["message"];

      Serial.println("✔ API Response GetInit: " + message);
  } else {
      Serial.println("❌ Lỗi gọi API: " + String(httpResponseCode));
  }
  preferences.end();
}

void getInitData(){
  HTTPClient http;
  preferences.begin("device_info", true);
  String deviceId = preferences.getString("deviceId", "Unknown");
  String token = preferences.getString("token", "Unknown");
  String url = "https://api.hmes.site/api/user/me/iot/devices/" + deviceId;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Token", token);
  http.addHeader("X-DeviceItemId", deviceId);
  int httpResponseCode = http.GET();
  String apiResponse = "";
  if (httpResponseCode > 0) {
      apiResponse = http.getString();
      DynamicJsonDocument doc(1024);
      deserializeJson(doc, apiResponse);

      String lastUpdated = doc["response"]["data"]["lastUpdatedDate"];
      String tempLastUpdate = lastUpdated.substring(0, lastUpdated.indexOf("+"));
      int refreshCycleHours = doc["response"]["data"]["refreshCycleHours"];
      preferences.putString("token", token);

      // Parse datetime
      int year   = lastUpdated.substring(0, 4).toInt();
      int month  = lastUpdated.substring(5, 7).toInt();
      int day    = lastUpdated.substring(8, 10).toInt();
      int hour   = lastUpdated.substring(11, 13).toInt();
      int minute = lastUpdated.substring(14, 16).toInt();
      int second = lastUpdated.substring(17, 19).toInt();

      struct tm timeinfo;
      timeinfo.tm_year = year - 1900;
      timeinfo.tm_mon  = month - 1;
      timeinfo.tm_mday = day;
      timeinfo.tm_hour = hour;
      timeinfo.tm_min  = minute;
      timeinfo.tm_sec  = second;

      time_t epochTime = mktime(&timeinfo);  // seconds
      lastSendTime = (unsigned long)epochTime * 1000UL; // convert to milliseconds

      interval = (unsigned long)refreshCycleHours * 60UL * 60UL * 1000UL;
      // if (newAccessToken.length() > 0) {
      Serial.println("✔ API Response GetInit: " + tempLastUpdate);
  } else {
      Serial.println("❌ Lỗi gọi API: " + String(httpResponseCode));
  }
  preferences.end();
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
    preferences.end();

    Serial.println("🔍 Đang kiểm tra SSID/PASSWORD đã lưu...");
    Serial.println("SSID: " + saved_ssid);
    Serial.println("PASSWORD: " + saved_password);

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

    // Không có SSID/password đã lưu
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
  Serial.println(String(topic));
  preferences.begin("device_info", true);
  String deviceId = preferences.getString("deviceId", "Unknown");
  String expectedTopic = mqtt_subscribe_update_refresh_cycle + deviceId + "/refreshCycleHours";
  String refreshTopic = mqtt_subscribe_topic + deviceId;
  String setRefreshRealTime = mqtt_set_refresh_realtime + deviceId;

  // You can add your logic to handle the received message here
  if (String(topic) == refreshTopic) {
    // if (message == "turn_on") {
    //   // Serial.println("Turning on the device...");
    //   // Add code to turn on the device or perform some action
    // } else if (message == "turn_off") {
    //   Serial.println("Turning off the device...");
    //   // Add code to turn off the device or perform some action
    // }
    sendTDSDataToAPI();
    calculatepH();
    getWaterLever();
    StaticJsonDocument<200> doc;

    doc["temperature"] = temperature;
    doc["soluteConcentration"] = tdsValue;
    doc["ph"] = pH;
    doc["waterLevel"] = waterLevel;

    String payload;
    serializeJson(doc, payload);

    String topic = "esp32/refresh/response/" + deviceId;
    client.publish(topic.c_str(), payload.c_str());
    updateLog(temperature, tdsValue, pH, waterLevel);
    Serial.println("✅ Đã refresh dữ liệu");
  } else if (String(topic) == expectedTopic) {
    Serial.println("🕒 Nhận yêu cầu cập nhật refresh cycle: " + message);
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, message);
    int refreshCycleHours = doc["refreshCycleHours"];
    interval = (unsigned long)refreshCycleHours * 60UL * 60UL * 1000UL;
    Serial.println("✅ Đã cập nhật refresh cycle: " + refreshCycleHours);
    // Xử lý logic cập nhật thời gian refresh tại đây
  } else if (String(topic) == setRefreshRealTime){
    Serial.println("🕒 Nhận yêu cầu cập nhật refresh realtime: " + message);
    if (message == "turn_on") {
      Serial.println("Turning on the device...");
      // Add code to turn on the device or perform some action
    } else if (message == "turn_off") {
      Serial.println("Turning off the device...");
      // Add code to turn off the device or perform some action
    }
  }
  preferences.end();
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

bool needReconnect = false;

void setup() {
    Serial.begin(115200);
    analogReadResolution(12);
    pinMode(TDS_PIN, INPUT);
    analogSetAttenuation(ADC_11db);
    pinMode(RELAY_PIN, OUTPUT);
    pinMode(POWER_PIN, OUTPUT);
    digitalWrite(POWER_PIN, LOW);
    digitalWrite(RELAY_PIN, HIGH);
    sensors.begin(); // Cảm biến nhiệt độ

    // preferences.begin("wifi", false);
    // preferences.remove("ssid");
    // preferences.remove("password");
    // preferences.end();

    // Thiết lập deviceId nếu chưa có
    preferences.begin("device_info", false);
    if (!preferences.isKey("deviceId")) {
        preferences.putString("deviceId", "B61A4675-8D10-4597-8702-42702D16F48F");
        Serial.println("✅ Ghi deviceId vào bộ nhớ");
    } else {
        Serial.println("🔄 deviceId đã tồn tại, không cần ghi lại");
    }
    preferences.end();

    // Bắt đầu xử lý WiFi
    preferences.begin("wifi", true);
    bool hasSavedSSID = preferences.isKey("ssid") && preferences.getString("ssid", "") != "";
    preferences.end();

    if (!hasSavedSSID) {
        Serial.println("⚠️ Không có WiFi đã lưu -> Bật AP Mode");
        WiFi.softAP(ap_ssid, ap_password);
        IPAddress apIP(192, 168, 2, 30);
        WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
        Serial.println("🚀 ESP32 đã khởi động ở chế độ Access Point");

        server.on("/scan", HTTP_GET, handleRoot);
        server.on("/connect", HTTP_POST, handleConnect);
        server.begin();
    } else if (connectToSavedWiFi()) {
        Serial.println("✅ Kết nối Wi-Fi đã lưu thành công!");
        getInitData();

        client.setServer(mqtt_server, mqtt_port);
        client.setCallback(mqttCallback);

        preferences.begin("device_info", true);
        String deviceId = preferences.getString("deviceId", "Unknown");
        preferences.end();

        while (!client.connected()) {
            Serial.println("🔄 Đang kết nối MQTT...");
            String clientId = "ESP32_Client" + deviceId;
            if (client.connect(clientId.c_str())) {
                Serial.println("✅ Đã kết nối MQTT!");
                client.subscribe((mqtt_subscribe_topic + deviceId).c_str());
                client.subscribe((mqtt_subscribe_update_refresh_cycle + deviceId + "/refreshCycleHours").c_str());
            } else {
                Serial.print("Lỗi MQTT: ");
                Serial.println(client.state());
                delay(5000);
            }
        }
    } else {
        Serial.println("❌ Có WiFi đã lưu nhưng không kết nối được -> Gắn cờ thử lại sau");
        needReconnect = true;
    }
}


void sendDeviceStatus(const char* status) {
    // 🔁 Nếu mất kết nối MQTT thì reconnect
    if (!client.connected()) {
        Serial.println("⚠️ MQTT ngắt kết nối. Đang thử kết nối lại...");
        reconnectMQTT(); // Bạn cần có hàm reconnectMQTT()
    }

    if (client.connected()) {
        preferences.begin("device_info", true);
        String deviceId = preferences.getString("deviceId", "Unknown");
        preferences.end();

        StaticJsonDocument<200> doc;
        doc["deviceId"] = deviceId;
        doc["status"] = status;

        char buffer[256];
        serializeJson(doc, buffer);

        if (client.publish("esp32/status", buffer)) {
            Serial.println("📤 Gửi JSON thành công: " + String(buffer));
        } else {
            Serial.println("❌ Gửi thất bại, topic: esp32/status");
        }
    } else {
        Serial.println("❌ Không thể gửi vì MQTT vẫn chưa kết nối lại.");
    }
}

void reconnectMQTT() {
    while (!client.connected()) {
        preferences.begin("device_info", true);
        String deviceId = preferences.getString("deviceId", "Unknown");
        preferences.end();

        while (!client.connected()) {
            Serial.println("🔄 Đang kết nối lại MQTT...");
            String clientId = "ESP32_Client" + deviceId;
            if (client.connect(clientId.c_str())) {
                Serial.println("✅ Đã kết nối MQTT!");
                client.subscribe((mqtt_subscribe_topic + deviceId).c_str());
                client.subscribe((mqtt_subscribe_update_refresh_cycle + deviceId + "/refreshCycleHours").c_str());
            } else {
                Serial.print("Lỗi MQTT: ");
                Serial.println(client.state());
                delay(5000);
            }
        }
    }
}


void updateLog(){
  sendTDSDataToAPI();
  calculatepH();
  getWaterLever();

  HTTPClient http;
  preferences.begin("device_info", true);
  String deviceId = preferences.getString("deviceId", "Unknown");
  String token = preferences.getString("token", "Unknown");
  String url = "https://api.hmes.site/api/iot/" + deviceId;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Token", token);
  http.addHeader("X-DeviceItemId", deviceId);
  StaticJsonDocument<200> doc;

  doc["temperature"] = temperature;
  doc["soluteConcentration"] = tdsValue;
  doc["ph"] = pH;
  doc["waterLevel"] = waterLevel;

  String payload;
  serializeJson(doc, payload);
  int httpResponseCode = http.POST(payload);
  if (httpResponseCode > 0) {
      // if (newAccessToken.length() > 0) {
      Serial.println("✔ API Response GetInit: ");
  } else {
      Serial.println("❌ Lỗi gọi API: " + String(httpResponseCode));
  }
  preferences.end();
}

// void loop() {
//     if (WiFi.status() == WL_CONNECTED) {
//         client.loop();
//         sendDeviceStatus("online");
//         // getWaterLever();

//         unsigned long currentMillis = millis();
//         if (currentMillis - lastSendTime >= interval) {
//             lastSendTime = currentMillis;

//             // sendTDSDataToAPI(); // Gửi dữ liệu TDS
//             // calculatepH();
//             // getWaterLever();
//             // // Nếu bạn muốn gửi dữ liệu lên MQTT luôn:
//             // StaticJsonDocument<200> doc;
//             // preferences.begin("device_info", true);
//             // String deviceId = preferences.getString("deviceId", "Unknown");
//             // preferences.end();

//             // doc["deviceId"] = deviceId;
//             // doc["tds"] = tdsValue;

//             // char buffer[256];
//             // serializeJson(doc, buffer);
//             // client.publish("esp32/tds", buffer); // gửi lên topic tds

//             // Serial.println("📤 Gửi dữ liệu TDS định kỳ: " + String(buffer));
//         }

//     } else {
//         server.handleClient(); // Khi đang ở chế độ AP
//         Serial.println("⚠️ Mất kết nối WiFi! Đang thử kết nối lại...");
//         if (connectToSavedWiFi()) {
//           Serial.println("✅ Kết nối lại WiFi thành công!");
//         } else {
//           Serial.println("❌ Không thể kết nối lại WiFi!");
//         }
//     } 

//     delay(5000); // Cho nhẹ CPU, không delay 5 tiếng ở đây nhé
// }

void loop() {
    if (WiFi.status() == WL_CONNECTED) {
        client.loop();
        sendDeviceStatus("online");
        getWaterLever();

        unsigned long currentMillis = millis();
        if (currentMillis - lastSendTime >= interval) {
            lastSendTime = currentMillis;
            sendTDSDataToAPI();
            // calculatepH();
        }

    } else if (needReconnect) {
        Serial.println("🔁 Thử kết nối lại WiFi đã lưu...");
        if (connectToSavedWiFi()) {
            Serial.println("✅ Kết nối lại WiFi thành công!");
            getInitData();

            client.setServer(mqtt_server, mqtt_port);
            client.setCallback(mqttCallback);

            preferences.begin("device_info", true);
            String deviceId = preferences.getString("deviceId", "Unknown");

            while (!client.connected()) {
                Serial.println("🔄 Đang kết nối MQTT...");
                String clientId = "ESP32_Client" + deviceId;
                if (client.connect(clientId.c_str())) {
                    Serial.println("✅ Đã kết nối MQTT!");
                    client.subscribe((mqtt_subscribe_topic + deviceId).c_str());
                    client.subscribe((mqtt_subscribe_update_refresh_cycle + deviceId + "/refreshCycleHours").c_str());
                    needReconnect = false;  // Reset cờ kết nối lại
                } else {
                    Serial.print("Lỗi MQTT: ");
                    Serial.println(client.state());
                    delay(5000);
                }
            }
            preferences.end();
        } else {
            Serial.println("❌ Vẫn chưa kết nối lại được WiFi...");
        }
    } else {
        server.handleClient(); // Khi đang ở chế độ AP

    }

    delay(5000); // Mỗi lần lặp là sau 5s
}
