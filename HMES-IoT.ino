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
#define THRESHOLD 1000
#define BUTTON_PIN 0

const unsigned long CHECK_INTERVAL = 5000;  // 5 giây

unsigned long lastLoopTime = 0;
bool buttonPressed = false;

int analogBuffer[SCOUNT];       
int analogBufferTemp[SCOUNT];
int analogBufferIndex = 0, copyIndex = 0;
bool isRealtime = false;
bool isAdmin = false;

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
const char* mqtt_server = "14.225.210.123";
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
    DynamicJsonDocument doc(1024);
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
  sensors.requestTemperatures();

  float temperatureC = sensors.getTempCByIndex(0);

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
  float voltage = adcValue * (VREF / RESOLUTION);
  pH = 7 + ((2.5 - voltage) / 0.18);

  Serial.print("Điện áp: ");
  Serial.print(voltage, 2);
  Serial.print(" V | pH = ");
  Serial.println(pH, 2);
}

int readAverage(int pin, int samples) {
  long total = 0;
  for (int i = 0; i < samples; i++) {
    total += analogRead(pin);
    delay(10);
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

    //ESP giải mã dữ liệu từ Mobile
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
        bool isConnected = preferences.getBool("reconnect", false);

        String payload = "{";
        payload += "\"deviceItemId\":\"" + deviceId + "\",";
        payload += "\"isReconnect\":" + String(isConnected ? "true" : "false");
        payload += "}";

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
            Serial.println("✔ API Response: " + String(httpResponseCode));
            getInitData();
            Serial.println("✅ Kết nối Wi-Fi thành công!");
            
            // 🔹 Lưu WiFi vào bộ nhớ ESP
            preferences.begin("wifi", false);
            preferences.putString("ssid", user_ssid);
            preferences.putString("password", user_password);
            preferences.end();
            preferences.putBool("reconnect", false);
            
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
      int dotIndex = tempLastUpdate.indexOf(".");
      if (dotIndex > 0) {
        tempLastUpdate = tempLastUpdate.substring(0, dotIndex);  // "2025-05-08T18:22:40"
      }
      
      preferences.putString("token", token);

      // Parse datetime
      int year   = tempLastUpdate.substring(0, 4).toInt();
      int month  = tempLastUpdate.substring(5, 7).toInt();
      int day    = tempLastUpdate.substring(8, 10).toInt();
      int hour   = tempLastUpdate.substring(11, 13).toInt();
      int minute = tempLastUpdate.substring(14, 16).toInt();
      int second = tempLastUpdate.substring(17, 19).toInt();

      struct tm timeinfo;
      timeinfo.tm_year = year - 1900;
      timeinfo.tm_mon  = month - 1;
      timeinfo.tm_mday = day;
      timeinfo.tm_hour = hour;
      timeinfo.tm_min  = minute;
      timeinfo.tm_sec  = second;

      time_t epochTime = mktime(&timeinfo);
      lastSendTime = (unsigned long)epochTime * 1000UL;

      interval = (unsigned long)refreshCycleHours * 60UL * 60UL * 1000UL;
      Serial.println("✔ API Response GetInit: " + tempLastUpdate);
  } else {
      Serial.println("❌ Lỗi gọi API: " + String(httpResponseCode));
  }
  preferences.end();
}


void handleClearWiFi() {
    HTTPClient http;
    preferences.begin("device_info", true);
    String deviceId = preferences.getString("deviceId", "Unknown");
    String token = preferences.getString("token", "Unknown");
    preferences.end();

    if (deviceId == "Unknown" || token == "Unknown") {
        Serial.println("❌ Không có thông tin deviceId hoặc token.");
        return;
    }

    String url = "https://api.hmes.site/api/user/me/iot/devices/deactive/";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Token", token);
    http.addHeader("X-DeviceItemId", deviceId);

    String payload = "\"" + deviceId + "\"";

    int httpResponseCode = http.POST(payload);

    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.println("✅ Phản hồi từ server: " + response);

        Serial.println("✅ Đã xoá WiFi và token, khởi động lại thiết bị...");
        delay(2000);
    } else {
        Serial.println("❌ Lỗi gọi API: " + String(httpResponseCode));
        String errorResponse = http.getString();
        Serial.println("📨 Nội dung lỗi: " + errorResponse);
        preferences.begin("device_info", false);
        preferences.putBool("reconnect", true);
        preferences.end();
    }
    
    preferences.begin("wifi", false);
    preferences.remove("ssid");
    preferences.remove("password");
    preferences.end();

    preferences.begin("device_info", false);
    preferences.remove("token");
    preferences.end();

    http.end();
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
        while (WiFi.status() != WL_CONNECTED && retry < 30) {
            delay(500);
            Serial.print(".");
            retry++;
            if (digitalRead(BUTTON_PIN) == LOW) {
              Serial.println("Đã nhấn nút BOOT");
              handleClearWiFi();
              delay(300);
            }
        }
        return WiFi.status() == WL_CONNECTED;
    }

    // Không có SSID/password đã lưu
    return false;
}

void sendTDSDataToAPI() {
  calculateTemp(); 

  for (int i = 0; i < SCOUNT; i++) {
    analogBuffer[i] = analogRead(TDS_PIN);
    delay(5);
  }

  // Sao chép mảng sang mảng tạm
  for (int i = 0; i < SCOUNT; i++) {
    analogBufferTemp[i] = analogBuffer[i];
  }

  // Lọc trung vị
  int median = getMedianNum(analogBufferTemp, SCOUNT);

  averageVoltage = (float)median * VREF / RESOLUTION;
  float compensationCoefficient = 1.0 + 0.02 * (temperature - 25.0);
  float compensationVoltage = averageVoltage / compensationCoefficient;

  tdsValue = (133.42 * compensationVoltage * compensationVoltage * compensationVoltage
              - 255.86 * compensationVoltage * compensationVoltage
              + 857.39 * compensationVoltage) * 0.5;

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

  if (String(topic) == refreshTopic) {
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
  } else if (String(topic) == setRefreshRealTime){
    Serial.println("🕒 Nhận yêu cầu cập nhật refresh realtime: " + message);
    if (message == "turn_on") {
      Serial.println("Turning on the device...");
    } else if (message == "turn_off") {
      Serial.println("Turning off the device...");
    }
  }
  preferences.end();
}

bool needReconnect = false;

const char* loginPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>ESP32 Login</title>
</head>
<body>
  <h2>Đăng nhập ESP32</h2>
  <form action="/login" method="POST">
    <label>Tên đăng nhập:</label><input type="text" name="username"><br><br>
    <label>Mật khẩu:</label><input type="password" name="password"><br><br>
    <input type="submit" value="Đăng nhập">
  </form>
</body>
</html>
)rawliteral";


void handleShowLoginPage() {
  server.send(200, "text/html", loginPage);
}

void handleLogin() {
  if (server.hasArg("username") && server.hasArg("password")) {
    String username = server.arg("username");
    String password = server.arg("password");

    if (username == "admin" && password == "HMES_IOT@123") {
      isAdmin = true;
      String deviceIdForm = R"rawliteral(
      <!DOCTYPE html>
      <html>
      <head>
        <meta charset="UTF-8">
        <title>Thiết lập Device ID</title>
      </head>
      <body>
        <h2>Đăng nhập thành công!</h2>
        <form action="/setDeviceId" method="POST">
          <label>Nhập Device ID:</label>
          <input type="text" name="deviceId" required>
          <input type="submit" value="Lưu">
        </form>
      </body>
      </html>
      )rawliteral";

      server.send(200, "text/html", deviceIdForm);
    } else {
      server.send(200, "text/html; charset=utf-8", "<h2>Sai thông tin đăng nhập.</h2>");
    }
  } else {
    server.send(400, "text/plain; charset=utf-8", "Thiếu thông tin.");
  }
}

void handleSetDeviceId() {
  if (!isAdmin) {
    server.send(403, "text/html; charset=utf-8", "<h2>Truy cập bị từ chối.</h2>");
    return;
  }

  if (server.hasArg("deviceId")) {
    String deviceId = server.arg("deviceId");

    preferences.begin("device_info", false);
    preferences.putString("deviceId", deviceId);
    preferences.end();

    server.send(200, "text/html; charset=utf-8", "<h2>Đã lưu Device ID thành công!</h2>");
  } else {
    server.send(400, "text/plain; charset=utf-8", "Thiếu Device ID.");
  }
}

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
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    preferences.begin("device_info", false);
    if (preferences.isKey("deviceId")) {
        String deviceId = preferences.getString("deviceId", "Unknown");
        Serial.print("DeviceId hiện tại: ");
        Serial.println(deviceId);
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
        server.on("/login", HTTP_GET, handleShowLoginPage);
        server.on("/login", HTTP_POST, handleLogin);
        server.on("/setDeviceId", HTTP_POST, handleSetDeviceId);
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
    if (!client.connected()) {
        Serial.println("⚠️ MQTT ngắt kết nối. Đang thử kết nối lại...");
        reconnectMQTT();
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
      Serial.print("✔ API Response Upload log: ");
      Serial.println(httpResponseCode);
  } else {
      Serial.println("❌ Lỗi gọi API: " + String(httpResponseCode));
  }
  preferences.end();
}

void loop() {
  unsigned long currentMillis = millis();

  if (digitalRead(BUTTON_PIN) == LOW) {
    Serial.println("Đã nhấn nút BOOT");
    handleClearWiFi();
    delay(300);
  }

  if (currentMillis - lastLoopTime >= CHECK_INTERVAL) {
    lastLoopTime = currentMillis;

    if (WiFi.status() == WL_CONNECTED) {
      client.loop();
      sendDeviceStatus("online");
      getWaterLever();

      if (currentMillis - lastSendTime >= interval) {
        lastSendTime = currentMillis;
        updateLog();
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
            needReconnect = false;
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
      server.handleClient();  // Chế độ AP
    }
  }
}
