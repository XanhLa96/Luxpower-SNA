#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <time.h>

// Thông tin WiFi
const char* ssid = "VIETTEL"; // Thay bằng SSID của bạn
const char* password = "11111111"; // Thay bằng mật khẩu WiFi

// Cấu hình inverter
const char* inverterHost = "10.10.10.1";
const int inverterPort = 8000;
const int defaultProtocol = 1; // Giao thức 1 hoặc 2
const int READ_INTERVAL = 5000; // 5 giây

// Cấu hình bộ đệm log
#define LOG_SIZE 50
#define LOG_LINE_SIZE 64
char logBuffer[LOG_SIZE][LOG_LINE_SIZE];
int logIndex = 0;
int logCount = 0;

// Web server trên cổng 80
ESP8266WebServer server(80);
WiFiClient client;

// Số serial mặc định cho datalog và inverter
const uint8_t DEFAULT_DATALOG_SN[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const uint8_t EMPTY_INVERTER_SN[10] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

const uint8_t DATALOG_SN[10] = {0x0B, 0x0A, 0x03, 0x02, 0x05, 0x00, 0x05, 0x06, 0x09, 0x02};
const uint8_t INVERTER_SN[10] = {0x04, 0x00, 0x04, 0x03, 0x06, 0x08, 0x00, 0x04, 0x09, 0x01};

// Bộ đệm phân tích dữ liệu
uint8_t parserBuffer[256];
int parserLength = 0;

// Giá trị đo mới nhất
int lastPvFlow = 0;
int lastConsumption = 0;
unsigned long lastReadTime = 0;
unsigned long lastHeapLogTime = 0;

// Template HTML trong PROGMEM để tiết kiệm RAM
const char html_template[] PROGMEM = "<!DOCTYPE html><html><head><title>Giám sát Inverter</title>"
                                     "<meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
                                     "<style>body{font-family:monospace;font-size:14px;padding:10px;}"
                                     "pre{border:1px solid #000;padding:10px;overflow-x:auto;white-space:pre-wrap;}"
                                     "button{padding:10px;background:#f00;color:#fff;border:none;cursor:pointer;}</style></head>"
                                     "<body><h1>Giám sát Inverter</h1>"
                                     "<p>Công suất PV: %d W</p>"
                                     "<p>Tiêu thụ: %d W</p>"
                                     "<p><form action='/reset' method='POST'><button type='submit'>Khởi động lại ESP8266</button></form></p>"
                                     "<h3>Log</h3><pre>%s</pre></body></html>";

void setup() {
  Serial.begin(115200);
  delay(10);

  // Kiểm tra giao thức
  if (defaultProtocol != 1 && defaultProtocol != 2) {
    addLog("Giao thức không hợp lệ. Sử dụng 1 hoặc 2.");
    while (true) yield();
  }
  addLog("Sử dụng giao thức TCP: %d", defaultProtocol);

  // Kết nối WiFi
  WiFi.begin(ssid, password);
  addLog("Đang kết nối WiFi: %s", ssid);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  addLog("WiFi đã kết nối, IP: %s", WiFi.localIP().toString().c_str());

  // Thiết lập tuyến web server
  server.on("/", handleRoot);
  server.on("/reset", handleReset);
  server.begin();
  addLog("Web server khởi động tại http://%s", WiFi.localIP().toString().c_str());

  // Kết nối đến inverter
  connectToInverter();
}

void loop() {
  server.handleClient();
  
  // Gửi yêu cầu đọc định kỳ
  if (millis() - lastReadTime >= READ_INTERVAL) {
    if (client.connected()) {
      sendReadInput();
      lastReadTime = millis();
    } else {
      addLog("Inverter mất kết nối, đang thử lại...");
      connectToInverter();
      lastReadTime = millis();
    }
  }

  // Ghi log heap mỗi 30 giây
  if (millis() - lastHeapLogTime >= 30000) {
    addLog("Heap trống: %d byte", ESP.getFreeHeap());
    lastHeapLogTime = millis();
  }

  // Đọc dữ liệu đến
  if (client.connected() && client.available()) {
    while (client.available()) {
      if (parserLength < sizeof(parserBuffer)) {
        parserBuffer[parserLength++] = client.read();
      } else {
        addLog("Tràn bộ đệm phân tích, đặt lại");
        parserLength = 0;
      }
    }
    parseFrames();
  }
}

// Thêm log với dấu thời gian
void addLog(const char* format, ...) {
  char buf[LOG_LINE_SIZE];
  time_t now;
  time(&now);
  struct tm *ti = localtime(&now);
  snprintf(buf, LOG_LINE_SIZE, "[%02d:%02d:%02d] ", ti->tm_hour, ti->tm_min, ti->tm_sec);
  
  va_list args;
  va_start(args, format);
  vsnprintf(buf + 11, LOG_LINE_SIZE - 11, format, args);
  va_end(args);

  strncpy(logBuffer[logIndex], buf, LOG_LINE_SIZE - 1);
  logBuffer[logIndex][LOG_LINE_SIZE - 1] = '\0';
  logIndex = (logIndex + 1) % LOG_SIZE;
  if (logCount < LOG_SIZE) logCount++;
  Serial.println(buf);
}

// Kết nối đến inverter
void connectToInverter() {
  if (client.connected()) client.stop();
  if (client.connect(inverterHost, inverterPort)) {
    addLog("Kết nối đến inverter tại %s:%d", inverterHost, inverterPort);
  } else {
    addLog("Kết nối inverter thất bại");
  }
}

// Tính CRC16 Modbus
uint16_t crc16Modbus(const uint8_t* buffer, int length) {
  uint16_t crc = 0xFFFF;
  for (int pos = 0; pos < length; pos++) {
    crc ^= buffer[pos];
    for (int i = 0; i < 8; i++) {
      if (crc & 0x0001) {
        crc = (crc >> 1) ^ 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

// Xây dựng bộ đệm lệnh đọc đầu vào
void buildReadInputCommandBuffer(uint8_t* cmd, const uint8_t* serialBuffer, uint16_t startAddress, uint16_t pointNumber) {
  cmd[0] = 0; // địa chỉ
  cmd[1] = 4; // mã chức năng R_INPUT (4)
  memcpy(&cmd[2], serialBuffer, 10); // datalogSn
  cmd[12] = startAddress & 0xFF;
  cmd[13] = (startAddress >> 8) & 0xFF;
  cmd[14] = pointNumber & 0xFF;
  cmd[15] = (pointNumber >> 8) & 0xFF;
  uint16_t crc = crc16Modbus(cmd, 16);
  cmd[16] = crc & 0xFF; // CRC thấp
  cmd[17] = (crc >> 8) & 0xFF; // CRC cao
}

// Xây dựng bộ đệm truyền dữ liệu
void buildTransferDataBuffer(uint8_t* buf, const uint8_t* datalogSnBuf, const uint8_t* commandBuf, int commandLen) {
  memcpy(buf, datalogSnBuf, 10);
  buf[10] = commandLen & 0xFF;
  buf[11] = (commandLen >> 8) & 0xFF;
  memcpy(&buf[12], commandBuf, commandLen);
}

// Xây dựng khung TCP
void buildTcpFrame(uint8_t* frame, uint16_t protocol, uint8_t functionCode, const uint8_t* dataBuf, int dataLen) {
  int payloadLen = dataLen + 2;
  frame[0] = 0xA1; // prefix0
  frame[1] = 0x1A; // prefix1
  frame[2] = protocol & 0xFF;
  frame[3] = (protocol >> 8) & 0xFF;
  frame[4] = payloadLen & 0xFF;
  frame[5] = (payloadLen >> 8) & 0xFF;
  frame[6] = 1;
  frame[7] = functionCode;
  memcpy(&frame[8], dataBuf, dataLen);
}

// Gửi yêu cầu đọc thanh ghi đầu vào
void sendReadInput() {
  uint8_t cmdBuf[18];
  buildReadInputCommandBuffer(cmdBuf, EMPTY_INVERTER_SN, 0, 40);
  uint8_t tdBuf[30];
  buildTransferDataBuffer(tdBuf, DEFAULT_DATALOG_SN, cmdBuf, 18);
  uint8_t tcpBuf[38];
  buildTcpFrame(tcpBuf, defaultProtocol, 194, tdBuf, 30);
  client.write(tcpBuf, 38);
  addLog("Đã gửi yêu cầu đọc thanh ghi đầu vào");
}

// Phân tích các khung nhận được
void parseFrames() {
  while (true) {
    if (parserLength < 2) break;
    if (parserBuffer[0] != 0xA1 || parserBuffer[1] != 0x1A) {
      int idx = -1;
      for (int i = 1; i < parserLength - 1; i++) {
        if (parserBuffer[i] == 0xA1 && parserBuffer[i + 1] == 0x1A) {
          idx = i;
          break;
        }
      }
      if (idx < 0) {
        parserLength = 0;
        break;
      }
      memmove(parserBuffer, &parserBuffer[idx], parserLength - idx);
      parserLength -= idx;
      continue;
    }
    if (parserLength < 6) break;
    int len = parserBuffer[5] * 256 + parserBuffer[4];
    int fullLen = len + 6;
    if (parserLength < fullLen) break;
    handleFrame(parserBuffer, fullLen);
    memmove(parserBuffer, &parserBuffer[fullLen], parserLength - fullLen);
    parserLength -= fullLen;
  }
}

// Xử lý khung TCP hoàn chỉnh
void handleFrame(uint8_t* frame, int len) {
  char hex[256] = "";
  for (int i = 0; i < len; i++) {
    char byte[4];
    snprintf(byte, sizeof(byte), "%02X ", frame[i]);
    strncat(hex, byte, sizeof(hex) - strlen(hex) - 1);
  }
  addLog("Khung thô: %s", hex);
  
  if (frame[7] == 194) { // TRANSLATE - phản hồi đọc đầu vào
    int r7 = getRegister2(frame, 7, len);
    int r8 = getRegister2(frame, 8, len);
    int r9 = getRegister2(frame, 9, len);
    int outInv = getRegister2(frame, 16, len);
    int inInv = getRegister2(frame, 17, len);
    int outGrid = getRegister2(frame, 26, len);
    int inGrid = getRegister2(frame, 27, len);

    lastPvFlow = r7 + r8 + r9;
    lastConsumption = (outInv - inInv) + (inGrid - outGrid);
    if (lastConsumption < 0) lastConsumption = 0;

    addLog("Công suất PV: %d W", lastPvFlow);
    addLog("Tiêu thụ: %d W", lastConsumption);
  }
}

// Lấy giá trị thanh ghi (2 byte)
uint16_t getRegister2(uint8_t* buf, int index, int len) {
  int p = (index * 2) + 35;
  if (p + 1 >= len) return 0;
  return ((uint16_t)buf[p + 1] << 8) + buf[p];
}

// Xử lý yêu cầu web
void handleRoot() {
  String logContent = "";
  int start = logCount >= LOG_SIZE ? logIndex : 0;
  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % LOG_SIZE;
    logContent += String(logBuffer[idx]) + "\n";
  }
  
  char html[1024];
  snprintf(html, sizeof(html), html_template, lastPvFlow, lastConsumption, logContent.c_str());
  server.send(200, "text/html", html);
}

void handleReset() {
  server.send(200, "text/html", "<html><body>Đang khởi động lại...</body></html>");
  addLog("Khởi động lại qua web");
  delay(500);
  ESP.restart();
}
