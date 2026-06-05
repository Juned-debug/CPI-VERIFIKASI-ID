#include <Arduino.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <ElegantOTA.h>
#include <WebServer.h>
// ============================================================
// KONFIGURASI WiFi & MQTT
// ============================================================
// const char* ssid         = "Produksi";
// const char* wifiPass     = "Berbek124*";
const char* mqttServer = "10.68.1.220";
const int mqttPort = 1884;
const char* mqttTopicPub = "monitoringmesincpi";
const char* mqttTopicSub = "monitoringmesincpi/1";
WebServer server(8080);
WiFiManager wm;
// ============================================================
// IDENTITAS MESIN
// ============================================================
const int ID_MESIN = 1;

// ============================================================
// EVENT ID
// ============================================================
#define EVENT_START_RUN 1   // tombol START ditekan → mesin akan nyala
#define EVENT_STOP_RUN 2    // foreman stop → mesin mati
#define EVENT_START_LOST 3  // tombol emergency → lost time mulai
#define EVENT_STOP_LOST 4   // '1' OK → lost time selesai
#define EVENT_START_DOWN 5  // '0' ENG → down time mulai
#define EVENT_STOP_DOWN 6   // triple auth selesai → down time selesai
#define EVENT_COUNTER 7     // [BARU] setiap sensor mendeteksi benda
// ============================================================
// KONFIGURASI PIN ESP32-S3 N16R8
// ============================================================
const int pinRelayPLC = 20;
const int pinAlarm = 19;
const int pinSensorBenda = 4;
const int pinBtnForemanStop = 5;
const int pinBtnEmergency = 6;
const int pinLampiCounter = 7;
const int pinBtnStart = 2;
// const int pinDown = 35;

#define I2C_SDA 8
#define I2C_SCL 9

// ============================================================
// TIDAK ADA KODE LOKAL
// ESP hanya mengirim user_id ke server via MQTT.
// Server yang memvalidasi dari database dan membalas:
//   {"valid":"true","role":"foreman"/"sac"/"opr"/"eng",...}
// atau {"valid":"false",...}
// ============================================================

// ============================================================
// VARIABEL COUNTER
// ============================================================
int counterBenda = 0;
const int targetBenda = 5;
bool sedangDitekan = false;
bool lampiCounterOn = false;

// ============================================================
// VARIABEL AUTH STATE
// ============================================================
bool sacOk = false;
bool oprOk = false;
bool engOk = false;

// ============================================================
// FLAG KONDISI
// ============================================================
bool butuhTripleAuth = false;
bool stopDownTime = false;

// ============================================================
// VARIABEL WAKTU
// ============================================================
unsigned long totalLostTime = 0;
unsigned long totalDownTime = 0;
unsigned long lastMillisUpdate = 0;
unsigned long timerInterval = 0;
const unsigned long jedaLooping = 10000;
const unsigned long waktuDualAuth = 60000;

// ============================================================
// DEBOUNCE
// ============================================================
bool btnStartLastState = HIGH;
bool btnEmergencyLastState = HIGH;
bool btndwnLastState = HIGH;
unsigned long btnStartDebounce = 0;
unsigned long btnEmergencyDebounce = 0;
unsigned long btndwnDebounce = 0;
const unsigned long debounceDelay = 50;

// ============================================================
// VARIABEL VALIDASI SERVER
// ============================================================
enum ValidationResult { VAL_PENDING,
                        VAL_VALID,
                        VAL_INVALID };
volatile ValidationResult serverReply = VAL_PENDING;

// Role yang diterima dari server
String serverRole = "";

const unsigned long VALIDATION_TIMEOUT = 10000;
unsigned long validationStartTime = 0;
String lastUserId = "";

// ============================================================
// KEYPAD 4x4
// ============================================================
#define ROW_NUM 4
#define COLUMN_NUM 4
char keys[ROW_NUM][COLUMN_NUM] = {
  { '1', '2', '3', 'A' },
  { '4', '5', '6', 'B' },
  { '7', '8', '9', 'C' },
  { '*', '0', '#', 'D' }
};
byte pin_rows[ROW_NUM] = { 21, 18, 17, 10 };
byte pin_column[COLUMN_NUM] = { 11, 12, 13, 14 };

Keypad keypad = Keypad(makeKeymap(keys), pin_rows, pin_column, ROW_NUM, COLUMN_NUM);

// ============================================================
// LCD & MQTT
// ============================================================
LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// ============================================================
// STATE MACHINE
// ============================================================
enum State {
  ST_IDLE,
  ST_FOREMAN_LOGIN,
  ST_READY,
  ST_DUAL_AUTH,
  ST_WAITING_SERVER,  // menunggu balasan validasi server
  ST_RUNNING_PRODUCTION,
  ST_PERIODIC_AUTH,
  ST_REPAIRING,
  ST_HALT,
  ST_TRIPLE_AUTH,
  ST_STOP_AUTH,
  ST_PREVENTIVE_MT
};

State currentState = ST_IDLE;
State stateSebelumStop = ST_IDLE;
State waitingCallerState = ST_IDLE;  // state pemanggil sebelum masuk waiting
State stateAfterInvalid = ST_IDLE;   // state tujuan jika server balas invalid

String auth_input = "";

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
void gantiState(State s);
void kirimEvent(int eventId, String userId, int production = 0);
void showFeedback(const char* msg, int delayMs = 900);
void handleBackspace(int col, int row);
void updateStatusTriple();
void updateLampiCounter();
void restoreLCD(State s);
void mulaiValidasiServer(String userId, State callerState, State afterInvalid);

// ============================================================
// MQTT CALLBACK
// Format server: {"valid":"true","role":"sac",...}
//            atau {"valid":true, "role":"opr",...}
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (String(topic) != String(mqttTopicSub)) return;

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    serverReply = VAL_INVALID;
    return;
  }

  // Parse "valid" — handle string "true"/"false" DAN boolean
  bool valid = false;
  JsonVariant v = doc["valid"];
  if (v.is<bool>()) valid = v.as<bool>();
  else if (v.is<const char*>()) valid = (strcmp(v.as<const char*>(), "true") == 0);
  else if (v.is<int>()) valid = (v.as<int>() != 0);

  // Parse "role"
  serverRole = "";
  if (doc["role"].is<const char*>()) {
    serverRole = String(doc["role"].as<const char*>());
    serverRole.toLowerCase();
  }

  serverReply = valid ? VAL_VALID : VAL_INVALID;
}

// ============================================================
// MQTT: kirim event ke server
// JSON: {"Id_mesin":1, "event":X, "user_id":"Y", "production":Z}
// Field production: 0 untuk event non-counter, counterBenda untuk event 7
// ============================================================
void kirimEvent(int eventId, String userId, int production) {
  if (!mqttClient.connected()) return;
  StaticJsonDocument<128> doc;
  doc["Id_mesin"] = ID_MESIN;
  doc["event"] = eventId;
  doc["user_id"] = userId;
  doc["production"] = production;
  char payload[128];
  serializeJson(doc, payload);
  mqttClient.publish(mqttTopicPub, payload);
}
void kirimEventkhusus(int eventId, String userId) {
  if (!mqttClient.connected()) return;
  StaticJsonDocument<128> doc;
  doc["Id_mesin"] = ID_MESIN;
  doc["event"] = eventId;
  doc["user_id"] = userId;
  char payload[128];
  serializeJson(doc, payload);
  mqttClient.publish(mqttTopicPub, payload);
}

// ============================================================
// MQTT: reconnect non-blocking
// ============================================================
void reconnectMQTT() {
  if (mqttClient.connected()) return;
  String clientId = "ESP32_Mesin_" + String(ID_MESIN);
  if (mqttClient.connect(clientId.c_str())) {
    mqttClient.subscribe(mqttTopicSub);
  }
}

// ============================================================
// HELPER: feedback LCD baris 1
// ============================================================
void showFeedback(const char* msg, int delayMs) {
  lcd.setCursor(0, 1);
  lcd.print("                ");
  lcd.setCursor(0, 1);
  lcd.print(msg);
  delay(delayMs);
  lcd.setCursor(0, 1);
  lcd.print("                ");
}

// ============================================================
// HELPER: backspace
// ============================================================
void handleBackspace(int col, int row) {
  if (auth_input.length() > 0) {
    auth_input.remove(auth_input.length() - 1);
    lcd.setCursor(col + (int)auth_input.length(), row);
    lcd.print(" ");
    lcd.setCursor(col + (int)auth_input.length(), row);
  }
}

// ============================================================
// HELPER: status triple auth
// ============================================================
void updateStatusTriple() {
  lcd.setCursor(0, 1);
  lcd.print(sacOk ? "S:V " : "S:. ");
  lcd.print(engOk ? "E:V " : "E:. ");
  lcd.print(oprOk ? "O:V" : "O:.");
}

// ============================================================
// HELPER: lampu counter
// ============================================================
void updateLampiCounter() {
  if (counterBenda > 0 && (counterBenda % targetBenda == 0)) {
    lampiCounterOn = true;
  } else {
    lampiCounterOn = false;
  }
  digitalWrite(pinLampiCounter, lampiCounterOn ? HIGH : LOW);
}

// ============================================================
// HELPER: restore tampilan LCD setelah server balas
// ============================================================
void restoreLCD(State s) {
  lcd.clear();
  if (s == ST_FOREMAN_LOGIN) {
    lcd.setCursor(0, 0);
    lcd.print("PIN FOREMAN:");
  } else if (s == ST_DUAL_AUTH) {
    lcd.setCursor(0, 0);
    lcd.print("AUTH SAC&OPR    ");
    // lcd.setCursor(13, 0);
    // lcd.print(sacOk ? "S" : ".");
    // lcd.print(oprOk ? "O" : ".");
    // lcd.print(" ");
    lcd.setCursor(0, 1);
    lcd.print("ID: ");
  } else if (s == ST_PERIODIC_AUTH) {
    lcd.setCursor(0, 0);
    lcd.print("VERIFIKASI ULANG");
    lcd.setCursor(0, 1);
    lcd.print("ID:        T:");
  } else if (s == ST_TRIPLE_AUTH) {
    lcd.setCursor(0, 0);
    lcd.print("TRIPLE AUTH:");
    updateStatusTriple();
  } else if (s == ST_STOP_AUTH) {
    lcd.setCursor(0, 0);
    lcd.print("STOP? PIN:");
    lcd.setCursor(0, 1);
    lcd.print("A:ENT C:CANCEL");
  } else if (s == ST_READY) {
    lcd.setCursor(0, 0);
    lcd.print("MESIN SIAP");
    lcd.setCursor(0, 1);
    lcd.print("TEKAN START...");
  }
}

// ============================================================
// MULAI VALIDASI SERVER
// Kirim user_id ke server, masuk ST_WAITING_SERVER
// Tidak memanggil gantiState() agar flag auth tidak direset
// ============================================================
void mulaiValidasiServer(String userId, State callerState, State afterInvalid) {
  lastUserId = userId;
  waitingCallerState = callerState;
  stateAfterInvalid = afterInvalid;
  serverReply = VAL_PENDING;
  serverRole = "";
  validationStartTime = millis();

  // Kirim ke server: event 0 = request validasi user
  kirimEventkhusus(0, userId);

  currentState = ST_WAITING_SERVER;
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("MENUNGGU SERVER ");
  lcd.setCursor(0, 1);
  lcd.print("ID: " + userId);
}

// ============================================================
// GANTI STATE — logika kontrol tidak berubah dari Doc12
// ============================================================
void gantiState(State s) {
  currentState = s;
  lcd.clear();
  auth_input = "";
  timerInterval = millis();

  if (s == ST_PREVENTIVE_MT){
    digitalWrite(pinRelayPLC, HIGH);
    lcd.setCursor(0, 0);
    lcd.print("MODE PREV MAINTE");
    lcd.setCursor(0,1);
    lcd.print("           D=STP");
    digitalWrite(pinRelayPLC, LOW);
  } else if (s == ST_IDLE) {
    digitalWrite(pinRelayPLC, HIGH);
    digitalWrite(pinAlarm, LOW);
    digitalWrite(pinLampiCounter, LOW);
    counterBenda = 0;
    lampiCounterOn = false;
    sacOk = oprOk = engOk = false;
    totalLostTime = 0;
    totalDownTime = 0;
    butuhTripleAuth = false;
    stopDownTime = false;
    serverReply = VAL_PENDING;
    serverRole = "";
    lcd.setCursor(0, 0);
    lcd.print("SISTEM STANDBY");
    lcd.setCursor(0, 1);
    lcd.print("AWAIT FOREMAN...");
  } else if (s == ST_FOREMAN_LOGIN) {
    lcd.setCursor(0, 0);
    lcd.print("PIN FOREMAN:");
  } else if (s == ST_READY) {
    digitalWrite(pinRelayPLC, HIGH);
    digitalWrite(pinAlarm, LOW);
    sacOk = oprOk = false;
    lcd.setCursor(0, 0);
    lcd.print("MESIN SIAP");
    lcd.setCursor(0, 1);
    lcd.print("TEKAN START...");
  } else if (s == ST_DUAL_AUTH) {
    digitalWrite(pinRelayPLC, HIGH);
    digitalWrite(pinAlarm, LOW);
    sacOk = oprOk = false;
    lcd.setCursor(0, 0);
    lcd.print("AUTH SAC&OPR");
    lcd.setCursor(0, 1);
    lcd.print("ID: ");
  } else if (s == ST_RUNNING_PRODUCTION) {
    digitalWrite(pinRelayPLC, LOW);
    digitalWrite(pinAlarm, LOW);
    lcd.setCursor(0, 0);
    lcd.print("MESIN ON ");
    lcd.setCursor(0, 1);
    lcd.print("NEXT: ");
  } else if (s == ST_PERIODIC_AUTH) {
    digitalWrite(pinAlarm, HIGH);
    lcd.setCursor(0, 0);
    lcd.print("VERIFIKASI ULANG");
    lcd.setCursor(0, 1);
    lcd.print("ID:        T:");
  } else if (s == ST_REPAIRING) {
    digitalWrite(pinRelayPLC, HIGH);
    digitalWrite(pinAlarm, LOW);
    // totalLostTime = 0;
    lcd.setCursor(0, 0);
    lcd.print("LOST TIME!");
    lcd.setCursor(0, 1);
    lcd.print("1:OK | 0:DWN");
  } else if (s == ST_HALT) {
    digitalWrite(pinRelayPLC, HIGH);
    digitalWrite(pinAlarm, LOW);
    totalDownTime = 0;
    stopDownTime = false;
    lcd.setCursor(0, 0);
    lcd.print("DOWN TIME!");
    lcd.setCursor(0, 1);
    lcd.print("A: RECOVERY");
  } else if (s == ST_TRIPLE_AUTH) {
    digitalWrite(pinRelayPLC, HIGH);
    digitalWrite(pinAlarm, LOW);
    sacOk = oprOk = engOk = false;
    lcd.setCursor(0, 0);
    lcd.print("TRIPLE AUTH:");
    updateStatusTriple();
  } else if (s == ST_STOP_AUTH) {
    lcd.setCursor(0, 0);
    lcd.print("STOP? PIN:");
    lcd.setCursor(0, 1);
    lcd.print("A:ENT C:CANCEL");
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  // WiFiManager wm;
  // wm.resetSettings();
  pinMode(pinRelayPLC, OUTPUT);
  pinMode(pinAlarm, OUTPUT);
  pinMode(pinBtnForemanStop, INPUT_PULLUP);
  pinMode(pinBtnEmergency, INPUT_PULLUP);
  pinMode(pinBtnStart, INPUT_PULLUP);
  pinMode(pinSensorBenda, INPUT_PULLUP);
  pinMode(pinLampiCounter, OUTPUT);
  // pinMode(pinDown, INPUT_PULLUP);

  digitalWrite(pinRelayPLC, HIGH);
  digitalWrite(pinAlarm, LOW);
  digitalWrite(pinLampiCounter, LOW);

  Wire.begin(I2C_SDA, I2C_SCL);
  delay(500);
  lcd.init();
  lcd.backlight();

  // Koneksi WiFi
  lcd.setCursor(0, 0);
  lcd.print("WiFi Manager... ");
  // WiFi.begin(ssid, wifiPass);
  bool res = wm.autoConnect("ESP32_Config", "rahasia123");
  //int wifiTry = 0;
  //while (WiFi.status() != WL_CONNECTED && wifiTry < 20) {
  //  delay(500); wifiTry++;
  //}
  if (!res) {
    Serial.println("Gagal konek ke WiFi dan kehabisan waktu (timeout).");
    lcd.setCursor(0, 1);
    lcd.print("Gagal Konek Wifi");
    delay(800);
  } else {
    Serial.print("Wifi Tersambung!");
    Serial.print("IP Address ESP32: ");
    Serial.println(WiFi.localIP());
    lcd.setCursor(0, 1);
    lcd.print("Wifi Tersambung!");
    lcd.setCursor(0, 0);
    lcd.print(WiFi.localIP());
    delay(3500);
    ElegantOTA.begin(&server);
    server.begin();
    Serial.println("Web Upload Program Nyala Gengs");
  }
  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);
  reconnectMQTT();

  gantiState(ST_IDLE);
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  server.handleClient();
  ElegantOTA.loop();
  mqttClient.loop();
  reconnectMQTT();

  char key = keypad.getKey();
  unsigned long skrg = millis();
  // {
  //   if (currentState != ST_READY && currentState != ST_DUAL_AUTH &&  currentState != ST_WAITING_SERVER && currentState != ST_RUNNING_PRODUCTION && currentState != ST_PERIODIC_AUTH && currentState != ST_REPAIRING && currentState != ST_HALT && currentState != ST_TRIPLE_AUTH && currentState != ST_STOP_AUTH){
  //     if (key == 'C'){
  //       gantiState(ST_PREVENTIVE_MT);
  //     }
  //   }
  // }
{
    // bool btndwnNow = digitalRead(pinDown);
    // if (btndwnNow != btndwnLastState) btndwnDebounce = skrg;
    // if ((skrg - btnEmergencyDebounce) > debounceDelay) {
      if (key == 'C') {
        if (currentState != ST_PREVENTIVE_MT && currentState != ST_REPAIRING && currentState != ST_HALT && currentState != ST_TRIPLE_AUTH && currentState != ST_STOP_AUTH && currentState != ST_WAITING_SERVER) {
          gantiState(ST_HALT);
          kirimEventkhusus(EVENT_STOP_LOST, lastUserId);
          delay(300);
          kirimEventkhusus(EVENT_START_DOWN, lastUserId);
          // btndwnLastState = btndwnNow;
          return;
        }
      }
    }
    // btndwnLastState = btndwnNow;
  

  // ----------------------------------------------------------
  // TOMBOL EMERGENCY — debounce
  // ----------------------------------------------------------
  {
    bool btnEmNow = digitalRead(pinBtnEmergency);
    if (btnEmNow != btnEmergencyLastState) btndwnDebounce = skrg;
    if ((skrg - btndwnDebounce) > debounceDelay) {
      if (btnEmNow == LOW) {
        if (currentState != ST_PREVENTIVE_MT && currentState != ST_READY && currentState != ST_IDLE && currentState != ST_REPAIRING && currentState != ST_HALT && currentState != ST_TRIPLE_AUTH && currentState != ST_STOP_AUTH && currentState != ST_WAITING_SERVER) {
          gantiState(ST_REPAIRING);
          kirimEventkhusus(EVENT_START_LOST, lastUserId);
          btnEmergencyLastState = btnEmNow;
          return;
        }
      }
    }
    btnEmergencyLastState = btnEmNow;
  }

  // ----------------------------------------------------------
  // TOMBOL FOREMAN STOP
  // ----------------------------------------------------------
  if (digitalRead(pinBtnForemanStop) == LOW) {
    if (currentState != ST_IDLE && currentState != ST_STOP_AUTH && currentState != ST_REPAIRING && currentState != ST_HALT && currentState != ST_TRIPLE_AUTH && currentState != ST_WAITING_SERVER) {
      stateSebelumStop = currentState;
      gantiState(ST_STOP_AUTH);
      return;
    }
  }

  // ----------------------------------------------------------
  // TOMBOL START — debounce, hanya aktif di ST_READY
  // ----------------------------------------------------------
  {
    bool btnStartNow = digitalRead(pinBtnStart);
    if (btnStartNow != btnStartLastState) btnStartDebounce = skrg;
    if ((skrg - btnStartDebounce) > debounceDelay) {
      if (btnStartNow == LOW){
        if (currentState == ST_READY){  // [FIX 1] event start saat tombol START ditekan
        Serial.println("[BTN START] Tombol START ditekan → EVENT_START_RUN dikirim");
        gantiState(ST_DUAL_AUTH);
        btnStartLastState = btnStartNow;
        return;
        }else if (currentState == ST_IDLE || currentState == ST_FOREMAN_LOGIN){
          Serial.println("Masuk Mode Preventive Maintenance");
          gantiState(ST_PREVENTIVE_MT);
          btnStartLastState = btnStartNow;
          return;
        }
      }
    }
    btnStartLastState = btnStartNow;
  }

  // ----------------------------------------------------------
  // COUNTER SENSOR — aktif saat dual auth, running, periodic
  // [EVENT 7] Setiap sensor mendeteksi → langsung kirim MQTT
  // ----------------------------------------------------------
  if (currentState == ST_DUAL_AUTH || currentState == ST_RUNNING_PRODUCTION || currentState == ST_PERIODIC_AUTH) {
    if (digitalRead(pinSensorBenda) == LOW && !sedangDitekan) {
      counterBenda++;
      sedangDitekan = true;
      updateLampiCounter();
      // [EVENT 7] Kirim langsung setiap counter naik
      kirimEvent(EVENT_COUNTER, lastUserId, counterBenda);
    }
    if (digitalRead(pinSensorBenda) == HIGH) sedangDitekan = false;
  }

  // ----------------------------------------------------------
  // DISPLAY LCD
  // ----------------------------------------------------------
  //
  if (currentState == ST_REPAIRING) {
    lcd.setCursor(0, 1);
    lcd.print("LT! 1:FIX 0:DWN ");
    lcd.setCursor(0, 0);
    lcd.print("SEGERA PERBAIKI!");
  } else if (!stopDownTime && (currentState == ST_HALT || currentState == ST_TRIPLE_AUTH)) {
    lcd.setCursor(0, 1);
    lcd.print("DT! A:START     ");
    lcd.setCursor(0, 0);
    lcd.print("SEGERA PERBAIKI!");
  }


// ----------------------------------------------------------
// STATE MACHINE
// ----------------------------------------------------------
switch (currentState) {

  case ST_PREVENTIVE_MT:
    {
      // Hitung sisa waktu dari 60 detik
      long sisa = 60L - (long)((skrg - timerInterval) / 1000);
      
      // Tampilkan timer di layar
      lcd.setCursor(0, 1);
      if (sisa >= 0) {
        lcd.print("Waktu: ");
        if (sisa < 10) lcd.print(" "); // Biar angka ga nge-bug
        lcd.print(String(sisa) + "s");
      }

      // Kondisi 1: Waktu habis otomatis
      if (sisa <= 0) {
        showFeedback("MT SELESAI!     ", 1000);
        gantiState(ST_IDLE); // Otomatis balik ke IDLE, mesin mati (karena IDLE relay HIGH)
        break;
      }

      // Kondisi 2: Dibatalkan secara manual sebelum waktu habis
      // Misalnya dengan menekan tombol 'C' di keypad
      if (key == 'D') {
        showFeedback("MT SELESAI!     ", 1000);
        gantiState(ST_IDLE); // Langsung balik ke IDLE dan matikan mesin
      }
    }
    break;

  // --------------------------------------------------------
    // --------------------------------------------------------
  case ST_IDLE:
    if (key) {
      if (key == '*') {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("MODE GANTI WIFI ");
        lcd.setCursor(0, 1);
        lcd.print("Konek AP ESP32  ");
        
        Serial.println("Membuka portal WiFi on-demand...");
        WiFi.disconnect(); // Putuskan koneksi saat ini
        delay(200);
        
        WiFi.mode(WIFI_AP_STA); // Aktifkan mode pancar hotspot + penerima
        wm.setConfigPortalTimeout(120); // Batasi waktu portal 2 menit
        
        // Membuka portal dengan nama "Mesin Filling 1" dan password "berbek124"
        if (!wm.startConfigPortal("Mesin Filling 1", "berbek124")) {
          // --- KONDISI GAGAL ATAU TIMEOUT ---
          showFeedback("GAGAL/TIMEOUT!  ", 1500);
          Serial.println("Batal setting, menyambung ulang ke WiFi lama...");
          
          WiFi.mode(WIFI_STA); // Matikan pancaran hotspot (AP)
          WiFi.begin();        // Panggil kredensial lama di memori tanpa parameter
          
          // Tunggu koneksi ulang maksimal 5 detik
          int waitTimeout = 0;
          lcd.setCursor(0, 1);
          lcd.print("RECONNECTING... ");
          while (WiFi.status() != WL_CONNECTED && waitTimeout < 10) {
            delay(500);
            waitTimeout++;
          }
          
          if (WiFi.status() == WL_CONNECTED) {
            showFeedback("TERHUBUNG LAGI! ", 1000);
            reconnectMQTT(); // Pastikan MQTT tersambung lagi
          } else {
            showFeedback("WIFI OFFLINE!   ", 1500);
          }
          
          gantiState(ST_IDLE); // Kembalikan layar ke mode standby
          
        } else {
          // --- KONDISI BERHASIL SIMPAN WIFI BARU ---
          showFeedback("WIFI TERSIMPAN! ", 1500);
          Serial.println("WiFi baru tersimpan, menyesuaikan koneksi...");
          
          WiFi.mode(WIFI_STA); // Matikan pancaran hotspot (AP)
          
          // Putuskan MQTT lama, lalu sambungkan dengan jaringan (IP) yang baru
          mqttClient.disconnect(); 
          reconnectMQTT();
          
          gantiState(ST_IDLE); // Kembalikan layar ke mode standby
        }
      } else {
        // Jika tombol yang ditekan BUKAN bintang
        gantiState(ST_FOREMAN_LOGIN);
      }
    }
    break;
  // --------------------------------------------------------
  // ST_FOREMAN_LOGIN — input bebas, validasi ke server
  // --------------------------------------------------------
  case ST_FOREMAN_LOGIN:
    if (key) {
      if (key == 'A') {
        if (auth_input.length() > 0) {
          mulaiValidasiServer(auth_input, ST_FOREMAN_LOGIN, ST_FOREMAN_LOGIN);
        }
        auth_input = "";
      } else if (key == 'B') {
        handleBackspace(12, 0);
      } else if (key >= '0' && key <= '9' && auth_input.length() < 8) {
        auth_input += key;
        lcd.setCursor(12, 0);
        lcd.print(auth_input);
      }
    }
    break;

  // --------------------------------------------------------
  // ST_READY — tunggu tombol START (ditangani di atas switch)
  // --------------------------------------------------------
  case ST_READY:
    break;

  // --------------------------------------------------------
  // ST_DUAL_AUTH — input bebas, validasi ke server
  // Countdown 60 detik, timeout → ST_READY
  // --------------------------------------------------------
  case ST_DUAL_AUTH:
    {
      long sisa = (long)(waktuDualAuth / 1000) - (long)((skrg - timerInterval) / 1000);
      lcd.setCursor(13, 0);
      if (sisa >= 0) {
        if (sisa < 10) lcd.print(" ");
        lcd.print(String(sisa) + "s  ");
      }
      if (sisa <= 0) {
        showFeedback("WAKTU HABIS!", 1000);
        if (currentState != ST_REPAIRING && currentState != ST_HALT && currentState != ST_TRIPLE_AUTH && currentState != ST_STOP_AUTH && currentState != ST_WAITING_SERVER) {
          gantiState(ST_REPAIRING);
          kirimEventkhusus(EVENT_START_LOST, lastUserId);
          break;
        }
        // [POIN 2] Dual auth timeout → mesin tidak jadi nyala → kirim EVENT_STOP_RUN
      
      }
    }
    if (key) {
      if (key == 'B') {
        handleBackspace(4, 1);
      } else if (key >= '0' && key <= '9' && auth_input.length() < 8) {
        auth_input += key;
        lcd.setCursor(4, 1);
        lcd.print(auth_input);
      } else if (key == 'A') {
        if (auth_input.length() > 0) {
          mulaiValidasiServer(auth_input, ST_DUAL_AUTH, ST_DUAL_AUTH);
        }
        auth_input = "";
        if (currentState == ST_DUAL_AUTH) {
          lcd.setCursor(0, 0);
          lcd.print("AUTH SAC&OPR    ");
          lcd.setCursor(0, 1);
          lcd.print("ID: ");
          lcd.setCursor(13, 0);
          lcd.print(sacOk ? "S" : ".");
          lcd.print(oprOk ? "O" : ".");
          lcd.print(" ");
        }
      }
    }
    // Keduanya sudah valid → nyalakan mesin
    if (sacOk && oprOk) {
      gantiState(ST_RUNNING_PRODUCTION);
    }
    break;

  // --------------------------------------------------------
  // ST_WAITING_SERVER — tunggu balasan validasi
  // Timeout 10 detik → invalid
  // --------------------------------------------------------
  case ST_WAITING_SERVER:
    {
      unsigned long elapsed = millis() - validationStartTime;

      // Animasi loading
      int dots = (elapsed / 400) % 4;
      lcd.setCursor(15, 0);
      switch (dots) {
        case 0: lcd.print(" "); break;
        case 1: lcd.print("."); break;
        case 2: lcd.print(":"); break;
        case 3: lcd.print("*"); break;
      }

      if (elapsed >= VALIDATION_TIMEOUT && serverReply == VAL_PENDING) {
        serverReply = VAL_INVALID;
      }

      // ── VALID ──────────────────────────────────────────
      if (serverReply == VAL_VALID) {
        serverReply = VAL_PENDING;

        // Dari ST_FOREMAN_LOGIN
        if (waitingCallerState == ST_FOREMAN_LOGIN) {
          if (serverRole == "foreman") {
            showFeedback("KODE BENAR!", 800);
            kirimEventkhusus(EVENT_START_RUN, lastUserId);
            gantiState(ST_READY);
          } else {
            showFeedback("BUKAN FOREMAN!", 900);
            currentState = ST_FOREMAN_LOGIN;
            restoreLCD(ST_FOREMAN_LOGIN);
          }
          break;
        }

        // Dari ST_STOP_AUTH
        if (waitingCallerState == ST_STOP_AUTH) {
          if (serverRole == "foreman") {
            showFeedback("KODE BENAR!", 800);
            kirimEventkhusus(EVENT_STOP_RUN, lastUserId);
            gantiState(ST_IDLE);
          } else {
            showFeedback("BUKAN FOREMAN!", 900);
            currentState = ST_STOP_AUTH;
            restoreLCD(ST_STOP_AUTH);
          }
          break;
        }

        // Dari ST_DUAL_AUTH
        if (waitingCallerState == ST_DUAL_AUTH) {
          if (serverRole == "sac") {
            if (sacOk) {
              showFeedback("SAC SUDAH INPUT!", 900);
            } else {
              sacOk = true;
              showFeedback("SAC: TERVERIF!", 800);
            }
          } else if (serverRole == "opr") {
            if (oprOk) {
              showFeedback("OPR SUDAH INPUT!", 900);
            } else {
              oprOk = true;
              showFeedback("OPR: TERVERIF!", 800);
            }
          } else {
            showFeedback("ROLE TIDAK SESUAI", 900);
          }
          currentState = ST_DUAL_AUTH;
          restoreLCD(ST_DUAL_AUTH);
          break;
        }

        // Dari ST_PERIODIC_AUTH
        if (waitingCallerState == ST_PERIODIC_AUTH) {
          if (serverRole == "sac" || serverRole == "opr") {
            showFeedback("TERVERIFIKASI!", 800);
            gantiState(ST_RUNNING_PRODUCTION);
          } else {
            showFeedback("ROLE TIDAK SESUAI", 900);
            currentState = ST_PERIODIC_AUTH;
            restoreLCD(ST_PERIODIC_AUTH);
          }
          break;
        }

        // Dari ST_TRIPLE_AUTH
        if (waitingCallerState == ST_TRIPLE_AUTH) {
          if (serverRole == "sac") {
            if (sacOk) {
              showFeedback("SAC SUDAH INPUT!", 900);
            } else {
              sacOk = true;
              showFeedback("SAC: TERVERIF!", 800);
            }
          } else if (serverRole == "eng") {
            if (engOk) {
              showFeedback("ENG SUDAH INPUT!", 900);
            } else {
              engOk = true;
              showFeedback("ENG: TERVERIF!", 800);
            }
          } else if (serverRole == "opr") {
            if (oprOk) {
              showFeedback("OPR SUDAH INPUT!", 900);
            } else {
              oprOk = true;
              showFeedback("OPR: TERVERIF!", 800);
            }
          } else {
            showFeedback("ROLE TIDAK SESUAI", 900);
          }

          if (sacOk && engOk && oprOk) {
            kirimEventkhusus(EVENT_STOP_DOWN, lastUserId);
            butuhTripleAuth = false;
            gantiState(ST_RUNNING_PRODUCTION);  // running dulu, baru periodic
          } else {
            currentState = ST_TRIPLE_AUTH;
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("TRIPLE AUTH:");
            updateStatusTriple();
          }
          break;
        }

        // Fallback
        gantiState(stateAfterInvalid);
        break;
      }

      // ── INVALID / TIMEOUT ──────────────────────────────
      if (serverReply == VAL_INVALID) {
        serverReply = VAL_PENDING;
        if (elapsed >= VALIDATION_TIMEOUT) {
          showFeedback("TIMEOUT SERVER!", 900);
        } else {
          showFeedback("ID TIDAK VALID!", 900);
        }
        currentState = stateAfterInvalid;
        restoreLCD(stateAfterInvalid);
      }
    }
    break;

  // --------------------------------------------------------
  // ST_RUNNING_PRODUCTION
  // --------------------------------------------------------
  case ST_RUNNING_PRODUCTION:
    {
      long sisa = (long)(jedaLooping / 1000) - (long)((skrg - timerInterval) / 1000);
      lcd.setCursor(6, 1);
      if (sisa >= 0) {
        if (sisa < 10) lcd.print(" ");
        lcd.print(String(sisa) + "s      ");
      }
      lcd.setCursor(11, 0);
      lcd.print("C:");
      lcd.print(counterBenda % targetBenda == 0 && counterBenda > 0
                  ? targetBenda
                  : counterBenda % targetBenda);
      lcd.print("/");
      lcd.print(targetBenda);

      if (sisa <= 0) gantiState(ST_PERIODIC_AUTH);
    }
    break;

  // --------------------------------------------------------
  // ST_PERIODIC_AUTH — input bebas, validasi ke server
  // Timeout 30 detik → ST_READY
  // --------------------------------------------------------
  case ST_PERIODIC_AUTH:
    {
      long sisa = 30L - (long)((skrg - timerInterval) / 1000);
      lcd.setCursor(13, 1);
      if (sisa >= 0) {
        if (sisa < 10) lcd.print(" ");
        lcd.print(String(sisa) + "s");
      }
      if (sisa <= 0) {
        digitalWrite(pinRelayPLC, HIGH);
        digitalWrite(pinAlarm, HIGH);
        showFeedback("VERIF TIMEOUT!", 1000);
        // [POIN 2] Periodic timeout → mesin mati → kirim EVENT_STOP_RUN
        gantiState(ST_REPAIRING);
        kirimEventkhusus(EVENT_START_LOST, lastUserId);
        break;
      }
    }
    if (key) {
      if (key == 'B') {
        handleBackspace(4, 1);
      } else if (key >= '0' && key <= '9' && auth_input.length() < 8) {
        auth_input += key;
        lcd.setCursor(4, 1);
        lcd.print(auth_input);
      } else if (key == 'A') {
        if (auth_input.length() > 0) {
          mulaiValidasiServer(auth_input, ST_PERIODIC_AUTH, ST_PERIODIC_AUTH);
        }
        auth_input = "";
      }
    }
    break;

  // --------------------------------------------------------
  // ST_REPAIRING — lost time
  // '1' OK → ST_READY, '0' ENG → ST_HALT
  // --------------------------------------------------------
  case ST_REPAIRING:
    if (key == '1') {
      // [POIN 4] Lost time OK → kirim EVENT_STOP_LOST, kembali ke ST_READY
      kirimEventkhusus(EVENT_STOP_LOST, lastUserId);
      gantiState(ST_READY);
    } else if (key == '0') {
      // [POIN 4] Lost time selesai masuk downtime → kirim EVENT_STOP_LOST dulu
      kirimEventkhusus(EVENT_STOP_LOST, lastUserId);
      // [POIN 5] Jeda 300ms agar event 4 tidak tertumpuk dengan event 5
      delay(300);
      // Setelah jeda, kirim EVENT_START_DOWN
      kirimEventkhusus(EVENT_START_DOWN, lastUserId);
      butuhTripleAuth = true;
      gantiState(ST_HALT);
    }
    break;

  // --------------------------------------------------------
  // ST_HALT — down time
  // --------------------------------------------------------
  case ST_HALT:
    if (key == 'A') {
      stopDownTime = true;
      sacOk = oprOk = engOk = false;
      gantiState(ST_TRIPLE_AUTH);
    }
    break;

  // --------------------------------------------------------
  // ST_TRIPLE_AUTH — input bebas, validasi ke server
  // --------------------------------------------------------
  case ST_TRIPLE_AUTH:
    if (key) {
      if (key == 'B') {
        handleBackspace(12, 0);
      } else if (key >= '0' && key <= '9' && auth_input.length() < 8) {
        auth_input += key;
        lcd.setCursor(12, 0);
        lcd.print(auth_input);
      } else if (key == 'A') {
        if (auth_input.length() > 0) {
          mulaiValidasiServer(auth_input, ST_TRIPLE_AUTH, ST_TRIPLE_AUTH);
        }
        auth_input = "";
        if (currentState == ST_TRIPLE_AUTH) {
          lcd.setCursor(12, 0);
          lcd.print("        ");
        }
      }
    }
    break;

  // --------------------------------------------------------
  // ST_STOP_AUTH — input bebas, validasi ke server
  // --------------------------------------------------------
  case ST_STOP_AUTH:
    if (key) {
      if (key == 'B') {
        handleBackspace(10, 0);
      } else if (key >= '0' && key <= '9' && auth_input.length() < 8) {
        auth_input += key;
        lcd.setCursor(10, 0);
        lcd.print(auth_input);
      } else if (key == 'A') {
        if (auth_input.length() > 0) {
          mulaiValidasiServer(auth_input, ST_STOP_AUTH, ST_STOP_AUTH);
        }
        auth_input = "";
      } else if (key == 'C') {
        gantiState(stateSebelumStop);
      }
    }
    break;

}  // end switch
}