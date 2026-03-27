#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>

// ================= WIFI / TELEGRAM =================
const char* ssid = "";
const char* password = "";

String botToken = "";
String chatID   = "";

// ================= LCD =================
LiquidCrystal_I2C lcd(0x27, 16, 2);   // change if needed

// ================= BUZZER =================
#define BUZZER_PIN 2

// ================= ULTRASONIC =================
const int trigPins[3] = {4, 18, 23};
const int echoPins[3] = {5, 19, 13};
float detectDistance = 25.0;   // adjust based on mounting

// ================= KEYPAD =================
const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {32, 33, 25, 26};
byte colPins[COLS] = {27, 14, 16, 17};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ================= SLOT DATA =================
bool slotOccupied[3] = {false, false, false};
bool timerActive[3]  = {false, false, false};
bool timeoutSent[3]  = {false, false, false};

unsigned long endMillis[3] = {0, 0, 0};
String slotPhone[3] = {"", "", ""};
int slotMinutes[3] = {0, 0, 0};

// ================= APP STATE =================
enum AppState {
  APP_IDLE,
  APP_SLOT_SELECTED,
  APP_ENTER_NEW_TIME,
  APP_ENTER_PHONE,
  APP_ENTER_EXTEND_TIME
};

AppState state = APP_IDLE;

int selectedSlot = -1;
String inputBuffer = "";
int tempMinutes = 0;

// ================= UI TIMING =================
unsigned long lastSensorUpdate = 0;
unsigned long lastLcdIdleUpdate = 0;
unsigned long messageUntil = 0;
bool lcdLocked = false;
int rotateIndex = 0;

// ================= HELPERS =================
String urlEncode(String text) {
  String encoded = "";
  char c, code0, code1;

  for (int i = 0; i < text.length(); i++) {
    c = text.charAt(i);

    if (isalnum((unsigned char)c)) {
      encoded += c;
    } else if (c == ' ') {
      encoded += "%20";
    } else if (c == '\n') {
      encoded += "%0A";
    } else {
      code1 = (c & 0x0F) + '0';
      if ((c & 0x0F) > 9) code1 = (c & 0x0F) - 10 + 'A';
      c = (c >> 4) & 0x0F;
      code0 = c + '0';
      if (c > 9) code0 = c - 10 + 'A';
      encoded += '%';
      encoded += code0;
      encoded += code1;
    }
  }
  return encoded;
}

void showLCD(String line1, String line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

void showTempMessage(String line1, String line2, unsigned long holdMs) {
  showLCD(line1, line2);
  lcdLocked = true;
  messageUntil = millis() + holdMs;
}

void releaseLCDIfNeeded() {
  if (lcdLocked && millis() >= messageUntil) {
    lcdLocked = false;
  }
}

void beep(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(onMs);
    digitalWrite(BUZZER_PIN, LOW);
    delay(offMs);
  }
}

float readDistanceOnceCM(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(3);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, 12000);
  if (duration <= 0) return 999.0;

  float d = duration * 0.0343 / 2.0;
  if (d < 2.0 || d > 400.0) return 999.0;

  return d;
}

float readDistanceFilteredCM(int trigPin, int echoPin) {
  float a = readDistanceOnceCM(trigPin, echoPin);
  float b = readDistanceOnceCM(trigPin, echoPin);
  float c = readDistanceOnceCM(trigPin, echoPin);

  float vals[3] = {a, b, c};

  for (int i = 0; i < 2; i++) {
    for (int j = i + 1; j < 3; j++) {
      if (vals[j] < vals[i]) {
        float t = vals[i];
        vals[i] = vals[j];
        vals[j] = t;
      }
    }
  }

  return vals[1];
}

void updateSensors() {
  if (millis() - lastSensorUpdate < 250) return;
  lastSensorUpdate = millis();

  for (int i = 0; i < 3; i++) {
    float d = readDistanceFilteredCM(trigPins[i], echoPins[i]);
    slotOccupied[i] = (d <= detectDistance);
  }
}

int availableSlots() {
  int count = 0;
  for (int i = 0; i < 3; i++) {
    if (!slotOccupied[i]) count++;
  }
  return count;
}

void showIdleScreen() {
  if (lcdLocked) return;
  if (millis() - lastLcdIdleUpdate < 1200) return;
  lastLcdIdleUpdate = millis();

  // rotate pages
  rotateIndex++;
  if (rotateIndex > 2) rotateIndex = 0;

  if (rotateIndex == 0) {
    String l1 = "S1:";
    l1 += slotOccupied[0] ? "OCC " : "VAC ";
    l1 += "S2:";
    l1 += slotOccupied[1] ? "OCC" : "VAC";

    String l2 = "S3:";
    l2 += slotOccupied[2] ? "OCC " : "VAC ";
    l2 += "A:";
    l2 += String(availableSlots());

    showLCD(l1, l2);
    return;
  }

  if (rotateIndex == 1) {
    if (availableSlots() == 0) {
      showLCD("PARKING FULL", "NO VACANCY");
    } else {
      showLCD("1-3 Sel Slot", "A Ext B Cancel");
    }
    return;
  }

  for (int step = 0; step < 3; step++) {
    int i = step;
    if (timerActive[i]) {
      unsigned long now = millis();
      unsigned long remain = (endMillis[i] > now) ? (endMillis[i] - now) : 0;
      int mm = remain / 60000UL;
      int ss = (remain % 60000UL) / 1000UL;

      char line1[17];
      snprintf(line1, sizeof(line1), "S%d %02dm %02ds", i + 1, mm, ss);

      String line2 = slotPhone[i];
      if (line2.length() > 16) line2 = line2.substring(0, 16);

      showLCD(String(line1), line2);
      return;
    }
  }

  showLCD("1-3 Sel Slot", "A Ext B Cancel");
}

void resetInputFlow() {
  selectedSlot = -1;
  inputBuffer = "";
  tempMinutes = 0;
  state = APP_IDLE;
}

bool sendTelegram(String msg) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  String url = "https://api.telegram.org/bot" + botToken + "/sendMessage";
  String payload = "chat_id=" + chatID + "&text=" + urlEncode(msg);

  if (!https.begin(client, url)) {
    Serial.println("HTTPS begin failed");
    return false;
  }

  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int httpCode = https.POST(payload);
  String response = https.getString();
  https.end();

  Serial.print("Telegram HTTP code: ");
  Serial.println(httpCode);
  Serial.println(response);

  return httpCode == 200;
}

void startNewTimer(int slot, int minutes, String phone) {
  slotMinutes[slot] = minutes;
  slotPhone[slot] = phone;
  timerActive[slot] = true;
  timeoutSent[slot] = false;
  endMillis[slot] = millis() + ((unsigned long)minutes * 60000UL);

  Serial.print("Started timer slot ");
  Serial.print(slot + 1);
  Serial.print(" for ");
  Serial.print(minutes);
  Serial.println(" minute(s)");

  showTempMessage("Slot " + String(slot + 1), "Timer Started", 900);
}

void extendTimer(int slot, int addMinutes) {
  if (!timerActive[slot]) {
    showTempMessage("No Active Timer", "Use # for new", 1000);
    return;
  }

  endMillis[slot] += ((unsigned long)addMinutes * 60000UL);
  slotMinutes[slot] += addMinutes;
  timeoutSent[slot] = false;

  showTempMessage("Slot " + String(slot + 1), "Time Extended", 900);
}

void cancelTimer(int slot) {
  timerActive[slot] = false;
  timeoutSent[slot] = false;
  endMillis[slot] = 0;
  slotMinutes[slot] = 0;
  slotPhone[slot] = "";

  showTempMessage("Slot " + String(slot + 1), "Timer Cancelled", 900);
}

void handleTimeouts() {
  unsigned long now = millis();

  for (int i = 0; i < 3; i++) {
    if (timerActive[i] && !timeoutSent[i] && now >= endMillis[i]) {
      timerActive[i] = false;
      timeoutSent[i] = true;

      String msg = "Parking Alert!\nSlot " + String(i + 1) +
                   " time over.\nMobile: " + slotPhone[i] +
                   "\nPlease remove or renew vehicle.";

      bool ok = sendTelegram(msg);
      beep(4, 150, 150);

      if (ok) showTempMessage("SLOT " + String(i + 1), "ALERT SENT", 1500);
      else    showTempMessage("SLOT " + String(i + 1), "SEND FAILED", 1500);
    }
  }
}

void handleKeypad() {
  char key = keypad.getKey();
  if (!key) return;

  Serial.print("Pressed: ");
  Serial.println(key);

  if (state == APP_IDLE) {
    if (key >= '1' && key <= '3') {
      selectedSlot = key - '1';
      state = APP_SLOT_SELECTED;
      showLCD("Slot " + String(selectedSlot + 1), "#N AExt BCan");
    }
    return;
  }

  switch (state) {
    case APP_SLOT_SELECTED:
      if (key == '#') {
        if (!slotOccupied[selectedSlot]) {
          showTempMessage("Slot Empty", "Park car first", 1000);
          resetInputFlow();
        } else {
          inputBuffer = "";
          state = APP_ENTER_NEW_TIME;
          showLCD("Enter Time Min", "");
        }
      }
      else if (key == 'A') {
        if (!timerActive[selectedSlot]) {
          showTempMessage("No Active Timer", "Use # for new", 1000);
          resetInputFlow();
        } else {
          inputBuffer = "";
          state = APP_ENTER_EXTEND_TIME;
          showLCD("Extend Minutes", "");
        }
      }
      else if (key == 'B') {
        cancelTimer(selectedSlot);
        resetInputFlow();
      }
      else if (key == '*') {
        resetInputFlow();
      }
      break;

    case APP_ENTER_NEW_TIME:
      if (key >= '0' && key <= '9') {
        if (inputBuffer.length() < 3) inputBuffer += key;
        showLCD("Enter Time Min", inputBuffer);
      }
      else if (key == '*') {
        inputBuffer = "";
        showLCD("Enter Time Min", "");
      }
      else if (key == '#') {
        if (inputBuffer.length() > 0) {
          tempMinutes = inputBuffer.toInt();
          if (tempMinutes > 0) {
            inputBuffer = "";
            state = APP_ENTER_PHONE;
            showLCD("Enter Mobile", "");
          }
        }
      }
      break;

    case APP_ENTER_PHONE:
      if (key >= '0' && key <= '9') {
        if (inputBuffer.length() < 10) inputBuffer += key;
        showLCD("Enter Mobile", inputBuffer);
      }
      else if (key == '*') {
        inputBuffer = "";
        showLCD("Enter Mobile", "");
      }
      else if (key == '#') {
        if (inputBuffer.length() == 10) {
          startNewTimer(selectedSlot, tempMinutes, inputBuffer);
          resetInputFlow();
        } else {
          showTempMessage("Invalid Number", "Enter 10 digit", 1000);
          inputBuffer = "";
          showLCD("Enter Mobile", "");
        }
      }
      break;

    case APP_ENTER_EXTEND_TIME:
      if (key >= '0' && key <= '9') {
        if (inputBuffer.length() < 3) inputBuffer += key;
        showLCD("Extend Minutes", inputBuffer);
      }
      else if (key == '*') {
        inputBuffer = "";
        showLCD("Extend Minutes", "");
      }
      else if (key == '#') {
        if (inputBuffer.length() > 0) {
          int addMin = inputBuffer.toInt();
          if (addMin > 0) {
            extendTimer(selectedSlot, addMin);
            resetInputFlow();
          }
        }
      }
      break;

    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  for (int i = 0; i < 3; i++) {
    pinMode(trigPins[i], OUTPUT);
    pinMode(echoPins[i], INPUT);
    digitalWrite(trigPins[i], LOW);
  }

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();

  showLCD("Parking Alert", "System Start");
  delay(1000);

  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected");
    Serial.println(WiFi.localIP());
    showLCD("WiFi Connected", "System Ready");
  } else {
    Serial.println("WiFi failed");
    showLCD("WiFi Failed", "Offline Mode");
  }

  delay(1200);
  resetInputFlow();
}

void loop() {
  releaseLCDIfNeeded();
  updateSensors();
  handleKeypad();
  handleTimeouts();

  if (state == APP_IDLE) {
    showIdleScreen();
  }
}
