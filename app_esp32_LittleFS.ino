
/*
  ESP32 16-channel Data Logger
  CD74HC4067 + LittleFS + Render / TiDB Cloud

  Device ID: ESP0001

  Live endpoint:
  https://datalogger4-remote.onrender.com/save3_csv.php

  LittleFS sync endpoint:
  https://datalogger4-remote.onrender.com/sync_littleFS.php

  LittleFS CSV format - EXACTLY 9 columns:
  id,value1,value2,value3,Date,Time,TimeStamp,timezone,device_id

  All 16 channels scanned.
  CH1, CH2 and CH3 sent to the existing PHP endpoint.

  TIME DESIGN:
  - ESP32 system clock is UTC.
  - Local Date and Time are generated separately.
  - India timezone = Asia/Kolkata.
  - Unix timestamp always represents UTC.
  - LittleFS upload never changes the system clock.
*/

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <math.h>
#include <LittleFS.h>
#include <vector>
#include <stdlib.h>

// =====================================================
// SERVER AND FILE SETTINGS
// =====================================================

const char *NETWORK_FILE = "/networks.txt";
const char *LOCAL_CSV_FILE = "/sensor_data.csv";

const char *SYNC_URL =
  "https://datalogger5-remote.onrender.com/sync_littlefs.php";

const char *serverURL =
  "https://datalogger5-remote.onrender.com/save3_csv.php";

const char *geoURL =
  "http://ip-api.com/json/?fields=status,timezone";

const char *deviceID = "ESP0001";
const char *apiToken = "EspSwitch52026";

const char *CSV_HEADER =
  "id,value1,value2,value3,Date,Time,TimeStamp,timezone,device_id";

// =====================================================
// ESP32 PIN ASSIGNMENTS
// =====================================================

const int MUX_S0 = 25;
const int MUX_S1 = 26;
const int MUX_S2 = 27;
const int MUX_S3 = 32;
const int MUX_SIGNAL_PIN = 34;

const int MUX_CHANNEL_COUNT = 16;

const unsigned long MUX_SETTLE_DELAY_MS = 2;

const float ADC_INPUT_FULL_SCALE_MV = 3300.0f;

int channelMv[MUX_CHANNEL_COUNT] = {0};

int adcvalue1 = 0;
int adcvalue2 = 0;
int adcvalue3 = 0;

// =====================================================
// GLOBAL VARIABLES
// =====================================================

bool littleFSReady = false;

unsigned long localCsvNextId = 1;

struct WifiRecord {
  String ssid;
  String password;
  String lastIP;
};

std::vector<WifiRecord> wifiRecords;

String serialCommandBuffer = "";

String installationTimezone = "";

String appliedPosixTimezone = "";

const unsigned long sendInterval = 5000UL;

const unsigned long timezoneRefreshInterval =
  6UL * 60UL * 60UL * 1000UL;

const unsigned long timezoneRetryInterval =
  5UL * 60UL * 1000UL;

unsigned long previousSendTime = 0;
unsigned long previousTimezoneCheck = 0;

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
// LITTLEFS WIFI CREDENTIALS
// =====================================================

bool loadWifiRecords() {

  wifiRecords.clear();

  if (!littleFSReady ||
      !LittleFS.exists(NETWORK_FILE)) {
    return true;
  }

  File file = LittleFS.open(NETWORK_FILE, FILE_READ);

  if (!file) return false;

  while (file.available()) {

    String ssid = file.readStringUntil('\n');
    ssid.trim();

    if (!ssid.length()) continue;

    String password = file.readStringUntil('\n');
    password.trim();

    String ip = file.readStringUntil('\n');
    ip.trim();

    WifiRecord r;

    r.ssid = ssid;
    r.password = password;
    r.lastIP = ip;

    wifiRecords.push_back(r);
  }

  file.close();

  return true;
}

bool saveWifiRecords() {

  if (!littleFSReady) {
    Serial.println("ERROR: LittleFS unavailable.");
    return false;
  }

  File file = LittleFS.open(NETWORK_FILE, FILE_WRITE);

  if (!file) {
    Serial.println("Cannot write networks.txt.");
    return false;
  }

  for (size_t i = 0; i < wifiRecords.size(); i++) {

    file.println(wifiRecords[i].ssid);
    file.println(wifiRecords[i].password);
    file.println(wifiRecords[i].lastIP);
  }

  file.close();

  return true;
}

// =====================================================
// WIFI CONNECTION
// =====================================================

bool connectToSavedNetworks() {

  if (!loadWifiRecords()) {
    Serial.println("Could not read networks.txt.");
    return false;
  }

  if (wifiRecords.empty()) {
    Serial.println("No saved Wi-Fi. Use SET WIFI.");
    return false;
  }

  WiFi.mode(WIFI_STA);

  for (size_t i = 0; i < wifiRecords.size(); i++) {

    Serial.println();
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

      wifiRecords[i].lastIP =
        WiFi.localIP().toString();

      saveWifiRecords();

      return true;
    }
  }

  Serial.println("All saved Wi-Fi networks failed.");

  return false;
}

// =====================================================
// SERIAL INPUT
// =====================================================

String readSerialLine(const char *promptText) {

  Serial.println(promptText);

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

// =====================================================
// WIFI COMMANDS
// =====================================================

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

  String password =
    readSerialLine("Enter Wi-Fi password:");

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

  if (!saveWifiRecords()) return;

  Serial.println("Wi-Fi saved. Connecting...");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  delay(200);

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

        wifiRecords[i].lastIP =
          WiFi.localIP().toString();

        break;
      }
    }

    saveWifiRecords();

  } else {

    Serial.println("Connection failed.");
  }
}

int askRecordNumber(const char *promptText) {

  String answer = readSerialLine(promptText);

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

  int index =
    askRecordNumber("Enter record number to edit:");

  if (index < 0) return;

  String ssid = readSerialLine("Enter new SSID:");

  if (!ssid.length()) return;

  String password =
    readSerialLine("Enter new password:");

  if (!password.length()) return;

  wifiRecords[index].ssid = ssid;
  wifiRecords[index].password = password;
  wifiRecords[index].lastIP = "";

  if (saveWifiRecords()) {
    Serial.println("Wi-Fi record updated.");
  }
}

void deleteWifiRecord() {

  loadWifiRecords();

  showNetworks();

  if (wifiRecords.empty()) return;

  int index =
    askRecordNumber("Enter record number to delete:");

  if (index < 0) return;

  wifiRecords.erase(wifiRecords.begin() + index);

  if (saveWifiRecords()) {
    Serial.println("Record deleted.");
  }
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

  Serial.println();

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

// Apply timezone only when it changes.
// This affects local-time conversion, NOT the UTC system clock.

bool applyInstallationTimezone() {

  String posix = posixTimezoneForIana(
    installationTimezone
  );

  if (!posix.length()) {

    Serial.println("Unsupported timezone.");
    return false;
  }

  // Always reapply timezone.
  // Do not rely on the previously cached setting.

  setenv("TZ", posix.c_str(), 1);
  tzset();

  appliedPosixTimezone = posix;

  Serial.print("Installation timezone applied: ");
  Serial.println(installationTimezone);

  return true;
}

// =====================================================
// TIMEZONE DETECTION
// =====================================================

bool detectTimezone() {

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  Serial.println("\nChecking installation timezone...");

  WiFiClient client;

  HTTPClient http;

  if (!http.begin(client, geoURL)) {
    return false;
  }

  http.setTimeout(10000);

  int code = http.GET();

  if (code != HTTP_CODE_OK) {

    Serial.print("Geolocation HTTP error: ");
    Serial.println(code);

    http.end();

    return false;
  }

  String response = http.getString();

  http.end();

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

  String detected =
    response.substring(start, finish);

  detected.trim();

  if (!detected.length() ||
      detected.indexOf('/') < 0) {

    return false;
  }

  installationTimezone = detected;

  Serial.print("Detected timezone: ");
  Serial.println(installationTimezone);

  return applyInstallationTimezone();
}

// =====================================================
// NTP TIME SYNCHRONIZATION
// =====================================================

bool synchronizeTime() {

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("Wi-Fi required for NTP.");

    return false;
  }

  Serial.println("\nSynchronizing UTC using NTP...");

  // Keep the ESP32 system clock in UTC.
  configTime(
    0,
    0,
    "pool.ntp.org",
    "time.nist.gov"
  );

  for (int i = 0; i < 30; i++) {

    time_t nowUtc = time(nullptr);

    if (nowUtc > 1700000000) {

      Serial.println("\nNTP synchronization successful.");

      Serial.print("UTC Unix timestamp: ");
      Serial.println((unsigned long)nowUtc);

      // IMPORTANT:
      // configTime() may reset the TZ environment.
      // Force the installation timezone to be applied again.

      appliedPosixTimezone = "";

      if (installationTimezone.length() > 0) {

        if (!applyInstallationTimezone()) {

          Serial.println(
            "ERROR: Could not reapply installation timezone."
          );

          return false;
        }
      }

      Serial.println(
        "Installation timezone reapplied after NTP."
      );

      printCurrentClock("CLOCK AFTER NTP SYNCHRONIZATION");

      return true;
    }

    Serial.print(".");

    delay(1000);
  }

  Serial.println("\nNTP synchronization failed.");

  return false;
}

// =====================================================
// PRINT UTC AND LOCAL TIME
// =====================================================

void printCurrentClock(const char *label) {

  time_t nowUtc = time(nullptr);

  Serial.println();

  Serial.println("--------------------------------");

  Serial.println(label);

  if (nowUtc <= 1700000000) {

    Serial.println("Clock is not synchronized.");

    Serial.println("--------------------------------");

    return;
  }

  struct tm utcParts;

  if (gmtime_r(&nowUtc, &utcParts)) {

    char utcBuffer[25];

    snprintf(
      utcBuffer,
      sizeof(utcBuffer),
      "%04d-%02d-%02d %02d:%02d:%02d",
      utcParts.tm_year + 1900,
      utcParts.tm_mon + 1,
      utcParts.tm_mday,
      utcParts.tm_hour,
      utcParts.tm_min,
      utcParts.tm_sec
    );

    Serial.print("UTC time: ");
    Serial.println(utcBuffer);
  }

  struct tm localParts;

  if (installationTimezone.length() &&
      localtime_r(&nowUtc, &localParts)) {

    char localBuffer[25];

    snprintf(
      localBuffer,
      sizeof(localBuffer),
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

    Serial.println(localBuffer);

  } else {

    Serial.println("Local timezone not available.");
  }

  Serial.print("UTC Unix timestamp: ");
  Serial.println((unsigned long)nowUtc);

  Serial.println("--------------------------------");
}

// =====================================================
// LITTLEFS CSV ID
// =====================================================

unsigned long findLastLocalCsvId() {

  if (!littleFSReady ||
      !LittleFS.exists(LOCAL_CSV_FILE)) {

    return 0;
  }

  File f = LittleFS.open(
    LOCAL_CSV_FILE,
    FILE_READ
  );

  if (!f) return 0;

  String line;

  unsigned long lastId = 0;

  if (f.available()) {
    line = f.readStringUntil('\n');
  }

  while (f.available()) {

    line = f.readStringUntil('\n');

    line.trim();

    if (!line.length()) continue;

    int comma = line.indexOf(',');

    if (comma > 0) {

      unsigned long id =
        (unsigned long)line.substring(0, comma).toInt();

      if (id > lastId) {
        lastId = id;
      }
    }
  }

  f.close();

  return lastId;
}

// =====================================================
// BACKUP OLD LITTLEFS FILE
// =====================================================

bool backupAndRemoveFile(
  const char *sourcePath,
  const char *backupPath
) {

  if (!LittleFS.exists(sourcePath) ||
      LittleFS.exists(backupPath)) {

    return false;
  }

  File source = LittleFS.open(
    sourcePath,
    FILE_READ
  );

  if (!source) return false;

  const size_t originalSize = source.size();

  File backup = LittleFS.open(
    backupPath,
    FILE_WRITE
  );

  if (!backup) {

    source.close();

    return false;
  }

  uint8_t buffer[128];

  bool copyOK = true;

  while (source.available()) {

    int bytesRead =
      source.read(buffer, sizeof(buffer));

    if (bytesRead <= 0) break;

    size_t bytesWritten =
      backup.write(buffer, (size_t)bytesRead);

    if (bytesWritten != (size_t)bytesRead) {

      copyOK = false;

      break;
    }
  }

  backup.flush();

  backup.close();

  source.close();

  File verify = LittleFS.open(
    backupPath,
    FILE_READ
  );

  if (!verify) {

    copyOK = false;

  } else {

    if (verify.size() != originalSize) {
      copyOK = false;
    }

    verify.close();
  }

  if (!copyOK) {

    LittleFS.remove(backupPath);

    Serial.println("Backup verification failed.");

    return false;
  }

  if (!LittleFS.remove(sourcePath)) {

    Serial.println("Original CSV could not be removed.");

    return false;
  }

  return true;
}

// =====================================================
// INITIALIZE CSV
// =====================================================

bool initializeLocalCsv() {

  if (!littleFSReady) {

    Serial.println("LittleFS unavailable.");

    return false;
  }

  if (!LittleFS.exists(LOCAL_CSV_FILE)) {

    File f = LittleFS.open(
      LOCAL_CSV_FILE,
      FILE_WRITE
    );

    if (!f) return false;

    f.println(CSV_HEADER);

    f.close();

    Serial.println("Created 9-column CSV.");
  }

  else {

    File f = LittleFS.open(
      LOCAL_CSV_FILE,
      FILE_READ
    );

    if (!f) return false;

    String header = f.readStringUntil('\n');

    header.trim();

    f.close();

    if (header != CSV_HEADER) {

      Serial.println("Old CSV header detected.");

      String backupPath = "/sensor_data_backup.csv";

      unsigned int backupNumber = 1;

      while (LittleFS.exists(backupPath.c_str()) &&
             backupNumber < 1000) {

        backupPath =
          "/sensor_data_backup_" +
          String(backupNumber) +
          ".csv";

        backupNumber++;
      }

      if (LittleFS.exists(backupPath.c_str())) {
        return false;
      }

      if (!backupAndRemoveFile(
            LOCAL_CSV_FILE,
            backupPath.c_str()
          )) {

        Serial.println("CSV backup failed.");

        return false;
      }

      Serial.print("Old CSV backed up: ");
      Serial.println(backupPath);

      File newFile = LittleFS.open(
        LOCAL_CSV_FILE,
        FILE_WRITE
      );

      if (!newFile) return false;

      newFile.println(CSV_HEADER);

      newFile.flush();

      newFile.close();
    }
  }

  localCsvNextId = findLastLocalCsvId() + 1;

  Serial.print("LittleFS CSV ready. Next ID: ");

  Serial.println(localCsvNextId);

  return true;
}

// =====================================================
// APPEND LITTLEFS RECORD
// =====================================================

bool appendLocalCsvRecord(
  const int values[16],
  time_t utcTimestamp,
  const String &timezone,
  const String &devId
) {

  if (!littleFSReady) return false;

  if (!initializeLocalCsv()) return false;

  if (utcTimestamp <= 1700000000) {

    Serial.println("Invalid UTC timestamp.");

    return false;
  }

  // Ensure the requested installation timezone is active.
  installationTimezone = timezone;

  // Force timezone application before localtime_r().
  appliedPosixTimezone = "";

  if (!applyInstallationTimezone()) {

    Serial.println("Timezone application failed.");

    return false;
  }

  // Generate UTC timestamp without changing it.
  struct tm utcTm;

  if (!gmtime_r(&utcTimestamp, &utcTm)) {
    return false;
  }

  char stamp[24];

  snprintf(
    stamp,
    sizeof(stamp),
    "%04d-%02d-%02d %02d:%02d:%02d",
    utcTm.tm_year + 1900,
    utcTm.tm_mon + 1,
    utcTm.tm_mday,
    utcTm.tm_hour,
    utcTm.tm_min,
    utcTm.tm_sec
  );

  // Convert UTC timestamp into installation-local time.
  struct tm localTm;

  if (!localtime_r(&utcTimestamp, &localTm)) {
    return false;
  }

  char dateBuf[12];
  char timeBuf[10];

  snprintf(
    dateBuf,
    sizeof(dateBuf),
    "%04d-%02d-%02d",
    localTm.tm_year + 1900,
    localTm.tm_mon + 1,
    localTm.tm_mday
  );

  snprintf(
    timeBuf,
    sizeof(timeBuf),
    "%02d:%02d:%02d",
    localTm.tm_hour,
    localTm.tm_min,
    localTm.tm_sec
  );

  File f = LittleFS.open(
    LOCAL_CSV_FILE,
    FILE_APPEND
  );

  if (!f) {

    Serial.println("Cannot append CSV.");

    return false;
  }

  // Exactly 9 columns.
  String row =
    String(localCsvNextId) + "," +
    String(values[0]) + "," +
    String(values[1]) + "," +
    String(values[2]) + "," +
    String(dateBuf) + "," +
    String(timeBuf) + "," +
    String(stamp) + "," +
    timezone + "," +
    devId;

  size_t written = f.println(row);

  f.flush();
  f.close();

  if (written == 0) return false;

  Serial.println("\nLittleFS record saved:");

  Serial.print("ID: ");
  Serial.println(localCsvNextId);

  Serial.print("Local Date: ");
  Serial.println(dateBuf);

  Serial.print("Local Time: ");
  Serial.println(timeBuf);

  Serial.print("UTC TimeStamp: ");
  Serial.println(stamp);

  Serial.print("Timezone: ");
  Serial.println(timezone);

  localCsvNextId++;

  return true;
}

// =====================================================
// SHOW CSV STATUS
// =====================================================

void showLocalCsvStatus() {

  if (!littleFSReady ||
      !LittleFS.exists(LOCAL_CSV_FILE)) {

    Serial.println("CSV unavailable.");

    return;
  }

  File f = LittleFS.open(
    LOCAL_CSV_FILE,
    FILE_READ
  );

  if (!f) return;

  unsigned long lines = 0;

  while (f.available()) {

    String line = f.readStringUntil('\n');

    if (line.length()) lines++;
  }

  f.close();

  Serial.print("CSV file: ");
  Serial.println(LOCAL_CSV_FILE);

  Serial.print("Total lines: ");
  Serial.println(lines);

  Serial.print("Records: ");
  Serial.println(lines > 0 ? lines - 1 : 0);

  Serial.print("Next ID: ");
  Serial.println(localCsvNextId);
}

// =====================================================
// POST ONE LITTLEFS RECORD
// =====================================================

bool postLittleFsRecord(const String &line) {

  String fields[9];

  int fieldCount = 0;

  int start = 0;

  for (int i = 0; i <= (int)line.length(); i++) {

    if (i == (int)line.length() ||
        line[i] == ',') {

      if (fieldCount >= 9) return false;

      fields[fieldCount++] =
        line.substring(start, i);

      start = i + 1;
    }
  }

  if (fieldCount != 9) {

    Serial.println("Malformed CSV record.");

    return false;
  }

  // ORIGINAL CSV VALUES ARE SENT UNCHANGED.
  // No date/time conversion takes place during upload.

  String form =
    "api_token=" + urlEncode(String(apiToken)) +
    "&csv_id=" + urlEncode(fields[0]) +
    "&value1=" + urlEncode(fields[1]) +
    "&value2=" + urlEncode(fields[2]) +
    "&value3=" + urlEncode(fields[3]) +
    "&Date=" + urlEncode(fields[4]) +
    "&Time=" + urlEncode(fields[5]) +
    "&TimeStamp=" + urlEncode(fields[6]) +
    "&timezone=" + urlEncode(fields[7]) +
    "&device_id=" + urlEncode(fields[8]);

  WiFiClientSecure client;

  client.setInsecure();

  HTTPClient http;

  if (!http.begin(client, SYNC_URL)) {

    Serial.println("HTTPS connection initialization failed.");

    return false;
  }

  // Reduced from 30 seconds to 10 seconds.

  http.setTimeout(10000);

  http.addHeader(
    "Content-Type",
    "application/x-www-form-urlencoded"
  );

  int code = http.POST(form);

  String response = http.getString();

  http.end();

  Serial.print("LittleFS HTTP code: ");

  Serial.println(code);

  Serial.println("Server response:");

  Serial.println(response);

  // The PHP endpoint should return HTTP 200
  // only when insertion or duplicate handling succeeds.
  // Check the JSON response to confirm database insertion.

  return code == HTTP_CODE_OK;
}

// =====================================================
// UPLOAD LITTLEFS CSV
// =====================================================


void uploadLittleFsCsv() {

  Serial.println();
  Serial.println("========================================");
  Serial.println("       UPLOAD LITTLEFS START");
  Serial.println("========================================");

  printCurrentClock("BEFORE LITTLEFS UPLOAD");

  if (!littleFSReady ||
      !LittleFS.exists(LOCAL_CSV_FILE)) {

    Serial.println("LittleFS CSV not available.");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("Wi-Fi disconnected.");
    return;
  }

  File f = LittleFS.open(LOCAL_CSV_FILE, FILE_READ);

  if (!f) {

    Serial.println("Cannot open CSV.");
    return;
  }

  // Allow sufficient time for reading each line.
  f.setTimeout(10000);

  String header = f.readStringUntil('\n');
  header.trim();

  if (header != CSV_HEADER) {

    Serial.println("CSV header mismatch. Upload cancelled.");
    f.close();
    return;
  }

  unsigned long attempted = 0;
  unsigned long succeeded = 0;
  unsigned long failed = 0;
  unsigned long malformed = 0;

  Serial.println("Uploading original saved records...");
  Serial.println("Historical Date/Time values will not be changed.");

  while (f.available()) {

    String line = f.readStringUntil('\n');
    line.trim();

    if (line.length() == 0) {
      continue;
    }

    // Count commas: nine columns require eight commas.
    unsigned int commaCount = 0;

    for (unsigned int i = 0; i < line.length(); i++) {

      if (line.charAt(i) == ',') {
        commaCount++;
      }
    }

    if (commaCount != 8) {

      malformed++;

      Serial.println();
      Serial.println("Malformed CSV record. Skipping safely.");
      Serial.println("Incomplete row:");
      Serial.println(line);

      continue;
    }

    attempted++;

    Serial.println();
    Serial.print("Uploading record: ");
    Serial.println(attempted);

    Serial.println("Original CSV row:");
    Serial.println(line);

    // Upload original CSV row without changing its values.
    bool posted = postLittleFsRecord(line);

    if (posted) {
      succeeded++;
    } else {
      failed++;
    }

    delay(100);
    yield();
  }

  f.close();

  Serial.println();
  Serial.println("========== UPLOAD SUMMARY ==========");

  Serial.print("Attempted: ");
  Serial.println(attempted);

  Serial.print("HTTP 200: ");
  Serial.println(succeeded);

  Serial.print("Failed: ");
  Serial.println(failed);

  Serial.print("Malformed/skipped: ");
  Serial.println(malformed);

  Serial.println("Original LittleFS CSV has been retained.");

  printCurrentClock("AFTER LITTLEFS UPLOAD");

  Serial.println("====================================");
  Serial.println("Upload completed.");
}

// =====================================================
// HELP
// =====================================================

void showHelp() {

  Serial.println();

  Serial.println("=========== COMMANDS ===========");

  Serial.println("HELP            - Show commands");

  Serial.println("SHOW NETWORKS   - List Wi-Fi networks");

  Serial.println("READ FILE       - Read networks.txt");

  Serial.println("SET WIFI        - Add Wi-Fi");

  Serial.println("EDIT WIFI       - Edit Wi-Fi");

  Serial.println("DELETE WIFI     - Delete Wi-Fi");

  Serial.println("CLEAR WIFI      - Delete all Wi-Fi");

  Serial.println("SHOW WIFI       - Show connection");

  Serial.println("SHOW CSV        - Show CSV status");

  Serial.println("UPLOAD LITTLEFS - Upload CSV records");

  Serial.println("STATUS          - Device status");

  Serial.println("RESTART         - Restart ESP32");

  Serial.println("================================");
}

// =====================================================
// STATUS
// =====================================================

void showStatus() {

  Serial.println();

  Serial.println("========== DEVICE STATUS ==========");

  Serial.print("Device ID: ");
  Serial.println(deviceID);

  Serial.print("LittleFS: ");

  Serial.println(
    littleFSReady ? "READY" : "NOT AVAILABLE"
  );

  loadWifiRecords();

  Serial.print("Saved Wi-Fi records: ");

  Serial.println(wifiRecords.size());

  showLocalCsvStatus();

  showWifi();

  Serial.print("Installation timezone: ");

  Serial.println(
    installationTimezone.length()
      ? installationTimezone
      : "(not detected)"
  );

  printCurrentClock("CURRENT DEVICE CLOCK");

  Serial.println("==================================");
}

// =====================================================
// SERIAL COMMAND PROCESSING
// =====================================================

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

      if (command == "HELP") {

        showHelp();

      }

      else if (command == "SHOW NETWORKS" ||
               command == "SHOW NETWORK") {

        showNetworks();

      }

      else if (command == "READ FILE") {

        readNetworkFile();

      }

      else if (command == "SET WIFI") {

        setWifiRecord();

      }

      else if (command == "EDIT WIFI") {

        editWifiRecord();

      }

      else if (command == "DELETE WIFI") {

        deleteWifiRecord();

      }

      else if (command == "CLEAR WIFI") {

        clearWifiRecords();

      }

      else if (command == "SHOW WIFI") {

        showWifi();

      }

      else if (command == "SHOW CSV") {

        showLocalCsvStatus();

      }

      else if (command == "UPLOAD LITTLEFS" ||
               command == "SYNC LITTLEFS") {

        uploadLittleFsCsv();

        // Restart the 5-second recording interval
        // from the END of the upload command.
        // The clock itself is NOT reset.

        previousSendTime = millis();

        Serial.println(
          "Normal recording will resume after 5 seconds."
        );

        printCurrentClock(
          "CLOCK AFTER UPLOAD COMMAND"
        );
      }

      else if (command == "STATUS") {

        showStatus();

      }

      else if (command == "RESTART") {

        Serial.println("Restarting ESP32...");

        delay(500);

        ESP.restart();
      }

      else {

        Serial.println("Unknown command. Type HELP.");
      }

    }

    else if (serialCommandBuffer.length() < 100) {

      serialCommandBuffer += c;
    }
  }
}

// =====================================================
// READ ALL 16 MULTIPLEXER CHANNELS
// =====================================================

void readMultiplexer() {

  const int samplesPerChannel = 8;

  const int adcMaximum = 4095;

  for (int channel = 0;
       channel < MUX_CHANNEL_COUNT;
       channel++) {

    digitalWrite(
      MUX_S0,
      (channel & 0x01) ? HIGH : LOW
    );

    digitalWrite(
      MUX_S1,
      (channel & 0x02) ? HIGH : LOW
    );

    digitalWrite(
      MUX_S2,
      (channel & 0x04) ? HIGH : LOW
    );

    digitalWrite(
      MUX_S3,
      (channel & 0x08) ? HIGH : LOW
    );

    delay(MUX_SETTLE_DELAY_MS);

    (void)analogRead(MUX_SIGNAL_PIN);

    delayMicroseconds(200);

    uint32_t totalRaw = 0;

    for (int sample = 0;
         sample < samplesPerChannel;
         sample++) {

      totalRaw +=
        (uint32_t)analogRead(MUX_SIGNAL_PIN);

      delayMicroseconds(200);
    }

    int rawAverage =
      (int)(totalRaw / samplesPerChannel);

    channelMv[channel] =
      (int)(
        (
          (uint32_t)rawAverage *
          (uint32_t)ADC_INPUT_FULL_SCALE_MV +
          (adcMaximum / 2)
        ) / adcMaximum
      );
  }

  adcvalue1 = channelMv[0];

  adcvalue2 = channelMv[1];

  adcvalue3 = channelMv[2];

  Serial.println("16-channel scan (approximate mV):");

  for (int channel = 0;
       channel < MUX_CHANNEL_COUNT;
       channel++) {

    Serial.print("CH");

    if (channel + 1 < 10) {
      Serial.print('0');
    }

    Serial.print(channel + 1);

    Serial.print(" = ");

    Serial.print(channelMv[channel]);

    Serial.println(" mV");
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

  Serial.println("Fixed Device ID: ESP0001");

  Serial.println("LittleFS CSV: 9 columns");

  Serial.println("System clock: UTC");

  Serial.println("Local time: Automatic timezone");

  Serial.println("========================================");

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

  analogSetPinAttenuation(
    MUX_SIGNAL_PIN,
    ADC_11db
  );

  Serial.println("Multiplexer initialized.");

  // Mount LittleFS.
  // Formatting on failure can erase existing data.

littleFSReady = LittleFS.begin(false);

  if (!littleFSReady) {

    Serial.println("LittleFS mount failed.");

  } else {

    Serial.println("LittleFS mounted.");

    if (!LittleFS.exists(NETWORK_FILE)) {

      File newFile = LittleFS.open(
        NETWORK_FILE,
        FILE_WRITE
      );

      if (newFile) newFile.close();
    }

    loadWifiRecords();

    connectToSavedNetworks();
  }

  if (littleFSReady) {

    initializeLocalCsv();
  }

  Serial.println();

  Serial.println("Type HELP in Serial Monitor.");

  Serial.println("Line ending: Newline");

  Serial.print("Live PHP endpoint: ");

  Serial.println(serverURL);

  if (WiFi.status() == WL_CONNECTED) {

    detectTimezone();

    synchronizeTime();

    printCurrentClock("STARTUP CLOCK");

  } else {

    Serial.println(
      "Wi-Fi unavailable. Time synchronization will retry."
    );
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

  // 2. Check Wi-Fi.

  bool wifiConnected =
    (WiFi.status() == WL_CONNECTED);

  if (!wifiConnected) {

    static unsigned long lastReconnectAttempt = 0;

    if (millis() - lastReconnectAttempt >= 15000UL) {

      lastReconnectAttempt = millis();

      Serial.println(
        "Wi-Fi disconnected. Reconnecting..."
      );

      connectToSavedNetworks();

      wifiConnected =
        (WiFi.status() == WL_CONNECTED);
    }
  }

  unsigned long nowMillis = millis();

  // 3. Retry timezone detection when required.

  unsigned long refreshInterval =
    installationTimezone.length() == 0
      ? timezoneRetryInterval
      : timezoneRefreshInterval;

  if (wifiConnected &&
      nowMillis - previousTimezoneCheck >= refreshInterval) {

    previousTimezoneCheck = nowMillis;

    if (!detectTimezone()) {

      Serial.println("Timezone refresh failed.");
    }
  }

  // 4. Maintain the normal 5-second scan interval.

  if (nowMillis - previousSendTime < sendInterval) {
    return;
  }

  previousSendTime = nowMillis;

  // 5. Require installation timezone.

  if (installationTimezone.length() == 0) {

    Serial.println(
      "Timezone not detected. Reading skipped."
    );

    return;
  }

  // 6. Read current UTC timestamp.

  time_t utcTimestamp = time(nullptr);

  if (utcTimestamp <= 1700000000) {

    Serial.println("UTC clock invalid. NTP retry.");

    if (!wifiConnected || !synchronizeTime()) {

      Serial.println("Reading skipped.");

      return;
    }

    utcTimestamp = time(nullptr);
  }

  // 7. Read all 16 channels.

  readMultiplexer();

  // 8. Display current clock.

  printCurrentClock("CURRENT RECORDING TIME");

  Serial.println();

  Serial.println("----------------------------------");

  Serial.println(
    "Saving 16-channel scan to LittleFS and Render"
  );

  Serial.print("Device ID: ");

  Serial.println(deviceID);

  Serial.print("UTC timestamp: ");

  Serial.println((unsigned long)utcTimestamp);

  Serial.print("Timezone: ");

  Serial.println(installationTimezone);

  // 9. Save to LittleFS.

  bool localSaved = appendLocalCsvRecord(
    channelMv,
    utcTimestamp,
    installationTimezone,
    String(deviceID)
  );

  if (!localSaved) {

    Serial.println(
      "ERROR: Record not saved to LittleFS."
    );
  }

  // 10. If Wi-Fi is disconnected, retain local record.

  if (!wifiConnected) {

    Serial.println(
      "Offline. LittleFS record retained."
    );

    return;
  }

  // 11. Send CH1-CH3 to existing Render endpoint.

  String postData =
    "device_id=" + urlEncode(String(deviceID)) +
    "&api_token=" + urlEncode(String(apiToken)) +
    "&value1=" + String(adcvalue1) +
    "&value2=" + String(adcvalue2) +
    "&value3=" + String(adcvalue3) +
    "&timestamp=" + String((unsigned long)utcTimestamp) +
    "&timezone=" + urlEncode(installationTimezone);

  WiFiClientSecure client;

  client.setInsecure();

  HTTPClient http;

  if (http.begin(client, serverURL)) {

    http.setTimeout(10000);

    http.addHeader(
      "Content-Type",
      "application/x-www-form-urlencoded"
    );

    int httpCode = http.POST(postData);

    Serial.print("HTTP response code: ");

    Serial.println(httpCode);

    if (httpCode > 0) {

      Serial.println("PHP endpoint response:");

      Serial.println(http.getString());

    } else {

      Serial.print("HTTP error: ");

      Serial.println(
        http.errorToString(httpCode)
      );

      Serial.println(
        "LittleFS copy retained."
      );
    }

    http.end();

  } else {

    Serial.println(
      "Could not initialize HTTPS connection."
    );

    Serial.println("LittleFS copy retained.");
  }

  Serial.println("----------------------------------");
}
