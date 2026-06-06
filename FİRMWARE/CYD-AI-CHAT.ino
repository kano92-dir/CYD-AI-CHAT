/* =====================================================================
   CYD AI Chat  v2.0  --  ESP32-2432S028R (Cheap Yellow Display)
   ---------------------------------------------------------------------
   Yatay yapay zeka sohbet uygulamasi.
     * OpenRouter API (anahtar kodda) + kod-ici otomatik model yedekleme
     * Acilis -> WiFi tarama -> sifre (klavye) -> SOHBET
     * Telefon benzeri ekran klavyesi + Turkce karakterler
     * Yeni arayuz: avatarlar, baloncuklar, cizilmis ikonlar
     * SOHBET HAFIZASI: konusma NVS'e kaydolur, acilista devam eder
     * KISILIK (persona) secimi, PARLAKLIK ayari, RGB LED durum isigi
   ---------------------------------------------------------------------
   ILK KURULUM: 1) Asagidaki API_KEY satirina kendi anahtarini yapistir.
                2) TFT_eSPI icindeki User_Setup.h'yi proje dosyasiyla degistir.
   KUTUPHANELER: TFT_eSPI, XPT2046_Touchscreen, ArduinoJson v7
   KART: "ESP32 Dev Module"
   ===================================================================== */

#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <vector>
#include <algorithm>

#define APP_VERSION "v2.0"

// =====================================================================
//  >>>>>  BURAYA KENDI OPENROUTER ANAHTARINI YAZ  <<<<<
// =====================================================================
static const char* API_KEY = "YOUR-OPENROUTER-APİ-KEY";

// =====================================================================
//  MODELLER  (hepsi su an ucretsiz; biri 429 verirse kod sonrakini dener)
// =====================================================================
struct ModelOpt { const char* name; const char* slug; };
ModelOpt MODELS[] = {
  { "Gemma 4 31B",  "google/gemma-4-31b-it:free"     },
  { "Gemma 4 26B",  "google/gemma-4-26b-a4b-it:free" },
  { "GPT-OSS 120B", "openai/gpt-oss-120b:free"       },
  { "GPT-OSS 20B",  "openai/gpt-oss-20b:free"        },
  { "Kimi K2.6",    "moonshotai/kimi-k2.6:free"      },
  { "Qwen3 Coder",  "qwen/qwen3-coder:free"          },
};
const int MODEL_COUNT = sizeof(MODELS) / sizeof(MODELS[0]);

// =====================================================================
//  KISILIKLER (persona) - sistem promptu
// =====================================================================
struct Persona { const char* name; const char* prompt; };
Persona PERSONAS[] = {
  { "Genel",    "You are a helpful assistant. Answer clearly, correctly and concisely." },
  { "Kisa&Net", "Answer very briefly and directly. No unnecessary explanation." },
  { "Ogretmen", "Explain patiently and clearly like a teacher, with simple examples." },
  { "Kodcu",    "You are a software assistant. Give short code examples and technical explanations." },
  { "Arkadas",  "Talk like a warm, witty and friendly chat buddy." },
};
const int PERSONA_COUNT = sizeof(PERSONAS) / sizeof(PERSONAS[0]);

// =====================================================================
//  DILLER  - AI bu dilde cevap verir
// =====================================================================
struct Lang { const char* name; const char* directive; };
Lang LANGS[] = {
  { "Turkce",  "Always respond ONLY in Turkish, regardless of the user's language." },
  { "English", "Always respond ONLY in English, regardless of the user's language." },
  { "Russian", "Always respond ONLY in Russian using Cyrillic script, regardless of the user's language." },
};
const int LANG_COUNT = sizeof(LANGS) / sizeof(LANGS[0]);

// =====================================================================
//  GENEL AYARLAR
// =====================================================================
static const char* OPENROUTER_URL = "https://openrouter.ai/api/v1/chat/completions";
static const int   MAX_TOKENS      = 400;
static const int   API_HISTORY     = 6;
static const int   HTTP_TIMEOUT_MS = 30000;

#define TFT_INVERT_COLORS true
#define SCREEN_ROTATION   1     // ekran dikey/yarim gorunuyorsa 3, sonra 0, sonra 2 dene

// Dokunmatik kalibrasyon (yatay)
#define TOUCH_DEBUG   0
#define TOUCH_MIN_X 200
#define TOUCH_MAX_X 3900
#define TOUCH_MIN_Y 200
#define TOUCH_MAX_Y 3900

// Dokunmatik SPI (CYD sabit)
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

// RGB LED (CYD sabit, aktif LOW)
#define LED_R 4
#define LED_G 16
#define LED_B 17

#define SCR_W 320
#define SCR_H 240

// =====================================================================
//  RENKLER (RGB565) - modern koyu tema
// =====================================================================
#define C_BG       0x0861
#define C_HEADER   0x18E3
#define C_ACCENT   0x051F
#define C_ACCENT2  0x05E8
#define C_USER_BUB 0x021F
#define C_AI_BUB   0x31A6
#define C_ERR_BUB  0x9000
#define C_TEXT     0xFFFF
#define C_DIM      0x8410
#define C_KEY      0x2945
#define C_KEY_HI   0x051F
#define C_KEY_SP   0x4208
#define C_FIELD    0x0000
#define C_SEL      0x03E0
#define C_GREEN    0x07E0
#define C_RED      0xF800

// =====================================================================
//  NESNELER
// =====================================================================
TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
Preferences prefs;

// =====================================================================
//  DURUMLAR
// =====================================================================
enum AppState { ST_WIFI_LIST, ST_KEYBOARD, ST_CONNECTING, ST_CHAT, ST_SETTINGS, ST_MODEL };
AppState state = ST_WIFI_LIST;
bool needRedraw = true;

enum KbTarget { KT_WIFI_PASS, KT_CHAT };
KbTarget kbTarget = KT_CHAT;
enum KbLayer { L_LOWER, L_UPPER, L_SYM, L_TR };
KbLayer kbLayer = L_LOWER;

// =====================================================================
//  GLOBAL VERILER
// =====================================================================
String inputText = "";
String wifiSSID  = "";
String wifiPass  = "";
int    selModel  = 0;
int    selPersona = 0;
int    selLang   = 0;
int    brightness = 210;     // 25..255
String lastUsed  = "";

enum Role { R_USER, R_AI, R_ERR };
struct Msg { Role role; String text; };
std::vector<Msg> messages;
const size_t MAX_MESSAGES = 30;

struct Net { String ssid; int rssi; bool secure; };
std::vector<Net> nets;
int wifiPage = 0;
const int NETS_PER_PAGE = 5;
int modelPage = 0;
const int MODELS_PER_PAGE = 5;

int chatScroll = 0;
int chatContentH = 0;

bool   sendPending = false;
String pendingSend = "";

struct Touch { bool down; int x; int y; };
struct KeyBox { int x, y, w, h; String label; String value; int special; };
std::vector<KeyBox> keys;

const int KB_TOP = 42;
const int ROW_H  = 49;
const int KGAP   = 2;
const char* rowsLower[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
const char* rowsUpper[3] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
const char* rowsSym[3]   = { "1234567890", "-_./:;,@#", "()&%+=?"  };

const int CHAT_HEADER_H = 28;
const int CHAT_INPUT_H  = 36;
const int QUICK_H       = 26;                                  // hizli komut cubugu
const int QUICK_Y       = SCR_H - CHAT_INPUT_H - QUICK_H;
const int CHAT_AREA_Y   = CHAT_HEADER_H;
const int CHAT_AREA_H   = SCR_H - CHAT_HEADER_H - CHAT_INPUT_H - QUICK_H;

// Hizli komutlar (etiket + hazir prompt; prompt gercek Turkce UTF-8)
const char* QUICK_LBL[4] = { "Cevir", "Ozetle", "Duzelt", "Kisalt" };
const char* QUICK_PRE[4] = {
  "\u015Eunu \u0130ngilizce'ye \u00E7evir: ",
  "\u015Eunu k\u0131saca \u00F6zetle: ",
  "\u015Eu metnin yaz\u0131m ve dil bilgisi hatalar\u0131n\u0131 d\u00FCzelt: ",
  "\u015Eunu daha k\u0131sa ve \u00F6z bi\u00E7imde yeniden yaz: "
};

// =====================================================================
//  RGB LED + PARLAKLIK
// =====================================================================
void led(bool r, bool g, bool b) {
  digitalWrite(LED_R, r ? LOW : HIGH);
  digitalWrite(LED_G, g ? LOW : HIGH);
  digitalWrite(LED_B, b ? LOW : HIGH);
}
void applyBrightness() { analogWrite(TFT_BL, brightness); }

// =====================================================================
//  SAAT / TARIH (NTP, Turkiye UTC+3)
// =====================================================================
const char* TR_MON[12] = {"Oca","Sub","Mar","Nis","May","Haz","Tem","Agu","Eyl","Eki","Kas","Ara"};
const char* TR_DOW[7]  = {"Paz","Pzt","Sal","Car","Per","Cum","Cmt"};  // tm_wday: 0=Pazar
unsigned long lastClock = 0;

void syncTime() { configTime(10800, 0, "pool.ntp.org", "time.google.com"); }  // +3 saat, yaz saati yok

String clockStr() {
  struct tm ti;
  if (!getLocalTime(&ti, 50)) return "--:--";
  char b[6]; snprintf(b, sizeof(b), "%02d:%02d", ti.tm_hour, ti.tm_min);
  return String(b);
}
String dateStr() {
  struct tm ti;
  if (!getLocalTime(&ti, 50)) return "";
  return String(TR_DOW[ti.tm_wday]) + " " + String(ti.tm_mday) + " " + String(TR_MON[ti.tm_mon]);
}

String greetingMsg() {
  if (selLang == 1) return "Hello! How can I help you today?";
  if (selLang == 2) return "\u041F\u0440\u0438\u0432\u0435\u0442! \u0427\u0435\u043C \u043C\u043E\u0433\u0443 \u043F\u043E\u043C\u043E\u0447\u044C?";
  return "Merhaba! Sana nasil yardimci olabilirim?";
}

// =====================================================================
//  EKRAN SADELESTIRME (Turkce + Kiril/Rusca -> Latin)
//  Gomulu font sadece ASCII cizebildigi icin Turkce ve Rusca harfler
//  EKRANDA Latin'e cevrilir. AI'ya giden/gelen metin gercek UTF-8 kalir.
// =====================================================================
const char* CYR_UP[32] = {"A","B","V","G","D","E","Zh","Z","I","Y","K","L","M","N","O","P",
                          "R","S","T","U","F","H","Ts","Ch","Sh","Sch","","Y","","E","Yu","Ya"};
const char* CYR_LO[32] = {"a","b","v","g","d","e","zh","z","i","y","k","l","m","n","o","p",
                          "r","s","t","u","f","h","ts","ch","sh","sch","","y","","e","yu","ya"};

const char* cpToLatin(int cp) {
  switch (cp) {
    case 0x00E7: return "c"; case 0x00C7: return "C";
    case 0x00F6: return "o"; case 0x00D6: return "O";
    case 0x00FC: return "u"; case 0x00DC: return "U";
    case 0x011F: return "g"; case 0x011E: return "G";
    case 0x0131: return "i"; case 0x0130: return "I";
    case 0x015F: return "s"; case 0x015E: return "S";
    case 0x00E2: return "a"; case 0x00EE: return "i"; case 0x00FB: return "u"; case 0x00E9: return "e";
    case 0x0401: return "Yo"; case 0x0451: return "yo";
  }
  if (cp >= 0x410 && cp <= 0x42F) return CYR_UP[cp - 0x410];
  if (cp >= 0x430 && cp <= 0x44F) return CYR_LO[cp - 0x430];
  return "?";
}

String foldTR(const String& in) {
  String out; out.reserve(in.length() + 8);
  int i = 0, n = in.length();
  while (i < n) {
    uint8_t c = (uint8_t)in[i];
    if (c < 0x80) { out += (char)c; i++; continue; }
    if (c >= 0xC2 && c <= 0xDF && i + 1 < n) {      // 2 baytli (Turkce + Kiril)
      uint8_t c2 = (uint8_t)in[i + 1];
      int cp = ((c & 0x1F) << 6) | (c2 & 0x3F);
      out += cpToLatin(cp);
      i += 2;
    } else if (c >= 0xE0) { out += "?"; i += 3; }   // 3 baytli: atla
    else { out += "?"; i += 2; }
  }
  return out;
}

void delLastChar() {
  int n = inputText.length();
  if (n == 0) return;
  int i = n - 1;
  while (i > 0 && ((uint8_t)inputText[i] & 0xC0) == 0x80) i--;
  inputText.remove(i);
}

bool apiKeyOk() {
  String k = API_KEY; k.trim();
  return k.startsWith("sk-or-v1-") && k.length() > 20;
}

// =====================================================================
//  DOKUNMA
// =====================================================================
Touch readTouch() {
  Touch t = {false, 0, 0};
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    int x = constrain(map(p.x, TOUCH_MIN_X, TOUCH_MAX_X, 0, SCR_W), 0, SCR_W - 1);
    int y = constrain(map(p.y, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, SCR_H), 0, SCR_H - 1);
    t.down = true; t.x = x; t.y = y;
#if TOUCH_DEBUG
    Serial.printf("RAW x=%d y=%d -> EKRAN x=%d y=%d\n", p.x, p.y, x, y);
#endif
  }
  return t;
}

// =====================================================================
//  CIZIM YARDIMCILARI
// =====================================================================
void drawButton(int x, int y, int w, int h, const String& label,
                uint16_t bg, uint16_t fg, uint8_t font = 2) {
  tft.fillRoundRect(x, y, w, h, 6, bg);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + w / 2, y + h / 2, font);
}

std::vector<String> wrapText(const String& raw, int maxW) {
  String text = foldTR(raw);
  std::vector<String> lines;
  String cur = "", word = "";
  tft.setTextFont(2);
  auto pushWord = [&](const String& w) {
    if (w.length() == 0) return;
    if (cur.length() == 0) cur = w;
    else {
      String trial = cur + " " + w;
      if (tft.textWidth(trial) <= maxW) cur = trial;
      else { lines.push_back(cur); cur = w; }
    }
    while (tft.textWidth(cur) > maxW && cur.length() > 1) {
      int k = cur.length();
      while (k > 1 && tft.textWidth(cur.substring(0, k)) > maxW) k--;
      lines.push_back(cur.substring(0, k));
      cur = cur.substring(k);
    }
  };
  for (int i = 0; i < (int)text.length(); i++) {
    char c = text[i];
    if (c == '\n') { pushWord(word); word = ""; lines.push_back(cur); cur = ""; }
    else if (c == ' ') { pushWord(word); word = ""; }
    else word += c;
  }
  pushWord(word);
  if (cur.length()) lines.push_back(cur);
  if (lines.empty()) lines.push_back("");
  return lines;
}

void addMessage(Role r, const String& txt) {
  messages.push_back({r, txt});
  if (messages.size() > MAX_MESSAGES) messages.erase(messages.begin());
}

int rssiBars(int rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}
void drawWifiIcon(int x, int y, int bars) {
  for (int i = 0; i < 4; i++) {
    int bh = 3 + i * 3;
    tft.fillRect(x + i * 5, y + 12 - bh, 3, bh, (i < bars) ? C_ACCENT2 : C_DIM);
  }
}

// Cizilmis ikonlar
void iconNewChat(int cx, int cy, uint16_t col) {     // arti
  tft.fillRoundRect(cx - 7, cy - 2, 14, 4, 2, col);
  tft.fillRoundRect(cx - 2, cy - 7, 4, 14, 2, col);
}
void iconMenu(int cx, int cy, uint16_t col) {        // ayarlar (3 cizgi)
  for (int i = -1; i <= 1; i++) tft.fillRoundRect(cx - 8, cy + i * 5 - 1, 16, 3, 1, col);
}

// =====================================================================
//  SOHBET HAFIZASI (NVS)
// =====================================================================
void saveChat() {
  JsonDocument d;
  JsonArray arr = d.to<JsonArray>();
  int start = max(0, (int)messages.size() - 10);
  for (int i = start; i < (int)messages.size(); i++) {
    if (messages[i].role == R_ERR) continue;
    JsonObject o = arr.add<JsonObject>();
    o["r"] = (messages[i].role == R_USER) ? 0 : 1;
    o["t"] = messages[i].text;
  }
  String s; serializeJson(d, s);
  if (s.length() > 3500) {                 // cok uzunsa son 5 mesaj
    JsonDocument d2; JsonArray a2 = d2.to<JsonArray>();
    int st2 = max(0, (int)messages.size() - 5);
    for (int i = st2; i < (int)messages.size(); i++) {
      if (messages[i].role == R_ERR) continue;
      JsonObject o = a2.add<JsonObject>();
      o["r"] = (messages[i].role == R_USER) ? 0 : 1;
      o["t"] = messages[i].text;
    }
    s = ""; serializeJson(d2, s);
  }
  if (s.length() <= 3900) prefs.putString("chatlog", s);
}

void loadChat() {
  String s = prefs.getString("chatlog", "");
  if (s.length() < 2) return;
  JsonDocument d;
  if (deserializeJson(d, s)) return;
  for (JsonObject o : d.as<JsonArray>()) {
    Role r = (o["r"].as<int>() == 0) ? R_USER : R_AI;
    messages.push_back({r, o["t"].as<String>()});
  }
}

// =====================================================================
//  SPLASH
// =====================================================================
void drawSplash(const String& sub) {
  tft.fillScreen(C_BG);
  // basit logo: konusma baloncugu
  tft.fillRoundRect(SCR_W/2 - 46, 64, 92, 60, 12, C_ACCENT);
  tft.fillTriangle(SCR_W/2 - 24, 124, SCR_W/2 - 4, 124, SCR_W/2 - 24, 140, C_ACCENT);
  tft.setTextColor(C_TEXT, C_ACCENT);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("AI", SCR_W/2, 94, 4);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString("CYD AI Chat", SCR_W/2, 158, 4);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(String(APP_VERSION) + "  -  " + sub, SCR_W/2, 186, 2);
}

// =====================================================================
//  WiFi TARAMA + LISTE
// =====================================================================
void scanWifi() {
  drawSplash("WiFi araniyor...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  int n = WiFi.scanNetworks();
  nets.clear();
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i).length() == 0) continue;
    Net net; net.ssid = WiFi.SSID(i); net.rssi = WiFi.RSSI(i);
    net.secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    nets.push_back(net);
  }
  std::sort(nets.begin(), nets.end(), [](const Net& a, const Net& b) { return a.rssi > b.rssi; });
  wifiPage = 0;
}

void drawWifiList() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, SCR_W, 28, C_HEADER);
  tft.setTextColor(C_TEXT, C_HEADER);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("WiFi Sec", 10, 14, 4);
  drawButton(SCR_W - 80, 3, 76, 22, "Yenile", C_ACCENT, C_TEXT, 2);

  if (nets.empty()) {
    tft.setTextColor(C_DIM, C_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Ag bulunamadi", SCR_W / 2, 120, 4);
    return;
  }
  int totalPages = (nets.size() + NETS_PER_PAGE - 1) / NETS_PER_PAGE;
  int start = wifiPage * NETS_PER_PAGE, rowH = 34, y = 32;
  for (int i = start; i < (int)nets.size() && i < start + NETS_PER_PAGE; i++) {
    tft.fillRoundRect(6, y, SCR_W - 12, rowH - 4, 6, C_KEY);
    tft.setTextColor(C_TEXT, C_KEY);
    tft.setTextDatum(ML_DATUM);
    String name = foldTR(nets[i].ssid);
    if (name.length() > 22) name = name.substring(0, 21) + "~";
    tft.drawString(name, 14, y + (rowH - 4) / 2, 2);
    drawWifiIcon(SCR_W - 62, y + 4, rssiBars(nets[i].rssi));
    if (nets[i].secure) {
      tft.setTextColor(C_DIM, C_KEY);
      tft.drawString("kilitli", SCR_W - 40, y + (rowH - 4) / 2, 1);
    }
    y += rowH;
  }
  tft.setTextColor(C_DIM, C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Sayfa " + String(wifiPage + 1) + "/" + String(totalPages), SCR_W / 2, 224, 2);
  if (wifiPage > 0)              drawButton(6, 214, 64, 22, "< Onceki", C_KEY_SP, C_TEXT, 1);
  if (wifiPage < totalPages - 1) drawButton(SCR_W - 76, 214, 72, 22, "Sonraki >", C_KEY_SP, C_TEXT, 1);
}

void handleWifiListTouch(int x, int y) {
  if (y < 28 && x > SCR_W - 80) { scanWifi(); needRedraw = true; return; }
  int totalPages = nets.empty() ? 1 : (nets.size() + NETS_PER_PAGE - 1) / NETS_PER_PAGE;
  if (y >= 214) {
    if (x < 72 && wifiPage > 0) { wifiPage--; needRedraw = true; return; }
    if (x > SCR_W - 82 && wifiPage < totalPages - 1) { wifiPage++; needRedraw = true; return; }
  }
  int rowH = 34, start = wifiPage * NETS_PER_PAGE;
  for (int i = start; i < (int)nets.size() && i < start + NETS_PER_PAGE; i++) {
    int ry = 32 + (i - start) * rowH;
    if (y >= ry && y < ry + rowH - 4 && x >= 6 && x <= SCR_W - 6) {
      wifiSSID = nets[i].ssid; inputText = "";
      if (!nets[i].secure) { wifiPass = ""; state = ST_CONNECTING; }
      else { kbTarget = KT_WIFI_PASS; kbLayer = L_LOWER; state = ST_KEYBOARD; }
      needRedraw = true; return;
    }
  }
}

// =====================================================================
//  BAGLANIYOR
// =====================================================================
bool connectWifi() {
  led(0, 0, 1);
  drawSplash("Baglaniyor: " + foldTR(wifiSSID));
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
  unsigned long t0 = millis();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(300);
    dots = (dots + 1) % 4;
    tft.fillRect(0, 204, SCR_W, 24, C_BG);
    String d = ""; for (int i = 0; i < dots; i++) d += " .";
    tft.setTextColor(C_ACCENT, C_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(d, SCR_W / 2, 212, 4);
  }
  led(0, 0, 0);
  if (WiFi.status() == WL_CONNECTED) {
    prefs.putString("ssid", wifiSSID);
    prefs.putString("pass", wifiPass);
    return true;
  }
  return false;
}

// =====================================================================
//  SOHBET EKRANI
// =====================================================================
const int AV = 24;            // avatar capi
int bubbleMaxW() { return SCR_W - (AV + 8) - 16; }

int computeChatHeight() {
  int total = 8;
  for (auto& m : messages) {
    std::vector<String> lines = wrapText(m.text, bubbleMaxW() - 14);
    total += (int)lines.size() * 18 + 14 + 8;
  }
  return total;
}

void drawChatHeader() {
  tft.fillRect(0, 0, SCR_W, CHAT_HEADER_H, C_HEADER);
  tft.setTextColor(C_TEXT, C_HEADER);
  tft.setTextDatum(ML_DATUM);
  String mn = foldTR(MODELS[selModel].name);
  if (mn.length() > 8) mn = mn.substring(0, 7) + "~";
  tft.drawString(mn, 8, CHAT_HEADER_H / 2, 2);

  // Saat (buyuk) + tarih (kucuk)
  String ck = clockStr();
  tft.setTextColor(C_TEXT, C_HEADER);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(ck, 96, CHAT_HEADER_H / 2, 2);
  int cw = tft.textWidth(ck);
  tft.setTextColor(C_DIM, C_HEADER);
  tft.drawString(foldTR(dateStr()), 96 + cw + 8, CHAT_HEADER_H / 2 + 1, 1);

  if (WiFi.status() == WL_CONNECTED) drawWifiIcon(SCR_W - 92, 7, rssiBars(WiFi.RSSI()));
  tft.fillRoundRect(SCR_W - 64, 3, 28, 22, 6, C_ACCENT2);  iconNewChat(SCR_W - 50, 14, C_BG);
  tft.fillRoundRect(SCR_W - 32, 3, 28, 22, 6, C_KEY_SP);   iconMenu(SCR_W - 18, 14, C_TEXT);
}

void drawAvatar(int x, int y, bool user) {
  uint16_t c = user ? C_ACCENT : C_AI_BUB;
  tft.fillCircle(x + AV / 2, y + AV / 2, AV / 2, c);
  tft.setTextColor(C_TEXT, c);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(user ? "S" : "AI", x + AV / 2, y + AV / 2, 2);
}

void drawChatArea() {
  chatContentH = computeChatHeight();
  int maxScroll = max(0, chatContentH - CHAT_AREA_H);
  chatScroll = constrain(chatScroll, 0, maxScroll);

  tft.setViewport(0, CHAT_AREA_Y, SCR_W, CHAT_AREA_H);
  tft.fillScreen(C_BG);

  int y = 8 - chatScroll;
  for (auto& m : messages) {
    std::vector<String> lines = wrapText(m.text, bubbleMaxW() - 14);
    int bw = 0;
    for (auto& ln : lines) bw = max(bw, (int)tft.textWidth(ln));
    bw += 14; if (bw > bubbleMaxW()) bw = bubbleMaxW();
    int bh = (int)lines.size() * 18 + 14;

    if (y + bh > -4 && y < CHAT_AREA_H + 4) {
      bool user = (m.role == R_USER);
      uint16_t bub = user ? C_USER_BUB : (m.role == R_AI) ? C_AI_BUB : C_ERR_BUB;
      int bx, ax;
      if (user) { bx = SCR_W - (AV + 6) - bw; ax = SCR_W - AV - 4; }
      else      { bx = AV + 8;                ax = 4; }
      if (m.role == R_ERR) { bx = AV + 8; ax = 4; }
      drawAvatar(ax, y, user);
      tft.fillRoundRect(bx, y, bw, bh, 8, bub);
      tft.setTextColor(C_TEXT, bub);
      tft.setTextDatum(TL_DATUM);
      int ty = y + 7;
      for (auto& ln : lines) { tft.drawString(ln, bx + 7, ty, 2); ty += 18; }
    }
    y += bh + 8;
  }
  tft.resetViewport();

  if (maxScroll > 0) {
    int barH = max(14, CHAT_AREA_H * CHAT_AREA_H / chatContentH);
    int barY = CHAT_AREA_Y + (CHAT_AREA_H - barH) * chatScroll / maxScroll;
    tft.fillRoundRect(SCR_W - 4, barY, 3, barH, 1, C_ACCENT);
  }
}

void drawChatInput() {
  int y = SCR_H - CHAT_INPUT_H;
  tft.fillRect(0, y, SCR_W, CHAT_INPUT_H, C_HEADER);
  tft.fillRoundRect(8, y + 5, SCR_W - 90, CHAT_INPUT_H - 10, 8, C_FIELD);
  tft.setTextColor(C_DIM, C_FIELD);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Mesaj yaz...", 16, y + CHAT_INPUT_H / 2, 2);
  drawButton(SCR_W - 76, y + 5, 68, CHAT_INPUT_H - 10, "Yaz", C_ACCENT, C_TEXT, 2);
}

void drawQuickBar() {
  tft.fillRect(0, QUICK_Y, SCR_W, QUICK_H, C_BG);
  int gap = 4, bw = (SCR_W - gap * 5) / 4;
  for (int i = 0; i < 4; i++) {
    int bx = gap + i * (bw + gap);
    drawButton(bx, QUICK_Y + 2, bw, QUICK_H - 4, QUICK_LBL[i], C_KEY, C_ACCENT2, 2);
  }
}

void drawChat() {
  tft.fillScreen(C_BG);
  drawChatHeader();
  drawChatArea();
  drawQuickBar();
  drawChatInput();
}

// =====================================================================
//  MODEL SECIM
// =====================================================================
void drawModelList() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, SCR_W, 28, C_HEADER);
  tft.setTextColor(C_TEXT, C_HEADER);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Model Sec", 10, 14, 4);
  drawButton(SCR_W - 72, 3, 68, 22, "< Geri", C_ACCENT, C_TEXT, 2);

  int totalPages = (MODEL_COUNT + MODELS_PER_PAGE - 1) / MODELS_PER_PAGE;
  int start = modelPage * MODELS_PER_PAGE, rowH = 34, y = 32;
  for (int i = start; i < MODEL_COUNT && i < start + MODELS_PER_PAGE; i++) {
    uint16_t bg = (i == selModel) ? C_SEL : C_KEY;
    tft.fillRoundRect(6, y, SCR_W - 12, rowH - 4, 6, bg);
    tft.setTextColor(C_TEXT, bg);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(foldTR(MODELS[i].name), 14, y + (rowH - 4) / 2, 2);
    tft.setTextColor(C_ACCENT2, bg);
    tft.setTextDatum(MR_DATUM);
    tft.drawString("ucretsiz", SCR_W - 16, y + (rowH - 4) / 2, 1);
    y += rowH;
  }
  tft.setTextColor(C_DIM, C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Sayfa " + String(modelPage + 1) + "/" + String(totalPages), SCR_W / 2, 224, 2);
  if (modelPage > 0)              drawButton(6, 214, 64, 22, "< Onceki", C_KEY_SP, C_TEXT, 1);
  if (modelPage < totalPages - 1) drawButton(SCR_W - 76, 214, 72, 22, "Sonraki >", C_KEY_SP, C_TEXT, 1);
}

void handleModelTouch(int x, int y) {
  if (y < 28 && x > SCR_W - 72) { state = ST_CHAT; needRedraw = true; return; }
  int totalPages = (MODEL_COUNT + MODELS_PER_PAGE - 1) / MODELS_PER_PAGE;
  if (y >= 214) {
    if (x < 72 && modelPage > 0) { modelPage--; needRedraw = true; return; }
    if (x > SCR_W - 82 && modelPage < totalPages - 1) { modelPage++; needRedraw = true; return; }
  }
  int rowH = 34, start = modelPage * MODELS_PER_PAGE;
  for (int i = start; i < MODEL_COUNT && i < start + MODELS_PER_PAGE; i++) {
    int ry = 32 + (i - start) * rowH;
    if (y >= ry && y < ry + rowH - 4 && x >= 6 && x <= SCR_W - 6) {
      selModel = i; prefs.putInt("model", selModel);
      state = ST_CHAT; needRedraw = true; return;
    }
  }
}

// =====================================================================
//  AYARLAR
// =====================================================================
void drawSettings() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, SCR_W, 28, C_HEADER);
  tft.setTextColor(C_TEXT, C_HEADER);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Ayarlar", 10, 14, 4);
  drawButton(SCR_W - 72, 3, 68, 22, "< Geri", C_ACCENT, C_TEXT, 2);

  tft.setTextColor(C_DIM, C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("WiFi: " + foldTR(wifiSSID.length() ? wifiSSID : String("yok")) +
                 (apiKeyOk() ? "   API: OK" : "   API: EKSIK"), 10, 34, 2);

  drawButton(10, 56,  146, 30, "WiFi Degistir", C_KEY, C_TEXT, 2);
  drawButton(164, 56, 146, 30, "Model Sec",     C_KEY, C_TEXT, 2);
  drawButton(10, 92,  146, 30, "Kisi: " + foldTR(PERSONAS[selPersona].name), C_KEY, C_TEXT, 2);
  drawButton(164, 92, 146, 30, "Dil: " + foldTR(LANGS[selLang].name), C_KEY, C_ACCENT2, 2);

  // Parlaklik
  tft.setTextColor(C_TEXT, C_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Parlaklik", 14, 143, 2);
  drawButton(150, 128, 40, 30, "-", C_KEY_SP, C_TEXT, 4);
  tft.setTextColor(C_TEXT, C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(String(map(brightness, 25, 255, 0, 100)) + "%", 215, 143, 2);
  drawButton(240, 128, 40, 30, "+", C_KEY_SP, C_TEXT, 4);

  drawButton(10, 166, 300, 30, "Sohbeti Temizle", C_KEY, C_TEXT, 2);
  drawButton(10, 202, 300, 30, "WiFi Sifirla", C_ERR_BUB, C_TEXT, 2);
}

void handleSettingsTouch(int x, int y) {
  if (y < 28 && x > SCR_W - 72) { state = ST_CHAT; needRedraw = true; return; }
  if (y >= 56 && y <= 86) {
    if (x < 160) { scanWifi(); state = ST_WIFI_LIST; needRedraw = true; return; }
    else { modelPage = 0; state = ST_MODEL; needRedraw = true; return; }
  }
  if (y >= 92 && y <= 122) {                 // kisilik (sol) / dil (sag)
    if (x < 160) { selPersona = (selPersona + 1) % PERSONA_COUNT; prefs.putInt("persona", selPersona); }
    else         { selLang    = (selLang + 1) % LANG_COUNT;       prefs.putInt("lang", selLang); }
    needRedraw = true; return;
  }
  if (y >= 128 && y <= 158) {                // parlaklik
    if (x >= 150 && x <= 190) brightness = max(25, brightness - 30);
    if (x >= 240 && x <= 280) brightness = min(255, brightness + 30);
    applyBrightness();
    prefs.putInt("bright", brightness);
    needRedraw = true; return;
  }
  if (y >= 166 && y <= 196) {                // sohbeti temizle
    messages.clear(); lastUsed = ""; chatScroll = 0;
    prefs.remove("chatlog");
    state = ST_CHAT; needRedraw = true; return;
  }
  if (y >= 202 && y <= 232) {                // wifi sifirla
    prefs.remove("ssid"); prefs.remove("pass");
    wifiSSID = ""; WiFi.disconnect(true, true); delay(200);
    scanWifi(); state = ST_WIFI_LIST; needRedraw = true; return;
  }
}

// =====================================================================
//  KLAVYE
// =====================================================================
void pushKey(int x, int y, int w, int h, String label, String value, int special) {
  KeyBox k; k.x = x; k.y = y; k.w = w; k.h = h;
  k.label = label; k.value = value; k.special = special;
  keys.push_back(k);
}

void buildKeyboard() {
  keys.clear();
  int h = ROW_H - KGAP;
  if (kbLayer == L_TR) {
    const char* lo_v[7] = { "\u00E7", "\u011F", "\u0131", "\u0130", "\u00F6", "\u015F", "\u00FC" };
    const char* lo_l[7] = { "c", "g", "i", "I", "o", "s", "u" };
    int kw = 38, n = 7, x0 = (SCR_W - n * kw) / 2;
    for (int i = 0; i < n; i++) pushKey(x0 + i * kw + KGAP, KB_TOP, kw - 2 * KGAP, h, lo_l[i], lo_v[i], 0);
    const char* up_v[5] = { "\u00C7", "\u011E", "\u00D6", "\u015E", "\u00DC" };
    const char* up_l[5] = { "C", "G", "O", "S", "U" };
    int kw2 = 40, m = 5, bw = 50, tot = m * kw2 + bw, x2 = (SCR_W - tot) / 2, y2 = KB_TOP + ROW_H;
    for (int i = 0; i < m; i++) pushKey(x2 + i * kw2 + KGAP, y2, kw2 - 2 * KGAP, h, up_l[i], up_v[i], 0);
    pushKey(x2 + m * kw2 + KGAP, y2, bw - 2 * KGAP, h, "<-", "", 2);
    int y3 = KB_TOP + 2 * ROW_H;
    pushKey(60, y3, SCR_W - 120, h, "ABC harfler", "", 6);
  } else {
    const char** rows = (kbLayer == L_LOWER) ? rowsLower : (kbLayer == L_UPPER) ? rowsUpper : rowsSym;
    { int n = strlen(rows[0]); int kw = SCR_W / n;
      for (int i = 0; i < n; i++) { String c = String(rows[0][i]); pushKey(i * kw + KGAP, KB_TOP, kw - 2 * KGAP, h, c, c, 0); } }
    { int n = strlen(rows[1]); int kw = 32; int x0 = (SCR_W - n * kw) / 2; int y = KB_TOP + ROW_H;
      for (int i = 0; i < n; i++) { String c = String(rows[1][i]); pushKey(x0 + i * kw + KGAP, y, kw - 2 * KGAP, h, c, c, 0); } }
    { int n = strlen(rows[2]); int y = KB_TOP + 2 * ROW_H; int side = 46, kw = 32;
      int x0 = (SCR_W - (2 * side + n * kw)) / 2;
      if (kbLayer == L_SYM) pushKey(x0 + KGAP, y, side - 2 * KGAP, h, "sym", "", 0);
      else pushKey(x0 + KGAP, y, side - 2 * KGAP, h, (kbLayer == L_UPPER) ? "SHFT" : "shft", "", 1);
      int xx = x0 + side;
      for (int i = 0; i < n; i++) { String c = String(rows[2][i]); pushKey(xx + i * kw + KGAP, y, kw - 2 * KGAP, h, c, c, 0); }
      pushKey(xx + n * kw + KGAP, y, side - 2 * KGAP, h, "<-", "", 2);
    }
  }
  { int y = KB_TOP + 3 * ROW_H, layW = 50, trW = 44, entW = 64;
    pushKey(KGAP, y, layW - 2 * KGAP, h, (kbLayer == L_SYM) ? "ABC" : "123", "", 5);
    pushKey(layW + KGAP, y, trW - 2 * KGAP, h, "TR", "", 6);
    int spX = layW + trW, spW = SCR_W - spX - entW;
    pushKey(spX + KGAP, y, spW - 2 * KGAP, h, "bosluk", " ", 3);
    pushKey(SCR_W - entW + KGAP, y, entW - 2 * KGAP, h, (kbTarget == KT_CHAT) ? "Gonder" : "Tamam", "", 4);
  }
}

void drawInputField() {
  tft.fillRect(0, 0, SCR_W, KB_TOP - 2, C_HEADER);
  drawButton(2, 4, 44, KB_TOP - 10, "X", C_ERR_BUB, C_TEXT, 4);
  int fx = 50, fw = SCR_W - 54;
  tft.fillRoundRect(fx, 4, fw, KB_TOP - 10, 6, C_FIELD);
  tft.setTextDatum(ML_DATUM);
  String shown = foldTR(inputText);
  tft.setTextFont(2);
  while (tft.textWidth(shown) > fw - 16 && shown.length() > 0) shown = shown.substring(1);
  tft.setTextColor(C_TEXT, C_FIELD);
  tft.drawString(shown, fx + 6, (KB_TOP - 2) / 2, 2);
  int cx = fx + 6 + tft.textWidth(shown);
  if (cx < SCR_W - 6) tft.fillRect(cx + 1, 8, 2, KB_TOP - 18, C_ACCENT);
}

void drawKeyboard() {
  tft.fillScreen(C_BG);
  drawInputField();
  buildKeyboard();
  for (auto& k : keys) {
    uint16_t bg = (k.special == 1 && kbLayer == L_UPPER) ? C_KEY_HI
                : (k.special == 6 && kbLayer == L_TR)    ? C_KEY_HI
                : (k.special != 0) ? C_KEY_SP : C_KEY;
    if (k.special == 4) bg = C_ACCENT;
    tft.fillRoundRect(k.x, k.y, k.w, k.h, 5, bg);
    tft.setTextColor(C_TEXT, bg);
    tft.setTextDatum(MC_DATUM);
    uint8_t f = (k.label.length() <= 1) ? 4 : 2;
    tft.drawString(k.label, k.x + k.w / 2, k.y + k.h / 2, f);
  }
}

void flashKey(const KeyBox& k) {
  tft.fillRoundRect(k.x, k.y, k.w, k.h, 5, C_KEY_HI);
  tft.setTextColor(C_TEXT, C_KEY_HI);
  tft.setTextDatum(MC_DATUM);
  uint8_t f = (k.label.length() <= 1) ? 4 : 2;
  tft.drawString(k.label, k.x + k.w / 2, k.y + k.h / 2, f);
}

void keyboardConfirm() {
  if (kbTarget == KT_WIFI_PASS) {
    wifiPass = inputText;
    state = ST_CONNECTING;
  } else {
    String msg = inputText; msg.trim();
    if (msg.length() > 0) { addMessage(R_USER, msg); pendingSend = msg; sendPending = true; }
    state = ST_CHAT;
  }
  inputText = ""; needRedraw = true;
}

int findKey(int x, int y) {
  for (int i = 0; i < (int)keys.size(); i++)
    if (x >= keys[i].x && x <= keys[i].x + keys[i].w && y >= keys[i].y && y <= keys[i].y + keys[i].h)
      return i;
  return -1;
}

// =====================================================================
//  OPENROUTER (kod-ici yedekleme)
// =====================================================================
void callOpenRouter() {
  if (WiFi.status() != WL_CONNECTED) { addMessage(R_ERR, "WiFi baglantisi yok."); return; }
  if (!apiKeyOk()) { addMessage(R_ERR, "API anahtari eksik. Kodda API_KEY satirini duzenle."); return; }

  led(0, 0, 1);   // dusunuyor: mavi

  int order[MODEL_COUNT]; int n = 0;
  order[n++] = selModel;
  for (int i = 0; i < MODEL_COUNT; i++) if (i != selModel) order[n++] = i;

  WiFiClientSecure client;
  client.setInsecure();
  int lastCode = 0; String lastDetail = "";

  for (int a = 0; a < n; a++) {
    int mi = order[a];
    int yb = SCR_H - CHAT_INPUT_H;
    tft.fillRect(0, yb, SCR_W, CHAT_INPUT_H, C_HEADER);
    tft.setTextColor(C_ACCENT, C_HEADER);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(foldTR(MODELS[mi].name) + " dusunuyor...", SCR_W / 2, yb + CHAT_INPUT_H / 2, 2);

    JsonDocument req;
    req["model"] = MODELS[mi].slug;
    req["max_tokens"] = MAX_TOKENS;
    JsonArray msgs = req["messages"].to<JsonArray>();
    { JsonObject s = msgs.add<JsonObject>();
      s["role"] = "system";
      s["content"] = String(PERSONAS[selPersona].prompt) + " " + LANGS[selLang].directive; }
    std::vector<const Msg*> hist;
    for (auto it = messages.rbegin(); it != messages.rend() && (int)hist.size() < API_HISTORY; ++it) {
      if (it->role == R_ERR) continue;
      hist.push_back(&(*it));
    }
    for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
      JsonObject m = msgs.add<JsonObject>();
      m["role"] = ((*it)->role == R_USER) ? "user" : "assistant";
      m["content"] = (*it)->text;
    }
    String payload; serializeJson(req, payload);

    HTTPClient https; https.setTimeout(HTTP_TIMEOUT_MS);
    if (!https.begin(client, OPENROUTER_URL)) { lastDetail = "baglanti baslatilamadi"; continue; }
    https.addHeader("Content-Type", "application/json");
    https.addHeader("Authorization", String("Bearer ") + API_KEY);
    https.addHeader("HTTP-Referer", "https://cyd.local");
    https.addHeader("X-Title", "CYD AI Chat");

    int code = https.POST(payload);
    if (code <= 0) { lastCode = code; lastDetail = https.errorToString(code); https.end(); continue; }
    String body = https.getString();
    https.end();

    if (code == 200) {
      JsonDocument doc;
      if (deserializeJson(doc, body)) { lastCode = 200; lastDetail = "cozumlenemedi"; continue; }
      const char* content = doc["choices"][0]["message"]["content"];
      if (content != nullptr && strlen(content) > 0) {
        lastUsed = foldTR(MODELS[mi].name);
        addMessage(R_AI, String(content));
        led(0, 1, 0);   // basari: yesil
        return;
      }
      lastCode = 200; lastDetail = "bos cevap"; continue;
    }
    lastCode = code;
    JsonDocument ed;
    if (deserializeJson(ed, body) == DeserializationError::Ok && !ed["error"]["message"].isNull())
      lastDetail = ed["error"]["message"].as<String>();
    else lastDetail = body.substring(0, 100);
    if (code == 401) break;
  }

  led(1, 0, 0);   // hata: kirmizi
  if (lastCode == 401)
    addMessage(R_ERR, "API anahtari gecersiz (401). Kodda API_KEY satirini kontrol et.");
  else if (lastCode == 429)
    addMessage(R_ERR, "Tum bedava modeller su an limitte (429). Birkac dakika bekle. Surekli "
                      "kullanacaksan OpenRouter hesabina bir kerelik 10$ kredi eklersen limit cok artar.");
  else
    addMessage(R_ERR, "Cevap alinamadi (" + String(lastCode) + "): " + lastDetail);
}

// =====================================================================
//  DOKUNMA DURUM TAKIBI
// =====================================================================
bool wasDown = false;
int  pressKey = -1;
unsigned long pressTime = 0, lastRepeat = 0, lastScrollDraw = 0;
bool chatDragging = false;
int  dragStartY = 0, dragStartScroll = 0;
bool dragMoved = false;

void handleChatTouch_press(int x, int y) {
  if (y < CHAT_HEADER_H) {
    if (x >= SCR_W - 64 && x < SCR_W - 34) { messages.clear(); lastUsed = ""; chatScroll = 0; prefs.remove("chatlog"); needRedraw = true; return; }
    if (x >= SCR_W - 34) { state = ST_SETTINGS; needRedraw = true; return; }
    if (x < SCR_W - 96)  { modelPage = 0; state = ST_MODEL; needRedraw = true; return; }
    return;
  }
  // Hizli komut cubugu
  if (y >= QUICK_Y && y < QUICK_Y + QUICK_H) {
    int gap = 4, bw = (SCR_W - gap * 5) / 4;
    for (int i = 0; i < 4; i++) {
      int bx = gap + i * (bw + gap);
      if (x >= bx && x <= bx + bw) {
        inputText = QUICK_PRE[i];
        kbTarget = KT_CHAT; kbLayer = L_LOWER;
        state = ST_KEYBOARD; needRedraw = true;
        return;
      }
    }
    return;
  }
  if (y >= SCR_H - CHAT_INPUT_H) {
    inputText = ""; kbTarget = KT_CHAT; kbLayer = L_LOWER;
    state = ST_KEYBOARD; needRedraw = true; return;
  }
  chatDragging = true; dragStartY = y; dragStartScroll = chatScroll; dragMoved = false;
}

void handleChatTouch_move(int x, int y) {
  if (!chatDragging) return;
  int dy = dragStartY - y;
  if (abs(dy) > 5) dragMoved = true;
  chatScroll = constrain(dragStartScroll + dy, 0, max(0, chatContentH - CHAT_AREA_H));
  if (millis() - lastScrollDraw > 40) { drawChatArea(); lastScrollDraw = millis(); }
}

void handleKeyboardPress(int tx, int ty) {
  if (ty < KB_TOP - 2 && tx < 48) {
    if (kbTarget == KT_WIFI_PASS) { scanWifi(); state = ST_WIFI_LIST; }
    else state = ST_CHAT;
    inputText = ""; needRedraw = true; return;
  }
  int ki = findKey(tx, ty);
  if (ki < 0) return;
  KeyBox k = keys[ki];
  pressKey = ki; pressTime = millis(); lastRepeat = millis();
  flashKey(k);
  switch (k.special) {
    case 0: case 3:
      if (k.value.length() && inputText.length() < 512) inputText += k.value;
      drawInputField(); break;
    case 1: kbLayer = (kbLayer == L_LOWER) ? L_UPPER : L_LOWER; drawKeyboard(); break;
    case 2: delLastChar(); drawInputField(); break;
    case 4: keyboardConfirm(); break;
    case 5: kbLayer = (kbLayer == L_SYM) ? L_LOWER : L_SYM; drawKeyboard(); break;
    case 6: kbLayer = (kbLayer == L_TR)  ? L_LOWER : L_TR;  drawKeyboard(); break;
  }
}

// =====================================================================
//  SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  led(0, 0, 0);

  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  tft.invertDisplay(TFT_INVERT_COLORS);

  prefs.begin("cydai", false);
  wifiSSID   = prefs.getString("ssid", "");
  selModel   = prefs.getInt("model", 0);
  selPersona = prefs.getInt("persona", 0);
  selLang    = prefs.getInt("lang", 0);
  brightness = prefs.getInt("bright", 210);
  if (selModel < 0 || selModel >= MODEL_COUNT) selModel = 0;
  if (selPersona < 0 || selPersona >= PERSONA_COUNT) selPersona = 0;
  if (selLang < 0 || selLang >= LANG_COUNT) selLang = 0;
  brightness = constrain(brightness, 25, 255);

  applyBrightness();
  drawSplash("baslatiliyor...");
  Serial.printf("Ekran: %d x %d (rot %d)\n", tft.width(), tft.height(), SCREEN_ROTATION);
  delay(700);

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(SCREEN_ROTATION);

  loadChat();

  if (wifiSSID.length() > 0) {
    String savedPass = prefs.getString("pass", "");
    wifiPass = savedPass;
    led(0, 0, 1);
    drawSplash("Kayitli WiFi'ye baglaniliyor...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), savedPass.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(200);
    led(0, 0, 0);
  }

  if (WiFi.status() == WL_CONNECTED) {
    syncTime();
    if (messages.empty()) {
      addMessage(R_AI, greetingMsg());
      if (!apiKeyOk()) addMessage(R_ERR, "Uyari: API anahtari girilmemis. Kodda API_KEY satirini duzenle.");
    }
    state = ST_CHAT;
  } else {
    scanWifi();
    state = ST_WIFI_LIST;
  }
  needRedraw = true;
}

// =====================================================================
//  LOOP
// =====================================================================
void loop() {
  if (needRedraw) {
    switch (state) {
      case ST_WIFI_LIST: drawWifiList();  break;
      case ST_KEYBOARD:  drawKeyboard();  break;
      case ST_CHAT:      drawChat();      break;
      case ST_SETTINGS:  drawSettings();  break;
      case ST_MODEL:     drawModelList(); break;
      default: break;
    }
    needRedraw = false;
  }

  if (state == ST_CONNECTING) {
    if (connectWifi()) {
      syncTime();
      if (messages.empty()) addMessage(R_AI, greetingMsg());
      if (!apiKeyOk()) addMessage(R_ERR, "Uyari: API anahtari girilmemis. Kodda API_KEY satirini duzenle.");
      state = ST_CHAT;
    } else {
      led(1, 0, 0);
      drawSplash("Baglanti basarisiz - sifre yanlis olabilir");
      delay(1800); led(0, 0, 0);
      scanWifi(); state = ST_WIFI_LIST;
    }
    needRedraw = true;
    return;
  }

  if (state == ST_CHAT && sendPending) {
    sendPending = false;
    drawChat();
    callOpenRouter();
    saveChat();
    pendingSend = "";
    chatScroll = 1000000;
    needRedraw = true;
    return;
  }

  // Saat canli ilersin diye ust bari periyodik tazele
  if (state == ST_CHAT && !sendPending && !wasDown && millis() - lastClock > 15000) {
    lastClock = millis();
    drawChatHeader();
  }

  Touch t = readTouch();
  if (t.down && !wasDown) {
    wasDown = true;
    switch (state) {
      case ST_WIFI_LIST: handleWifiListTouch(t.x, t.y); break;
      case ST_SETTINGS:  handleSettingsTouch(t.x, t.y); break;
      case ST_MODEL:     handleModelTouch(t.x, t.y);    break;
      case ST_CHAT:      handleChatTouch_press(t.x, t.y); break;
      case ST_KEYBOARD:  handleKeyboardPress(t.x, t.y);   break;
      default: break;
    }
  } else if (t.down && wasDown) {
    if (state == ST_CHAT) handleChatTouch_move(t.x, t.y);
    else if (state == ST_KEYBOARD && pressKey >= 0 && pressKey < (int)keys.size() && keys[pressKey].special == 2) {
      unsigned long now = millis();
      if (now - pressTime > 450 && now - lastRepeat > 120) { delLastChar(); drawInputField(); lastRepeat = now; }
    }
  } else if (!t.down && wasDown) {
    wasDown = false;
    if (state == ST_CHAT) chatDragging = false;
    pressKey = -1;
  }
  delay(12);
}
