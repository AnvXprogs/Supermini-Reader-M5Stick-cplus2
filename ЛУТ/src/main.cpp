// Supermini Reader 2.0 — M5StickC Plus2 port
//
// Display : ST7789 135x240, used in landscape (rotation 1) => 240x135 px
// Font    : efontJA_10 (u8g2), the only bundled font with Cyrillic glyphs
// Buttons : BtnA = down/next, BtnB = up/prev, BtnPWR (side) = OK / hold = WiFi
// Storage : LittleFS, one file per note: first line = title, rest = body

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <vector>
#include <algorithm>

// ---------------------------------------------------------------- layout ----

// efontJA_16 is a 16px cell font; 18px of line pitch keeps descenders clear.
static const int LINE_H = 18;
static const int TEXT_X = 2;
static const int TEXT_Y = 2;

// Filled in by computeLayout() once the panel geometry is known.
static int SCREEN_ROWS = 8;
static int CHARS_PER_LINE = 21;

// --------------------------------------------------------------- network ----

const char *AP_SSID = "Esp hpora";
const char *AP_PASS = "12345678";
IPAddress apIP(192, 168, 4, 1);
DNSServer dnsServer;
WebServer server(80);
bool wifiActive = false;

// ----------------------------------------------------------------- state ----

enum AppState { ST_LIST, ST_VIEW, ST_WIFI };
AppState state = ST_LIST;

struct FileEntry { String fname; String title; };
std::vector<FileEntry> fileEntries;

int selected = 0;
int listStart = 0;

String currentFileName = "";
std::vector<uint32_t> pageOffsets;
bool currentFileEmpty = false;
int page = 0;

uint8_t brightness = 128;
const uint8_t BRIGHTNESS_STEP = 16;
String lastFileName = "";

bool settingsDirty = false;
unsigned long settingsChangedAt = 0;
const unsigned long SETTINGS_SAVE_DELAY = 800;

const char *SETTINGS_PATH = "/_cfg.txt";

int scrollPos = 0;
unsigned long scrollTimer = 0;
const unsigned long SCROLL_INTERVAL = 300;
int lastScrollIdx = -1;

const unsigned long LONGPRESS_MS = 600;
const unsigned long REPEAT_START_MS = 500;
const unsigned long REPEAT_MS = 180;

// Repeat-on-hold is layered on top of M5Unified's own debounced button state.
struct Rpt { unsigned long tRepeat; bool armed; };
Rpt rptA = {0, false}, rptB = {0, false};

struct FileData { String title; String content; };

// ----------------------------------------------------------- prototypes ----

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
void handleFileUpload();
void setupWebServer();

// ------------------------------------------------------------ layout fit ----

// Measure the widest glyph we actually render (Cyrillic 'Ш' / Latin 'W') so the
// character-grid constants match the real panel instead of being hardcoded.
static void computeLayout() {
  M5.Display.setFont(&fonts::efontJA_16);
  M5.Display.setTextSize(1);

  int w = M5.Display.textWidth("W");
  int wCyr = M5.Display.textWidth("Ш");
  if (wCyr > w) w = wCyr;
  if (w < 1) w = 6;

  int usableW = M5.Display.width() - TEXT_X * 2;
  int usableH = M5.Display.height() - TEXT_Y;

  CHARS_PER_LINE = usableW / w;
  if (CHARS_PER_LINE < 8) CHARS_PER_LINE = 8;

  SCREEN_ROWS = usableH / LINE_H;
  if (SCREEN_ROWS < 1) SCREEN_ROWS = 1;

  Serial.printf("layout: %dx%d px, glyph %dpx -> %d cols x %d rows\n",
                M5.Display.width(), M5.Display.height(), w,
                CHARS_PER_LINE, SCREEN_ROWS);
}

// ----------------------------------------------------------------- setup ----

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.clear_display = true;
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  cfg.internal_mic = false;
  cfg.internal_spk = false;
  // If the ST7789 probe fails, still come up as a Plus2 rather than as an
  // AtomS3Lite (the ESP32 default fallback), which has no display at all.
  cfg.fallback_board = m5gfx::board_t::board_M5StickCPlus2;
  M5.begin(cfg);

  Serial.printf("board id = %d\n", (int)M5.getBoard());

  M5.Display.setRotation(1);
  M5.Display.setBrightness(brightness);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextWrap(false);
  M5.Display.setFont(&fonts::efontJA_16);
  M5.Display.setTextSize(1);
  M5.Display.cp437(false);
  computeLayout();

  randomSeed(micros());

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    M5.Display.drawString("FS error", TEXT_X, TEXT_Y);
  }

  WiFi.mode(WIFI_OFF);
  setupWebServer();
  refreshFileList();
  loadSettings();

  M5.Display.setBrightness(brightness);

  if (lastFileName.length() > 0) {
    for (size_t i = 0; i < fileEntries.size(); i++) {
      if (fileEntries[i].fname == lastFileName) { selected = (int)i; break; }
    }
  }

  drawScreen();
}

// ------------------------------------------------------------------ loop ----

void loop() {
  M5.update();

  if (wifiActive) {
    dnsServer.processNextRequest();
    server.handleClient();
  }

  handleButtons();

  if (state == ST_LIST && !fileEntries.empty()) {
    const String &title = fileEntries[selected].title;
    if (utf8Length(title) > CHARS_PER_LINE - 2 &&
        millis() - scrollTimer > SCROLL_INTERVAL) {
      scrollTimer = millis();
      scrollPos++;
      drawScreen();
    }
  }

  maybeSaveSettings();
  delay(5);
}

// --------------------------------------------------------------- buttons ----

// Returns true on the initial press and then on each repeat tick while held.
static bool pressOrRepeat(m5::Button_Class &btn, Rpt &r) {
  if (btn.wasPressed()) {
    r.tRepeat = millis();
    r.armed = true;
    return true;
  }
  if (!btn.isPressed()) {
    r.armed = false;
    return false;
  }
  if (!r.armed) return false;
  unsigned long held = millis() - r.tRepeat;
  if (held >= REPEAT_START_MS && (held - REPEAT_START_MS) % REPEAT_MS < 6) {
    r.tRepeat = millis() - REPEAT_START_MS;
    return true;
  }
  return false;
}

void handleButtons() {
  // BtnPWR (top/side) = down/next, BtnB (front) = up/prev, BtnA = OK.
  bool up = pressOrRepeat(M5.BtnB, rptB);
  bool down = pressOrRepeat(M5.BtnPWR, rptA);
  bool okShort = M5.BtnA.wasReleased() &&
                 !M5.BtnA.wasReleaseFor(LONGPRESS_MS);
  bool okLong = M5.BtnA.wasReleaseFor(LONGPRESS_MS);

  switch (state) {
    case ST_LIST:
      if (up) moveSelection(-1);
      if (down) moveSelection(1);
      if (okShort) openSelectedFile();
      if (okLong) toggleWifi();
      break;

    case ST_VIEW:
      if (up) prevPage();
      if (down) nextPage();
      if (okShort) { state = ST_LIST; drawScreen(); }
      if (okLong) toggleWifi();
      break;

    case ST_WIFI:
      if (up) changeBrightness(BRIGHTNESS_STEP);
      if (down) changeBrightness(-BRIGHTNESS_STEP);
      if (okShort) {
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setCursor(TEXT_X, TEXT_Y + LINE_H * 3);
        const char* msg = "Выключение...";
        for (size_t i = 0; i < strlen(msg); i++) M5.Display.write((uint8_t)msg[i]);
        delay(500);
        M5.Power.powerOff();
      }
      if (okLong) toggleWifi();
      break;
  }
}

// ---------------------------------------------------------------- actions ----

void moveSelection(int dir) {
  int total = (int)fileEntries.size();
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

void nextPage() {
  if (page < (int)pageOffsets.size() - 1) { page++; drawScreen(); }
}

void changeBrightness(int delta) {
  int v = constrain((int)brightness + delta, 10, 255);
  brightness = (uint8_t)v;
  M5.Display.setBrightness(brightness);
  markSettingsDirty();
  drawScreen();
}

void toggleWifi() {
  wifiActive = !wifiActive;

  if (wifiActive) {
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    if (strlen(AP_PASS) >= 8) {
      WiFi.softAP(AP_SSID, AP_PASS);
    } else {
      WiFi.softAP(AP_SSID);  // open network when no usable password is set
    }
    // Keep radio power low; the phone is centimetres away and the Plus2 runs
    // off a 200 mAh cell. Must be called after softAP() or it is overwritten.
    WiFi.setTxPower(WIFI_POWER_2dBm);
    dnsServer.start(53, "*", apIP);
    server.begin();
    state = ST_WIFI;
  } else {
    server.stop();
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    state = ST_LIST;
    refreshFileList();
    if (selected >= (int)fileEntries.size()) selected = 0;
  }
  drawScreen();
}

// --------------------------------------------------------------- drawing ----

void drawScreen() {
  M5.Display.fillScreen(TFT_BLACK);
  switch (state) {
    case ST_LIST: drawList(); break;
    case ST_VIEW: drawView(); break;
    case ST_WIFI: drawWifiInfo(); break;
  }
}

void drawList() {
  const int VIS = SCREEN_ROWS;
  const int MARGIN = 1;
  const int WIDTH = CHARS_PER_LINE - 2;
  int total = (int)fileEntries.size();

  if (total == 0) {
    M5.Display.setCursor(TEXT_X, TEXT_Y + LINE_H);
    for (size_t i = 0; i < strlen("Нет файлов"); i++) {
      M5.Display.write((uint8_t)"Нет файлов"[i]);
    }
    return;
  }

  if (selected < listStart + MARGIN) {
    listStart = max(0, selected - MARGIN);
  }
  if (selected > listStart + VIS - 1 - MARGIN) {
    listStart = selected - VIS + 1 + MARGIN;
  }

  int maxStart = max(0, total - VIS);
  listStart = constrain(listStart, 0, maxStart);

  for (int row = 0; row < VIS && (listStart + row) < total; row++) {
    int idx = listStart + row;
    const String &title = fileEntries[idx].title;
    int titleChars = utf8Length(title);
    String prefix = (idx == selected) ? "> " : "  ";
    String shown;

    if (titleChars <= WIDTH) {
      shown = title;
    } else if (idx == selected) {
      if (lastScrollIdx != selected) {
        scrollPos = 0;
        lastScrollIdx = selected;
      }
      shown = marqueeText(title, WIDTH, scrollPos);
    } else {
      shown = utf8Substring(title, 0, WIDTH);
    }

    M5.Display.setCursor(TEXT_X, TEXT_Y + row * LINE_H);
    String line = prefix + shown;
    for (size_t i = 0; i < line.length(); i++) {
      M5.Display.write((uint8_t)line[i]);
    }
  }
}

void drawView() {
  if (currentFileEmpty || currentFileName.length() == 0) {
    M5.Display.setCursor(TEXT_X, TEXT_Y + LINE_H);
    M5.Display.print("Пусто");
    return;
  }

  File f = LittleFS.open(pathOf(currentFileName), "r");
  if (!f) {
    M5.Display.setCursor(TEXT_X, TEXT_Y + LINE_H);
    M5.Display.print("Ошибка чтения");
    return;
  }

  uint32_t offset =
      (page >= 0 && page < (int)pageOffsets.size()) ? pageOffsets[page] : 0;
  f.seek(offset);
  lsReset();

  String line;
  uint32_t dummy;
  for (int row = 0; row < SCREEN_ROWS; row++) {
    if (!lsNextLine(f, line, dummy)) break;
    M5.Display.setCursor(TEXT_X, TEXT_Y + row * LINE_H);
    // Print character by character to avoid formatting issues with % and other control chars
    for (size_t i = 0; i < line.length(); i++) {
      M5.Display.write((uint8_t)line[i]);
    }
  }
  f.close();
}

void drawWifiInfo() {
  M5.Display.setCursor(TEXT_X, TEXT_Y);
  const char* s1 = "WiFi: ";
  for (size_t i = 0; i < strlen(s1); i++) M5.Display.write((uint8_t)s1[i]);
  for (size_t i = 0; i < strlen(AP_SSID); i++) M5.Display.write((uint8_t)AP_SSID[i]);

  M5.Display.setCursor(TEXT_X, TEXT_Y + LINE_H * 2);
  const char* s2 = "IP: ";
  for (size_t i = 0; i < strlen(s2); i++) M5.Display.write((uint8_t)s2[i]);
  String ip = WiFi.softAPIP().toString();
  for (size_t i = 0; i < ip.length(); i++) M5.Display.write((uint8_t)ip[i]);

  M5.Display.setCursor(TEXT_X, TEXT_Y + LINE_H * 4);
  const char* s3 = "Pass: ";
  for (size_t i = 0; i < strlen(s3); i++) M5.Display.write((uint8_t)s3[i]);
  const char* pass = strlen(AP_PASS) >= 8 ? AP_PASS : "(open)";
  for (size_t i = 0; i < strlen(pass); i++) M5.Display.write((uint8_t)pass[i]);

  M5.Display.setCursor(TEXT_X, TEXT_Y + LINE_H * 6);
  const char* s4 = "Яркость: ";
  for (size_t i = 0; i < strlen(s4); i++) M5.Display.write((uint8_t)s4[i]);
  String br = String(brightness);
  for (size_t i = 0; i < br.length(); i++) M5.Display.write((uint8_t)br[i]);

  M5.Display.setCursor(TEXT_X, TEXT_Y + LINE_H * 8);
  const char* s5 = "[A] Выключить";
  for (size_t i = 0; i < strlen(s5); i++) M5.Display.write((uint8_t)s5[i]);
}

// ----------------------------------------------------------- UTF-8 utils ----

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

// ------------------------------------------------------- line-wrap reader ----

bool lsHavePending = false;
String lsPendingWord = "";
bool lsPendingBreak = false;

void lsReset() {
  lsHavePending = false;
  lsPendingWord = "";
  lsPendingBreak = false;
}

// Reads a word from the file; a word is delimited by space, newline, or max width.
// isBreak is set to true if the delimiter was a newline.
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

// Returns the next display line, wrapping words at CHARS_PER_LINE.
// lineStartPos is set to the file offset where this line's first word starts.
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
      lineStartPos = f.position() - lsPendingWord.length() -
                     (lsPendingBreak ? 1 : 0);
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

// --------------------------------------------------- page index builder ----

// Scans the file, breaking it into pages of SCREEN_ROWS lines each.
void buildPageIndex(const String &fname) {
  pageOffsets.clear();
  currentFileName = fname;
  currentFileEmpty = false;

  File f = LittleFS.open(pathOf(fname), "r");
  if (!f) {
    currentFileEmpty = true;
    pageOffsets.push_back(0);
    return;
  }

  // First line is the title; skip it.
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

// ------------------------------------------------------------ settings ----

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
    if (v >= 10 && v <= 255) brightness = (uint8_t)v;
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

// ----------------------------------------------------------- file utils ----

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
      if (title.length() == 0) title = fname;
      fileEntries.push_back({fname, title});
    }
    f = root.openNextFile();
  }

  // Sort newest first by numeric ID embedded in the filename.
  std::sort(fileEntries.begin(), fileEntries.end(),
            [](const FileEntry &a, const FileEntry &b) {
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

// --------------------------------------------------------------- web UI ----

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
      "a{color:#4caf50;text-decoration:none}");
}

void handleRoot() {
  refreshFileList();
  size_t total = LittleFS.totalBytes();
  size_t used = LittleFS.usedBytes();
  int pct = total ? (int)(used * 100 / total) : 0;

  String html = F(
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<title>ESP Шпаргалка</title><style>");
  html += pageStyle();
  html += F("</style></head><body>");

  html += "<h2>ESP Шпаргалка</h2>";
  html += "<div>Занято: " + String(used) + " / " + String(total) + " байт (" +
          String(pct) + "%)</div>";
  html += "<div class='bar'><div class='fill' style='width:" + String(pct) +
          "%'></div></div>";

  if (server.hasArg("msg")) {
    String msg = server.arg("msg");
    if (msg == "nospace")
      html += "<p class='msg-err'>Не хватает места!</p>";
    if (msg == "empty")
      html += "<p class='msg-err'>Введите заголовок/Файл слишком большой</p>";
    if (msg == "ok") html += "<p class='msg-ok'>Сохранено</p>";
    if (msg == "upload_ok")
      html += "<p class='msg-ok'>Файл успешно загружен!</p>";
    if (msg == "upload_err")
      html += "<p class='msg-err'>Ошибка загрузки файла</p>";
  }

  html += F(
      "<h3>Загрузить файл</h3>"
      "<form method='POST' action='/fileupload' enctype='multipart/form-data'>"
      "<input type='file' name='file' accept='.txt' required>"
      "<button type='submit'>Загрузить файл</button>"
      "</form>");

  html += F(
      "<h3>Создать новый файл</h3>"
      "<form method='POST' action='/upload'>"
      "<input type='text' name='title' placeholder='Заголовок' required>"
      "<textarea name='content' placeholder='Содержимое'></textarea>"
      "<button type='submit'>Создать</button>"
      "</form>");

  html += "<h3>Файлы</h3>";
  if (fileEntries.empty()) {
    html += "<p class='hint'>Нет файлов</p>";
  } else {
    for (auto &e : fileEntries) {
      html += "<div class='item'><div class='title'>" + htmlEscape(e.title) +
              "</div>"
              "<div class='row'>"
              "<form method='GET' action='/edit'>"
              "<input type='hidden' name='f' value='" +
              e.fname +
              "'>"
              "<button class='edit' type='submit'>Открыть/Изменить</button></form>"
              "<form method='POST' action='/delete' onsubmit=\"return confirm('Удалить файл?')\">"
              "<input type='hidden' name='f' value='" +
              e.fname +
              "'>"
              "<button class='del' type='submit'>Удалить</button></form>"
              "</div></div>";
    }
  }

  html += "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

// ---------------------------------------------------------- web handlers ----

void handleUpload() {
  String title = server.arg("title");
  String content = server.arg("content");
  title.trim();
  if (title.length() == 0) {
    redirectMsg("empty");
    return;
  }

  long extra = title.length() + content.length() + 8;
  if (!hasSpace(extra)) {
    redirectMsg("nospace");
    return;
  }

  String fn = genFilename();
  writeFile(fn, title, content);
  redirectMsg("ok");
}

void handleEditPage() {
  if (!server.hasArg("f")) {
    handleRedirect();
    return;
  }
  String fn = server.arg("f");
  if (!LittleFS.exists(pathOf(fn))) {
    handleRedirect();
    return;
  }

  FileData d = readFile(fn);

  String html = F(
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<title>Изменить файл</title><style>");
  html += pageStyle();
  html += F("</style></head><body>");
  html += "<h2>Изменить файл</h2>";
  html += "<form method='POST' action='/save'>"
          "<input type='hidden' name='f' value='" +
          fn +
          "'>"
          "<input type='text' name='title' value='" +
          htmlEscape(d.title) +
          "' required>"
          "<textarea name='content'>" +
          htmlEscape(d.content) +
          "</textarea>"
          "<button type='submit'>Сохранить</button>"
          "</form>"
          "<form method='POST' action='/delete' onsubmit=\"return confirm('Удалить файл?')\">"
          "<input type='hidden' name='f' value='" +
          fn +
          "'>"
          "<button class='del' type='submit'>Удалить</button></form>"
          "<p><a href='/'>&larr; Назад к списку</a></p></body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

void handleSave() {
  String fn = server.arg("f");
  String title = server.arg("title");
  String content = server.arg("content");
  title.trim();

  if (title.length() == 0 || !LittleFS.exists(pathOf(fn))) {
    redirectMsg("empty");
    return;
  }

  size_t oldSize = 0;
  {
    File f = LittleFS.open(pathOf(fn), "r");
    if (f) {
      oldSize = f.size();
      f.close();
    }
  }
  long newSize = title.length() + content.length() + 2;
  long delta = newSize - (long)oldSize;
  if (delta > 0 && !hasSpace(delta)) {
    redirectMsg("nospace");
    return;
  }

  writeFile(fn, title, content);
  redirectMsg("ok");
}

void handleDelete() {
  if (server.hasArg("f")) {
    LittleFS.remove(pathOf(server.arg("f")));
  }
  redirectMsg("ok");
}

void handleFileUpload() {
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    int lastSlash = filename.lastIndexOf('/');
    if (lastSlash != -1) {
      filename = filename.substring(lastSlash + 1);
    }
    if (!filename.endsWith(".txt")) {
      filename += ".txt";
    }
    String fullPath = genFilename();
    upload.filename = fullPath;

    File f = LittleFS.open(fullPath, "w");
    if (f) f.close();
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    File f = LittleFS.open(upload.filename, "a");
    if (f) {
      f.write(upload.buf, upload.currentSize);
      f.close();
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    File f = LittleFS.open(upload.filename, "r");
    if (!f) {
      redirectMsg("upload_err");
      return;
    }

    String firstLine = f.readStringUntil('\n');
    firstLine.trim();
    String content = f.readString();
    f.close();

    if (firstLine.length() == 0) {
      String filename = upload.filename;
      int lastSlash = filename.lastIndexOf('/');
      if (lastSlash != -1) {
        filename = filename.substring(lastSlash + 1);
      }
      int dotPos = filename.lastIndexOf('.');
      if (dotPos != -1) {
        filename = filename.substring(0, dotPos);
      }

      File f2 = LittleFS.open(upload.filename, "w");
      if (f2) {
        f2.println(filename);
        f2.print(content);
        f2.close();
      }
    } else {
      if (firstLine.length() > 100 || firstLine.indexOf(' ') != -1) {
        String filename = upload.filename;
        int lastSlash = filename.lastIndexOf('/');
        if (lastSlash != -1) {
          filename = filename.substring(lastSlash + 1);
        }
        int dotPos = filename.lastIndexOf('.');
        if (dotPos != -1) {
          filename = filename.substring(0, dotPos);
        }

        File f2 = LittleFS.open(upload.filename, "w");
        if (f2) {
          f2.println(filename);
          f2.println(firstLine);
          f2.print(content);
          f2.close();
        }
      }
    }

    refreshFileList();
    redirectMsg("upload_ok");
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/upload", HTTP_POST, handleUpload);
  server.on("/edit", HTTP_GET, handleEditPage);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/delete", HTTP_POST, handleDelete);

  server.on("/fileupload", HTTP_POST,
            []() { redirectMsg("upload_ok"); }, handleFileUpload);

  server.on("/generate_204", HTTP_GET, handleRedirect);
  server.on("/gen_204", HTTP_GET, handleRedirect);
  server.on("/hotspot-detect.html", HTTP_GET, handleRedirect);
  server.on("/library/test/success.html", HTTP_GET, handleRedirect);
  server.on("/ncsi.txt", HTTP_GET, handleRedirect);
  server.on("/connecttest.txt", HTTP_GET, handleRedirect);
  server.on("/fwlink", HTTP_GET, handleRedirect);

  server.onNotFound(handleRedirect);
}
