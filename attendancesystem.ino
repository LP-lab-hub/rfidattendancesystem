#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <HTTPClient.h>


const char* WIFI_SSID = "smthn";
const char* WIFI_PASSWORD = "s4mu3l1s$0m3one1mp";
const unsigned long SESSION_SEC = 30UL * 3;
const char* SERVER_URL = "http://192.168.137.157:5000/attendance";


#define RST 20
#define SS_PIN 17
#define BTN 15
#define BUZ 14
#define GREEN 12
#define RED 13
#define TRIG 6      // HC-SR04 trigger
#define ECHO 7      // HC-SR04 echo (through a voltage divider!)

const int GATE_CM = 10;                     // closer 80cm  than this = someone is in the gate
#define GATE_DEBUG 1   // 1 = print live sensor distances in Serial Monitor, 0 = silent
const unsigned long GATE_WINDOW_MS = 5000;  // after a valid tap: green LED on, student must pass the gate within this time

MFRC522 rfid(SS_PIN, RST);
LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522::MIFARE_Key key;

String seen[300];
int seenCount = 0;
int state = 0;
int menuSel = 0;
int sid = 0;
String lectureId = "";    // entered in Serial Monitor when a lecture starts
bool drawn = false;
bool msgShown = false;
bool eraseNext = false;   // true after typing 'd' in register mode
unsigned long startMs = 0, lastLcd = 0, msgUntil = 0;


void connectWiFi() {
  Serial.println("Connecting to WiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;

  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected!");
    Serial.print("Pico IP: ");
    Serial.println(WiFi.localIP());

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Connected");
    delay(1000);
  } else {
    Serial.println("WiFi connection failed");

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Failed");
    delay(1000);
  }
}

int readCm() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  unsigned long us = pulseIn(ECHO, HIGH, 25000);   // 25 ms timeout
  if (us == 0) return 999;                         // no echo = nothing close
  return us / 58;
}

// Green LED on for up to GATE_WINDOW_MS. Returns true if someone walks through the gate.
bool waitForGate() {
  digitalWrite(GREEN, HIGH);
  unsigned long t0 = millis();
  bool armed = true;
  int hits = 0;
  bool passed = false;

  int d = readCm();
#if GATE_DEBUG
  Serial.print("gate start d="); Serial.println(d);
#endif
  if (d > 2 && d < GATE_CM) armed = false;   // beam already blocked at tap time: it must clear first

  while (millis() - t0 < GATE_WINDOW_MS) {
    d = readCm();
#if GATE_DEBUG
    Serial.print("gate d="); Serial.println(d);
#endif
    bool person = (d > 2 && d < GATE_CM);
    if (person) {
      if (hits < 10) hits++;
      if (armed && hits >= 2) { passed = true; break; }   // 2 readings in a row = real person
    } else {
      hits = 0;
      armed = true;
    }
    delay(40);
  }

  digitalWrite(GREEN, LOW);
  return passed;
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(60000);
  delay(1500);

  pinMode(BTN, INPUT_PULLUP);
  pinMode(BUZ, OUTPUT);
  pinMode(GREEN, OUTPUT);
  pinMode(RED, OUTPUT);
  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);

  Wire.setSDA(4);
  Wire.setSCL(5);
  Wire.begin();
  lcd.init();
  lcd.backlight();

  connectWiFi();

  SPI.begin();
  rfid.PCD_Init();
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;
  LittleFS.begin();

  Serial.print("connected");
  lcd.print("Smart Attendance");
  delay(1200);
}

void loop() {

  // ---------- BUTTON: 0 = none, 1 = short press, 2 = hold 1 s ----------
  int btn = 0;
  if (digitalRead(BTN) == LOW) {
    delay(30);
    if (digitalRead(BTN) == LOW) {
      unsigned long t = millis();
      while (digitalRead(BTN) == LOW && millis() - t < 1000) delay(10);
      btn = (millis() - t >= 1000) ? 2 : 1;
      while (digitalRead(BTN) == LOW) delay(10);   // wait for release
    }
  }

  // ---------- SERIAL COMMANDS ----------
  // 1 = lecture, 2 = register, e = exit to menu, d = erase a card (register mode)
  if (Serial.available()) {
    char ch = Serial.read();
    if (ch == '1' && state == 0) { menuSel = 0; btn = 2; }
    else if (ch == '2' && state == 0) { menuSel = 1; btn = 2; }
    else if ((ch == 'e' || ch == 'E') && state != 0) { btn = 2; }
    else if ((ch == 'd' || ch == 'D') && state == 2) {
      eraseNext = true;
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("eraser");
      lcd.setCursor(0, 1); lcd.print("tap and hold");
    }
  }

  // ---------- READ CARD (lecture and register modes) ----------
  bool hasCard = false, ok = false, registered = false;
  String uid = "";
  char name[17] = {0}, roll[17] = {0};
  byte a[18], b[18], size;

  if (state != 0 && rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    hasCard = true;
    for (byte i = 0; i < rfid.uid.size && i < 4; i++) {
      if (rfid.uid.uidByte[i] < 0x10) uid += "0";
      uid += String(rfid.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();

    // name is stored in block 4, roll number in block 5
    ok = (rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, 7, &key, &(rfid.uid))
          == MFRC522::STATUS_OK);
    if (ok) {
      size = 18;
      ok = (rfid.MIFARE_Read(4, a, &size) == MFRC522::STATUS_OK);
    }
    if (ok) {
      size = 18;
      ok = (rfid.MIFARE_Read(5, b, &size) == MFRC522::STATUS_OK);
    }
    if (ok) {
      for (int i = 0; i < 16; i++) {
        char c = a[i];
        if (c < 32 || c > 126 || c == '|') c = ' ';
        name[i] = c;
        c = b[i];
        if (c < 32 || c > 126 || c == '|') c = ' ';
        roll[i] = c;
      }
      name[16] = 0;
      roll[16] = 0;
      for (int i = 15; i >= 0 && name[i] == ' '; i--) name[i] = 0;
      for (int i = 15; i >= 0 && roll[i] == ' '; i--) roll[i] = 0;
      registered = (name[0] != 0);      // empty name = new card
    }
    // in register mode keep the card selected (for writing a new card or erasing one)
    if (!(state == 2 && ok && (!registered || eraseNext))) {
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    }
  }

  int fb = 0;   // feedback: 1 = good (green), 2 = bad (red)

  // =====================================================
  //  STATE 0: MENU
  // =====================================================
  if (state == 0) {
    if (btn == 1) { menuSel = 1 - menuSel; drawn = false; }

    if (!drawn) {
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("Tap=next Hold=OK");
      lcd.setCursor(0, 1); lcd.print(menuSel == 0 ? "1.Setup lecture" : "2.Register cards");
      drawn = true;
    }

    if (btn == 2) {
      digitalWrite(BUZ, HIGH); delay(200); digitalWrite(BUZ, LOW);
      drawn = false;
      if (menuSel == 0) {
        // ---- ask for the lecture ID in the Serial Monitor ----
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("Enter lecture ID");
        lcd.setCursor(0, 1); lcd.print("in Serial");
        while (Serial.available()) Serial.read();
        Serial.println("Enter lecture name: ");
        lectureId = Serial.readStringUntil('\n');
        lectureId.trim();
        lectureId.replace("/", "-");     // keep records.txt and JSON safe
        lectureId.replace("\"", "");
        lectureId.replace("\\", "");
        lectureId.replace("|", "-");

        if (lectureId.length() == 0) {
          Serial.println("Cancelled - no lecture ID");
          lcd.clear();
          lcd.setCursor(0, 0); lcd.print("Cancelled");
          delay(1000);
        } else {
          int n = 0;
          File f = LittleFS.open("/session.txt", "r");
          if (f) { n = f.readString().toInt(); f.close(); }
          sid = n + 1;
          f = LittleFS.open("/session.txt", "w");
          if (f) { f.print(sid); f.close(); }
          seenCount = 0;
          startMs = millis();
          lastLcd = 0;
          msgUntil = 0;
          msgShown = true;
          Serial.print("Lecture ");
          Serial.print(lectureId);
          Serial.print(" started, session ");
          Serial.println(sid);
          state = 1;
        }
      } else {
        eraseNext = false;
        Serial.println("Register mode");
        state = 2;
      }
    }
  }

  // =====================================================
  //  STATE 1: LECTURE
  // =====================================================
  else if (state == 1) {
    unsigned long elapsed = (millis() - startMs) / 1000;

    if (btn == 2 || elapsed >= SESSION_SEC) {
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("Lecture ended");
      lcd.setCursor(0, 1); lcd.print("Attended: " + String(seenCount));
      Serial.print("Lecture ");
      Serial.print(lectureId);
      Serial.print(" ended. Present: ");
      Serial.println(seenCount);
      Serial.println("--- saved records (session/lecture/uid/name/roll/seconds) ---");
      File f = LittleFS.open("/records.txt", "r");
      if (f) {
        while (f.available()) Serial.write(f.read());
        f.close();
      }
      Serial.println("ended");
      digitalWrite(BUZ, HIGH); delay(1200); digitalWrite(BUZ, LOW);
      delay(1500);
      state = 0;
      drawn = false;
    } else {
      if (millis() > msgUntil) {
        if (msgShown) {
          lcd.clear();
          lcd.setCursor(0, 1); lcd.print("Scan your card");
          msgShown = false;
          lastLcd = 0;
        }
        if (millis() - lastLcd >= 1000) {
          lastLcd = millis();
          unsigned long left = SESSION_SEC - elapsed;
          char l1[17];
          snprintf(l1, sizeof(l1), "Left %02lu:%02lu P:%-3d", left / 60, left % 60, seenCount);
          lcd.setCursor(0, 0);
          lcd.print(l1);
        }
      }

      if (hasCard) {
        lcd.clear();
        if (!ok) {
          lcd.setCursor(0, 0); lcd.print("Card error");
          lcd.setCursor(0, 1); lcd.print("Try again");
          fb = 2;
        } else if (!registered) {
          lcd.setCursor(0, 0); lcd.print("ACCESS DENIED");
          lcd.setCursor(0, 1); lcd.print("Not registered");
          Serial.println("Access denied - unregistered card " + uid);
          fb = 2;
        } else {
          bool dup = false;
          for (int i = 0; i < seenCount; i++) {
            if (seen[i] == uid) dup = true;
          }
          if (dup) {
            lcd.setCursor(0, 0); lcd.print("Already");
            lcd.setCursor(0, 1); lcd.print("attending");
            Serial.print("Already attending: ");
            Serial.println(name);
            fb = 2;
          } else {
            // valid card: green LED on, student must walk through the gate in time
            lcd.setCursor(0, 0); lcd.print(name);
            lcd.setCursor(0, 1); lcd.print("Walk through...");
            Serial.print("Card OK: ");
            Serial.print(name);
            Serial.println(" - waiting for gate");

            if (waitForGate()) {
              if (seenCount < 300) seen[seenCount++] = uid;
              File f = LittleFS.open("/records.txt", "a");
              if (f) {
                f.printf("%d/%s/%s/%s/%s/%lu\n", sid, lectureId.c_str(), uid.c_str(), name, roll, elapsed);
                f.close();
              }
              sendAttendance(uid, String(name), String(roll), sid, lectureId, elapsed);
              lcd.clear();
              lcd.setCursor(0, 0); lcd.print(name);
              lcd.setCursor(0, 1); lcd.print("Roll:"); lcd.print(roll);
              Serial.print("Present: ");
              Serial.print(name);
              Serial.print(" / ");
              Serial.print(roll);
              Serial.print(" / ");
              Serial.print(elapsed);
              Serial.println("s");
              fb = 1;
            } else {
              lcd.clear();
              lcd.setCursor(0, 0); lcd.print("DENIED");
              lcd.setCursor(0, 1); lcd.print("No gate entry");
              Serial.print("Denied - nobody passed the gate: ");
              Serial.println(name);
              fb = 3;
            }
          }
        }
        msgShown = true;
        msgUntil = millis() + 1500;
      }
    }
  }

  // =====================================================
  //  STATE 2: REGISTER / ERASE CARDS
  // =====================================================
  else if (state == 2) {
    if (btn == 2) {                       // exit to menu
      eraseNext = false;
      state = 0;
      drawn = false;
    } else {
      if (!drawn) {
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("REGISTER MODE");
        lcd.setCursor(0, 1); lcd.print("Tap a new card");
        Serial.println("Register mode: tap a new card / d = erase a card / e = exit");
        drawn = true;
      }

      if (hasCard) {
        lcd.clear();
        if (!ok) {
          lcd.setCursor(0, 0); lcd.print("Card error");
          lcd.setCursor(0, 1); lcd.print("Try again");
          fb = 2;

        } else if (registered && eraseNext) {
          // ---- ERASE a registered card ----
          byte z[16] = {0};
          MFRC522::StatusCode e1 = rfid.MIFARE_Write(4, z, 16);
          MFRC522::StatusCode e2 = rfid.MIFARE_Write(5, z, 16);
          rfid.PICC_HaltA();
          rfid.PCD_StopCrypto1();
          eraseNext = false;
          if (e1 == MFRC522::STATUS_OK && e2 == MFRC522::STATUS_OK) {
            lcd.setCursor(0, 0); lcd.print("Card erased");
            lcd.setCursor(0, 1); lcd.print(name);
            Serial.print("Erased: ");
            Serial.print(name);
            Serial.print(" / ");
            Serial.println(roll);
            fb = 1;
          } else {
            lcd.setCursor(0, 0); lcd.print("Erase failed");
            lcd.setCursor(0, 1); lcd.print("Keep card still");
            Serial.println("Erase failed - tap again and keep the card on the reader");
            fb = 2;
          }

        } else if (!registered && eraseNext) {
          lcd.setCursor(0, 0); lcd.print("Already empty");
          Serial.println("Card is already empty");
          rfid.PICC_HaltA();
          rfid.PCD_StopCrypto1();
          eraseNext = false;
          fb = 2;

        } else if (registered) {
          lcd.setCursor(0, 0); lcd.print("Already saved:");
          lcd.setCursor(0, 1); lcd.print(name);
          Serial.print("Card already registered: ");
          Serial.print(name);
          Serial.print("/");
          Serial.println(roll);
          fb = 2;

        } else {
          // ---- NEW CARD: ask for details in Serial Monitor ----
          lcd.setCursor(0, 0); lcd.print("New card found!");
          lcd.setCursor(0, 1); lcd.print("Type in Serial");
          Serial.println("New card " + uid + " - keep it on the reader.");
          while (Serial.available()) Serial.read();

          Serial.println("Enter NAME (max 16 chars) then press Enter:");
          String n = Serial.readStringUntil('\n');
          n.trim();
          String r = "";
          if (n.length() > 0) {
            Serial.println("Enter ROLL NUMBER (max 10 chars) then press Enter:");
            r = Serial.readStringUntil('\n');
            r.trim();
          }

          lcd.clear();
          if (n.length() == 0 || r.length() == 0) {
            lcd.setCursor(0, 0); lcd.print("Cancelled");
            Serial.println("Cancelled");
            fb = 2;
          } else {
            byte w1[18], w2[18];
            snprintf((char*)w1, 17, "%-16.16s", n.c_str());
            snprintf((char*)w2, 17, "%-16.10s", r.c_str());
            MFRC522::StatusCode s1 = rfid.MIFARE_Write(4, w1, 16);
            MFRC522::StatusCode s2 = rfid.MIFARE_Write(5, w2, 16);
            if (s1 == MFRC522::STATUS_OK && s2 == MFRC522::STATUS_OK) {
              lcd.setCursor(0, 0); lcd.print("Registered!");
              lcd.setCursor(0, 1); lcd.print(n.substring(0, 16));
              Serial.println("Registered: " + n + " | " + r);
              fb = 1;
            } else {
              lcd.setCursor(0, 0); lcd.print("Write failed");
              lcd.setCursor(0, 1); lcd.print("Keep card still");
              Serial.println("Write failed - tap the card again and keep it on the reader");
              fb = 2;
            }
          }
          rfid.PICC_HaltA();
          rfid.PCD_StopCrypto1();
        }
        delay(2000);          // show the result
        drawn = false;        // then redraw the prompt
      }
    }
  }

  // ---------- LED + buzzer feedback ----------
  if (fb == 1) {
    digitalWrite(GREEN, HIGH);
    digitalWrite(BUZ, HIGH); delay(100); digitalWrite(BUZ, LOW);
    delay(200);
    digitalWrite(GREEN, LOW);
  }
  if (fb == 2) {
    digitalWrite(RED, HIGH);
    for (int i = 0; i < 2; i++) {
      digitalWrite(BUZ, HIGH); delay(60);
      digitalWrite(BUZ, LOW);  delay(60);
    }
    digitalWrite(RED, LOW);
  }

  if (fb == 3) {             // gate denial: red LED + 3 long beeps
    digitalWrite(RED, HIGH);
    for (int i = 0; i < 3; i++) {
      digitalWrite(BUZ, HIGH); delay(300);
      digitalWrite(BUZ, LOW);  delay(100);
    }
    delay(500);
    digitalWrite(RED, LOW);
  }

  delay(50);
}


void sendAttendance(String uid, String name, String roll, int session, String lecture, unsigned long elapsed) {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Attendance not uploaded.");
    return;
  }

  HTTPClient http;

  if (http.begin(SERVER_URL)) {

    http.setTimeout(3000);   // don't freeze the scanner if the server is down
    http.addHeader("Content-Type", "application/json");

    String json = "{";
    json += "\"session\":" + String(session) + ",";
    json += "\"lecture\":\"" + lecture + "\",";
    json += "\"uid\":\"" + uid + "\",";
    json += "\"name\":\"" + name + "\",";
    json += "\"roll\":\"" + roll + "\",";
    json += "\"elapsed\":" + String(elapsed);
    json += "}";

    Serial.println("Sending:");
    Serial.println(json);

    int responseCode = http.POST(json);

    Serial.print("Server response: ");
    Serial.println(responseCode);

    if (responseCode > 0) {
      String response = http.getString();
      Serial.println(response);
    } else {
      Serial.println("Upload failed");
    }

    http.end();

  } else {
    Serial.println("Could not connect to server");
  }
}
