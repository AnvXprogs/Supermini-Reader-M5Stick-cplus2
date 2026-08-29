#include <Wire.h>
#include <GyverOLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <vector>
#include <algorithm>
#include <esp_wifi.h>

#define PIN_SDA   11
#define PIN_SCL   13
#define PIN_UP    5
#define PIN_DOWN  7
#define PIN_OK    6

GyverOLED<SSD1306_128x64, OLED_BUFFER> oled;

const int SCREEN_ROWS = 8;
const int CHARS_PER_LINE = 21;

const char* AP_SSID = "Esp hpora";
const char* AP_PASS = "12345678";
IPAddress apIP(192, 168, 4, 1);
DNSServer dnsServer;
WebServer server(80);
bool wifiActive = false;

enum AppState { ST_LIST, ST_VIEW, ST_WIFI };
AppState state = ST_LIST;

struct FileEntry { String fname; String title; };
std::vector<FileEntry> fileEntries;

int selected  = 0;
int listStart = 0;

String currentFileName = "";
std::vector<uint32_t> pageOffsets;
bool currentFileEmpty = false;
int page = 0;

uint8_t brightness = 200;
const uint8_t BRIGHTNESS_STEP = 15;
String lastFileName = "";

bool settingsDirty = false;
unsigned long settingsChangedAt = 0;
const unsigned long SETTINGS_SAVE_DELAY = 800;

const char* SETTINGS_PATH = "/_cfg.txt";

int scrollPos = 0;
unsigned long scrollTimer = 0;
const unsigned long SCROLL_INTERVAL = 300;
int lastScrollIdx = -1;

const int DEBOUNCE_MAX = 6;
const unsigned long LONGPRESS_MS    = 600;
const unsigned long REPEAT_START_MS = 500;
const unsigned long REPEAT_MS       = 180;

struct Btn {
  uint8_t pin;
  int counter;
  bool stable;
  unsigned long tPress, tRepeat;
  bool longFired;
  bool pressedEvent, releasedEvent, longEvent, repeatEvent;
};

Btn btnUp, btnDown, btnOk;

void btnInit(Btn &b, uint8_t pin);
void btnUpdate(Btn &b);
void handleButtons();
void moveSelection(int dir);
void openSelectedFile();
void prevPage();
void nextPage();
void changeBrightness(int delta);
void toggleWifi();
void drawScreen();
void drawList();
void drawView();
void drawWifiInfo();

int utf8Length(const String &s);
String utf8Substring(const String &s, int startChar, int charCount);
String marqueeText(const String &title, int width, int offset);

bool lsReadWord(File &f, String &word, bool &isBreak);
bool lsNextLine(File &f, String &line, uint32_t &lineStartPos);
void lsReset();
void buildPageIndex(const String &fname);

void markSettingsDirty();
void maybeSaveSettings();
void loadSettings();
void saveSettings();

String pathOf(const String &fn);
void refreshFileList();
unsigned long extractFileId(const String &fname);
struct FileData;
FileData readFile(const String &fname);
void writeFile(const String &fname, String title, const String &content);
String genFilename();
bool hasSpace(long extraBytes);

String htmlEscape(String s);
void redirectMsg(const char *m);
void handleRedirect();
String pageStyle();
void handleRoot();
void handleUpload();
void handleEditPage();
void handleSave();
void handleDelete();
void setupWebServer();

struct FileData { String title; String content; };

void btnInit(Btn &b, uint8_t pin) {
  b.pin = pin;
  pinMode(pin, INPUT_PULLUP);
  bool pressedNow = (digitalRead(pin) == LOW);
  b.counter = pressedNow ? DEBOUNCE_MAX : 0;
  b.stable = pressedNow;
  b.tPress = 0;
  b.tRepeat = 0;
  b.longFired = false;
  b.pressedEvent = b.releasedEvent = b.longEvent = b.repeatEvent = false;
}

void btnUpdate(Btn &b) {
  b.pressedEvent = b.releasedEvent = b.longEvent = b.repeatEvent = false;

  bool pressedNow = (digitalRead(b.pin) == LOW);

  if (pressedNow) {
    if (b.counter < DEBOUNCE_MAX) b.counter++;
  } else {
    if (b.counter > 0) b.counter--;
  }

  bool newStable = b.stable;
  if (b.counter >= DEBOUNCE_MAX) newStable = true;
  else if (b.counter <= 0) newStable = false;

  if (newStable != b.stable) {
    b.stable = newStable;
    if (b.stable) {
      b.tPress = millis();
      b.tRepeat = millis();
      b.longFired = false;
      b.pressedEvent = true;
    } else {
      b.releasedEvent = true;
    }
  }

  if (b.stable) {
    unsigned long held = millis() - b.tPress;
    if (!b.longFired && held >= LONGPRESS_MS) {
      b.longFired = true;
      b.longEvent = true;
    }
    if (held >= REPEAT_START_MS && millis() - b.tRepeat >= REPEAT_MS) {
      b.tRepeat = millis();
      b.repeatEvent = true;
    }
  }
}

void setup() {
  Serial.begin(9600);
  randomSeed(micros());

  btnInit(btnUp,   PIN_UP);
  btnInit(btnDown, PIN_DOWN);
  btnInit(btnOk,   PIN_OK);

  Wire.begin(PIN_SDA, PIN_SCL);
  oled.init();
  oled.clear();
  oled.update();

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount error");
  }

  WiFi.mode(WIFI_OFF);
  setupWebServer();
  refreshFileList();
  loadSettings();
  oled.setContrast(brightness);

  if (lastFileName.length() > 0) {
    for (size_t i = 0; i < fileEntries.size(); i++) {
      if (fileEntries[i].fname == lastFileName) { selected = i; break; }
    }
  }
  esp_wifi_set_max_tx_power(WIFI_POWER_2dBm);
  drawScreen();
}

void loop() {
  btnUpdate(btnUp);
  btnUpdate(btnDown);
  btnUpdate(btnOk);

  if (wifiActive) {
    dnsServer.processNextRequest();
    server.handleClient();
  }

  handleButtons();

  if (state == ST_LIST && !fileEntries.empty()) {
    String title = fileEntries[selected].title;
    if (utf8Length(title) > CHARS_PER_LINE - 2 && millis() - scrollTimer > SCROLL_INTERVAL) {
      scrollTimer = millis();
      scrollPos++;
      drawScreen();
    }
  }

  maybeSaveSettings();
  delay(5);
}

void handleButtons() {
  switch (state) {
    case ST_LIST:
      if (btnUp.pressedEvent   || btnUp.repeatEvent)   moveSelection(-1);
      if (btnDown.pressedEvent || btnDown.repeatEvent)  moveSelection(1);
      if (btnOk.releasedEvent && !btnOk.longFired)      openSelectedFile();
      if (btnOk.longEvent)                              toggleWifi();
      break;

    case ST_VIEW:
      if (btnUp.pressedEvent   || btnUp.repeatEvent)   prevPage();
      if (btnDown.pressedEvent || btnDown.repeatEvent)  nextPage();
      if (btnOk.releasedEvent && !btnOk.longFired) {
        state = ST_LIST;
        drawScreen();
      }
      break;

    case ST_WIFI:
      if (btnUp.pressedEvent   || btnUp.repeatEvent)   changeBrightness(BRIGHTNESS_STEP);
      if (btnDown.pressedEvent || btnDown.repeatEvent)  changeBrightness(-BRIGHTNESS_STEP);
      if (btnOk.longEvent)                              toggleWifi();
      break;
  }
}

void moveSelection(int dir) {
  int total = fileEntries.size();
  if (total == 0) return;
  selected += dir;
  if (selected < 0) selected = total - 1;
  if (selected >= total) selected = 0;
  scrollPos = 0;
  lastScrollIdx = selected;
  markSettingsDirty();
  drawScreen();
}

void openSelectedFile() {
  if (fileEntries.empty()) return;
  buildPageIndex(fileEntries[selected].fname);
  page = 0;
  state = ST_VIEW;
  markSettingsDirty();
  drawScreen();
}

void prevPage() { if (page > 0) { page--; drawScreen(); } }
void nextPage() { if (page < (int)pageOffsets.size() - 1) { page++; drawScreen(); } }

void changeBrightness(int delta) {
  int v = (int)brightness + delta;
  v = constrain(v, 5, 255);
  brightness = (uint8_t)v;
  oled.setContrast(brightness);
  markSettingsDirty();
  drawScreen();
}

void toggleWifi() {
  wifiActive = !wifiActive;

  if (wifiActive) {
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID, AP_PASS);
    dnsServer.start(53, "*", apIP);
    server.begin();
    state = ST_WIFI;
  } else {
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    state = ST_LIST;
    refreshFileList();
    if (selected >= (int)fileEntries.size()) selected = 0;
  }
  drawScreen();
}

void drawScreen() {
  oled.clear();
  switch (state) {
    case ST_LIST: drawList(); break;
    case ST_VIEW: drawView(); break;
    case ST_WIFI: drawWifiInfo(); break;
  }
  oled.update();
}

void drawList() {
  const int VIS = SCREEN_ROWS;
  const int MARGIN = 1;
  const int WIDTH = CHARS_PER_LINE - 2;
  int total = fileEntries.size();

  if (total == 0) {
    oled.setCursor(0, 3);
    oled.print("No files");
    return;
  }

  if (selected < listStart + MARGIN)
    listStart = max(0, selected - MARGIN);
  if (selected > listStart + VIS - 1 - MARGIN)
    listStart = selected - VIS + 1 + MARGIN;

  int maxStart = max(0, total - VIS);
  listStart = constrain(listStart, 0, maxStart);

  for (int row = 0; row < VIS && (listStart + row) < total; row++) {
    int idx = listStart + row;
    String title = fileEntries[idx].title;
    int titleChars = utf8Length(title);
    String prefix = (idx == selected) ? "> " : "  ";
    String shown;

    if (titleChars <= WIDTH) {
      shown = title;
    } else if (idx == selected) {
      if (lastScrollIdx != selected) { scrollPos = 0; lastScrollIdx = selected; }
      shown = marqueeText(title, WIDTH, scrollPos);
    } else {
      shown = utf8Substring(title, 0, WIDTH);
    }

    oled.setCursor(0, row);
    oled.print(prefix + shown);
  }
}

void drawView() {
  if (currentFileEmpty || currentFileName.length() == 0) {
    oled.setCursor(0, 3);
    oled.print("Empty");
    return;
  }

  File f = LittleFS.open(pathOf(currentFileName), "r");
  if (!f) {
    oled.setCursor(0, 3);
    oled.print("Read error");
    return;
  }

  uint32_t offset = (page >= 0 && page < (int)pageOffsets.size()) ? pageOffsets[page] : 0;
  f.seek(offset);

  lsReset();

  String line;
  uint32_t dummy;
  for (int row = 0; row < SCREEN_ROWS; row++) {
    if (!lsNextLine(f, line, dummy)) break;
    oled.setCursor(0, row);
    oled.print(line);
  }
  f.close();
}

void drawWifiInfo() {
  oled.setCursor(0, 0);
  oled.print("WiFi: " + String(AP_SSID));
  oled.setCursor(0, 2);
  oled.print("IP: " + WiFi.softAPIP().toString());
  oled.setCursor(0, 4);
  oled.print("Pass: " + String(AP_PASS));
  oled.setCursor(0, 6);
  oled.print("Brightness: " + String(brightness));
}

int utf8Length(const String &s) {
  int count = 0;
  size_t i = 0;
  while (i < s.length()) {
    uint8_t c = (uint8_t)s[i];
    if (c < 0x80) i += 1;
    else if ((c & 0xE0) == 0xC0) i += 2;
    else if ((c & 0xF0) == 0xE0) i += 3;
    else if ((c & 0xF8) == 0xF0) i += 4;
    else i += 1;
    count++;
  }
  return count;
}

String utf8Substring(const String &s, int startChar, int charCount) {
  int idxChar = 0;
  size_t i = 0;
  int byteStart = -1;
  size_t byteEnd = s.length();

  while (i < s.length()) {
    if (idxChar == startChar) byteStart = (int)i;
    if (idxChar == startChar + charCount) { byteEnd = i; break; }
    uint8_t c = (uint8_t)s[i];
    if (c < 0x80) i += 1;
    else if ((c & 0xE0) == 0xC0) i += 2;
    else if ((c & 0xF0) == 0xE0) i += 3;
    else if ((c & 0xF8) == 0xF0) i += 4;
    else i += 1;
    idxChar++;
  }
  if (idxChar == startChar && byteStart == -1) byteStart = (int)i;
  if (byteStart == -1) return "";
  return s.substring(byteStart, byteEnd);
}

String marqueeText(const String &title, int width, int offset) {
  const String sep = "    ";
  int period = utf8Length(title) + utf8Length(sep);
  if (period <= 0) return title;

  String loop = title + sep + title + sep;
  int pos = ((offset % period) + period) % period;
  return utf8Substring(loop, pos, width);
}

bool   lsHavePending = false;
String lsPendingWord = "";
bool   lsPendingBreak = false;

void lsReset() {
  lsHavePending = false;
  lsPendingWord = "";
  lsPendingBreak = false;
}

bool lsReadWord(File &f, String &word, bool &isBreak) {
  word = "";
  isBreak = false;
  int ch;
  while ((ch = f.read()) != -1) {
    if (ch == ' ' || ch == '\r') {
      if (word.length() == 0) continue;
      return true;
    }
    if (ch == '\n') {
      isBreak = true;
      return true;
    }
    word += (char)ch;
    if (utf8Length(word) >= CHARS_PER_LINE) return true;
  }
  return word.length() > 0;
}

bool lsNextLine(File &f, String &line, uint32_t &lineStartPos) {
  line = "";
  bool gotAny = false;
  bool startCaptured = false;

  while (true) {
    if (!lsHavePending) {
      uint32_t before = f.position();
      if (!lsReadWord(f, lsPendingWord, lsPendingBreak)) {
        return gotAny;
      }
      if (!startCaptured) { lineStartPos = before; startCaptured = true; }
      lsHavePending = true;
    } else if (!startCaptured) {
      lineStartPos = f.position() - lsPendingWord.length() - (lsPendingBreak ? 1 : 0);
      startCaptured = true;
    }

    if (lsPendingWord.length() == 0) {
      lsHavePending = false;
      return true;
    }

    int lineChars = utf8Length(line);
    int wordChars = utf8Length(lsPendingWord);
    int extra = (lineChars > 0 ? 1 : 0) + wordChars;

    if (lineChars + extra <= CHARS_PER_LINE) {
      if (line.length() > 0) line += ' ';
      line += lsPendingWord;
      gotAny = true;
      lsHavePending = false;
      if (lsPendingBreak) return true;
    } else {
      return true;
    }
  }
}

void buildPageIndex(const String &fname) {
  pageOffsets.clear();
  currentFileName = fname;
  currentFileEmpty = false;

  File f = LittleFS.open(pathOf(fname), "r");
  if (!f) { currentFileEmpty = true; pageOffsets.push_back(0); return; }

  f.readStringUntil('\n');
  lsReset();

  uint32_t startPos = f.position();
  pageOffsets.push_back(startPos);

  String line;
  uint32_t lineStart;
  int rowInPage = 0;
  bool any = false;

  while (lsNextLine(f, line, lineStart)) {
    any = true;
    if (rowInPage == SCREEN_ROWS) {
      pageOffsets.push_back(lineStart);
      rowInPage = 0;
    }
    rowInPage++;
  }
  f.close();

  currentFileEmpty = !any;
}

void markSettingsDirty() {
  settingsDirty = true;
  settingsChangedAt = millis();
}

void maybeSaveSettings() {
  if (settingsDirty && millis() - settingsChangedAt > SETTINGS_SAVE_DELAY) {
    saveSettings();
    settingsDirty = false;
  }
}

void loadSettings() {
  if (!LittleFS.exists(SETTINGS_PATH)) return;
  File f = LittleFS.open(SETTINGS_PATH, "r");
  if (!f) return;
  String line1 = f.readStringUntil('\n');
  String line2 = f.readStringUntil('\n');
  f.close();

  line1.trim();
  line2.trim();

  if (line1.length() > 0) {
    int v = line1.toInt();
    if (v >= 5 && v <= 255) brightness = (uint8_t)v;
  }
  lastFileName = line2;
}

void saveSettings() {
  String fnameToSave = "";
  if (!fileEntries.empty() && selected >= 0 && selected < (int)fileEntries.size()) {
    fnameToSave = fileEntries[selected].fname;
  }
  File f = LittleFS.open(SETTINGS_PATH, "w");
  if (!f) return;
  f.println(String(brightness));
  f.println(fnameToSave);
  f.close();
}

String pathOf(const String &fn) {
  return fn.startsWith("/") ? fn : "/" + fn;
}

unsigned long extractFileId(const String &fname) {
  int start = fname.indexOf("d_");
  if (start == -1) return 0;
  start += 2;
  int end = fname.indexOf(".txt", start);
  if (end == -1) end = fname.length();
  String digits = fname.substring(start, end);
  if (digits.length() == 0) return 0;
  for (size_t i = 0; i < digits.length(); i++) {
    if (!isDigit(digits[i])) return 0;
  }
  return strtoul(digits.c_str(), nullptr, 10);
}

void refreshFileList() {
  fileEntries.clear();
  File root = LittleFS.open("/");
  if (!root) return;
  File f = root.openNextFile();
  while (f) {
    String fname = String(f.name());
    if (!f.isDirectory() && fname.indexOf("_cfg.txt") == -1) {
      String title = f.readStringUntil('\n');
      title.trim();
      fileEntries.push_back({fname, title});
    }
    f = root.openNextFile();
  }

  std::sort(fileEntries.begin(), fileEntries.end(), [](const FileEntry &a, const FileEntry &b) {
    return extractFileId(a.fname) > extractFileId(b.fname);
  });
}

FileData readFile(const String &fname) {
  FileData d;
  File f = LittleFS.open(pathOf(fname), "r");
  if (!f) return d;
  d.title = f.readStringUntil('\n');
  d.title.trim();
  d.content = f.readString();
  f.close();
  return d;
}

void writeFile(const String &fname, String title, const String &content) {
  title.replace("\n", " ");
  title.replace("\r", "");
  File f = LittleFS.open(pathOf(fname), "w");
  if (!f) return;
  f.println(title);
  f.print(content);
  f.close();
}

String genFilename() {
  unsigned long maxId = 0;
  File root = LittleFS.open("/");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      if (!f.isDirectory()) {
        unsigned long id = extractFileId(String(f.name()));
        if (id > maxId) maxId = id;
      }
      f = root.openNextFile();
    }
  }
  unsigned long newId = maxId + 1;
  String fn = "/d_" + String(newId) + ".txt";
  while (LittleFS.exists(fn)) {
    newId++;
    fn = "/d_" + String(newId) + ".txt";
  }
  return fn;
}

bool hasSpace(long extraBytes) {
  long freeBytes = (long)LittleFS.totalBytes() - (long)LittleFS.usedBytes();
  const long MARGIN = 512;
  return (freeBytes - extraBytes) > MARGIN;
}

String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  return s;
}

void redirectMsg(const char *m) {
  server.sendHeader("Location", String("/?msg=") + m, true);
  server.send(302, "text/plain", "");
}

void handleRedirect() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

String pageStyle() {
  return F(
    "*{box-sizing:border-box}"
    "body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:#0e0e10;color:#eee;"
    "padding:16px;max-width:520px;margin:auto;line-height:1.4}"
    "h2{color:#4caf50;margin:4px 0 12px}"
    "h3{color:#8bc34a;margin:24px 0 8px;font-size:16px;text-transform:uppercase;letter-spacing:.04em}"
    "input,textarea{width:100%;margin:6px 0;padding:10px;border-radius:8px;"
    "border:1px solid #333;background:#1b1b1f;color:#eee;font-size:16px}"
    "input:focus,textarea:focus{outline:none;border-color:#4caf50}"
    "textarea{min-height:150px;resize:vertical;font-family:inherit}"
    "button{width:100%;padding:12px;border:none;border-radius:8px;background:#4caf50;color:#fff;"
    "font-size:16px;font-weight:600;margin:6px 0;cursor:pointer}"
    "button:active{opacity:.8}"
    "button.del{background:#c0392b}"
    "button.edit{background:#2980b9}"
    ".row{display:flex;gap:8px}"
    ".row form{flex:1}"
    ".item{background:#1b1b1f;border-radius:10px;padding:12px;margin:10px 0}"
    ".item .title{font-weight:600;margin-bottom:8px;word-break:break-word}"
    ".bar{background:#2a2a2e;border-radius:8px;overflow:hidden;height:14px;margin:8px 0}"
    ".fill{background:#4caf50;height:100%}"
    ".hint{color:#888;font-size:13px;margin:4px 0 0}"
    ".msg-ok{color:#4caf50}.msg-err{color:#e74c3c}"
    "a{color:#4caf50;text-decoration:none}"
  );
}

void handleRoot() {
  refreshFileList();
  size_t total = LittleFS.totalBytes();
  size_t used  = LittleFS.usedBytes();
  int pct = total ? (int)(used * 100 / total) : 0;

  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>ESP Cheatsheet</title><style>");
  html += pageStyle();
  html += F("</style></head><body>");

  html += "<h2>ESP Cheatsheet</h2>";
  html += "<div>Used: " + String(used) + " / " + String(total) + " bytes (" + String(pct) + "%)</div>";
  html += "<div class='bar'><div class='fill' style='width:" + String(pct) + "%'></div></div>";

  if (server.hasArg("msg")) {
    String msg = server.arg("msg");
    if (msg == "nospace") html += "<p class='msg-err'>Not enough space!</p>";
    if (msg == "empty")   html += "<p class='msg-err'>Enter title / File too large</p>";
    if (msg == "ok")      html += "<p class='msg-ok'>Saved</p>";
  }

  html += F("<h3>Add file</h3>"
    "<form method='POST' action='/upload'>"
    "<input type='text' name='title' placeholder='Title' required>"
    "<textarea name='content' placeholder='Content'></textarea>"
    "<button type='submit'>Upload</button>"
    "</form></body></html>");

  html += "<h3>Files</h3>";
  if (fileEntries.empty()) {
    html += "<p class='hint'>No files</p>";
  } else {
    for (auto &e : fileEntries) {
      html += "<div class='item'><div class='title'>" + htmlEscape(e.title) + "</div>"
              "<div class='row'>"
              "<form method='GET' action='/edit'>"
              "<input type='hidden' name='f' value='" + e.fname + "'>"
              "<button class='edit' type='submit'>Open/Edit</button></form>"
              "<form method='POST' action='/delete' onsubmit=\"return confirm('Delete file?')\">"
              "<input type='hidden' name='f' value='" + e.fname + "'>"
              "<button class='del' type='submit'>Delete</button></form>"
              "</div></div>";
    }
  }

  server.send(200, "text/html; charset=utf-8", html);
}

void handleUpload() {
  String title = server.arg("title");
  String content = server.arg("content");
  title.trim();
  if (title.length() == 0) { redirectMsg("empty"); return; }

  long extra = title.length() + content.length() + 8;
  if (!hasSpace(extra)) { redirectMsg("nospace"); return; }

  String fn = genFilename();
  writeFile(fn, title, content);
  redirectMsg("ok");
}

void handleEditPage() {
  if (!server.hasArg("f")) { handleRedirect(); return; }
  String fn = server.arg("f");
  if (!LittleFS.exists(pathOf(fn))) { handleRedirect(); return; }

  FileData d = readFile(fn);

  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>Edit file</title><style>");
  html += pageStyle();
  html += F("</style></head><body>");
  html += "<h2>Edit file</h2>";
  html += "<form method='POST' action='/save'>"
          "<input type='hidden' name='f' value='" + fn + "'>"
          "<input type='text' name='title' value='" + htmlEscape(d.title) + "' required>"
          "<textarea name='content'>" + htmlEscape(d.content) + "</textarea>"
          "<button type='submit'>Save</button>"
          "</form>"
          "<form method='POST' action='/delete' onsubmit=\"return confirm('Delete file?')\">"
          "<input type='hidden' name='f' value='" + fn + "'>"
          "<button class='del' type='submit'>Delete</button></form>"
          "<p><a href='/'>&larr; Back to list</a></p></body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

void handleSave() {
  String fn = server.arg("f");
  String title = server.arg("title");
  String content = server.arg("content");
  title.trim();

  if (title.length() == 0 || !LittleFS.exists(pathOf(fn))) { redirectMsg("empty"); return; }

  size_t oldSize = 0;
  {
    File f = LittleFS.open(pathOf(fn), "r");
    if (f) { oldSize = f.size(); f.close(); }
  }
  long newSize = title.length() + content.length() + 2;
  long delta = newSize - (long)oldSize;
  if (delta > 0 && !hasSpace(delta)) { redirectMsg("nospace"); return; }

  writeFile(fn, title, content);
  redirectMsg("ok");
}

void handleDelete() {
  if (server.hasArg("f")) {
    LittleFS.remove(pathOf(server.arg("f")));
  }
  redirectMsg("ok");
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/upload", HTTP_POST, handleUpload);
  server.on("/edit", HTTP_GET, handleEditPage);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/delete", HTTP_POST, handleDelete);

  server.on("/generate_204", HTTP_GET, handleRedirect);
  server.on("/gen_204", HTTP_GET, handleRedirect);
  server.on("/hotspot-detect.html", HTTP_GET, handleRedirect);
  server.on("/library/test/success.html", HTTP_GET, handleRedirect);
  server.on("/ncsi.txt", HTTP_GET, handleRedirect);
  server.on("/connecttest.txt", HTTP_GET, handleRedirect);
  server.on("/fwlink", HTTP_GET, handleRedirect);

  server.onNotFound(handleRedirect);
}