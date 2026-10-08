// FRC 75 marquee controller firmware
//
// Board:  Raspberry Pi Pico 2, Earle Philhower's arduino-pico core
//         Tools > Flash Size > "4MB (Sketch: 3MB, FS: 1MB)"  (the messages drive lives in that 1 MB)
// Libs:   Adafruit NeoPXL8, Adafruit GFX Library, RTClib
//
// Scrolls the lines of messages.txt across the 8 LED rows. Plug the board into a computer
// and it shows up as a USB drive; edit messages.txt, save, eject, and the new text scrolls.
// The current is held under ROW_LIMIT_MA per row and HALF_LIMIT_MA per power half, which is what
// the board's 1 oz copper traces can carry (IPC-2221, 10 C rise).
//
// Set the clock over USB serial (115200 baud):  T2026-10-08 09:30:00

#include <Adafruit_NeoPXL8.h>
#include <Adafruit_GFX.h>
#include <RTClib.h>
#include <Wire.h>
#include <FatFS.h>
#include <FatFSUSB.h>

// ---------- settings ----------
const int LEDS_PER_ROW = 150;          // pixels in each row
const int ROWS = 8;
const bool FLIP_ROW[ROWS] = {false, false, false, false, false, false, false, false};  // true if that strip's data enters from the right
const uint16_t SCROLL_MS = 30;         // ms per 1-pixel scroll step
const uint8_t BRIGHTNESS = 255;        // 0-255, applied to every colour
const uint32_t DEFAULT_COLOR = 0xFF0000;
const uint32_t ROW_LIMIT_MA = 3000;    // per-row cap: 1.5 mm row traces on 1 oz copper carry ~3.2 A
// Per-half cap {rows 1-4, rows 5-8}: ~2 mm input traces on 1 oz copper carry ~4 A.
// Half B's input crosses layers through single 0.3 mm vias, so it stays at 2000 until
// those spots get 6-8 vias each; then set it to 4000.
const uint32_t HALF_LIMIT_MA[2] = {4000, 2000};
const uint32_t MA_PER_CHANNEL = 20;    // WS2812B, one colour channel at full brightness
const uint32_t IDLE_MA_PER_LED = 1;    // WS2812B draw with all channels off
const int MAX_MESSAGES = 32;

int8_t pins[8] = {2, 3, 4, 5, 6, 7, 8, 9};  // GP2..GP9 = row 1 (top) .. row 8; must be 8 consecutive GPIOs
const int STATUS_LED = 22, SDA_PIN = 20, SCL_PIN = 21;
const char *MSG_FILE = "/messages.txt";
const char *DEFAULT_MESSAGES =
  "// One message per line. Lines starting with // are ignored.\n"
  "// Start a line with #RRGGBB to pick its colour, e.g.  #00FF00 GO TEAM\n"
  "// {TIME} and {DATE} are replaced with the current time and date.\n"
  "FRC TEAM 75\n"
  "#FFFFFF {TIME}\n";

// ---------- state ----------
Adafruit_NeoPXL8 leds(LEDS_PER_ROW, pins, NEO_GRB);
GFXcanvas1 canvas(LEDS_PER_ROW, ROWS);   // 1 canvas pixel = 1 LED; the 5x7 font fits the 8 rows
RTC_DS3231 rtc;
bool rtcOk = false;

String messages[MAX_MESSAGES];
uint32_t colors[MAX_MESSAGES];
int messageCount = 0, current = 0;
String text;               // current message with {TIME}/{DATE} filled in
uint32_t color;
int x;                     // left edge of the text, scrolls from LEDS_PER_ROW down to -width

volatile bool driveMounted = false, reloadNeeded = false, fileBusy = false;

// ---------- USB drive ----------
void onPlug(uint32_t) { driveMounted = true; FatFS.end(); }        // PC owns the drive now
void onUnplug(uint32_t) { driveMounted = false; reloadNeeded = true; }
bool driveReady(uint32_t) { return !fileBusy; }                   // don't hand over the drive mid-read

void loadMessages() {
  fileBusy = true;
  if (!FatFS.exists(MSG_FILE)) {
    File f = FatFS.open(MSG_FILE, "w");
    f.print(DEFAULT_MESSAGES);
    f.close();
  }
  messageCount = 0;
  File f = FatFS.open(MSG_FILE, "r");
  while (f && f.available() && messageCount < MAX_MESSAGES) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("//")) continue;
    uint32_t c = DEFAULT_COLOR;
    if (line.length() >= 7 && line[0] == '#') {
      c = strtoul(line.substring(1, 7).c_str(), nullptr, 16);
      line = line.substring(7);
      line.trim();
    }
    messages[messageCount] = line;
    colors[messageCount] = c;
    messageCount++;
  }
  if (f) f.close();
  fileBusy = false;
  if (messageCount == 0) { messages[0] = "NO MESSAGES"; colors[0] = DEFAULT_COLOR; messageCount = 1; }
  current = messageCount - 1;  // next startMessage() begins at the first line
}

// ---------- text ----------
String fillTokens(String s) {
  char t[12] = "--:--", d[16] = "";
  if (rtcOk) {
    DateTime now = rtc.now();
    int h = now.hour() % 12;
    snprintf(t, sizeof t, "%d:%02d %s", h ? h : 12, now.minute(), now.hour() < 12 ? "AM" : "PM");
    static const char *mon[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    snprintf(d, sizeof d, "%s %d", mon[now.month() - 1], now.day());
  }
  s.replace("{TIME}", t);
  s.replace("{DATE}", d);
  return s;
}

void startMessage() {
  current = (current + 1) % messageCount;
  text = fillTokens(messages[current]);
  color = colors[current];
  x = LEDS_PER_ROW;
}

// Lower scale (256 = full colour) so `units` (lit pixels x (r+g+b)) on `rows` rows stays under limitMa.
void cap(uint32_t &scale, uint32_t units, uint32_t limitMa, int rows) {
  uint32_t litMa = units * MA_PER_CHANNEL / 255;
  uint32_t budget = limitMa - IDLE_MA_PER_LED * LEDS_PER_ROW * rows;  // mA left after idle draw
  if (litMa > budget && budget * 256 / litMa < scale) scale = budget * 256 / litMa;
}

// Draw the text at position x and push it to the strips. One brightness scale covers the
// whole frame, so if a limit is hit every row dims evenly.
void drawFrame() {
  canvas.fillScreen(0);
  canvas.setCursor(x, 0);
  canvas.print(text);

  uint32_t r = ((color >> 16) & 0xFF) * BRIGHTNESS / 255;
  uint32_t g = ((color >> 8) & 0xFF) * BRIGHTNESS / 255;
  uint32_t b = (color & 0xFF) * BRIGHTNESS / 255;

  uint32_t scale = 256, half[2] = {0, 0};
  for (int row = 0; row < ROWS; row++) {
    uint32_t lit = 0;
    for (int col = 0; col < LEDS_PER_ROW; col++) lit += canvas.getPixel(col, row) ? 1 : 0;
    cap(scale, lit * (r + g + b), ROW_LIMIT_MA, 1);
    half[row / (ROWS / 2)] += lit * (r + g + b);
  }
  for (int h = 0; h < 2; h++) cap(scale, half[h], HALF_LIMIT_MA[h], ROWS / 2);

  uint32_t on = leds.Color(r * scale >> 8, g * scale >> 8, b * scale >> 8);
  for (int row = 0; row < ROWS; row++) {
    for (int col = 0; col < LEDS_PER_ROW; col++) {
      int led = FLIP_ROW[row] ? LEDS_PER_ROW - 1 - col : col;
      leds.setPixelColor(row * LEDS_PER_ROW + led, canvas.getPixel(col, row) ? on : 0);
    }
  }
  leds.show();
}

// ---------- clock setting over serial: T2026-10-08 09:30:00 ----------
void checkSerial() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  int Y, M, D, h, m, s;
  if (rtcOk && sscanf(cmd.c_str(), "T%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &s) == 6) {
    rtc.adjust(DateTime(Y, M, D, h, m, s));
    Serial.println("Clock set.");
  } else {
    Serial.println("Set the clock with: T2026-10-08 09:30:00");
  }
}

void fail(int ms) {  // blink the status LED forever; ms tells you what failed
  while (true) { digitalWrite(STATUS_LED, !digitalRead(STATUS_LED)); delay(ms); }
}

void setup() {
  pinMode(STATUS_LED, OUTPUT);
  Serial.begin(115200);

  if (!leds.begin()) fail(100);           // fast blink: LED driver didn't start
  leds.show();                            // all off

  Wire.setSDA(SDA_PIN);
  Wire.setSCL(SCL_PIN);
  Wire.begin();
  rtcOk = rtc.begin(&Wire);
  if (rtcOk && rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));  // first boot / dead coin cell

  if (!FatFS.begin()) fail(300);          // medium blink: no filesystem, check Tools > Flash Size
  loadMessages();
  FatFSUSB.onPlug(onPlug);
  FatFSUSB.onUnplug(onUnplug);
  FatFSUSB.driveReady(driveReady);
  FatFSUSB.begin();

  canvas.setTextWrap(false);
  canvas.setTextColor(1);
  startMessage();
}

void loop() {
  static uint32_t lastStep = 0;

  if (reloadNeeded && !driveMounted) {    // the computer ejected the drive: pick up the edited file
    reloadNeeded = false;
    FatFS.begin();
    loadMessages();
    startMessage();
  }

  if (millis() - lastStep >= SCROLL_MS) {
    lastStep = millis();
    if (--x < -(int)text.length() * 6) startMessage();  // default font is 6 px per character
    drawFrame();
  }

  digitalWrite(STATUS_LED, (millis() / 1000) % 2);  // slow heartbeat: 1 s on, 1 s off
  checkSerial();
}
