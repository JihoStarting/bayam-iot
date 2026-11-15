#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

// ====================== KONFIGURASI ======================
// Kredensial (Wi-Fi, token bot, chat ID) disimpan di include/secrets.h
// Salin include/secrets.h.example menjadi include/secrets.h lalu isi nilainya.
#include "secrets.h"

#define SDA_PIN 22
#define SCL_PIN 21
#define LDR_DO  18
#define DHT_PIN 4
#define DHT_TYPE DHT11

// true jika pin DO bernilai HIGH saat GELAP.
// Kalau hasilnya kebalik, ubah jadi false.
const bool DO_HIGH_GELAP = true;

// Rentang ideal untuk bayam
const float SUHU_MIN = 25.0;
const float SUHU_MAX = 35.0;
const float LEMBAB_MIN = 60.0;

// Interval laporan otomatis (bisa diubah lewat Telegram, disimpan di flash)
const unsigned long KIRIM_DEFAULT_MS = 10000;
const unsigned long KIRIM_MIN_DETIK  = 5;
const unsigned long KIRIM_MAX_DETIK  = 86400;
const unsigned long POLL_MS = 5000;
const unsigned long BACA_MS = 2500;

Adafruit_SSD1306 display(128, 64, &Wire, -1);
DHT dht(DHT_PIN, DHT_TYPE);
Preferences prefs;

uint8_t alamat = 0;
float suhu = NAN, lembab = NAN;
bool gelap = false;
bool dhtOk = false;
long updateOffset = 0;
unsigned long kirimMs = KIRIM_DEFAULT_MS;   // 0 = laporan otomatis mati
unsigned long tBaca = 0, tKirim = 0, tPoll = 0;

void scanI2C() {
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("Perangkat ditemukan di 0x%02X\n", a);
      if (a == 0x3C || a == 0x3D) alamat = a;
    }
  }
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("Menghubungkan Wi-Fi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " terhubung" : " GAGAL");
}

void bacaSensor() {
  suhu = dht.readTemperature();
  lembab = dht.readHumidity();
  dhtOk = !(isnan(suhu) || isnan(lembab));

  int high = 0;
  for (int i = 0; i < 15; i++) {
    if (digitalRead(LDR_DO) == HIGH) high++;
    delay(2);
  }
  bool doHigh = high > 7;
  gelap = DO_HIGH_GELAP ? doHigh : !doHigh;

  Serial.printf("DO=%s -> %s\n", doHigh ? "HIGH" : "LOW", gelap ? "GELAP" : "TERANG");
}

const char* teksCahaya() {
  return gelap ? "GELAP" : "TERANG";
}

String teksStatus() {
  if (!dhtOk) return "SENSOR ERROR (cek kabel DHT)";
  String alasan = "";
  if (lembab < LEMBAB_MIN) alasan += "kelembapan rendah, ";
  if (gelap)               alasan += "terlalu gelap, ";
  if (suhu < SUHU_MIN)     alasan += "terlalu dingin, ";
  if (suhu > SUHU_MAX)     alasan += "terlalu panas, ";
  if (alasan == "") return "IDEAL";
  alasan.remove(alasan.length() - 2);
  return "PERLU PENANGANAN (" + alasan + ")";
}

// Ubah milidetik jadi teks: "10 detik", "5 menit", "1 jam"
String teksInterval(unsigned long ms) {
  if (ms == 0) return "MATI";
  unsigned long d = ms / 1000;
  if (d % 3600 == 0) return String(d / 3600) + " jam";
  if (d % 60 == 0)   return String(d / 60) + " menit";
  return String(d) + " detik";
}

bool kirimTelegram(const String& teks) {
  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/sendMessage";
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  String body = "{\"chat_id\":\"" + String(CHAT_ID) + "\",\"text\":\"" + teks + "\"}";
  int code = http.POST(body);
  Serial.printf("Telegram HTTP %d\n", code);
  if (code != 200) Serial.println(http.getString());
  http.end();
  return code == 200;
}

// Daftarkan menu perintah (muncul saat mengetik "/")
void daftarkanMenu() {
  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/setMyCommands";
  if (!http.begin(client, url)) return;
  http.addHeader("Content-Type", "application/json");

  String body = "{\"commands\":["
    "{\"command\":\"cek\",\"description\":\"Cek kondisi bayam sekarang\"},"
    "{\"command\":\"interval\",\"description\":\"Lihat atau atur interval laporan otomatis\"},"
    "{\"command\":\"help\",\"description\":\"Tampilkan daftar perintah\"}"
    "]}";

  int code = http.POST(body);
  Serial.printf("setMyCommands HTTP %d\n", code);
  http.end();
}

String bentukPesan(const String& sumber) {
  String s = "Monitoring Bayam\\n";
  s += dhtOk ? "Suhu       : " + String(suhu, 1) + " C\\n" : "Suhu       : --\\n";
  s += dhtOk ? "Kelembapan : " + String(lembab, 0) + " %\\n" : "Kelembapan : --\\n";
  s += "Cahaya     : " + String(teksCahaya()) + "\\n";
  s += "STATUS: " + teksStatus() + "\\n";
  s += "(" + sumber + ")";
  return s;
}

// Atur interval dari teks argumen: "30", "5m", "1h", "off"
void aturInterval(String arg) {
  arg.trim();
  arg.toLowerCase();

  if (arg.length() == 0) {
    kirimTelegram("Interval laporan otomatis: " + teksInterval(kirimMs) +
                  "\\nUbah: /interval 30 (detik), /interval 5m, /interval 1h, /interval off");
    return;
  }

  if (arg == "off" || arg == "0") {
    kirimMs = 0;
    prefs.putULong("kirimMs", kirimMs);
    kirimTelegram("Laporan otomatis dimatikan. Gunakan /cek untuk cek manual.");
    return;
  }

  unsigned long mult = 1;
  if (arg.endsWith("h"))      { mult = 3600; arg.remove(arg.length() - 1); }
  else if (arg.endsWith("m")) { mult = 60;   arg.remove(arg.length() - 1); }
  else if (arg.endsWith("s")) { arg.remove(arg.length() - 1); }

  long nilai = arg.toInt();
  unsigned long detik = (nilai > 0) ? (unsigned long)nilai * mult : 0;

  if (detik < KIRIM_MIN_DETIK || detik > KIRIM_MAX_DETIK) {
    kirimTelegram("Nilai tidak valid. Gunakan 5 detik sampai 24 jam.\\nContoh: /interval 30, /interval 5m, /interval 1h");
    return;
  }

  kirimMs = detik * 1000UL;
  prefs.putULong("kirimMs", kirimMs);
  tKirim = millis();
  kirimTelegram("Interval laporan diubah menjadi " + teksInterval(kirimMs));
}

// Proses satu pesan (potongan JSON satu update)
void prosesPesan(const String& seg) {
  bool dariSaya = seg.indexOf("\"chat\":{\"id\":" + String(CHAT_ID)) >= 0;
  if (!dariSaya) return;

  int t = seg.indexOf("\"text\":\"");
  if (t < 0) return;
  t += 8;
  int e = seg.indexOf('"', t);
  if (e < 0) return;
  String teks = seg.substring(t, e);
  teks.trim();

  Serial.println("Perintah: " + teks);

  if (teks.startsWith("/cek")) {
    bacaSensor();
    kirimTelegram(bentukPesan("Cek manual"));
  } else if (teks.startsWith("/interval")) {
    int sp = teks.indexOf(' ');
    aturInterval(sp >= 0 ? teks.substring(sp + 1) : "");
  } else if (teks.startsWith("/help") || teks.startsWith("/start")) {
    kirimTelegram("Perintah:\\n/cek - cek kondisi sekarang\\n/interval - lihat interval\\n/interval 30 - tiap 30 detik\\n/interval 5m - tiap 5 menit\\n/interval 1h - tiap 1 jam\\n/interval off - matikan laporan otomatis");
  }
}

// Cek pesan masuk dan proses semua update secara berurutan
void cekPerintah() {
  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) +
               "/getUpdates?timeout=0&offset=" + String(updateOffset);
  if (!http.begin(client, url)) return;
  int code = http.GET();
  if (code != 200) { Serial.printf("getUpdates HTTP %d\n", code); http.end(); return; }
  String payload = http.getString();
  http.end();

  int pos = 0;
  while ((pos = payload.indexOf("\"update_id\":", pos)) >= 0) {
    pos += 12;
    int next = payload.indexOf("\"update_id\":", pos);
    String seg = (next >= 0) ? payload.substring(pos, next) : payload.substring(pos);
    updateOffset = seg.toInt() + 1;
    prosesPesan(seg);
  }
}

void tampilOLED() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("MONITORING BAYAM");
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  display.setCursor(0, 14);
  if (dhtOk) display.printf("Suhu  : %.1f C", suhu);
  else       display.print("Suhu  : error");
  display.setCursor(0, 24);
  if (dhtOk) display.printf("Lembab: %.0f %%", lembab);
  else       display.print("Lembab: error");
  display.setCursor(0, 34);
  display.printf("Cahaya: %s", teksCahaya());

  display.setCursor(0, 44);
  display.printf("Lapor : %s", teksInterval(kirimMs).c_str());

  display.setCursor(0, 56);
  display.print(WiFi.status() == WL_CONNECTED ? "WiFi OK " : "WiFi --  ");
  display.print(teksStatus().startsWith("IDEAL") ? "IDEAL" : "PERLU");
  display.display();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(LDR_DO, INPUT);
  Wire.begin(SDA_PIN, SCL_PIN);
  dht.begin();

  prefs.begin("bayam", false);
  kirimMs = prefs.getULong("kirimMs", KIRIM_DEFAULT_MS);
  Serial.println("Interval: " + teksInterval(kirimMs));

  scanI2C();
  if (alamat == 0 || !display.begin(SSD1306_SWITCHCAPVCC, alamat)) {
    Serial.println("OLED tidak ditemukan, lanjut tanpa layar");
    alamat = 0;
  }

  connectWiFi();
  daftarkanMenu();

  // buang pesan lama tanpa dieksekusi
  {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/getUpdates?timeout=0&offset=-1";
    if (WiFi.status() == WL_CONNECTED && http.begin(client, url)) {
      if (http.GET() == 200) {
        String p = http.getString();
        int i = p.lastIndexOf("\"update_id\":");
        if (i >= 0) updateOffset = p.substring(i + 12).toInt() + 1;
      }
      http.end();
    }
  }

  bacaSensor();
  kirimTelegram(bentukPesan("Alat aktif"));
  tBaca = tKirim = tPoll = millis();
}

void loop() {
  unsigned long now = millis();

  if (now - tBaca >= BACA_MS) {
    tBaca = now;
    bacaSensor();
    if (alamat) tampilOLED();
    Serial.printf("Suhu=%.1f Lembab=%.0f Cahaya=%s -> %s\n",
                  suhu, lembab, teksCahaya(), teksStatus().c_str());
  }
  if (kirimMs > 0 && now - tKirim >= kirimMs) {
    tKirim = now;
    kirimTelegram(bentukPesan("Otomatis tiap " + teksInterval(kirimMs)));
  }
  if (now - tPoll >= POLL_MS) {
    tPoll = now;
    cekPerintah();
  }
}