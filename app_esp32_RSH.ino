/*
  ESP32 16-CHANNEL DATA LOGGER
  CD74HC4067 + LittleFS + SD CARD + RENDER / TiDB CLOUD

  Device ID: ESP0001

  LIVE ENDPOINT:
  https://datalogger4-remote.onrender.com/save3_csv.php

  LITTLEFS SYNC:
  https://datalogger4-remote.onrender.com/sync_littlefs.php

  SD CARD SYNC:
  https://datalogger4-remote.onrender.com/sync_sdcard.php

  LITTLEFS CSV: EXACTLY 9 COLUMNS
  id,value1,value2,value3,Date,Time,TimeStamp,timezone,device_id

  SD CARD CSV: EXACTLY 25 COLUMNS
  id,value1,value2,value3,Date,Time,TimeStamp,timezone,device_id,
  CH1,CH2,CH3,CH4,CH5,CH6,CH7,CH8,CH9,CH10,CH11,CH12,CH13,CH14,
  CH15,CH16

  All 16 channels are scanned.
  CH1-CH3 are sent to the live endpoint.
  Historical sync sends only the first 9 fields to PHP/TiDB.

  Commands:
  HELP
  SHOW NETWORKS
  READ FILE
  SET WIFI
  EDIT WIFI
  DELETE WIFI
  CLEAR WIFI
  SHOW WIFI
  SHOW CSV
  SHOW SD
  UPLOAD LITTLEFS
  UPLOAD SDCARD
  STOP UPLOAD
  STATUS
  RESTART

  IMPORTANT:
  - LittleFS and SD card retain their original CSV files.
  - STOP UPLOAD stops historical transfer after the current
    HTTP request finishes.
  - Upload success requires HTTP 200 and JSON success:true.
  - System time is UTC; Date and Time are installation-local.
  - SD CS pin defaults to GPIO 5. Change it to match wiring.
*/

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <math.h>
#include <LittleFS.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <stdlib.h>

// =====================================================
// SERVER SETTINGS
// =====================================================

const char *LIVE_URL =
  "https://datalogger5-remote.onrender.com/save3_csv.php";

const char *LITTLEFS_SYNC_URL =
  "https://datalogger5-remote.onrender.com/sync_littlefs.php";

const char *SD_SYNC_URL =
  "https://datalogger5-remote.onrender.com/sync_sdcard.php";

const char *GEO_URL =
  "http://ip-api.com/json/?fields=status,timezone";

const char *DEVICE_ID = "ESP0001";
const char *API_TOKEN = "EspSwitch52026";

// =====================================================
// FILE SETTINGS
// =====================================================

const char *NETWORK_FILE = "/networks.txt";
const char *LITTLEFS_CSV = "/sensor_data.csv";
const char *SD_CSV = "/sensor_data.csv";

const char *LITTLEFS_HEADER =
  "id,value1,value2,value3,Date,Time,TimeStamp,timezone,device_id";

const char *SD_HEADER =
  "id,value1,value2,value3,Date,Time,TimeStamp,timezone,device_id,"
  "CH1,CH2,CH3,CH4,CH5,CH6,CH7,CH8,"
  "CH9,CH10,CH11,CH12,CH13,CH14,CH15,CH16";

// =====================================================
// HARDWARE SETTINGS
// =====================================================

// CD74HC4067 multiplexer
const int MUX_S0 = 25;
const int MUX_S1 = 26;
const int MUX_S2 = 27;
const int MUX_S3 = 32;
const int MUX_SIGNAL_PIN = 34;

// SD card chip-select pin.
// Change this if your actual wiring uses another GPIO.
const int SD_CS_PIN = 5;

const int CHANNEL_COUNT = 16;
const int SAMPLES_PER_CHANNEL = 8;

const unsigned long MUX_SETTLE_DELAY_MS = 2;

const int ADC_MAXIMUM = 4095;
const float ADC_FULL_SCALE_MV = 3300.0f;

int channelMv[CHANNEL_COUNT] = {0};

int adcvalue1 = 0;
int adcvalue2 = 0;
int adcvalue3 = 0;

// =====================================================
// GLOBAL VARIABLES
// =====================================================

bool littleFSReady = false;
bool sdReady = false;

unsigned long nextRecordId = 1;

bool uploadInProgress = false;
bool stopUploadRequested = false;

String installationTimezone = "";

String serialCommandBuffer = "";

unsigned long previousSendTime = 0;
unsigned long previousTimezoneCheck = 0;
unsigned long previousReconnectAttempt = 0;

const unsigned long SEND_INTERVAL = 5000UL;

const unsigned long TIMEZONE_REFRESH_INTERVAL =
  6UL * 60UL * 60UL * 1000UL;

const unsigned long TIMEZONE_RETRY_INTERVAL =
  5UL * 60UL * 1000UL;

struct WifiRecord {
  String ssid;
  String password;
  String lastIP;
};

std::vector<WifiRecord> wifiRecords;

// =====================================================
// URL ENCODING
// =====================================================

String urlEncode(const String &input) {
  String output = "";
  const char hex[] = "0123456789ABCDEF";

  for (unsigned int i = 0; i < input.length(); i++) {
    unsigned char c = input[i];

    if ((c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' ||
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

// =====================================================
// CSV FIELD SPLITTER
// These CSV files contain no quoted fields or embedded
// commas, so simple comma splitting is sufficient.
// =====================================================

bool splitCsv(
  const String &line,
  std::vector<String> &fields
) {
  fields.clear();

  int start = 0;

  for (int i = 0; i <= (int)line.length(); i++) {
    if (i == (int)line.length() || line[i] == ',') {
      fields.push_back(line.substring(start, i));
      start = i + 1;
    }
  }

  return true;
}

// =====================================================
// WIFI CREDENTIAL FILE
// Format: SSID, password, last IP on separate lines
// =====================================================

bool loadWifiRecords() {
  wifiRecords.clear();

  if (!littleFSReady ||
      !LittleFS.exists(NETWORK_FILE)) {
    return true;
  }

  File f = LittleFS.open(NETWORK_FILE, FILE_READ);

  if (!f) return false;

  while (f.available()) {
    String ssid = f.readStringUntil('\n');
    ssid.trim();

    if (!ssid.length()) continue;

    String password = f.readStringUntil('\n');
    password.trim();

    String ip = f.readStringUntil('\n');
    ip.trim();

    WifiRecord r;
    r.ssid = ssid;
    r.password = password;
    r.lastIP = ip;

    wifiRecords.push_back(r);
  }

  f.close();
  return true;
}

bool saveWifiRecords() {
  if (!littleFSReady) return false;

  File f = LittleFS.open(NETWORK_FILE, FILE_WRITE);

  if (!f) return false;

  for (size_t i = 0; i < wifiRecords.size(); i++) {
    f.println(wifiRecords[i].ssid);
    f.println(wifiRecords[i].password);
    f.println(wifiRecords[i].lastIP);
  }

  f.close();
  return true;
}

// =====================================================
// WIFI CONNECTION
// =====================================================

bool connectToSavedNetworks() {
  if (!loadWifiRecords()) {
    Serial.println("Cannot read networks.txt.");
    return false;
  }

  if (wifiRecords.empty()) {
    Serial.println("No saved Wi-Fi. Use SET WIFI.");
    return false;
  }

  WiFi.mode(WIFI_STA);

  for (size_t i = 0; i < wifiRecords.size(); i++) {
    Serial.print("Trying Wi-Fi: ");
    Serial.println(wifiRecords[i].ssid);

    WiFi.disconnect();
    delay(300);

    WiFi.begin(
      wifiRecords[i].ssid.c_str(),
      wifiRecords[i].password.c_str()
    );

    unsigned long started = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - started < 12000UL) {
      delay(250);
      Serial.print(".");

      if (Serial.available()) break;
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Wi-Fi connected.");

      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());

      wifiRecords[i].lastIP = WiFi.localIP().toString();
      saveWifiRecords();

      return true;
    }
  }

  Serial.println("All saved Wi-Fi networks failed.");
  return false;
}

String readSerialLine(const char *prompt) {
  Serial.println(prompt);

  String result = "";
  unsigned long started = millis();

  while (millis() - started < 120000UL) {
    while (Serial.available()) {
      char c = (char)Serial.read();

      if (c == '\r') continue;

      if (c == '\n') {
        result.trim();
        return result;
      }

      if (result.length() < 120) {
        result += c;
      }
    }

    delay(10);
  }

  Serial.println("Input timed out.");
  return "";
}

void showNetworks() {
  loadWifiRecords();

  Serial.println("\n------ SAVED WIFI NETWORKS ------");

  if (wifiRecords.empty()) {
    Serial.println("No saved networks.");
  } else {
    for (size_t i = 0; i < wifiRecords.size(); i++) {
      Serial.print(i + 1);
      Serial.print(". SSID: ");
      Serial.print(wifiRecords[i].ssid);
      Serial.print(" | IP: ");

      Serial.println(
        wifiRecords[i].lastIP.length()
          ? wifiRecords[i].lastIP
          : "(not connected)"
      );
    }
  }

  Serial.println("--------------------------------");
}

void readNetworkFile() {
  loadWifiRecords();

  Serial.println("\n------ networks.txt ------");

  for (size_t i = 0; i < wifiRecords.size(); i++) {
    Serial.print("Record: ");
    Serial.println(i + 1);

    Serial.print("SSID: ");
    Serial.println(wifiRecords[i].ssid);

    Serial.print("Password: ");
    Serial.println(wifiRecords[i].password);

    Serial.print("IP: ");
    Serial.println(wifiRecords[i].lastIP);
  }
}

void setWifiRecord() {
  String ssid = readSerialLine("Enter Wi-Fi SSID:");
  if (!ssid.length()) return;

  String password = readSerialLine("Enter Wi-Fi password:");
  if (!password.length()) return;

  loadWifiRecords();

  for (size_t i = 0; i < wifiRecords.size(); i++) {
    if (wifiRecords[i].ssid == ssid &&
        wifiRecords[i].password == password) {
      Serial.println("Already saved.");
      return;
    }
  }

  WifiRecord r;
  r.ssid = ssid;
  r.password = password;
  r.lastIP = "";

  wifiRecords.push_back(r);

  if (!saveWifiRecords()) {
    Serial.println("Could not save Wi-Fi.");
    return;
  }

  Serial.println("Wi-Fi saved. Connecting...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long started = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - started < 15000UL) {
    delay(250);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected.");
    Serial.println(WiFi.localIP());

    for (size_t i = 0; i < wifiRecords.size(); i++) {
      if (wifiRecords[i].ssid == ssid) {
        wifiRecords[i].lastIP = WiFi.localIP().toString();
        break;
      }
    }

    saveWifiRecords();
  } else {
    Serial.println("Connection failed.");
  }
}

int askRecordNumber(const char *prompt) {
  String answer = readSerialLine(prompt);
  int number = answer.toInt();

  if (number < 1 ||
      number > (int)wifiRecords.size()) {
    return -1;
  }

  return number - 1;
}

void editWifiRecord() {
  loadWifiRecords();
  showNetworks();

  if (wifiRecords.empty()) return;

  int index = askRecordNumber("Enter record number to edit:");
  if (index < 0) return;

  String ssid = readSerialLine("Enter new SSID:");
  if (!ssid.length()) return;

  String password = readSerialLine("Enter new password:");
  if (!password.length()) return;

  wifiRecords[index].ssid = ssid;
  wifiRecords[index].password = password;
  wifiRecords[index].lastIP = "";

  Serial.println(
    saveWifiRecords() ? "Wi-Fi record updated."
                      : "Could not update Wi-Fi record."
  );
}

void deleteWifiRecord() {
  loadWifiRecords();
  showNetworks();

  if (wifiRecords.empty()) return;

  int index = askRecordNumber("Enter record number to delete:");
  if (index < 0) return;

  wifiRecords.erase(wifiRecords.begin() + index);

  Serial.println(
    saveWifiRecords() ? "Record deleted."
                      : "Could not save Wi-Fi file."
  );
}

void clearWifiRecords() {
  if (!littleFSReady) return;

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);

  wifiRecords.clear();

  if (LittleFS.exists(NETWORK_FILE)) {
    LittleFS.remove(NETWORK_FILE);
  }

  Serial.println("All Wi-Fi credentials deleted.");
}

void showWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi: CONNECTED");

    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());

    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("RSSI: ");
    Serial.println(WiFi.RSSI());
  } else {
    Serial.println("Wi-Fi: DISCONNECTED");
  }
}

// =====================================================
// TIMEZONE MAPPING
// =====================================================

String posixTimezoneForIana(const String &iana) {
  if (iana == "Asia/Kolkata" ||
      iana == "Asia/Calcutta")
    return "IST-5:30";

  if (iana == "Asia/Kathmandu")
    return "NPT-5:45";

  if (iana == "Asia/Dubai")
    return "GST-4";

  if (iana == "Asia/Singapore")
    return "SGT-8";

  if (iana == "Asia/Tokyo")
    return "JST-9";

  if (iana == "Asia/Shanghai" ||
      iana == "Asia/Hong_Kong")
    return "CST-8";

  if (iana == "UTC" ||
      iana == "Etc/UTC" ||
      iana == "Etc/GMT")
    return "UTC0";

  if (iana == "Europe/London")
    return "GMT0BST,M3.5.0/1,M10.5.0";

  if (iana == "Europe/Paris" ||
      iana == "Europe/Berlin" ||
      iana == "Europe/Rome")
    return "CET-1CEST,M3.5.0,M10.5.0/3";

  if (iana == "America/New_York")
    return "EST5EDT,M3.2.0,M11.1.0";

  if (iana == "America/Chicago")
    return "CST6CDT,M3.2.0,M11.1.0";

  if (iana == "America/Denver")
    return "MST7MDT,M3.2.0,M11.1.0";

  if (iana == "America/Los_Angeles")
    return "PST8PDT,M3.2.0,M11.1.0";

  if (iana == "Australia/Sydney")
    return "AEST-10AEDT,M10.1.0,M4.1.0/3";

  return "";
}

bool applyInstallationTimezone() {
  String posix = posixTimezoneForIana(installationTimezone);

  if (!posix.length()) {
    Serial.print("Unsupported timezone: ");
    Serial.println(installationTimezone);
    return false;
  }

  setenv("TZ", posix.c_str(), 1);
  tzset();

  return true;
}

// =====================================================
// TIMEZONE DETECTION
// =====================================================

bool detectTimezone() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;

  if (!http.begin(client, GEO_URL)) return false;

  http.setTimeout(10000);

  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    Serial.print("Timezone HTTP error: ");
    Serial.println(code);
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  Serial.println("Timezone service response:");
  Serial.println(response);

  if (response.indexOf("\"status\":\"success\"") < 0) {
    return false;
  }

  String key = "\"timezone\":\"";
  int start = response.indexOf(key);

  if (start < 0) return false;

  start += key.length();

  int finish = response.indexOf("\"", start);
  if (finish < 0) return false;

  String detected = response.substring(start, finish);
  detected.trim();

  if (!detected.length() ||
      detected.indexOf('/') < 0) {
    return false;
  }

  String previous = installationTimezone;
  installationTimezone = detected;

  if (!applyInstallationTimezone()) {
    installationTimezone = previous;
    return false;
  }

  Serial.print("Installation timezone: ");
  Serial.println(installationTimezone);

  return true;
}

// =====================================================
// NTP UTC TIME
// =====================================================

bool synchronizeTime() {
  if (WiFi.status() != WL_CONNECTED) return false;

  Serial.println("Synchronizing UTC using NTP...");

  configTime(
    0,
    0,
    "pool.ntp.org",
    "time.nist.gov"
  );

  for (int i = 0; i < 30; i++) {
    time_t nowUtc = time(nullptr);

    if (nowUtc > 1700000000) {
      applyInstallationTimezone();

      Serial.println("NTP synchronization successful.");
      return true;
    }

    delay(1000);
    Serial.print(".");
  }

  Serial.println("\nNTP synchronization failed.");
  return false;
}

void printCurrentClock(const char *label) {
  time_t nowUtc = time(nullptr);

  Serial.println();
  Serial.println("--------------------------------");
  Serial.println(label);

  if (nowUtc <= 1700000000) {
    Serial.println("Clock not synchronized.");
    Serial.println("--------------------------------");
    return;
  }

  struct tm utcParts;

  if (gmtime_r(&nowUtc, &utcParts)) {
    char buf[25];

    snprintf(
      buf, sizeof(buf),
      "%04d-%02d-%02d %02d:%02d:%02d",
      utcParts.tm_year + 1900,
      utcParts.tm_mon + 1,
      utcParts.tm_mday,
      utcParts.tm_hour,
      utcParts.tm_min,
      utcParts.tm_sec
    );

    Serial.print("UTC time: ");
    Serial.println(buf);
  }

  if (installationTimezone.length()) {
    applyInstallationTimezone();

    struct tm localParts;

    if (localtime_r(&nowUtc, &localParts)) {
      char buf[25];

      snprintf(
        buf, sizeof(buf),
        "%04d-%02d-%02d %02d:%02d:%02d",
        localParts.tm_year + 1900,
        localParts.tm_mon + 1,
        localParts.tm_mday,
        localParts.tm_hour,
        localParts.tm_min,
        localParts.tm_sec
      );

      Serial.print("Local time (");
      Serial.print(installationTimezone);
      Serial.print("): ");
      Serial.println(buf);
    }
  }

  Serial.print("UTC Unix timestamp: ");
  Serial.println((unsigned long)nowUtc);

  Serial.println("--------------------------------");
}

// =====================================================
// FIND THE LAST ID IN A CSV FILE
// =====================================================

unsigned long findLastIdFromFile(
  bool isSD,
  const char *path
) {
  bool exists = isSD
    ? (sdReady && SD.exists(path))
    : (littleFSReady && LittleFS.exists(path));

  if (!exists) return 0;

  File f = isSD
    ? SD.open(path, FILE_READ)
    : LittleFS.open(path, FILE_READ);

  if (!f) return 0;

  unsigned long lastId = 0;

  // Skip header.
  if (f.available()) {
    f.readStringUntil('\n');
  }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();

    if (!line.length()) continue;

    int comma = line.indexOf(',');

    if (comma > 0) {
      unsigned long id =
        (unsigned long)line.substring(0, comma).toInt();

      if (id > lastId) lastId = id;
    }
  }

  f.close();
  return lastId;
}

// =====================================================
// INITIALIZE LITTLEFS CSV
// Existing data is not erased.
// =====================================================

bool initializeLittleFsCsv() {
  if (!littleFSReady) return false;

  if (!LittleFS.exists(LITTLEFS_CSV)) {
    File f = LittleFS.open(LITTLEFS_CSV, FILE_WRITE);

    if (!f) return false;

    f.println(LITTLEFS_HEADER);
    f.close();
  } else {
    File f = LittleFS.open(LITTLEFS_CSV, FILE_READ);

    if (!f) return false;

    String header = f.readStringUntil('\n');
    header.trim();
    f.close();

    if (header != LITTLEFS_HEADER) {
      Serial.println(
        "ERROR: LittleFS CSV header mismatch. "
        "Existing file preserved."
      );
      return false;
    }
  }

  return true;
}

// =====================================================
// INITIALIZE SD CARD CSV
// Existing data is not erased.
// =====================================================

bool initializeSdCsv() {
  if (!sdReady) return false;

  if (!SD.exists(SD_CSV)) {
    File f = SD.open(SD_CSV, FILE_WRITE);

    if (!f) return false;

    f.println(SD_HEADER);
    f.close();
  } else {
    File f = SD.open(SD_CSV, FILE_READ);

    if (!f) return false;

    String header = f.readStringUntil('\n');
    header.trim();
    f.close();

    if (header != SD_HEADER) {
      Serial.println(
        "ERROR: SD CSV header mismatch. "
        "Existing file preserved."
      );
      return false;
    }
  }

  return true;
}

// =====================================================
// INITIALIZE BOTH STORAGE SYSTEMS
// =====================================================

void initializeStorage() {
  if (littleFSReady) {
    if (!initializeLittleFsCsv()) {
      Serial.println("LittleFS CSV initialization failed.");
    }
  }

  if (sdReady) {
    if (!initializeSdCsv()) {
      Serial.println("SD CSV initialization failed.");
    }
  }

  unsigned long lastLittleFs =
    findLastIdFromFile(false, LITTLEFS_CSV);

  unsigned long lastSd =
    findLastIdFromFile(true, SD_CSV);

  unsigned long highest = lastLittleFs;

  if (lastSd > highest) highest = lastSd;

  nextRecordId = highest + 1;

  Serial.print("Next shared local record ID: ");
  Serial.println(nextRecordId);
}

// =====================================================
// CREATE RECORD VALUES
// Date and Time are local; TimeStamp is UTC.
// =====================================================

bool createTimeFields(
  time_t utcTimestamp,
  char *dateBuf,
  size_t dateSize,
  char *timeBuf,
  size_t timeSize,
  char *stampBuf,
  size_t stampSize
) {
  if (utcTimestamp <= 1700000000) return false;

  struct tm utcTm;

  if (!gmtime_r(&utcTimestamp, &utcTm)) return false;

  snprintf(
    stampBuf, stampSize,
    "%04d-%02d-%02d %02d:%02d:%02d",
    utcTm.tm_year + 1900,
    utcTm.tm_mon + 1,
    utcTm.tm_mday,
    utcTm.tm_hour,
    utcTm.tm_min,
    utcTm.tm_sec
  );

  if (!installationTimezone.length()) return false;

  if (!applyInstallationTimezone()) return false;

  struct tm localTm;

  if (!localtime_r(&utcTimestamp, &localTm)) return false;

  snprintf(
    dateBuf, dateSize,
    "%04d-%02d-%02d",
    localTm.tm_year + 1900,
    localTm.tm_mon + 1,
    localTm.tm_mday
  );

  snprintf(
    timeBuf, timeSize,
    "%02d:%02d:%02d",
    localTm.tm_hour,
    localTm.tm_min,
    localTm.tm_sec
  );

  return true;
}

// =====================================================
// APPEND RECORD TO LITTLEFS: 9 COLUMNS
// =====================================================

bool appendLittleFsRecord(
  unsigned long id,
  const int values[16],
  const char *date,
  const char *timeText,
  const char *utcStamp,
  const String &timezone
) {
  if (!littleFSReady) return false;
  if (!initializeLittleFsCsv()) return false;

  File f = LittleFS.open(LITTLEFS_CSV, FILE_APPEND);

  if (!f) {
    Serial.println("Cannot append LittleFS CSV.");
    return false;
  }

  String row =
    String(id) + "," +
    String(values[0]) + "," +
    String(values[1]) + "," +
    String(values[2]) + "," +
    String(date) + "," +
    String(timeText) + "," +
    String(utcStamp) + "," +
    timezone + "," +
    String(DEVICE_ID);

  size_t written = f.println(row);
  f.flush();
  f.close();

  if (!written) return false;

  Serial.print("LittleFS saved ID: ");
  Serial.println(id);

  return true;
}

// =====================================================
// APPEND RECORD TO SD CARD: 25 COLUMNS
// =====================================================

bool appendSdRecord(
  unsigned long id,
  const int values[16],
  const char *date,
  const char *timeText,
  const char *utcStamp,
  const String &timezone
) {
  if (!sdReady) return false;
  if (!initializeSdCsv()) return false;

  File f = SD.open(SD_CSV, FILE_APPEND);

  if (!f) {
    Serial.println("Cannot append SD CSV.");
    return false;
  }

  // First 9 fields.
  String row =
    String(id) + "," +
    String(values[0]) + "," +
    String(values[1]) + "," +
    String(values[2]) + "," +
    String(date) + "," +
    String(timeText) + "," +
    String(utcStamp) + "," +
    timezone + "," +
    String(DEVICE_ID);

  // Additional 16 channel fields.
  for (int i = 0; i < CHANNEL_COUNT; i++) {
    row += ",";
    row += String(values[i]);
  }

  size_t written = f.println(row);
  f.flush();
  f.close();

  if (!written) return false;

  Serial.print("SD card saved ID: ");
  Serial.println(id);

  return true;
}

// =====================================================
// LIVE RECORDING: SEND CH1-CH3 TO save3_csv.php
// =====================================================

bool postLiveRecord(
  unsigned long utcTimestamp,
  const String &timezone
) {
  if (WiFi.status() != WL_CONNECTED) return false;

  String postData =
    "device_id=" + urlEncode(String(DEVICE_ID)) +
    "&api_token=" + urlEncode(String(API_TOKEN)) +
    "&value1=" + String(adcvalue1) +
    "&value2=" + String(adcvalue2) +
    "&value3=" + String(adcvalue3) +
    "&timestamp=" + String((unsigned long)utcTimestamp) +
    "&timezone=" + urlEncode(timezone);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  if (!http.begin(client, LIVE_URL)) {
    Serial.println("Cannot initialize live HTTPS request.");
    return false;
  }

  http.setTimeout(10000);

  http.addHeader(
    "Content-Type",
    "application/x-www-form-urlencoded"
  );

  int code = http.POST(postData);
  String response = http.getString();

  Serial.print("Live HTTP response code: ");
  Serial.println(code);

  Serial.println("Live PHP response:");
  Serial.println(response);

  http.end();

  return code == HTTP_CODE_OK;
}

// =====================================================
// CHECK STOP UPLOAD COMMAND
// Supports CR, LF and no line ending.
// =====================================================

bool checkStopUploadCommand() {
  static String stopBuffer = "";
  static unsigned long lastCharTime = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();

    if (c == '\r' || c == '\n') {
      stopBuffer.trim();
      stopBuffer.toUpperCase();

      bool isStop = stopBuffer == "STOP UPLOAD";
      stopBuffer = "";

      if (isStop) {
        stopUploadRequested = true;

        Serial.println();
        Serial.println("STOP UPLOAD received.");
        Serial.println(
          "Current HTTP request will finish first."
        );

        return true;
      }
    } else if (stopBuffer.length() < 100) {
      stopBuffer += c;
      lastCharTime = millis();
    }
  }

  if (stopBuffer.length() > 0 &&
      millis() - lastCharTime >= 300UL) {
    stopBuffer.trim();
    stopBuffer.toUpperCase();

    bool isStop = stopBuffer == "STOP UPLOAD";
    stopBuffer = "";

    if (isStop) {
      stopUploadRequested = true;

      Serial.println();
      Serial.println("STOP UPLOAD received.");
      Serial.println(
        "Current HTTP request will finish first."
      );

      return true;
    }
  }

  return stopUploadRequested;
}

// =====================================================
// POST ONE HISTORICAL RECORD
// LITTLEFS ROW HAS 9 COLUMNS.
// SD ROW HAS 25 COLUMNS.
// ONLY FIRST 9 FIELDS ARE SENT TO PHP.
// =====================================================

bool postHistoricalRecord(
  const String &line,
  bool fromSD
) {
  std::vector<String> fields;

  splitCsv(line, fields);

  const size_t expectedColumns = fromSD ? 25 : 9;

  if (fields.size() != expectedColumns) {
    Serial.print("Malformed historical CSV. Expected ");
    Serial.print(expectedColumns);
    Serial.print(" fields, found ");
    Serial.println(fields.size());

    return false;
  }

  // This upload intentionally sends only the nine
  // database fields, even when the source is the
  // 25-column SD card CSV.

  String form =
    "api_token=" + urlEncode(String(API_TOKEN)) +
    "&csv_id=" + urlEncode(fields[0]) +
    "&value1=" + urlEncode(fields[1]) +
    "&value2=" + urlEncode(fields[2]) +
    "&value3=" + urlEncode(fields[3]) +
    "&Date=" + urlEncode(fields[4]) +
    "&Time=" + urlEncode(fields[5]) +
    "&TimeStamp=" + urlEncode(fields[6]) +
    "&timezone=" + urlEncode(fields[7]) +
    "&device_id=" + urlEncode(fields[8]);

  const char *endpoint = fromSD
    ? SD_SYNC_URL
    : LITTLEFS_SYNC_URL;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  if (!http.begin(client, endpoint)) {
    Serial.println("Cannot initialize sync HTTPS request.");
    return false;
  }

  http.setTimeout(10000);

  http.addHeader(
    "Content-Type",
    "application/x-www-form-urlencoded"
  );

  int code = http.POST(form);
  String response = http.getString();

  http.end();

  Serial.print("Historical HTTP code: ");
  Serial.println(code);

  Serial.println("Historical PHP response:");
  Serial.println(response);

  // HTTP 200 alone is not sufficient.
  bool jsonSuccess =
    response.indexOf("\"success\":true") >= 0 ||
    response.indexOf("\"success\": true") >= 0;

  return code == HTTP_CODE_OK && jsonSuccess;
}

// =====================================================
// HISTORICAL UPLOAD ENGINE
// Reads the file without deleting or rewriting it.
// =====================================================

void uploadHistoricalCsv(bool fromSD) {
  if (uploadInProgress) {
    Serial.println("An upload is already running.");
    return;
  }

  stopUploadRequested = false;
  uploadInProgress = true;

  const char *path = fromSD ? SD_CSV : LITTLEFS_CSV;
  const char *expectedHeader = fromSD ? SD_HEADER : LITTLEFS_HEADER;

  Serial.println();
  Serial.println("========================================");

  if (fromSD) {
    Serial.println("START SD CARD HISTORICAL UPLOAD");
  } else {
    Serial.println("START LITTLEFS HISTORICAL UPLOAD");
  }

  Serial.println("========================================");

  printCurrentClock("BEFORE HISTORICAL UPLOAD");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi is disconnected.");
    uploadInProgress = false;
    return;
  }

  if (fromSD && !sdReady) {
    Serial.println("SD card is not available.");
    uploadInProgress = false;
    return;
  }

  if (!fromSD && !littleFSReady) {
    Serial.println("LittleFS is not available.");
    uploadInProgress = false;
    return;
  }

  File f = fromSD
    ? SD.open(path, FILE_READ)
    : LittleFS.open(path, FILE_READ);

  if (!f) {
    Serial.println("Cannot open historical CSV.");
    uploadInProgress = false;
    return;
  }

  f.setTimeout(10000);

  String header = f.readStringUntil('\n');
  header.trim();

  if (header != expectedHeader) {
    Serial.println("CSV header mismatch. Upload cancelled.");
    f.close();
    uploadInProgress = false;
    return;
  }

  unsigned long attempted = 0;
  unsigned long succeeded = 0;
  unsigned long failed = 0;
  unsigned long malformed = 0;

  bool stopped = false;

  while (f.available()) {
    // Check for STOP UPLOAD before each record.
    if (checkStopUploadCommand()) {
      stopped = true;
      break;
    }

    String line = f.readStringUntil('\n');
    line.trim();

    if (!line.length()) continue;

    // Validate column count before sending.
    std::vector<String> fields;
    splitCsv(line, fields);

    const size_t expectedColumns = fromSD ? 25 : 9;

    if (fields.size() != expectedColumns) {
      malformed++;

      Serial.print("Skipping malformed row: ");
      Serial.println(line);

      continue;
    }

    attempted++;

    Serial.print("Uploading record ID ");
    Serial.println(fields[0]);

    // The original source file is never modified.
    bool ok = postHistoricalRecord(line, fromSD);

    if (ok) {
      succeeded++;
    } else {
      failed++;
    }

    // Check serial input again after the HTTP request.
    if (checkStopUploadCommand()) {
      stopped = true;
      break;
    }

    delay(100);
    yield();
  }

  f.close();

  Serial.println();
  Serial.println("========== UPLOAD SUMMARY ==========");

  Serial.print("Records attempted: ");
  Serial.println(attempted);

  Serial.print("Successful: ");
  Serial.println(succeeded);

  Serial.print("Failed: ");
  Serial.println(failed);

  Serial.print("Malformed/skipped: ");
  Serial.println(malformed);

  if (stopped) {
    Serial.println("Upload stopped by user.");
  } else {
    Serial.println("Historical upload finished.");
  }

  Serial.println("Original CSV file has been retained.");

  printCurrentClock("AFTER HISTORICAL UPLOAD");

  Serial.println("====================================");

  uploadInProgress = false;
  stopUploadRequested = false;
}

// =====================================================
// CSV STATUS
// =====================================================

void showOneCsvStatus(
  bool fromSD,
  const char *path
) {
  bool ready = fromSD ? sdReady : littleFSReady;

  if (!ready) {
    Serial.println(
      fromSD ? "SD card unavailable." : "LittleFS unavailable."
    );
    return;
  }

  bool exists = fromSD
    ? SD.exists(path)
    : LittleFS.exists(path);

  if (!exists) {
    Serial.println("CSV file does not exist.");
    return;
  }

  File f = fromSD
    ? SD.open(path, FILE_READ)
    : LittleFS.open(path, FILE_READ);

  if (!f) {
    Serial.println("Cannot open CSV.");
    return;
  }

  unsigned long lines = 0;

  while (f.available()) {
    String line = f.readStringUntil('\n');

    if (line.length()) lines++;
  }

  f.close();

  Serial.print("File: ");
  Serial.println(path);

  Serial.print("Total lines including header: ");
  Serial.println(lines);

  Serial.print("Records: ");
  Serial.println(lines > 0 ? lines - 1 : 0);

  Serial.print("Last record ID: ");
  Serial.println(findLastIdFromFile(fromSD, path));
}

void showCsvStatus() {
  Serial.println("\n--- LITTLEFS CSV ---");
  showOneCsvStatus(false, LITTLEFS_CSV);

  Serial.println("\n--- SD CARD CSV ---");
  showOneCsvStatus(true, SD_CSV);

  Serial.print("\nNext shared ID: ");
  Serial.println(nextRecordId);
}

// =====================================================
// MULTIPLEXER: SCAN ALL 16 CHANNELS
// =====================================================

void readMultiplexer() {
  for (int channel = 0; channel < CHANNEL_COUNT; channel++) {
    digitalWrite(MUX_S0, (channel & 0x01) ? HIGH : LOW);
    digitalWrite(MUX_S1, (channel & 0x02) ? HIGH : LOW);
    digitalWrite(MUX_S2, (channel & 0x04) ? HIGH : LOW);
    digitalWrite(MUX_S3, (channel & 0x08) ? HIGH : LOW);

    delay(MUX_SETTLE_DELAY_MS);

    // Discard the first reading after changing channels.
    (void)analogRead(MUX_SIGNAL_PIN);
    delayMicroseconds(200);

    uint32_t totalRaw = 0;

    for (int sample = 0; sample < SAMPLES_PER_CHANNEL; sample++) {
      totalRaw += (uint32_t)analogRead(MUX_SIGNAL_PIN);
      delayMicroseconds(200);
    }

    int rawAverage =
      (int)(totalRaw / SAMPLES_PER_CHANNEL);

    channelMv[channel] =
      (int)(
        (
          (uint32_t)rawAverage * 3300UL +
          (ADC_MAXIMUM / 2)
        ) / ADC_MAXIMUM
      );
  }

  adcvalue1 = channelMv[0];
  adcvalue2 = channelMv[1];
  adcvalue3 = channelMv[2];

  Serial.println("16-channel scan (approximate mV):");

  for (int i = 0; i < CHANNEL_COUNT; i++) {
    Serial.print("CH");
    if (i + 1 < 10) Serial.print("0");
    Serial.print(i + 1);
    Serial.print(" = ");
    Serial.print(channelMv[i]);
    Serial.println(" mV");
  }
}

// =====================================================
// HELP
// =====================================================

void showHelp() {
  Serial.println();
  Serial.println("=========== COMMANDS ===========");
  Serial.println("HELP            - Show commands");
  Serial.println("SHOW NETWORKS   - List saved Wi-Fi");
  Serial.println("READ FILE       - Read networks.txt");
  Serial.println("SET WIFI        - Add Wi-Fi");
  Serial.println("EDIT WIFI       - Edit Wi-Fi");
  Serial.println("DELETE WIFI     - Delete Wi-Fi");
  Serial.println("CLEAR WIFI      - Delete all Wi-Fi");
  Serial.println("SHOW WIFI       - Show connection");
  Serial.println("SHOW CSV        - Show both CSV files");
  Serial.println("SHOW SD         - Show SD card CSV");
  Serial.println("UPLOAD LITTLEFS - Upload LittleFS history");
  Serial.println("UPLOAD SDCARD   - Upload SD card history");
  Serial.println("SYNC LITTLEFS   - Upload LittleFS history");
  Serial.println("SYNC SDCARD     - Upload SD card history");
  Serial.println("STOP UPLOAD     - Stop historical upload");
  Serial.println("STATUS          - Device status");
  Serial.println("RESTART         - Restart ESP32");
  Serial.println("================================");
}

// =====================================================
// DEVICE STATUS
// =====================================================

void showStatus() {
  Serial.println();
  Serial.println("========== DEVICE STATUS ==========");

  Serial.print("Device ID: ");
  Serial.println(DEVICE_ID);

  Serial.print("LittleFS: ");
  Serial.println(littleFSReady ? "READY" : "NOT AVAILABLE");

  Serial.print("SD card: ");
  Serial.println(sdReady ? "READY" : "NOT AVAILABLE");

  Serial.print("Upload in progress: ");
  Serial.println(uploadInProgress ? "YES" : "NO");

  Serial.print("Timezone: ");
  Serial.println(
    installationTimezone.length()
      ? installationTimezone
      : "(not detected)"
  );

  loadWifiRecords();

  Serial.print("Saved Wi-Fi networks: ");
  Serial.println(wifiRecords.size());

  showWifi();
  showCsvStatus();

  printCurrentClock("CURRENT DEVICE CLOCK");

  Serial.println("==================================");
}

// =====================================================
// SERIAL COMMAND PROCESSING
// =====================================================

void executeSerialCommand(String command) {
  command.trim();
  command.toUpperCase();

  while (command.indexOf("  ") >= 0) {
    command.replace("  ", " ");
  }

  if (!command.length()) return;

  Serial.print("Command received: [");
  Serial.print(command);
  Serial.println("]");

  if (command == "HELP") {
    showHelp();

  } else if (command == "SHOW NETWORKS" ||
             command == "SHOW NETWORK") {
    showNetworks();

  } else if (command == "READ FILE") {
    readNetworkFile();

  } else if (command == "SET WIFI") {
    setWifiRecord();

  } else if (command == "EDIT WIFI") {
    editWifiRecord();

  } else if (command == "DELETE WIFI") {
    deleteWifiRecord();

  } else if (command == "CLEAR WIFI") {
    clearWifiRecords();

  } else if (command == "SHOW WIFI") {
    showWifi();

  } else if (command == "SHOW CSV") {
    showCsvStatus();

  } else if (command == "SHOW SD") {
    showOneCsvStatus(true, SD_CSV);

  } else if (command == "UPLOAD LITTLEFS" ||
             command == "SYNC LITTLEFS") {
    uploadHistoricalCsv(false);
    previousSendTime = millis();

  } else if (command == "UPLOAD SDCARD" ||
             command == "UPLOAD SD CARD" ||
             command == "SYNC SDCARD" ||
             command == "SYNC SD CARD") {
    uploadHistoricalCsv(true);
    previousSendTime = millis();

  } else if (command == "STOP UPLOAD") {
    if (uploadInProgress) {
      stopUploadRequested = true;
      Serial.println("Stop requested.");
    } else {
      Serial.println("No historical upload is running.");
    }

  } else if (command == "STATUS") {
    showStatus();

  } else if (command == "RESTART") {
    Serial.println("Restarting ESP32...");
    delay(500);
    ESP.restart();

  } else {
    Serial.println("Unknown command. Type HELP.");
  }
}

void handleSerialCommands() {
  static unsigned long lastCharacterTime = 0;

  // Do not let the normal command reader consume input
  // while historical upload is reading STOP UPLOAD.
  if (uploadInProgress) return;

  while (Serial.available()) {
    char c = (char)Serial.read();

    if (c == '\r' || c == '\n') {
      if (serialCommandBuffer.length() > 0) {
        String command = serialCommandBuffer;
        serialCommandBuffer = "";
        executeSerialCommand(command);
      }
    } else if (serialCommandBuffer.length() < 100) {
      serialCommandBuffer += c;
      lastCharacterTime = millis();
    }
  }

  // Also support Serial Monitor "No line ending".
  if (serialCommandBuffer.length() > 0 &&
      millis() - lastCharacterTime >= 300UL) {
    String command = serialCommandBuffer;
    serialCommandBuffer = "";
    executeSerialCommand(command);
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println("ESP32 16-CHANNEL DATA LOGGER");
  Serial.println("Device ID: ESP0001");
  Serial.println("LittleFS: 9-column CSV");
  Serial.println("SD card: 25-column CSV");
  Serial.println("Live endpoint: save3_csv.php");
  Serial.println("LittleFS sync: sync_littlefs.php");
  Serial.println("SD sync: sync_sdcard.php");
  Serial.println("System clock: UTC");
  Serial.println("========================================");

  // Initialize multiplexer.
  pinMode(MUX_S0, OUTPUT);
  pinMode(MUX_S1, OUTPUT);
  pinMode(MUX_S2, OUTPUT);
  pinMode(MUX_S3, OUTPUT);
  pinMode(MUX_SIGNAL_PIN, INPUT);

  digitalWrite(MUX_S0, LOW);
  digitalWrite(MUX_S1, LOW);
  digitalWrite(MUX_S2, LOW);
  digitalWrite(MUX_S3, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(MUX_SIGNAL_PIN, ADC_11db);

  Serial.println("Multiplexer initialized.");

  // Mount LittleFS without formatting on failure.
  littleFSReady = LittleFS.begin(false);

  if (littleFSReady) {
    Serial.println("LittleFS mounted.");

    if (!LittleFS.exists(NETWORK_FILE)) {
      File f = LittleFS.open(NETWORK_FILE, FILE_WRITE);
      if (f) f.close();
    }
  } else {
    Serial.println("ERROR: LittleFS mount failed.");
  }

  // Initialize SD card.
  // SPI.begin() uses the ESP32 board's default SPI pins.
  // Set SD_CS_PIN to match your wiring.
  SPI.begin();

  sdReady = SD.begin(SD_CS_PIN);

  if (sdReady) {
    Serial.println("SD card mounted.");
    Serial.print("SD card size (MB): ");
    Serial.println(SD.cardSize() / (1024ULL * 1024ULL));
  } else {
    Serial.println("WARNING: SD card mount failed.");
    Serial.println("Check card format, wiring and CS pin.");
  }

  // Connect to saved Wi-Fi.
  if (littleFSReady) {
    loadWifiRecords();
    connectToSavedNetworks();
  }

  // Initialize the two CSV files independently.
  initializeStorage();

  Serial.println();
  Serial.println("Type HELP in Serial Monitor.");
  Serial.println("Baud rate: 115200");

  Serial.print("Live endpoint: ");
  Serial.println(LIVE_URL);

  Serial.print("LittleFS sync endpoint: ");
  Serial.println(LITTLEFS_SYNC_URL);

  Serial.print("SD sync endpoint: ");
  Serial.println(SD_SYNC_URL);

  if (WiFi.status() == WL_CONNECTED) {
    detectTimezone();
    synchronizeTime();
    printCurrentClock("STARTUP CLOCK");
  } else {
    Serial.println("Wi-Fi unavailable. Time sync will retry.");
  }

  previousTimezoneCheck = millis();
  previousSendTime = millis();
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {
  // 1. Process serial commands.
  handleSerialCommands();

  // Do not scan new records during historical upload.
  if (uploadInProgress) return;

  // 2. Reconnect Wi-Fi if required.
  bool wifiConnected = WiFi.status() == WL_CONNECTED;

  if (!wifiConnected &&
      millis() - previousReconnectAttempt >= 15000UL) {
    previousReconnectAttempt = millis();

    Serial.println("Wi-Fi disconnected. Reconnecting...");

    connectToSavedNetworks();

    wifiConnected = WiFi.status() == WL_CONNECTED;
  }

  unsigned long nowMillis = millis();

  // 3. Detect/refresh timezone.
  unsigned long refreshInterval =
    installationTimezone.length()
      ? TIMEZONE_REFRESH_INTERVAL
      : TIMEZONE_RETRY_INTERVAL;

  if (wifiConnected &&
      nowMillis - previousTimezoneCheck >= refreshInterval) {
    previousTimezoneCheck = nowMillis;

    if (!detectTimezone()) {
      Serial.println("Timezone refresh failed.");
    }
  }

  // 4. Normal recording interval.
  if (nowMillis - previousSendTime < SEND_INTERVAL) {
    return;
  }

  previousSendTime = nowMillis;

  // 5. Require a detected installation timezone.
  if (!installationTimezone.length()) {
    Serial.println("Timezone not detected. Reading skipped.");
    return;
  }

  // 6. Get UTC timestamp.
  time_t utcTimestamp = time(nullptr);

  if (utcTimestamp <= 1700000000) {
    Serial.println("UTC clock invalid. Trying NTP.");

    if (!wifiConnected || !synchronizeTime()) {
      Serial.println("Reading skipped.");
      return;
    }

    utcTimestamp = time(nullptr);

    if (utcTimestamp <= 1700000000) {
      Serial.println("UTC clock still invalid.");
      return;
    }
  }

  // 7. Scan all 16 channels.
  readMultiplexer();

  printCurrentClock("CURRENT RECORDING TIME");

  // 8. Build the record's time fields.
  char dateBuf[12];
  char timeBuf[10];
  char stampBuf[24];

  if (!createTimeFields(
        utcTimestamp,
        dateBuf,
        sizeof(dateBuf),
        timeBuf,
        sizeof(timeBuf),
        stampBuf,
        sizeof(stampBuf)
      )) {
    Serial.println("Could not create date/time fields.");
    return;
  }

  // 9. Save the same record ID to both local systems.
  unsigned long recordId = nextRecordId;

  Serial.println();
  Serial.println("----------------------------------");
  Serial.print("Saving record ID: ");
  Serial.println(recordId);

  Serial.print("Local Date: ");
  Serial.println(dateBuf);

  Serial.print("Local Time: ");
  Serial.println(timeBuf);

  Serial.print("UTC TimeStamp: ");
  Serial.println(stampBuf);

  Serial.print("Timezone: ");
  Serial.println(installationTimezone);

  bool littleSaved = false;
  bool sdSaved = false;

  if (littleFSReady) {
    littleSaved = appendLittleFsRecord(
      recordId,
      channelMv,
      dateBuf,
      timeBuf,
      stampBuf,
      installationTimezone
    );
  }

  if (sdReady) {
    sdSaved = appendSdRecord(
      recordId,
      channelMv,
      dateBuf,
      timeBuf,
      stampBuf,
      installationTimezone
    );
  }

  Serial.print("LittleFS save: ");
  Serial.println(littleSaved ? "SUCCESS" : "FAILED");

  Serial.print("SD card save: ");
  Serial.println(sdSaved ? "SUCCESS" : "FAILED");

  if (littleSaved || sdSaved) {
    nextRecordId++;
  } else {
    Serial.println(
      "WARNING: Record could not be saved to either storage."
    );
  }

  // 10. Send live CH1-CH3 readings to Render.
  // Historical upload is separate and does not happen here.
  if (wifiConnected) {
    postLiveRecord(utcTimestamp, installationTimezone);
  } else {
    Serial.println("Offline. Local records retained.");
  }

  Serial.println("----------------------------------");
}
