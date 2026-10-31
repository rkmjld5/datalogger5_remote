/*
  ESP32 16-channel data logger
  CD74HC4067 + LittleFS + SD card + XAMPP

  Live endpoint (unchanged):
  http://192.168.1.9/datalogger7_local_RSH/save3_csv.php

  SD sync endpoint:
  http://192.168.1.9/datalogger7_local_RSH/sync_sdcard.php

  Both local and SD CSV files use exactly 9 columns:
  id,value1,value2,value3,Date,Time,TimeStamp,timezone,device_id

  All 16 mux channels are read. CH1-CH3 are saved as value1-value3.
  Board/pin assumptions: ESP32 DevKit V1 / WROOM-32.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <math.h>
#include <LittleFS.h>
#include <SPI.h>
#include <SD.h>
#include <vector>

// -------------------- Server and file settings --------------------

const char *NETWORK_FILE = "/networks.txt";
const char *LOCAL_CSV_FILE = "/sensor_data.csv";

// Render endpoints MUST use HTTPS. Using HTTP caused HTTP 307 redirects.
const char *SYNC_URL =
  "https://datalogger4-remote.onrender.com/sync_sdcard.php";
const char *serverURL =
  "https://datalogger4-remote.onrender.com/save3_csv.php";
const char *geoURL =
  "http://ip-api.com/json/?fields=status,timezone";

const char *deviceID = "ESP0001";
const char *apiToken = "EspSwitch52026";

const char *CSV_HEADER =
  "id,value1,value2,value3,Date,Time,TimeStamp,timezone,device_id";

// -------------------- ESP32 pin assignments --------------------

// SD SPI: SCK=18, MISO=19, MOSI=23, CS=5
const int SD_CS_PIN = 5;
const int SD_SCK_PIN = 18;
const int SD_MISO_PIN = 19;
const int SD_MOSI_PIN = 23;

// CD74HC4067 select pins and signal pin
const int MUX_S0 = 25;
const int MUX_S1 = 26;
const int MUX_S2 = 27;
const int MUX_S3 = 32;
const int MUX_SIGNAL_PIN = 34;  // ADC1 input-only pin

const int MUX_CHANNEL_COUNT = 16;
const unsigned long MUX_SETTLE_DELAY_MS = 2;

// Approximate only. ESP32 ADC needs calibration for accurate mV readings.
const float ADC_INPUT_FULL_SCALE_MV = 3300.0f;

int channelMv[MUX_CHANNEL_COUNT] = {0};
int adcvalue1 = 0;
int adcvalue2 = 0;
int adcvalue3 = 0;

bool sdReady = false;
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

const unsigned long sendInterval = 5000UL;
const unsigned long timezoneRefreshInterval = 6UL * 60UL * 60UL * 1000UL;
const unsigned long timezoneRetryInterval = 5UL * 60UL * 1000UL;
unsigned long previousSendTime = 0;
unsigned long previousTimezoneCheck = 0;

// -------------------- URL encoding --------------------

String urlEncode(const String &input) {
  String output = "";
  const char hex[] = "0123456789ABCDEF";

  for (unsigned int i = 0; i < input.length(); i++) {
    unsigned char c = input[i];
    if ((c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      output += (char)c;
    } else {
      output += '%';
      output += hex[c >> 4];
      output += hex[c & 0x0F];
    }
  }
  return output;
}

// -------------------- LittleFS Wi-Fi credentials --------------------
// Each record occupies 3 lines: SSID, password, last successful IP.

bool loadWifiRecords() {
  wifiRecords.clear();
  if (!littleFSReady || !LittleFS.exists(NETWORK_FILE)) return true;

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
    Serial.println("ERROR: LittleFS is not available.");
    return false;
  }

  File file = LittleFS.open(NETWORK_FILE, FILE_WRITE);
  if (!file) {
    Serial.println("ERROR: Cannot write networks.txt.");
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

// -------------------- Wi-Fi connection --------------------

bool connectToSavedNetworks() {
  if (!loadWifiRecords()) {
    Serial.println("Could not read networks.txt.");
    return false;
  }
  if (wifiRecords.empty()) {
    Serial.println("No saved Wi-Fi credentials. Use SET WIFI.");
    return false;
  }

  WiFi.mode(WIFI_STA);

  for (size_t i = 0; i < wifiRecords.size(); i++) {
    Serial.println();
    Serial.print("Trying Wi-Fi record ");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.println(wifiRecords[i].ssid);

    WiFi.disconnect();
    delay(300);
    WiFi.begin(wifiRecords[i].ssid.c_str(), wifiRecords[i].password.c_str());

    unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - started < 12000UL) {
      delay(250);
      Serial.print(".");
      if (Serial.available()) break;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("Connected to: ");
      Serial.println(WiFi.SSID());
      Serial.print("ESP32 IP: ");
      Serial.println(WiFi.localIP());

      wifiRecords[i].lastIP = WiFi.localIP().toString();
      saveWifiRecords();
      return true;
    }
    Serial.println("This network did not connect; trying the next saved network.");
  }

  Serial.println("Could not connect to any saved Wi-Fi network.");
  return false;
}

// -------------------- Serial command helpers --------------------

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
      if (result.length() < 120) result += c;
    }
    delay(10);
  }

  Serial.println("Input timed out.");
  return "";
}

void showNetworks() {
  loadWifiRecords();
  Serial.println("\n------ SAVED WI-FI NETWORKS ------");

  if (wifiRecords.empty()) {
    Serial.println("No saved Wi-Fi credentials.");
  } else {
    for (size_t i = 0; i < wifiRecords.size(); i++) {
      Serial.print(i + 1);
      Serial.print(". SSID: ");
      Serial.print(wifiRecords[i].ssid);
      Serial.print(" | Saved IP: ");
      Serial.println(wifiRecords[i].lastIP.length()
                       ? wifiRecords[i].lastIP
                       : "(not connected yet)");
    }
  }
  Serial.println("----------------------------------");
}

void readNetworkFile() {
  loadWifiRecords();
  Serial.println("\n------ networks.txt (passwords visible) ------");

  if (wifiRecords.empty()) {
    Serial.println("File is empty or has no valid records.");
  } else {
    for (size_t i = 0; i < wifiRecords.size(); i++) {
      Serial.print("Record "); Serial.println(i + 1);
      Serial.print("SSID: "); Serial.println(wifiRecords[i].ssid);
      Serial.print("Password: "); Serial.println(wifiRecords[i].password);
      Serial.print("Last successful IP: "); Serial.println(wifiRecords[i].lastIP);
      Serial.println("---");
    }
  }
  Serial.println("----------------------------------------------");
}

void setWifiRecord() {
  String ssid = readSerialLine("Enter Wi-Fi SSID:");
  if (!ssid.length()) {
    Serial.println("SSID cannot be empty.");
    return;
  }

  String password = readSerialLine("Enter Wi-Fi password:");
  if (!password.length()) {
    Serial.println("Password cannot be empty.");
    return;
  }

  loadWifiRecords();
  for (size_t i = 0; i < wifiRecords.size(); i++) {
    if (wifiRecords[i].ssid == ssid && wifiRecords[i].password == password) {
      Serial.println("This SSID/password is already saved.");
      return;
    }
  }

  WifiRecord r;
  r.ssid = ssid;
  r.password = password;
  r.lastIP = "";
  wifiRecords.push_back(r);
  if (!saveWifiRecords()) return;

  Serial.println("Wi-Fi credential saved. Trying the new network...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(200);
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000UL) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());

    for (size_t i = 0; i < wifiRecords.size(); i++) {
      if (wifiRecords[i].ssid == ssid && wifiRecords[i].password == password) {
        wifiRecords[i].lastIP = WiFi.localIP().toString();
        break;
      }
    }
    saveWifiRecords();
  } else {
    Serial.println("Connection failed. Credential retained for future retries.");
  }
}

int askRecordNumber(const char *promptText) {
  String answer = readSerialLine(promptText);
  int number = answer.toInt();
  if (number < 1 || number > (int)wifiRecords.size()) return -1;
  return number - 1;
}

void editWifiRecord() {
  loadWifiRecords();
  showNetworks();
  if (wifiRecords.empty()) return;

  int index = askRecordNumber("Enter record number to edit:");
  if (index < 0) {
    Serial.println("Invalid record number.");
    return;
  }

  String ssid = readSerialLine("Enter new SSID:");
  if (!ssid.length()) {
    Serial.println("SSID cannot be empty.");
    return;
  }

  String password = readSerialLine("Enter new password:");
  if (!password.length()) {
    Serial.println("Password cannot be empty.");
    return;
  }

  wifiRecords[index].ssid = ssid;
  wifiRecords[index].password = password;
  wifiRecords[index].lastIP = "";
  if (saveWifiRecords()) Serial.println("Record updated.");
}

void deleteWifiRecord() {
  loadWifiRecords();
  showNetworks();
  if (wifiRecords.empty()) return;

  int index = askRecordNumber("Enter record number to delete:");
  if (index < 0) {
    Serial.println("Invalid record number.");
    return;
  }

  wifiRecords.erase(wifiRecords.begin() + index);
  if (saveWifiRecords()) Serial.println("Record deleted.");
}

void clearWifiRecords() {
  if (!littleFSReady) {
    Serial.println("LittleFS is not available.");
    return;
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  wifiRecords.clear();

  if (LittleFS.exists(NETWORK_FILE)) LittleFS.remove(NETWORK_FILE);
  Serial.println("All saved Wi-Fi credentials deleted.");
}

void showWifi() {
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi status: CONNECTED");
    Serial.print("SSID: "); Serial.println(WiFi.SSID());
    Serial.print("IP address: "); Serial.println(WiFi.localIP());
    Serial.print("RSSI: "); Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("Wi-Fi status: DISCONNECTED");
  }
}

// -------------------- Timezone detection --------------------

bool detectTimezone() {
  if (WiFi.status() != WL_CONNECTED) return false;

  Serial.println("\nChecking installation timezone...");
  WiFiClient client;
  HTTPClient http;

  if (!http.begin(client, geoURL)) {
    Serial.println("Could not start geolocation request.");
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
  Serial.println("Geolocation response:");
  Serial.println(response);

  if (response.indexOf("\"status\":\"success\"") < 0) {
    Serial.println("Geolocation request was unsuccessful.");
    return false;
  }

  String key = "\"timezone\":\"";
  int start = response.indexOf(key);
  if (start < 0) {
    Serial.println("Timezone field not found.");
    return false;
  }

  start += key.length();
  int finish = response.indexOf("\"", start);
  if (finish < 0) return false;

  String detected = response.substring(start, finish);
  detected.trim();

  if (!detected.length() || detected.indexOf('/') < 0) {
    Serial.println("Invalid timezone returned.");
    return false;
  }

  installationTimezone = detected;
  Serial.print("Detected timezone: ");
  Serial.println(installationTimezone);
  return true;
}

// -------------------- NTP time --------------------

bool synchronizeTime() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi required for NTP synchronization.");
    return false;
  }

  Serial.println("\nSynchronizing UTC time using NTP...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  for (int i = 0; i < 30; i++) {
    time_t now = time(nullptr);
    if (now > 1700000000) {
      Serial.println("\nNTP synchronization successful.");
      Serial.print("UTC Unix timestamp: ");
      Serial.println((unsigned long)now);
      return true;
    }
    Serial.print(".");
    delay(1000);
  }

  Serial.println("\nNTP synchronization failed.");
  return false;
}

// -------------------- IANA timezone to POSIX mapping --------------------

String posixTimezoneForIana(const String &iana) {
  if (iana == "Asia/Kolkata" || iana == "Asia/Calcutta") return "IST-5:30";
  if (iana == "Asia/Kathmandu") return "NPT-5:45";
  if (iana == "Asia/Dubai") return "GST-4";
  if (iana == "Asia/Singapore") return "SGT-8";
  if (iana == "Asia/Tokyo") return "JST-9";
  if (iana == "Asia/Shanghai" || iana == "Asia/Hong_Kong") return "CST-8";
  if (iana == "UTC" || iana == "Etc/UTC" || iana == "Etc/GMT") return "UTC0";
  if (iana == "Europe/London") return "GMT0BST,M3.5.0/1,M10.5.0";
  if (iana == "Europe/Paris" || iana == "Europe/Berlin" ||
      iana == "Europe/Rome") return "CET-1CEST,M3.5.0,M10.5.0/3";
  if (iana == "America/New_York") return "EST5EDT,M3.2.0,M11.1.0";
  if (iana == "America/Chicago") return "CST6CDT,M3.2.0,M11.1.0";
  if (iana == "America/Denver") return "MST7MDT,M3.2.0,M11.1.0";
  if (iana == "America/Los_Angeles") return "PST8PDT,M3.2.0,M11.1.0";
  if (iana == "Australia/Sydney") return "AEST-10AEDT,M10.1.0,M4.1.0/3";
  return "";
}

// -------------------- CSV ID scan --------------------

unsigned long findLastLocalCsvId() {
  if (!sdReady || !SD.exists(LOCAL_CSV_FILE)) return 0;

  File f = SD.open(LOCAL_CSV_FILE, FILE_READ);
  if (!f) return 0;

  String line;
  unsigned long lastId = 0;
  if (f.available()) line = f.readStringUntil('\n');

  while (f.available()) {
    line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;

    int comma = line.indexOf(',');
    if (comma > 0) {
      unsigned long id = (unsigned long)line.substring(0, comma).toInt();
      if (id > lastId) lastId = id;
    }
  }

  f.close();
  return lastId;
}

// -------------------- Safe backup of old CSV --------------------

bool backupAndRemoveFile(const char *sourcePath, const char *backupPath) {
  if (!SD.exists(sourcePath) || SD.exists(backupPath)) return false;

  File source = SD.open(sourcePath, FILE_READ);
  if (!source) return false;
  const size_t originalSize = source.size();

  File backup = SD.open(backupPath, FILE_WRITE);
  if (!backup) {
    source.close();
    return false;
  }

  uint8_t buffer[128];
  bool copyOK = true;

  while (source.available()) {
    int bytesRead = source.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) break;

    size_t bytesWritten = backup.write(buffer, (size_t)bytesRead);
    if (bytesWritten != (size_t)bytesRead) {
      copyOK = false;
      break;
    }
  }

  backup.flush();
  backup.close();
  source.close();

  File verify = SD.open(backupPath, FILE_READ);
  if (!verify) {
    copyOK = false;
  } else {
    if (verify.size() != originalSize) copyOK = false;
    verify.close();
  }

  if (!copyOK) {
    SD.remove(backupPath);
    Serial.println("Backup verification failed; original CSV was kept.");
    return false;
  }

  if (!SD.remove(sourcePath)) {
    Serial.println("Backup exists, but original CSV could not be removed.");
    return false;
  }

  return true;
}

// -------------------- Initialize 9-column SD CSV --------------------

bool initializeLocalCsv() {
  if (!sdReady) {
    Serial.println("ERROR: SD card is not available.");
    return false;
  }

  if (!SD.exists(LOCAL_CSV_FILE)) {
    File f = SD.open(LOCAL_CSV_FILE, FILE_WRITE);
    if (!f) {
      Serial.println("ERROR: Cannot create /sensor_data.csv.");
      return false;
    }
    f.println(CSV_HEADER);
    f.close();
    Serial.println("Created sensor_data.csv with exactly 9 columns.");
  } else {
    File f = SD.open(LOCAL_CSV_FILE, FILE_READ);
    if (!f) {
      Serial.println("ERROR: Cannot read sensor_data.csv.");
      return false;
    }

    String header = f.readStringUntil('\n');
    header.trim();
    f.close();

    if (header != CSV_HEADER) {
      Serial.println("Existing SD CSV has an old/different header.");
      Serial.println("Backing it up before creating a new 9-column file.");

      String backupPath = "/sensor_data_backup.csv";
      unsigned int backupNumber = 1;

      while (SD.exists(backupPath.c_str()) && backupNumber < 1000) {
        backupPath = "/sensor_data_backup_" + String(backupNumber) + ".csv";
        backupNumber++;
      }

      if (SD.exists(backupPath.c_str())) {
        Serial.println("ERROR: No unused backup filename available.");
        return false;
      }

      if (!backupAndRemoveFile(LOCAL_CSV_FILE, backupPath.c_str())) {
        Serial.println("ERROR: Could not safely back up old CSV.");
        return false;
      }

      Serial.print("Old CSV preserved as: ");
      Serial.println(backupPath);

      File newFile = SD.open(LOCAL_CSV_FILE, FILE_WRITE);
      if (!newFile) {
        Serial.println("ERROR: Could not create new CSV.");
        return false;
      }

      newFile.println(CSV_HEADER);
      newFile.flush();
      newFile.close();

      Serial.println("New 9-column sensor_data.csv created.");
    }
  }

  localCsvNextId = findLastLocalCsvId() + 1;
  Serial.print("SD CSV ready. Next local ID: ");
  Serial.println(localCsvNextId);
  return true;
}

// -------------------- Append a 9-column CSV record --------------------

bool appendLocalCsvRecord(
  const int values[16],
  time_t utcTimestamp,
  const String &timezone,
  const String &devId
) {
  if (!sdReady || !initializeLocalCsv()) return false;

  time_t raw = utcTimestamp;
  struct tm *utcTm = gmtime(&raw);
  if (!utcTm) return false;

  char stamp[24];
  snprintf(stamp, sizeof(stamp),
           "%04d-%02d-%02d %02d:%02d:%02d",
           utcTm->tm_year + 1900, utcTm->tm_mon + 1, utcTm->tm_mday,
           utcTm->tm_hour, utcTm->tm_min, utcTm->tm_sec);

  String posixTZ = posixTimezoneForIana(timezone);
  if (!posixTZ.length()) {
    Serial.println("WARNING: Unknown timezone; using UTC for local Date/Time.");
    posixTZ = "UTC0";
  }

  setenv("TZ", posixTZ.c_str(), 1);
  tzset();

  struct tm *localTm = localtime(&raw);
  if (!localTm) return false;

  char dateBuf[12];
  char timeBuf[10];

  snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d",
           localTm->tm_year + 1900, localTm->tm_mon + 1, localTm->tm_mday);
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
           localTm->tm_hour, localTm->tm_min, localTm->tm_sec);

  File f = SD.open(LOCAL_CSV_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("ERROR: Cannot append to sensor_data.csv.");
    return false;
  }

  // Exactly 9 columns, matching the local CSV/database format.
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

  if (written == 0) {
    Serial.println("ERROR: CSV record write failed.");
    return false;
  }

  Serial.print("Saved 9-column record to SD. Local CSV ID: ");
  Serial.println(localCsvNextId);
  localCsvNextId++;
  return true;
}

// -------------------- Show SD CSV status --------------------

void showLocalCsvStatus() {
  if (!sdReady || !SD.exists(LOCAL_CSV_FILE)) {
    Serial.println("SD card sensor_data.csv is not available.");
    return;
  }

  File f = SD.open(LOCAL_CSV_FILE, FILE_READ);
  if (!f) {
    Serial.println("Cannot open local CSV file.");
    return;
  }

  unsigned long lines = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length()) lines++;
  }
  f.close();

  Serial.print("SD CSV file: ");
  Serial.println(LOCAL_CSV_FILE);
  Serial.print("Header plus records: ");
  Serial.println(lines);
  Serial.print("Saved data records: ");
  Serial.println(lines > 0 ? lines - 1 : 0);
  Serial.print("Next local ID: ");
  Serial.println(localCsvNextId);
}

// -------------------- Send one 9-column SD row to sync endpoint --------------------

bool postSdCardRecord(const String &line) {
  String fields[9];
  int fieldCount = 0;
  int start = 0;

  for (int i = 0; i <= (int)line.length(); i++) {
    if (i == (int)line.length() || line[i] == ',') {
      if (fieldCount >= 9) return false;
      fields[fieldCount++] = line.substring(start, i);
      start = i + 1;
    }
  }

  if (fieldCount != 9) {
    Serial.print("Skipping malformed SD CSV row; columns found: ");
    Serial.println(fieldCount);
    return false;
  }

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

  // HTTPS connection to Render. setInsecure() is for testing only;
  // it encrypts the connection but does not verify the server certificate.
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, SYNC_URL)) {
    Serial.println("Could not initialize HTTPS connection to SD sync endpoint.");
    return false;
  }

  http.setTimeout(30000);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int code = http.POST(form);
  String response = http.getString();
  http.end();

  Serial.print("Sync HTTP code: ");
  Serial.println(code);
  Serial.println(response);

  return code == HTTP_CODE_OK;
}

// -------------------- Upload SD CSV --------------------

void uploadSdCardCsv() {
  if (!sdReady || !SD.exists(LOCAL_CSV_FILE)) {
    Serial.println("SD CSV missing or SD card not ready.");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi is not connected. Cannot upload now.");
    return;
  }

  File f = SD.open(LOCAL_CSV_FILE, FILE_READ);
  if (!f) {
    Serial.println("Cannot open SD card CSV.");
    return;
  }

  String header = f.readStringUntil('\n');
  header.trim();

  if (header != CSV_HEADER) {
    Serial.println("Unexpected SD CSV header. Upload cancelled.");
    f.close();
    return;
  }

  unsigned long attempted = 0;
  unsigned long succeeded = 0;
  unsigned long failed = 0;

  Serial.println("Uploading 9-column SD records to Render...");

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;

    attempted++;
    if (postSdCardRecord(line)) succeeded++;
    else failed++;

    delay(100);
    yield();
  }

  f.close();

  Serial.println("------ SD TRANSFER SUMMARY ------");
  Serial.print("Records attempted: "); Serial.println(attempted);
  Serial.print("HTTP-successful requests: "); Serial.println(succeeded);
  Serial.print("Failed requests: "); Serial.println(failed);
  Serial.println("HTTP 200 alone does not guarantee database insertion.");
  Serial.println("---------------------------------");
}

// -------------------- Help and status --------------------

void showHelp() {
  Serial.println();
  Serial.println("=========== COMMANDS ===========");
  Serial.println("HELP           - Show commands");
  Serial.println("SHOW NETWORKS  - List saved SSIDs and IPs");
  Serial.println("READ FILE      - Read networks.txt (shows passwords)");
  Serial.println("SET WIFI       - Add a Wi-Fi credential");
  Serial.println("EDIT WIFI      - Edit a record by number");
  Serial.println("DELETE WIFI    - Delete a record by number");
  Serial.println("CLEAR WIFI     - Delete all credentials");
  Serial.println("SHOW WIFI      - Show current Wi-Fi connection");
  Serial.println("SHOW CSV       - Show local SD CSV record count");
  Serial.println("UPLOAD SD CARD - Transfer SD CSV records to XAMPP");
  Serial.println("SD CSV format: 9 columns, same as local CSV");
  Serial.println("STATUS         - Show status");
  Serial.println("RESTART        - Restart ESP32");
  Serial.println("================================");
}

void showStatus() {
  Serial.println("\n========== DEVICE STATUS ==========");
  Serial.print("Device ID: "); Serial.println(deviceID);
  Serial.println("Controller: ESP32");
  Serial.println("Analog input: CD74HC4067 16-channel multiplexer");

  Serial.print("LittleFS: ");
  Serial.println(littleFSReady ? "READY" : "NOT AVAILABLE");

  loadWifiRecords();
  Serial.print("Saved Wi-Fi records: ");
  Serial.println(wifiRecords.size());

  Serial.print("SD card: ");
  Serial.println(sdReady ? "READY" : "NOT AVAILABLE");

  showLocalCsvStatus();
  showWifi();

  Serial.print("Installation timezone: ");
  Serial.println(installationTimezone.length()
                   ? installationTimezone
                   : "(not detected)");

  Serial.print("UTC Unix timestamp: ");
  Serial.println((unsigned long)time(nullptr));
  Serial.println("===================================");
}

// -------------------- Command processing --------------------

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
      else if (command == "SHOW NETWORKS" || command == "SHOW NETWORK")
        showNetworks();
      else if (command == "READ FILE") readNetworkFile();
      else if (command == "SET WIFI") setWifiRecord();
      else if (command == "EDIT WIFI") editWifiRecord();
      else if (command == "DELETE WIFI") deleteWifiRecord();
      else if (command == "CLEAR WIFI") clearWifiRecords();
      else if (command == "SHOW WIFI") showWifi();
      else if (command == "SHOW CSV") showLocalCsvStatus();
      else if (command == "UPLOAD SD CARD" || command == "SYNC SD CARD")
        uploadSdCardCsv();
      else if (command == "STATUS") showStatus();
      else if (command == "RESTART") {
        Serial.println("Restarting ESP32...");
        delay(500);
        ESP.restart();
      } else {
        Serial.println("Unknown command. Type HELP.");
      }
    } else if (serialCommandBuffer.length() < 100) {
      serialCommandBuffer += c;
    }
  }
}

// -------------------- Read all 16 multiplexer channels --------------------
// CD74HC4067 channel 0 maps to CH1, channel 1 to CH2, ... channel 15 to CH16.
// The result is an approximate millivolt value based on the 12-bit ADC reading.
// Calibrate against a reliable meter if accurate millivolt measurements are required.
void readMultiplexer() {
  const int samplesPerChannel = 8;
  const int adcMaximum = 4095;

  for (int channel = 0; channel < MUX_CHANNEL_COUNT; channel++) {
    // Select the required CD74HC4067 channel using S0-S3.
    digitalWrite(MUX_S0, (channel & 0x01) ? HIGH : LOW);
    digitalWrite(MUX_S1, (channel & 0x02) ? HIGH : LOW);
    digitalWrite(MUX_S2, (channel & 0x04) ? HIGH : LOW);
    digitalWrite(MUX_S3, (channel & 0x08) ? HIGH : LOW);

    delay(MUX_SETTLE_DELAY_MS);

    // Discard the first conversion after switching mux channels.
    (void)analogRead(MUX_SIGNAL_PIN);
    delayMicroseconds(200);

    uint32_t totalRaw = 0;
    for (int sample = 0; sample < samplesPerChannel; sample++) {
      totalRaw += (uint32_t)analogRead(MUX_SIGNAL_PIN);
      delayMicroseconds(200);
    }

    int rawAverage = (int)(totalRaw / samplesPerChannel);
    channelMv[channel] = (int)(((uint32_t)rawAverage *
                                (uint32_t)ADC_INPUT_FULL_SCALE_MV +
                                (adcMaximum / 2)) / adcMaximum);
  }

  // The existing PHP endpoint stores only these first three channel values.
  adcvalue1 = channelMv[0];  // CD74HC4067 CH0 / first channel
  adcvalue2 = channelMv[1];  // CD74HC4067 CH1 / second channel
  adcvalue3 = channelMv[2];  // CD74HC4067 CH2 / third channel

  Serial.println("Multiplexer scan (approximate mV):");
  for (int channel = 0; channel < MUX_CHANNEL_COUNT; channel++) {
    Serial.print("CH");
    if (channel + 1 < 10) Serial.print('0');
    Serial.print(channel + 1);
    Serial.print(" = ");
    Serial.print(channelMv[channel]);
    Serial.println(" mV");
  }
}

// -------------------- Setup --------------------

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println("ESP32 16-CHANNEL MULTIPLEXER DATA LOGGER");
  Serial.println("Fixed Device ID: ESP0001");
  Serial.println("SD CSV: exactly 9 columns");
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
  analogSetPinAttenuation(MUX_SIGNAL_PIN, ADC_11db);

  Serial.println("CD74HC4067 pins initialized.");

  // Mount LittleFS. Formatting on failure can erase existing LittleFS data.
  // Back up networks.txt first if it contains credentials you need to retain.
  littleFSReady = LittleFS.begin(true);

  if (!littleFSReady) {
    Serial.println("ERROR: LittleFS mount failed.");
  } else {
    Serial.println("LittleFS mounted successfully.");

    if (!LittleFS.exists(NETWORK_FILE)) {
      File newFile = LittleFS.open(NETWORK_FILE, FILE_WRITE);
      if (newFile) newFile.close();
    }

    loadWifiRecords();
    connectToSavedNetworks();
  }

  Serial.println("Type HELP in Serial Monitor. Line ending: Newline.");
  Serial.print("Live PHP endpoint: ");
  Serial.println(serverURL);

  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  if (SD.begin(SD_CS_PIN, SPI, 4000000)) {
    sdReady = true;
    Serial.println("SD card initialized successfully.");
    initializeLocalCsv();
  } else {
    sdReady = false;
    Serial.println("ERROR: SD card initialization failed.");
    Serial.println("Check wiring, module voltage, FAT32 format and CS pin.");
  }

  if (WiFi.status() == WL_CONNECTED) {
    detectTimezone();
    synchronizeTime();
  } else {
    Serial.println("Wi-Fi unavailable at startup; timezone/NTP will retry.");
  }

  previousTimezoneCheck = millis();
  previousSendTime = millis();
}

// -------------------- Main loop --------------------

void loop() {
  handleSerialCommands();

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

  unsigned long refreshInterval =
    installationTimezone.length() == 0
      ? timezoneRetryInterval
      : timezoneRefreshInterval;

  if (wifiConnected &&
      nowMillis - previousTimezoneCheck >= refreshInterval) {
    previousTimezoneCheck = nowMillis;
    if (!detectTimezone()) Serial.println("Timezone refresh failed.");
  }

  if (nowMillis - previousSendTime < sendInterval) return;
  previousSendTime = nowMillis;

  if (installationTimezone.length() == 0) {
    Serial.println("Timezone not detected yet; reading skipped.");
    return;
  }

  time_t utcTimestamp = time(nullptr);

  if (utcTimestamp <= 1700000000) {
    Serial.println("UTC clock invalid; attempting NTP synchronization.");
    if (!wifiConnected || !synchronizeTime()) {
      Serial.println("Reading skipped until valid time is available.");
      return;
    }
    utcTimestamp = time(nullptr);
  }

  readMultiplexer();

  Serial.println("\n----------------------------------");
  Serial.println("Saving 16-channel scan to SD and, if online, Render");
  Serial.print("Device ID: "); Serial.println(deviceID);
  Serial.print("UTC timestamp: "); Serial.println((unsigned long)utcTimestamp);
  Serial.print("Timezone: "); Serial.println(installationTimezone);

  bool localSaved = appendLocalCsvRecord(
    channelMv, utcTimestamp, installationTimezone, String(deviceID)
  );

  if (!localSaved) {
    Serial.println("ERROR: Reading was not saved to SD.");
  }

  if (!wifiConnected) {
    Serial.println("Wi-Fi offline: SD copy retained for later upload.");
    return;
  }

  // Keep save3_csv.php's existing parameter names and endpoint unchanged.
  String postData =
    "device_id=" + urlEncode(String(deviceID)) +
    "&api_token=" + urlEncode(String(apiToken)) +
    "&value1=" + String(adcvalue1) +
    "&value2=" + String(adcvalue2) +
    "&value3=" + String(adcvalue3) +
    "&timestamp=" + String((unsigned long)utcTimestamp) +
    "&timezone=" + urlEncode(installationTimezone);

  // HTTPS connection to Render. setInsecure() is for testing only;
  // install/validate the server CA certificate for production use.
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  if (http.begin(client, serverURL)) {
    http.setTimeout(30000);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    int httpCode = http.POST(postData);
    Serial.print("HTTP response code: ");
    Serial.println(httpCode);

    if (httpCode > 0) {
      Serial.println("PHP endpoint response:");
      Serial.println(http.getString());
    } else {
      Serial.print("HTTP error: ");
      Serial.println(http.errorToString(httpCode));
      Serial.println("SD card copy is retained for later transfer.");
    }

    http.end();
  } else {
    Serial.println("Could not initialize HTTPS connection to Render.");
    Serial.println("SD card copy is retained.");
  }
}
