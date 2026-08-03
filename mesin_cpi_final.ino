#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <ElegantOTA.h>
#include <WebServer.h>
#include "EspUsbHost.h"

// ============================================================
// KONFIGURASI WiFi & MQTT
// ============================================================
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
#define EVENT_START_RUN 1   
#define EVENT_STOP_RUN 2    
#define EVENT_START_LOST 3  
#define EVENT_STOP_LOST 4   
#define EVENT_START_DOWN 5  
#define EVENT_STOP_DOWN 6   
#define EVENT_COUNTER 7     

// ============================================================
// KONFIGURASI PIN ESP32-S3 N16R8
// ============================================================
const int pinRelayPLC = 11; 
const int pinAlarm = 10;
const int pinSensorBenda = 4;
// const int pinBtnForemanStop = 14; // [DIHAPUS] Diganti input keyboard
const int pinBtnEmergency = 12;      // Sistem Active Low
const int pinLampiCounter = 7;
const int pinBtnStart = 13;

#define I2C_SDA 8
#define I2C_SCL 9

// ============================================================
// VARIABEL COUNTER
// ============================================================
int counterBenda = 0;
const int targetBenda = 70;
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
unsigned int sesiLostTime = 0;
unsigned int sesiDownTime = 0;
unsigned long lastMillisUpdate = 0;
unsigned long timerInterval = 0;
const unsigned long jedaLooping = 300000;
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
enum ValidationResult { VAL_PENDING, VAL_VALID, VAL_INVALID };
volatile ValidationResult serverReply = VAL_PENDING;

String serverRole = "";
const unsigned long VALIDATION_TIMEOUT = 10000;
unsigned long validationStartTime = 0;
String lastUserId = "";
String idOrangLama = "";
String auth_input = "";

// ============================================================
// USB KEYBOARD (Pengganti Keypad 4x4)
// ============================================================
EspUsbHost usb;
volatile char globalUsbKey = 0; 

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
  ST_IDLE, ST_FOREMAN_LOGIN, ST_READY,ST_WAIT_START_DELAY, ST_DUAL_AUTH,
  ST_WAITING_SERVER, ST_RUNNING_PRODUCTION, ST_PERIODIC_AUTH,
  ST_REPAIRING, ST_HALT, ST_TRIPLE_AUTH, ST_STOP_AUTH,
  ST_PREVENTIVE_MT, ST_GANTI_PEMAIN
};

State currentState = ST_IDLE;
State stateSebelumStop = ST_IDLE;
State waitingCallerState = ST_IDLE;  
State stateAfterInvalid = ST_IDLE;  
unsigned long waktuMulaiStop = 0; 

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
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (String(topic) != String(mqttTopicSub)) return;

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    serverReply = VAL_INVALID;
    return;
  }

  bool valid = false;
  JsonVariant v = doc["valid"];
  if (v.is<bool>()) valid = v.as<bool>();
  else if (v.is<const char*>()) valid = (strcmp(v.as<const char*>(), "true") == 0);
  else if (v.is<int>()) valid = (v.as<int>() != 0);

  serverRole = "";
  if (doc["role"].is<const char*>()) {
    serverRole = String(doc["role"].as<const char*>());
  }

  serverReply = valid ? VAL_VALID : VAL_INVALID;
}

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

void reconnectMQTT() {
  if (mqttClient.connected()) return;
  String clientId = "ESP32_Mesin_" + String(ID_MESIN);
  if (mqttClient.connect(clientId.c_str())) {
    mqttClient.subscribe(mqttTopicSub);
  }
}

// HELPER LCD
void showFeedback(const char* msg, int delayMs) {
  lcd.setCursor(0, 1);
  lcd.print("                ");
  lcd.setCursor(0, 1);
  lcd.print(msg);
  delay(delayMs);
  lcd.setCursor(0, 1);
  lcd.print("                ");
}

void handleBackspace(int col, int row) {
  if (auth_input.length() > 0) {
    auth_input.remove(auth_input.length() - 1);
    lcd.setCursor(col + (int)auth_input.length(), row);
    lcd.print(" ");
    lcd.setCursor(col + (int)auth_input.length(), row);
  }
}

void updateStatusTriple() {
  lcd.setCursor(0, 1);
  lcd.print(sacOk ? "S:V " : "S:. ");
  lcd.print(engOk ? "E:V " : "E:. ");
  lcd.print(oprOk ? "O:V" : "O:.");
}

void updateLampiCounter() {
  if (counterBenda > 0 && (counterBenda % targetBenda == 0)) {
    lampiCounterOn = true;
  } else {
    lampiCounterOn = false;
  }
  digitalWrite(pinLampiCounter, lampiCounterOn ? HIGH : LOW);
}

void restoreLCD(State s) {
  lcd.clear();
  if (s == ST_FOREMAN_LOGIN) {
    lcd.setCursor(0, 0);
    lcd.print("PIN FOREMAN:");
  } else if (s == ST_DUAL_AUTH) {
    lcd.setCursor(0, 0);
    lcd.print("AUTH SAC&OPR    ");
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
  } else if (s == ST_GANTI_PEMAIN) {
    lcd.setCursor(0, 0);
    lcd.print("GANTI ORANG (LT)");
    lcd.setCursor(0, 1);
    lcd.print("ID: ");
  }else if (s == ST_WAIT_START_DELAY) {
    lcd.setCursor(0, 0);
    lcd.print("MENUNGGU 1 MENIT");
    }
}

void mulaiValidasiServer(String userId, State callerState, State afterInvalid) {
  lastUserId = userId;
  waitingCallerState = callerState;
  stateAfterInvalid = afterInvalid;
  serverReply = VAL_PENDING;
  serverRole = "";
  validationStartTime = millis();

  kirimEventkhusus(0, userId);

  currentState = ST_WAITING_SERVER;
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("MENUNGGU SERVER ");
  lcd.setCursor(0, 1);
  lcd.print("ID: " + userId);
}

void gantiState(State s) {
  bool isMasukMenuStop = (s == ST_STOP_AUTH);
  bool isResume = (currentState == ST_STOP_AUTH && s == stateSebelumStop);

  currentState = s;
  lcd.clear();
  auth_input = "";

  if (isResume || isMasukMenuStop) {
    // nothing
  }else {
    timerInterval = millis();
  }
  if (s == ST_READY || s == ST_RUNNING_PRODUCTION || s == ST_IDLE){
    sesiLostTime = 0;
    sesiDownTime = 0;
  }
  if (s == ST_PREVENTIVE_MT){
    digitalWrite(pinRelayPLC, HIGH);
    lcd.setCursor(0, 0);
    lcd.print("MODE PREV MAINTE");
    lcd.setCursor(0,1);
    lcd.print("           D=STP");
  } else if (s == ST_IDLE) {
    digitalWrite(pinRelayPLC, LOW);
    digitalWrite(pinAlarm, HIGH);
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
    digitalWrite(pinRelayPLC, LOW);
    digitalWrite(pinAlarm, HIGH);
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
    digitalWrite(pinRelayPLC, HIGH);
    digitalWrite(pinAlarm, HIGH);
    lcd.setCursor(0, 0);
    lcd.print("MESIN ON ");
    lcd.setCursor(0, 1);
    lcd.print("NEXT: ");
  } else if (s == ST_PERIODIC_AUTH) {
    digitalWrite(pinAlarm, LOW);
    lcd.setCursor(0, 0);
    lcd.print("VERIFIKASI ULANG");
    lcd.setCursor(0, 1);
    lcd.print("ID:        T:");
  } else if (s == ST_REPAIRING) {
    digitalWrite(pinRelayPLC, LOW);
    digitalWrite(pinAlarm, LOW);
    lcd.setCursor(0, 0);
    lcd.print("LOST TIME!");
    lcd.setCursor(0, 1);
    lcd.print("1:OK | 0:DWN");
  } else if (s == ST_HALT) {
    digitalWrite(pinRelayPLC, LOW);
    digitalWrite(pinAlarm, LOW);
    stopDownTime = false;
    lcd.setCursor(0, 0);
    lcd.print("DOWN TIME!");
    lcd.setCursor(0, 1);
    lcd.print("A: RECOVERY");
  } else if (s == ST_TRIPLE_AUTH) {
    digitalWrite(pinRelayPLC, LOW);
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
  } else if (s == ST_GANTI_PEMAIN) {
    lcd.setCursor(0, 0);
    lcd.print("GANTI ORANG (LT)");
    lcd.setCursor(0, 1);
    lcd.print("ID: ");
  } else if (s == ST_WAIT_START_DELAY) {
    digitalWrite(pinRelayPLC, HIGH);
    digitalWrite(pinAlarm, HIGH);
    lcd.setCursor(0, 0);
    lcd.print("MENUNGGU 1 MENIT");
    lcd.setCursor(0, 1);
    lcd.print("SISA:           ");
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  pinMode(pinRelayPLC, OUTPUT);
  pinMode(pinAlarm, OUTPUT);
  // pinMode(pinBtnForemanStop, INPUT_PULLUP); // [DIHAPUS]
  pinMode(pinBtnEmergency, INPUT_PULLUP);
  pinMode(pinBtnStart, INPUT_PULLUP);
  pinMode(pinSensorBenda, INPUT_PULLUP);
  pinMode(pinLampiCounter, OUTPUT);

  digitalWrite(pinRelayPLC, LOW);
  digitalWrite(pinAlarm, HIGH);
  digitalWrite(pinLampiCounter, LOW);

  Wire.begin(I2C_SDA, I2C_SCL);
  delay(500);
  lcd.init();
  lcd.backlight();

  // Konfigurasi WiFi
  lcd.setCursor(0, 0);
  lcd.print("WiFi Manager... ");
  bool res = wm.autoConnect("ESP32_Config", "rahasia123");
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
    Serial.println("Web Upload Program Nyala");
  }

  // MQTT Init
  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);
  reconnectMQTT();

  // Konfigurasi USB Host
  usb.setKeyboardLayout(ESP_USB_HOST_KEYBOARD_LAYOUT_EN_US);

  usb.onDeviceConnected([](const EspUsbHostDeviceInfo &device) {
    Serial.print("USB terhubung: ");
    espUsbHostPrint(device);
  });

  usb.onDeviceDisconnected([](const EspUsbHostDeviceInfo &device) {
    Serial.print("USB terputus: ");
    espUsbHostPrint(device);
  });

  usb.onKeyboard([](const EspUsbHostKeyboardEvent &event) {
    if (!event.pressed) return; 

    char mappedKey = 0;
    
    // PEMETAAN KEYBOARD (DENGAN TAMBAHAN TOMBOL 'S' UNTUK STOP)
    if (event.ascii == '\r' || event.ascii == '\n') {
      mappedKey = 'A'; 
    } else if (event.ascii == 0x08 || event.ascii == 0x7F) {
      mappedKey = 'B'; 
    } else if (event.ascii == 0x1B) {
      mappedKey = 'C'; 
    } else if (event.ascii == 'D' || event.ascii == 'd') {
      mappedKey = 'D'; 
    } else if (event.ascii == 'S' || event.ascii == 's') { // [BARU] Memicu Stop Auth
      mappedKey = 'S'; 
    } else if (event.ascii == '*' || (event.ascii >= '0' && event.ascii <= '9')) {
      mappedKey = event.ascii; 
    }

    if (mappedKey != 0) {
      globalUsbKey = mappedKey;
    }
  });

  if (!usb.begin()) {
    Serial.printf("usb.begin() failed: %s\n", usb.lastErrorName());
  }

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
  
  // Baca buffer karakter dari USB Keyboard Callback
  char key = globalUsbKey;
  globalUsbKey = 0; // Bersihkan agar tidak terbaca 2x

  unsigned long skrg = millis();
  static unsigned long lastTimerUpdate = 0;
  
  if (skrg - lastTimerUpdate >= 1000) {
    lastTimerUpdate = skrg;

    bool isSesiLT = (currentState == ST_REPAIRING || currentState == ST_GANTI_PEMAIN || 
                    (currentState == ST_WAITING_SERVER && waitingCallerState == ST_GANTI_PEMAIN));
    bool isSesiDT = (currentState == ST_HALT || currentState == ST_TRIPLE_AUTH || 
                    (currentState == ST_WAITING_SERVER && waitingCallerState == ST_TRIPLE_AUTH));

    if (isSesiLT) {
      totalLostTime++;
      sesiLostTime++; 
      
      if (sesiLostTime <= 15) {
        digitalWrite(pinAlarm, LOW); 
      } else if (sesiLostTime > 15 && sesiLostTime <= 300) {
        digitalWrite(pinAlarm, HIGH); 
      } else if (sesiLostTime > 300 && sesiLostTime <= 315) {
        digitalWrite(pinAlarm, LOW); 
      } else {
        digitalWrite(pinAlarm, HIGH); 
      }
    } 
    else if (isSesiDT) {
      totalDownTime++;
      sesiDownTime++; 
      
      if (sesiDownTime <= 15) {
        digitalWrite(pinAlarm, LOW); 
      } else {
        digitalWrite(pinAlarm, HIGH); 
      }
    }
  }

  // ----------------------------------------------------------
  // INTERRUPT KEYBOARD
  // ----------------------------------------------------------
  if (key == 'C') {
    if (currentState != ST_PREVENTIVE_MT && currentState != ST_REPAIRING && currentState != ST_HALT && currentState != ST_TRIPLE_AUTH && currentState != ST_STOP_AUTH && currentState != ST_WAITING_SERVER) {
      gantiState(ST_HALT);
      kirimEventkhusus(EVENT_STOP_LOST, lastUserId);
      delay(300);
      kirimEventkhusus(EVENT_START_DOWN, lastUserId);
      return;
    }
  }

  // [BARU] LOGIKA FOREMAN STOP DARI KEYBOARD ('S')
  if (key == 'S') {
    if (currentState != ST_IDLE && currentState != ST_STOP_AUTH && currentState != ST_REPAIRING && currentState != ST_HALT && currentState != ST_TRIPLE_AUTH && currentState != ST_WAITING_SERVER) {
      stateSebelumStop = currentState;
      waktuMulaiStop = millis();
      gantiState(ST_STOP_AUTH);
      return;
    }
  }

  // ----------------------------------------------------------
  // TOMBOL EMERGENCY (System Active Low, ter-trigger bila LOW)
  // ----------------------------------------------------------
  {
    bool btnEmNow = digitalRead(pinBtnEmergency);
    if (btnEmNow != btnEmergencyLastState) btndwnDebounce = skrg;
    if ((skrg - btndwnDebounce) > debounceDelay) {
      // Tombol divalidasi dan memang memicu state Lost Time jika posisinya LOW
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
  // TOMBOL START
  // ----------------------------------------------------------
  {
    bool btnStartNow = digitalRead(pinBtnStart);
    if (btnStartNow != btnStartLastState) btnStartDebounce = skrg;
    if ((skrg - btnStartDebounce) > debounceDelay) {
      if (btnStartNow == LOW){
        if (currentState == ST_READY){
          Serial.println("[BTN START] Ditekan, menunggu 1 menit sebelum Dual Auth");
          gantiState(ST_WAIT_START_DELAY); 
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
  // COUNTER SENSOR
  // ----------------------------------------------------------
  if (currentState == ST_DUAL_AUTH || currentState == ST_RUNNING_PRODUCTION || currentState == ST_PERIODIC_AUTH) {
    if (digitalRead(pinSensorBenda) == LOW && !sedangDitekan) {
      counterBenda++;
      sedangDitekan = true;
      updateLampiCounter();
      kirimEvent(EVENT_COUNTER, lastUserId, counterBenda);
    }
    if (digitalRead(pinSensorBenda) == HIGH) sedangDitekan = false;
  }

  // ----------------------------------------------------------
  // DISPLAY LCD
  // ----------------------------------------------------------
  if (currentState == ST_REPAIRING) {
    lcd.setCursor(0, 1);
    lcd.print("1:FIX 0:DWN D:ID");
    lcd.setCursor(0, 0);
    lcd.print("SEGERA PERBAIKI!");
  } else if (!stopDownTime && (currentState == ST_HALT || currentState == ST_TRIPLE_AUTH)) {
    lcd.setCursor(0, 1);
    lcd.print("DT! A:START     ");
    lcd.setCursor(0, 0);
    lcd.print("SEGERA PERBAIKI!");
  }

  // ----------------------------------------------------------
  // STATE MACHINE SWITCH
  // ----------------------------------------------------------
  switch (currentState) {

    case ST_WAIT_START_DELAY:
      {
        long sisa = 60L - (long)((skrg - timerInterval) / 1000);
        lcd.setCursor(6, 1);
        if (sisa >= 0) {
          if (sisa < 10) lcd.print(" ");
          lcd.print(String(sisa) + "s  ");
        }
        if (sisa <= 0) {
          showFeedback("JEDA SELESAI!   ", 1000);
          gantiState(ST_DUAL_AUTH);
        }
      }
      break;

    case ST_PREVENTIVE_MT:
      {
        long sisa = 600L - (long)((skrg - timerInterval) / 1000);
        lcd.setCursor(0, 1);
        if (sisa >= 0) {
          lcd.print("Waktu: ");
          if (sisa < 100) lcd.print(" "); 
          if (sisa < 10) lcd.print(" "); 
          lcd.print(String(sisa) + "s     ");
        }
        if (sisa <= 0) {
          showFeedback("MT SELESAI!     ", 1000);
          gantiState(ST_IDLE); 
          break;
        }
        if (key == 'D') {
          showFeedback("MT SELESAI!     ", 1000);
          gantiState(ST_IDLE);
        }
      }
      break;

    case ST_IDLE:
      if (key) {
        if (key == '*') {
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("MODE GANTI WIFI ");
          lcd.setCursor(0, 1);
          lcd.print("Konek AP ESP32  ");
          
          Serial.println("Membuka portal WiFi on-demand...");
          WiFi.disconnect();
          delay(200);
          
          WiFi.mode(WIFI_AP_STA);
          wm.setConfigPortalTimeout(120); 
          
          if (!wm.startConfigPortal("Mesin Filling 1", "berbek124")) {
            showFeedback("GAGAL/TIMEOUT!  ", 1500);
            Serial.println("Batal setting, menyambung ulang ke WiFi lama...");
            
            WiFi.mode(WIFI_STA);
            WiFi.begin();        
            
            int waitTimeout = 0;
            lcd.setCursor(0, 1);
            lcd.print("RECONNECTING... ");
            while (WiFi.status() != WL_CONNECTED && waitTimeout < 10) {
              delay(500);
              waitTimeout++;
            }
            
            if (WiFi.status() == WL_CONNECTED) {
              showFeedback("TERHUBUNG LAGI! ", 1000);
              reconnectMQTT(); 
            } else {
              showFeedback("WIFI OFFLINE!   ", 1500);
            }
            gantiState(ST_IDLE); 
            
          } else {
            showFeedback("WIFI TERSIMPAN! ", 1500);
            Serial.println("WiFi baru tersimpan, menyesuaikan koneksi...");
            
            WiFi.mode(WIFI_STA); 
            mqttClient.disconnect(); 
            reconnectMQTT();
            
            gantiState(ST_IDLE); 
          }
        } else {
          gantiState(ST_FOREMAN_LOGIN);
        }
      }
      break;

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

    case ST_READY:
      break;

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
      if (sacOk && oprOk) {
        gantiState(ST_RUNNING_PRODUCTION);
      }
      break;

    case ST_WAITING_SERVER:
      {
        unsigned long elapsed = millis() - validationStartTime;

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

        if (serverReply == VAL_VALID) {
          serverReply = VAL_PENDING;

          if (waitingCallerState == ST_FOREMAN_LOGIN) {
            if (serverRole == "Foreman") {
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
          
          if (waitingCallerState == ST_GANTI_PEMAIN) {
            showFeedback("GANTI ID SUKSES!", 800);
            kirimEventkhusus(EVENT_STOP_LOST, idOrangLama);
            delay(300);
            kirimEventkhusus(EVENT_START_LOST, lastUserId);
            gantiState(ST_REPAIRING);
            break;
          }
          
          if (waitingCallerState == ST_STOP_AUTH) {
            if (serverRole == "Foreman") {
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

          if (waitingCallerState == ST_DUAL_AUTH) {
            if (serverRole == "Produksi") {
              if (!oprOk) { 
                oprOk = true; 
                showFeedback("OPR: TERVERIF!", 800); 
              } 
              else if (!sacOk) { 
                sacOk = true; 
                showFeedback("SAC: TERVERIF!", 800); 
              } 
              else {
                showFeedback("SUDAH INPUT SEMUA", 900);
              }
            } else {
              showFeedback("ROLE TIDAK SESUAI", 900);
            }
            currentState = ST_DUAL_AUTH;
            restoreLCD(ST_DUAL_AUTH);
            break;
          }

          if (waitingCallerState == ST_PERIODIC_AUTH) {
            if (serverRole == "Produksi") {
              showFeedback("TERVERIFIKASI!", 800);
              gantiState(ST_RUNNING_PRODUCTION);
            } else {
              showFeedback("ROLE TIDAK SESUAI", 900);
              currentState = ST_PERIODIC_AUTH;
              restoreLCD(ST_PERIODIC_AUTH);
            }
            break;
          }

          if (waitingCallerState == ST_TRIPLE_AUTH) {
            if (serverRole == "eng") {
              if (engOk) {
                showFeedback("ENG SUDAH INPUT!", 900);
              } else { 
                engOk = true; 
                showFeedback("ENG: TERVERIF!", 800); 
              }
            } 
            else if (serverRole == "Produksi") {
              if (!engOk) {
                showFeedback("TUNGGU ENG DULU!", 900);
              } 
              else if (!sacOk) { 
                sacOk = true; 
                showFeedback("SAC: TERVERIF!", 800); 
              } 
              else if (!oprOk) {
                oprOk = true; 
                showFeedback("OPR: TERVERIF!", 800); 
              } 
              else {
                showFeedback("SUDAH INPUT SEMUA", 900);
              }
            } 
            else {
              showFeedback("ROLE TIDAK SESUAI", 900);
            }

            if (sacOk && engOk && oprOk) {
              kirimEventkhusus(EVENT_STOP_DOWN, lastUserId);
              butuhTripleAuth = false;
              gantiState(ST_RUNNING_PRODUCTION); 
            } else {
              currentState = ST_TRIPLE_AUTH;
              lcd.clear();
              lcd.setCursor(0, 0);
              lcd.print("TRIPLE AUTH:");
              updateStatusTriple();
            }
            break;
          }

          gantiState(stateAfterInvalid);
          break;
        }

        if (serverReply == VAL_INVALID) {
          serverReply = VAL_PENDING;
          if (elapsed >= VALIDATION_TIMEOUT) {
            showFeedback("TIMEOUT SERVER!", 900);
          } else {
            showFeedback("ID TIDAK VALID!", 900);
          }
          if (waitingCallerState == ST_GANTI_PEMAIN) {
            lastUserId = idOrangLama;
          }
          currentState = stateAfterInvalid;
          restoreLCD(stateAfterInvalid);
        }
      }
      break;

    case ST_RUNNING_PRODUCTION:
      {
        long sisa = (long)(jedaLooping / 1000) - (long)((skrg - timerInterval) / 1000);
        lcd.setCursor(6, 1);
        if (sisa >= 0) {
          if (sisa < 10) lcd.print(" ");
          lcd.print(String(sisa) + "s      ");
        }
        lcd.setCursor(9, 0);
        lcd.print("C:");
        int tampilCounter = (counterBenda % targetBenda == 0 && counterBenda > 0) 
                              ? targetBenda 
                              : (counterBenda % targetBenda);
        if (tampilCounter <10){
          lcd.print(" ");
        }
        lcd.print(tampilCounter);
        lcd.print("/");
        lcd.print(targetBenda);
        lcd.print("  ");

        if (sisa <= 0) gantiState(ST_PERIODIC_AUTH);
      }
      break;

    case ST_PERIODIC_AUTH:
      {
        long sisa = 30L - (long)((skrg - timerInterval) / 1000);
        lcd.setCursor(13, 1);
        if (sisa >= 0) {
          if (sisa < 10) lcd.print(" ");
          lcd.print(String(sisa) + "s");
        }
        if (sisa <= 0) {
          digitalWrite(pinRelayPLC, LOW);
          digitalWrite(pinAlarm, LOW);
          showFeedback("VERIF TIMEOUT!", 1000);
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

    case ST_REPAIRING:
      if (key == '1') {
        kirimEventkhusus(EVENT_STOP_LOST, lastUserId);
        gantiState(ST_READY);
      } else if (key == '0') {
        kirimEventkhusus(EVENT_STOP_LOST, lastUserId);
        delay(300);
        kirimEventkhusus(EVENT_START_DOWN, lastUserId);
        butuhTripleAuth = true;
        gantiState(ST_HALT);
      } else if (key == 'D') {
        gantiState(ST_GANTI_PEMAIN);
      }
      break;

    case ST_HALT:
      if (key == 'A') {
        stopDownTime = true;
        sacOk = oprOk = engOk = false;
        gantiState(ST_TRIPLE_AUTH);
      }
      break;
      
    case ST_GANTI_PEMAIN:
      if (key) {
        if (key == 'B') {
          handleBackspace(4, 1);
        } else if (key >= '0' && key <= '9' && auth_input.length() < 8) {
          auth_input += key;
          lcd.setCursor(4, 1);
          lcd.print(auth_input);
        } else if (key == 'A') {
          if (auth_input.length() > 0) {
            idOrangLama = lastUserId; 
            mulaiValidasiServer(auth_input, ST_GANTI_PEMAIN, ST_GANTI_PEMAIN);
          }
          auth_input = "";
        } else if (key == 'C') {
          gantiState(ST_REPAIRING); 
        }
      }
      break;
    
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

  } 
}
