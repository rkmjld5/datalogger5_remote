/*
  ESP8266 + ADS1115 data logger
  Local sensor records: SD card /sensor_data.csv
  Wi-Fi credentials: LittleFS /networks.txt
  Automatic upload: save3_csv.php
  Manual historical upload: sync_sdcard.php

  NodeMCU ESP8266 wiring:
    ADS1115 VDD -> 3.3V, GND -> GND, SDA -> D2, SCL -> D1
    SD module VCC -> 3.3V (unless module specifically supports 5V),
    SD GND -> GND, CS -> D0, SCK -> D5, MISO -> D6, MOSI -> D7.
  Use a proper 3.3V-compatible SD module. Keep D0 (GPIO16) available for SD CS.
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_ADS1X15.h>
#include <time.h>
#include <math.h>
#include <LittleFS.h>
#include <vector>

// ---------------- WI-FI / STORAGE ----------------
const char *NETWORK_FILE = "/networks.txt";  // LittleFS, Wi-Fi credentials only
const char *SD_CSV_FILE = "/sensor_data.csv"; // SD card, 25-column readings
const char *LITTLEFS_CSV_FILE = "/sensor_data.csv"; // LittleFS, 9-column readings
const char *SYNC_SD_URL = "https://datalogger5-remote.onrender.com/sync_sdcard.php";
const char *SYNC_LITTLEFS_URL = "https://datalogger5-remote.onrender.com/sync_littlefs.php";
const char *SERVER_URL = "https://datalogger5-remote.onrender.com/save3_csv.php";
const char *GEO_URL = "http://ip-api.com/json/?fields=status,timezone";
const char *SD_CSV_HEADER = "id,value1,value2,value3,Date,Time,TimeStamp,timezone,device_id,CH1,CH2,CH3,CH4,CH5,CH6,CH7,CH8,CH9,CH10,CH11,CH12,CH13,CH14,CH15,CH16";
const char *LITTLEFS_CSV_HEADER = "id,value1,value2,value3,Date,Time,TimeStamp,timezone,device_id";
const uint8_t SD_CS_PIN = D0; // GPIO16; SPI pins remain D5/D6/D7






const char *deviceID = "ESP0001";
const char *apiToken = "EspSwitch52026";

const unsigned long sendInterval = 5000UL;
const unsigned long timezoneRefreshInterval = 6UL * 60UL * 60UL * 1000UL;
const unsigned long timezoneRetryInterval = 5UL * 60UL * 1000UL;
const unsigned long adsRetryInterval = 5000UL;

unsigned long previousSendTime = 0;
unsigned long previousTimezoneCheck = 0;
unsigned long previousADSCheck = 0;
unsigned long localCsvNextId = 1;
volatile bool stopHistoricalUpload = false;
bool historicalUploadRunning = false;

struct WifiRecord {
  String ssid;
  String password;
  String lastIP;
};

std::vector<WifiRecord> wifiRecords;
bool littleFSReady = false;
bool sdReady = false;
bool adsReady = false;
String installationTimezone = "";
String serialCommandBuffer = "";

Adafruit_ADS1115 ads;
int adcvalue1 = 0, adcvalue2 = 0, adcvalue3 = 0;

// ---------------- GENERAL HELPERS ----------------
String urlEncode(const String &input) {
  String output;
  const char hex[] = "0123456789ABCDEF";
  for (unsigned int i = 0; i < input.length(); i++) {
    unsigned char c = input[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      output += (char)c;
    } else {
      output += '%';
      output += hex[c >> 4];
      output += hex[c & 0x0F];
    }
  }
  return output;
}

String posixTimezoneForIana(const String &iana) {
  if (iana == "Asia/Kolkata" || iana == "Asia/Calcutta") return "IST-5:30";
  if (iana == "Asia/Kathmandu") return "NPT-5:45";
  if (iana == "Asia/Dubai") return "GST-4";
  if (iana == "Asia/Singapore") return "SGT-8";
  if (iana == "Asia/Tokyo") return "JST-9";
  if (iana == "Asia/Shanghai" || iana == "Asia/Hong_Kong") return "CST-8";
  if (iana == "UTC" || iana == "Etc/UTC" || iana == "Etc/GMT") return "UTC0";
  if (iana == "Europe/London") return "GMT0BST,M3.5.0/1,M10.5.0";
  if (iana == "Europe/Paris" || iana == "Europe/Berlin" || iana == "Europe/Rome")
    return "CET-1CEST,M3.5.0,M10.5.0/3";
  if (iana == "America/New_York") return "EST5EDT,M3.2.0,M11.1.0";
  if (iana == "America/Chicago") return "CST6CDT,M3.2.0,M11.1.0";
  if (iana == "America/Denver") return "MST7MDT,M3.2.0,M11.1.0";
  if (iana == "America/Los_Angeles") return "PST8PDT,M3.2.0,M11.1.0";
  if (iana == "Australia/Sydney") return "AEST-10AEDT,M10.1.0,M4.1.0/3";
  return "";
}

// ---------------- LITTLEFS WI-FI CREDENTIALS ----------------
bool loadWifiRecords() {
  wifiRecords.clear();
  if (!littleFSReady || !LittleFS.exists(NETWORK_FILE)) return true;
  File file = LittleFS.open(NETWORK_FILE, "r");
  if (!file) return false;
  while (file.available()) {
    String ssid = file.readStringUntil('\n'); ssid.trim();
    if (!ssid.length()) continue;
    String password = file.readStringUntil('\n'); password.trim();
    if (!password.length()) continue;
    String ip = file.readStringUntil('\n'); ip.trim();
    WifiRecord r; r.ssid = ssid; r.password = password; r.lastIP = ip;
    wifiRecords.push_back(r);
  }
  file.close();
  return true;
}

bool saveWifiRecords() {
  if (!littleFSReady) {
    Serial.println("ERROR: LittleFS unavailable; cannot save Wi-Fi credentials.");
    return false;
  }
  File file = LittleFS.open(NETWORK_FILE, "w");
  if (!file) { Serial.println("ERROR: Cannot write networks.txt."); return false; }
  for (size_t i = 0; i < wifiRecords.size(); i++) {
    file.println(wifiRecords[i].ssid);
    file.println(wifiRecords[i].password);
    file.println(wifiRecords[i].lastIP);
  }
  file.close();
  return true;
}

// ---------------- SD CARD CSV ----------------
unsigned long findLastLocalCsvId() {
  if (!sdReady || !SD.exists(SD_CSV_FILE)) return 0;
  File f = SD.open(SD_CSV_FILE, FILE_READ);
  if (!f) return 0;
  String line;
  unsigned long lastId = 0;
  if (f.available()) line = f.readStringUntil('\n'); // header
  while (f.available()) {
    line = f.readStringUntil('\n'); line.trim();
    if (!line.length()) continue;
    int comma = line.indexOf(',');
    if (comma > 0) {
      unsigned long id = (unsigned long)line.substring(0, comma).toInt();
      if (id > lastId) lastId = id;
    }
    yield();
  }
  f.close();
  return lastId;
}

bool initializeLittleFsCsv() {
  if (!littleFSReady) return false;
  if (!LittleFS.exists(LITTLEFS_CSV_FILE)) {
    File f = LittleFS.open(LITTLEFS_CSV_FILE, "w");
    if (!f) { Serial.println("ERROR: Cannot create LittleFS sensor_data.csv."); return false; }
    f.println(LITTLEFS_CSV_HEADER); f.close();
  } else {
    File f = LittleFS.open(LITTLEFS_CSV_FILE, "r");
    if (!f) return false;
    String header = f.readStringUntil('\n'); header.trim(); f.close();
    if (header != LITTLEFS_CSV_HEADER) {
      Serial.println("ERROR: LittleFS sensor_data.csv has an unexpected header; file not overwritten.");
      return false;
    }
  }
  return true;
}

bool initializeLocalCsv() {
  if (!sdReady) return false;
  if (!SD.exists(SD_CSV_FILE)) {
    File f = SD.open(SD_CSV_FILE, FILE_WRITE);
    if (!f) { Serial.println("ERROR: Cannot create /sensor_data.csv on SD card."); return false; }
    f.println(SD_CSV_HEADER); f.close();
  } else {
    File f = SD.open(SD_CSV_FILE, FILE_READ);
    if (!f) return false;
    String header = f.readStringUntil('\n'); header.trim(); f.close();
    if (header != SD_CSV_HEADER) {
      Serial.println("ERROR: SD sensor_data.csv header is not the required 25-column header.");
      Serial.println("Existing file was NOT overwritten. Back it up before changing its header.");
      return false;
    }
  }
  localCsvNextId = findLastLocalCsvId() + 1;
  Serial.print("SD 25-column CSV ready. Next local ID: "); Serial.println(localCsvNextId);
  return true;
}

bool appendLocalCsvRecord(int v1, int v2, int v3, time_t utcTimestamp,
                          const String &timezone, const String &devId) {
  if (!sdReady || !initializeLocalCsv()) return false;
  if (!littleFSReady || !initializeLittleFsCsv()) {
    Serial.println("ERROR: LittleFS CSV is unavailable; record not saved.");
    return false;
  }

  time_t raw = utcTimestamp;
  struct tm *utcTm = gmtime(&raw);
  if (!utcTm) return false;
  char stamp[24];
  snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d",
           utcTm->tm_year + 1900, utcTm->tm_mon + 1, utcTm->tm_mday,
           utcTm->tm_hour, utcTm->tm_min, utcTm->tm_sec);

  String posixTZ = posixTimezoneForIana(timezone);
  if (!posixTZ.length()) { Serial.println("WARNING: Unknown timezone conversion; using UTC for local Date/Time."); posixTZ = "UTC0"; }
  setenv("TZ", posixTZ.c_str(), 1); tzset();
  struct tm *localTm = localtime(&raw);
  if (!localTm) return false;
  char dateBuf[12], timeBuf[10];
  snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", localTm->tm_year + 1900, localTm->tm_mon + 1, localTm->tm_mday);
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", localTm->tm_hour, localTm->tm_min, localTm->tm_sec);

  String base = String(localCsvNextId) + "," + String(v1) + "," + String(v2) + "," + String(v3) + "," +
                dateBuf + "," + timeBuf + "," + stamp + "," + timezone + "," + devId;

  // SD has 25 columns: standard 9 fields + CH1..CH16.
  // This sketch reads only three ADS1115 inputs. CH1..CH3 mirror those values;
  // CH4..CH16 are intentionally empty, not fabricated readings.
  String sdRow = base + "," + String(v1) + "," + String(v2) + "," + String(v3);
  for (int ch = 4; ch <= 16; ch++) sdRow += ",";
  File sf = SD.open(SD_CSV_FILE, FILE_WRITE);
  if (!sf) { Serial.println("ERROR: Cannot append to SD CSV."); return false; }
  size_t sdWritten = sf.println(sdRow); sf.flush(); sf.close();
  if (sdWritten == 0) return false;

  // LittleFS stores only the standard 9 fields.
  File lf = LittleFS.open(LITTLEFS_CSV_FILE, "a");
  if (!lf) { Serial.println("ERROR: SD saved, but LittleFS CSV append failed."); return false; }
  size_t fsWritten = lf.println(base); lf.flush(); lf.close();
  if (fsWritten == 0) { Serial.println("ERROR: SD saved, but LittleFS CSV append failed."); return false; }

  Serial.print("Saved 25-column record on SD and 9-column record in LittleFS. Local ID: ");
  Serial.println(localCsvNextId);
  localCsvNextId++;
  return true;
}

void showLocalCsvStatus() {
  if (!sdReady || !SD.exists(SD_CSV_FILE)) {
    Serial.println("SD card sensor_data.csv is not available.");
    return;
  }
  File f = SD.open(SD_CSV_FILE, FILE_READ);
  if (!f) { Serial.println("Cannot open SD CSV file."); return; }
  unsigned long lines = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length()) lines++;
    yield();
  }
  f.close();
  Serial.print("Local CSV file: SD:/sensor_data.csv (25 columns)"); Serial.println();
  Serial.print("Header plus records: "); Serial.println(lines);
  Serial.print("Saved data records: "); Serial.println(lines > 0 ? lines - 1 : 0);
  Serial.print("Next local ID: "); Serial.println(localCsvNextId);
}

// ---------------- HISTORICAL UPLOAD: SD / LITTLEFS ----------------
// Both sync endpoints receive ONLY the first nine standard fields.
bool postCsvRecord(const String &line, const char *endpoint) {
  String fields[9]; int fieldCount = 0, start = 0;
  for (int i = 0; i <= (int)line.length(); i++) {
    if (i == (int)line.length() || line[i] == ',') {
      if (fieldCount < 9) fields[fieldCount++] = line.substring(start, i);
      start = i + 1;
    }
  }
  if (fieldCount != 9) { Serial.println("Invalid CSV row: fewer than 9 standard fields."); return false; }
  String form = "api_token=" + urlEncode(String(apiToken)) +
      "&csv_id=" + urlEncode(fields[0]) + "&value1=" + urlEncode(fields[1]) +
      "&value2=" + urlEncode(fields[2]) + "&value3=" + urlEncode(fields[3]) +
      "&Date=" + urlEncode(fields[4]) + "&Time=" + urlEncode(fields[5]) +
      "&TimeStamp=" + urlEncode(fields[6]) + "&timezone=" + urlEncode(fields[7]) +
      "&device_id=" + urlEncode(fields[8]);
  BearSSL::WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, endpoint)) return false;
  http.setTimeout(12000); http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST(form); String response = http.getString(); http.end();
  Serial.print("Sync HTTP code: "); Serial.println(code); Serial.println(response);
  return code == HTTP_CODE_OK;
}

bool checkStopUploadCommand() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      serialCommandBuffer.trim(); String cmd = serialCommandBuffer; serialCommandBuffer = ""; cmd.toUpperCase();
      if (cmd == "STOP UPLOAD") { stopHistoricalUpload = true; Serial.println("STOP UPLOAD received. Finishing current request, then stopping."); }
      else if (cmd.length()) Serial.println("Upload is running. Only STOP UPLOAD is accepted during transfer.");
    } else if (serialCommandBuffer.length() < 100) serialCommandBuffer += c;
  }
  return stopHistoricalUpload;
}

void uploadCsvFile(bool fromSD) {
  if (historicalUploadRunning) { Serial.println("A historical upload is already running."); return; }
  if (WiFi.status() != WL_CONNECTED) { Serial.println("Wi-Fi is not connected."); return; }
  if (fromSD && (!sdReady || !SD.exists(SD_CSV_FILE))) { Serial.println("SD CSV file is missing."); return; }
  if (!fromSD && (!littleFSReady || !LittleFS.exists(LITTLEFS_CSV_FILE))) { Serial.println("LittleFS CSV file is missing."); return; }

  File f = fromSD ? SD.open(SD_CSV_FILE, FILE_READ) : LittleFS.open(LITTLEFS_CSV_FILE, "r");
  if (!f) { Serial.println("Cannot open CSV file."); return; }
  String header = f.readStringUntil('\n'); header.trim();
  String expected = fromSD ? String(SD_CSV_HEADER) : String(LITTLEFS_CSV_HEADER);
  if (header != expected) { Serial.println("CSV header mismatch. Upload cancelled."); f.close(); return; }

  historicalUploadRunning = true; stopHistoricalUpload = false;
  const char *endpoint = fromSD ? SYNC_SD_URL : SYNC_LITTLEFS_URL;
  Serial.println(fromSD ? "Starting SD historical upload. Only 9 standard fields go to TiDB." : "Starting LittleFS historical upload. Only 9 standard fields go to TiDB.");
  Serial.println("Send STOP UPLOAD to stop after the current HTTP request.");
  unsigned long attempted = 0, succeeded = 0, failed = 0;
  while (f.available()) {
    if (checkStopUploadCommand()) break;
    String line = f.readStringUntil('\n'); line.trim(); if (!line.length()) continue;
    attempted++;
    if (postCsvRecord(line, endpoint)) succeeded++; else failed++;
    delay(100); yield();
  }
  f.close(); historicalUploadRunning = false;
  Serial.println("--------- HISTORICAL UPLOAD SUMMARY ---------");
  Serial.print("Records attempted: "); Serial.println(attempted);
  Serial.print("Requests successful: "); Serial.println(succeeded);
  Serial.print("Requests failed: "); Serial.println(failed);
  Serial.println(stopHistoricalUpload ? "Upload stopped by user." : "Upload scan completed.");
  Serial.println("Live sensor recording will continue in the main loop.");
  Serial.println("---------------------------------------------");
}
void uploadSdCsv() { uploadCsvFile(true); }
void uploadLittleFsCsv() { uploadCsvFile(false); }

// ---------------- WI-FI MANAGEMENT COMMANDS ----------------
void showNetworks() {
  loadWifiRecords();
  Serial.println("\n------ SAVED WI-FI NETWORKS ------");
  if (wifiRecords.empty()) Serial.println("No saved Wi-Fi credentials.");
  for (size_t i = 0; i < wifiRecords.size(); i++) {
    Serial.print(i + 1); Serial.print(". SSID: "); Serial.print(wifiRecords[i].ssid);
    Serial.print(" | Saved IP: ");
    Serial.println(wifiRecords[i].lastIP.length() ? wifiRecords[i].lastIP : "(not connected yet)");
  }
  Serial.println("----------------------------------");
}

void readNetworkFile() {
  loadWifiRecords();
  Serial.println("\n------ networks.txt (passwords visible) ------");
  if (wifiRecords.empty()) Serial.println("File is empty or no valid records.");
  for (size_t i = 0; i < wifiRecords.size(); i++) {
    Serial.print("Record "); Serial.println(i + 1);
    Serial.print("SSID: "); Serial.println(wifiRecords[i].ssid);
    Serial.print("Password: "); Serial.println(wifiRecords[i].password);
    Serial.print("Last successful IP: "); Serial.println(wifiRecords[i].lastIP);
    Serial.println("---");
  }
  Serial.println("----------------------------------------------");
}

bool connectToSavedNetworks() {
  if (!loadWifiRecords()) { Serial.println("Could not read networks.txt."); return false; }
  if (wifiRecords.empty()) { Serial.println("No saved Wi-Fi credentials. Use SET WIFI."); return false; }
  WiFi.mode(WIFI_STA);
  for (size_t i = 0; i < wifiRecords.size(); i++) {
    Serial.print("\nTrying Wi-Fi record "); Serial.print(i + 1);
    Serial.print(": "); Serial.println(wifiRecords[i].ssid);
    WiFi.disconnect(); delay(200);
    WiFi.begin(wifiRecords[i].ssid.c_str(), wifiRecords[i].password.c_str());
    unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 12000UL) {
      delay(250); Serial.print(".");
      if (Serial.available()) break;
      yield();
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("Connected to: "); Serial.println(WiFi.SSID());
      Serial.print("ESP8266 IP: "); Serial.println(WiFi.localIP());
      wifiRecords[i].lastIP = WiFi.localIP().toString();
      saveWifiRecords();
      return true;
    }
    Serial.println("This network did not connect; trying the next saved network.");
  }
  Serial.println("Could not connect to any saved Wi-Fi network.");
  return false;
}

String readSerialLine(const char *promptText) {
  Serial.println(promptText);
  String result;
  unsigned long started = millis();
  while (millis() - started < 120000UL) {
    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\r') continue;
      if (c == '\n') { result.trim(); return result; }
      if (result.length() < 120) result += c;
    }
    delay(10); yield();
  }
  Serial.println("Input timed out.");
  return "";
}

void setWifiRecord() {
  String ssid = readSerialLine("Enter Wi-Fi SSID:");
  if (!ssid.length()) { Serial.println("SSID cannot be empty."); return; }
  String password = readSerialLine("Enter Wi-Fi password:");
  if (!password.length()) { Serial.println("Password cannot be empty."); return; }
  loadWifiRecords();
  for (size_t i = 0; i < wifiRecords.size(); i++) {
    if (wifiRecords[i].ssid == ssid && wifiRecords[i].password == password) {
      Serial.println("This SSID/password is already saved."); return;
    }
  }
  WifiRecord r; r.ssid = ssid; r.password = password; r.lastIP = "";
  wifiRecords.push_back(r);
  if (!saveWifiRecords()) return;
  Serial.println("Wi-Fi credential saved in LittleFS networks.txt.");
  Serial.println("Trying the newly added network...");
  WiFi.mode(WIFI_STA); WiFi.disconnect(); WiFi.begin(ssid.c_str(), password.c_str());
  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000UL) {
    delay(250); Serial.print("."); yield();
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP: "); Serial.println(WiFi.localIP());
    for (size_t i = 0; i < wifiRecords.size(); i++)
      if (wifiRecords[i].ssid == ssid && wifiRecords[i].password == password) {
        wifiRecords[i].lastIP = WiFi.localIP().toString(); break;
      }
    saveWifiRecords();
  } else Serial.println("Connection failed. Credential retained for future retries.");
}

int askRecordNumber(const char *promptText) {
  String answer = readSerialLine(promptText);
  int number = answer.toInt();
  if (number < 1 || number > (int)wifiRecords.size()) return -1;
  return number - 1;
}

void editWifiRecord() {
  loadWifiRecords(); showNetworks();
  if (wifiRecords.empty()) return;
  int index = askRecordNumber("Enter record number to edit:");
  if (index < 0) { Serial.println("Invalid record number."); return; }
  String ssid = readSerialLine("Enter new SSID:");
  if (!ssid.length()) { Serial.println("SSID cannot be empty."); return; }
  String password = readSerialLine("Enter new password:");
  if (!password.length()) { Serial.println("Password cannot be empty."); return; }
  wifiRecords[index].ssid = ssid; wifiRecords[index].password = password;
  wifiRecords[index].lastIP = "";
  if (saveWifiRecords()) Serial.println("Record updated.");
}

void deleteWifiRecord() {
  loadWifiRecords(); showNetworks();
  if (wifiRecords.empty()) return;
  int index = askRecordNumber("Enter record number to delete:");
  if (index < 0) { Serial.println("Invalid record number."); return; }
  wifiRecords.erase(wifiRecords.begin() + index);
  if (saveWifiRecords()) Serial.println("Record deleted.");
}

void clearWifiRecords() {
  if (!littleFSReady) { Serial.println("LittleFS is not available."); return; }
  WiFi.disconnect(true); WiFi.mode(WIFI_STA); wifiRecords.clear();
  if (LittleFS.exists(NETWORK_FILE)) LittleFS.remove(NETWORK_FILE);
  Serial.println("All saved Wi-Fi credentials deleted.");
}

void showWifi() {
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi status: CONNECTED");
    Serial.print("SSID: "); Serial.println(WiFi.SSID());
    Serial.print("IP address: "); Serial.println(WiFi.localIP());
    Serial.print("Signal strength (RSSI): "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
  } else Serial.println("Wi-Fi status: DISCONNECTED");
}

void showStatus() {
  Serial.println("\n========== DEVICE STATUS ==========");
  Serial.print("Device ID: "); Serial.println(deviceID);
  Serial.print("LittleFS (Wi-Fi credentials): "); Serial.println(littleFSReady ? "READY" : "NOT AVAILABLE");
  Serial.print("SD card: "); Serial.println(sdReady ? "READY" : "NOT AVAILABLE");
  Serial.print("ADS1115 ready: "); Serial.println(adsReady ? "YES" : "NO");
  loadWifiRecords();
  Serial.print("Saved Wi-Fi records: "); Serial.println(wifiRecords.size());
  showLocalCsvStatus(); showWifi();
  Serial.print("Installation timezone: ");
  Serial.println(installationTimezone.length() ? installationTimezone : "(not detected)");
  Serial.print("UTC Unix timestamp: "); Serial.println((unsigned long)time(nullptr));
  Serial.println("===================================");
}

void showHelp() {
  Serial.println("\n=========== COMMANDS ===========");
  Serial.println("HELP            - Show commands");
  Serial.println("SHOW NETWORKS   - List SSIDs and saved IPs");
  Serial.println("READ FILE       - Read networks.txt (shows passwords)");
  Serial.println("SET WIFI        - Append a Wi-Fi credential");
  Serial.println("EDIT WIFI       - Edit a record by number");
  Serial.println("DELETE WIFI     - Delete a record by number");
  Serial.println("CLEAR WIFI      - Delete all credentials");
  Serial.println("SHOW WIFI       - Show current Wi-Fi connection");
  Serial.println("SHOW CSV        - Show SD card CSV record count");
  Serial.println("UPLOAD SD CARD  - Transfer SD CSV (only 9 fields) to Render");
  Serial.println("UPLOAD LITTLEFS - Transfer LittleFS CSV to Render");
  Serial.println("STOP UPLOAD     - Stop current historical upload");
  Serial.println("STATUS          - Show status");
  Serial.println("RESTART         - Restart ESP8266");
  Serial.println("================================");
}

void handleSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      serialCommandBuffer.trim();
      String command = serialCommandBuffer;
      serialCommandBuffer = "";
      command.toUpperCase();
      if (!command.length()) continue;
      if (command == "HELP") showHelp();
      else if (command == "SHOW NETWORKS" || command == "SHOW NETWORK") showNetworks();
      else if (command == "READ FILE") readNetworkFile();
      else if (command == "SET WIFI") setWifiRecord();
      else if (command == "EDIT WIFI") editWifiRecord();
      else if (command == "DELETE WIFI") deleteWifiRecord();
      else if (command == "CLEAR WIFI") clearWifiRecords();
      else if (command == "SHOW WIFI") showWifi();
      else if (command == "SHOW CSV") showLocalCsvStatus();
      else if (command == "UPLOAD SD CARD" || command == "SYNC SD CARD") uploadSdCsv();
      else if (command == "UPLOAD LITTLEFS" || command == "SYNC LITTLEFS") uploadLittleFsCsv();
      else if (command == "STOP UPLOAD") { stopHistoricalUpload = true; Serial.println("Stop requested."); }
      else if (command == "STATUS") showStatus();
      else if (command == "RESTART") { Serial.println("Restarting ESP8266..."); delay(500); ESP.restart(); }
      else Serial.println("Unknown command. Type HELP.");
    } else if (serialCommandBuffer.length() < 100) serialCommandBuffer += c;
  }
}

// ---------------- TIME / GEOLOCATION ----------------
bool detectTimezone() {
  Serial.println("\nChecking installation timezone...");
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, GEO_URL)) { Serial.println("Could not start geolocation request."); return false; }
  http.setTimeout(10000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.print("Geolocation HTTP error: "); Serial.println(code); http.end(); return false;
  }
  String response = http.getString(); http.end();
  Serial.println("Geolocation response:"); Serial.println(response);
  if (response.indexOf("\"status\":\"success\"") < 0) {
    Serial.println("Geolocation request was unsuccessful."); return false;
  }
  String key = "\"timezone\":\"";
  int start = response.indexOf(key);
  if (start < 0) { Serial.println("Timezone field not found."); return false; }
  start += key.length();
  int finish = response.indexOf("\"", start);
  if (finish < 0) { Serial.println("Invalid timezone response."); return false; }
  String detected = response.substring(start, finish); detected.trim();
  if (!detected.length() || detected.indexOf('/') < 0) {
    Serial.println("Invalid timezone returned."); return false;
  }
  installationTimezone = detected;
  Serial.print("Detected timezone: "); Serial.println(installationTimezone);
  return true;
}

bool synchronizeTime() {
  Serial.println("\nSynchronizing UTC time using NTP...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  for (int i = 0; i < 30; i++) {
    time_t now = time(nullptr);
    if (now > 1700000000) {
      Serial.println("NTP synchronization successful.");
      Serial.print("UTC Unix timestamp: "); Serial.println((unsigned long)now);
      return true;
    }
    Serial.print("."); delay(1000); yield();
  }
  Serial.println("\nNTP synchronization failed.");
  return false;
}

// ---------------- ADS1115 ----------------
bool initializeADS1115() {
  const uint8_t addresses[] = {0x48, 0x49, 0x4A, 0x4B};
  Serial.println("Checking ADS1115 I2C addresses...");
  for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
    uint8_t address = addresses[i];
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();
    Serial.print("I2C address 0x"); if (address < 0x10) Serial.print("0");
    Serial.print(address, HEX); Serial.print(": ");
    if (error == 0 && ads.begin(address)) {
      ads.setGain(GAIN_TWOTHIRDS);
      Serial.println("ADS1115 detected.");
      Serial.print("Using ADS1115 I2C address: 0x"); Serial.println(address, HEX);
      return true;
    }
    Serial.println("no response");
  }
  Serial.println("ADS1115 not detected. Will retry automatically.");
  Serial.println("Check 3.3V, GND, SDA=D2, SCL=D1 and ADDR.");
  return false;
}

bool readADS1115() {
  int16_t raw0 = ads.readADC_SingleEnded(0);
  int16_t raw1 = ads.readADC_SingleEnded(1);
  int16_t raw2 = ads.readADC_SingleEnded(2);
  float voltage0 = ads.computeVolts(raw0);
  float voltage1 = ads.computeVolts(raw1);
  float voltage2 = ads.computeVolts(raw2);
  adcvalue1 = (int)round(voltage0 * 1000.0);
  adcvalue2 = (int)round(voltage1 * 1000.0);
  adcvalue3 = (int)round(voltage2 * 1000.0);
  Serial.println("\n----- ADS1115 READINGS -----");
  Serial.print("A0 raw count: "); Serial.println(raw0);
  Serial.print("A0 voltage: "); Serial.print(voltage0, 4); Serial.println(" V");
  Serial.print("A1 raw count: "); Serial.println(raw1);
  Serial.print("A1 voltage: "); Serial.print(voltage1, 4); Serial.println(" V");
  Serial.print("A2 raw count: "); Serial.println(raw2);
  Serial.print("A2 voltage: "); Serial.print(voltage2, 4); Serial.println(" V");
  Serial.println("----- UPLOAD VALUES (mV) -----");
  Serial.print("value1 = "); Serial.println(adcvalue1);
  Serial.print("value2 = "); Serial.println(adcvalue2);
  Serial.print("value3 = "); Serial.println(adcvalue3);
  return true;
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n==================================");
  Serial.println("ESP8266 ADS1115 SD CARD DATA LOGGER");
  Serial.println("Fixed Device ID: ESP0001");
  Serial.println("SD: 25 columns (9 standard + CH1..CH16); LittleFS: 9 columns");
  Serial.println("TiDB upload: only 9 standard fields; CH1..CH16 stay local");
  Serial.println("==================================");

  Wire.begin(D2, D1); // SDA=D2/GPIO4, SCL=D1/GPIO5
  adsReady = initializeADS1115();
  previousADSCheck = millis();

  littleFSReady = LittleFS.begin();
  if (!littleFSReady) {
    Serial.println("ERROR: LittleFS mount failed; Wi-Fi commands cannot save credentials.");
  } else {
    Serial.println("LittleFS mounted (Wi-Fi credentials only).");
    if (!LittleFS.exists(NETWORK_FILE)) {
      File newFile = LittleFS.open(NETWORK_FILE, "w");
      if (newFile) newFile.close();
    }
    loadWifiRecords();
    initializeLittleFsCsv();
  }

  // Initialize SPI SD card. SPI pins: SCK D5, MISO D6, MOSI D7, CS D0.
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  SPI.begin();
  if (SD.begin(SD_CS_PIN)) {
    sdReady = true;
    Serial.println("SD card initialized successfully.");
    initializeLocalCsv();
  } else {
    sdReady = false;
    Serial.println("ERROR: SD card initialization failed.");
    Serial.println("Check SD wiring, card format (FAT16/FAT32), power and CS=D0.");
  }

  connectToSavedNetworks();
  Serial.println("\nType HELP in Serial Monitor. Line ending: Newline.");
  Serial.print("Fixed device ID: "); Serial.println(deviceID);
  Serial.print("Automatic upload endpoint: "); Serial.println(SERVER_URL);

  bool timezoneOK = false;
  if (WiFi.status() == WL_CONNECTED) timezoneOK = detectTimezone();
  previousTimezoneCheck = millis();
  if (!timezoneOK) Serial.println("Timezone detection will be retried.");

  if (WiFi.status() == WL_CONNECTED) synchronizeTime();
}

// ---------------- MAIN LOOP ----------------
void loop() {
  handleSerialCommands();

  if (!adsReady && millis() - previousADSCheck >= adsRetryInterval) {
    previousADSCheck = millis();
    adsReady = initializeADS1115();
    if (adsReady) Serial.println("ADS1115 is now ready.");
  }

  bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  if (!wifiConnected) {
    static unsigned long lastReconnectAttempt = 0;
    if (millis() - lastReconnectAttempt >= 15000UL) {
      lastReconnectAttempt = millis();
      Serial.println("Wi-Fi disconnected. Trying saved credentials in order...");
      connectToSavedNetworks();
      wifiConnected = (WiFi.status() == WL_CONNECTED);
    }
  }

  unsigned long nowMillis = millis();
  unsigned long refreshInterval = installationTimezone.length() == 0
      ? timezoneRetryInterval : timezoneRefreshInterval;
  if (wifiConnected && nowMillis - previousTimezoneCheck >= refreshInterval) {
    previousTimezoneCheck = nowMillis;
    if (!detectTimezone()) Serial.println("Timezone refresh failed.");
  }

  if (nowMillis - previousSendTime < sendInterval) return;
  previousSendTime = nowMillis;

  if (!adsReady) {
    Serial.println("Reading skipped: ADS1115 not detected; retrying automatically.");
    return;
  }
  if (!sdReady) {
    Serial.println("Reading skipped: SD card is not ready; local storage is required.");
    return;
  }
  if (!installationTimezone.length()) {
    Serial.println("Reading skipped: installation timezone is not available yet.");
    return;
  }

  time_t utcTimestamp = time(nullptr);
  if (utcTimestamp <= 1700000000) {
    Serial.println("UTC clock is invalid; NTP synchronization requires Wi-Fi.");
    if (!wifiConnected || !synchronizeTime()) {
      Serial.println("Reading skipped until valid time is available.");
      return;
    }
    utcTimestamp = time(nullptr);
    if (utcTimestamp <= 1700000000) {
      Serial.println("UTC time is still invalid; reading skipped.");
      return;
    }
  }

  readADS1115();
  Serial.println("\n----------------------------------");
  Serial.println("Saving ADS1115 reading to SD card and, if online, Render");
  Serial.print("Device ID: "); Serial.println(deviceID);
  Serial.print("UTC timestamp: "); Serial.println((unsigned long)utcTimestamp);
  Serial.print("Timezone: "); Serial.println(installationTimezone);

  bool localSaved = appendLocalCsvRecord(adcvalue1, adcvalue2, adcvalue3,
                                          utcTimestamp, installationTimezone,
                                          String(deviceID));
  if (!localSaved) {
    Serial.println("ERROR: Reading was not saved to SD card; upload skipped to avoid losing the local record.");
    return;
  }

  if (!wifiConnected) {
    Serial.println("Wi-Fi offline: SD card copy retained for later transfer.");
    return;
  }

  String postData =
    "device_id=" + urlEncode(String(deviceID)) +
    "&api_token=" + urlEncode(String(apiToken)) +
    "&value1=" + String(adcvalue1) +
    "&value2=" + String(adcvalue2) +
    "&value3=" + String(adcvalue3) +
    "&timestamp=" + String((unsigned long)utcTimestamp) +
    "&timezone=" + urlEncode(installationTimezone);

  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (http.begin(client, SERVER_URL)) {
    http.setTimeout(15000);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    int httpCode = http.POST(postData);
    Serial.print("Render HTTP response code: "); Serial.println(httpCode);
    if (httpCode > 0) {
      Serial.println("Render endpoint response:");
      Serial.println(http.getString());
    } else {
      Serial.print("HTTPS error: "); Serial.println(http.errorToString(httpCode));
      Serial.println("SD card copy is retained for later transfer.");
    }
    http.end();
  } else {
    Serial.println("Could not initialize Render HTTPS connection.");
    Serial.println("SD card copy is retained.");
  }
}
