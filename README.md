//# Embedded-Systems-Project-A-Password-Controlled-Alarm-System-with-Infrared-Remote-Access

//For Modelling and Simulation

<img width="480" height="360" alt="image" src="https://github.com/user-attachments/assets/c18b1711-b9ca-4c3f-a415-221108a0e132" /> 

<img width="480" height="360" alt="image" src="https://github.com/user-attachments/assets/56314c53-bd32-4e4c-a7db-6e51de7cde2b" />

<img width="480" height="640" alt="image" src="https://github.com/user-attachments/assets/c21812a6-a99c-4ad2-8aa2-2601f49ac71a" />

<img width="624" height="378" alt="image" src="https://github.com/user-attachments/assets/0021e926-b101-4293-808a-30e01bf6c173" />



#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <IRremote.h>
#include <EEPROM.h>
#include <avr/sleep.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

const int IR_RECEIVE_PIN = 6;
const int PIR_PIN        = 5;
const int BUZZER_PIN     = 4;

const int GREEN_LED_PIN = 2;
const int RED_LED_PIN   = 3;

const char PASSWORD[] = "123456";
const int  PASS_LENGTH = 6;

char userInput[7];
int  inputLength = 0;

const bool DEBUG_SHOW_INPUT = false;

bool peekActive = false;
unsigned long lastUpSignalTime = 0;
const unsigned long PEEK_TIMEOUT_MS = 200;

bool armed = false;

int pirState = LOW;
bool motionDisplayed = false;

const int EEPROM_ARMED_ADDR = 0;

const unsigned long INACTIVITY_TIMEOUT_MS = 15000;
unsigned long lastActivityTime = 0;
bool backlightOn = true;

#define BTN_1     0x45
#define BTN_2     0x46
#define BTN_3     0x47
#define BTN_4     0x44
#define BTN_5     0x40
#define BTN_6     0x43
#define BTN_7     0x07
#define BTN_8     0x15
#define BTN_9     0x09
#define BTN_0     0x19
#define BTN_DEL   0x16
#define BTN_CLEAR 0x0D
#define BTN_UP    0x18
#define BTN_DOWN  0x52
#define BTN_LEFT  0x08
#define BTN_RIGHT 0x5A
#define BTN_OK    0x1C

enum ButtonAction {
  ACTION_NONE = 0,
  ACTION_DIGIT,
  ACTION_DELETE,
  ACTION_CLEAR,
  ACTION_SUBMIT,
  ACTION_ARM,
  ACTION_OTHER
};

ButtonAction translateButton(uint8_t code, char &outDigit) {
  switch (code) {
    case BTN_0: outDigit = '0'; return ACTION_DIGIT;
    case BTN_1: outDigit = '1'; return ACTION_DIGIT;
    case BTN_2: outDigit = '2'; return ACTION_DIGIT;
    case BTN_3: outDigit = '3'; return ACTION_DIGIT;
    case BTN_4: outDigit = '4'; return ACTION_DIGIT;
    case BTN_5: outDigit = '5'; return ACTION_DIGIT;
    case BTN_6: outDigit = '6'; return ACTION_DIGIT;
    case BTN_7: outDigit = '7'; return ACTION_DIGIT;
    case BTN_8: outDigit = '8'; return ACTION_DIGIT;
    case BTN_9: outDigit = '9'; return ACTION_DIGIT;
    case BTN_DEL:   return ACTION_DELETE;
    case BTN_CLEAR: return ACTION_CLEAR;
    case BTN_OK:    return ACTION_SUBMIT;
    case BTN_DOWN:
      return ACTION_ARM;
    case BTN_LEFT:
    case BTN_RIGHT:
      return ACTION_OTHER;
    default:
      return ACTION_NONE;
  }
}

void setup() {
  lcd.init();
  lcd.backlight();

  IrReceiver.begin(IR_RECEIVE_PIN);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(PIR_PIN, INPUT);

  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);

  digitalWrite(BUZZER_PIN, LOW);

  armed = (EEPROM.read(EEPROM_ARMED_ADDR) == 1);
  updateArmLEDs();

  lastActivityTime = millis();

  updateStatusLine(false);
  updatePasswordLine();
}

void loop() {
  pirState = digitalRead(PIR_PIN);

  if (armed) {
    bool motionNow = (pirState == HIGH);
    if (motionNow != motionDisplayed) {
      motionDisplayed = motionNow;
      updateStatusLine(motionDisplayed);
    }
    if (motionNow) {
      wakeBacklight();
      motionAlarm();
    }
  } else if (motionDisplayed) {
    motionDisplayed = false;
    updateStatusLine(false);
  }

  if (IrReceiver.decode()) {
    uint8_t code = IrReceiver.decodedIRData.command;
    bool isRepeat = IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT;

    wakeBacklight();

    if (code == BTN_UP) {
      peekActive = true;
      lastUpSignalTime = millis();
      updatePasswordLine();
    } else if (!isRepeat) {
      handleButton(code);
    }
    IrReceiver.resume();
  }

  if (peekActive && (millis() - lastUpSignalTime > PEEK_TIMEOUT_MS)) {
    peekActive = false;
    updatePasswordLine();
  }

  if (backlightOn && (millis() - lastActivityTime > INACTIVITY_TIMEOUT_MS)) {
    lcd.noBacklight();
    backlightOn = false;
  }

  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_enable();
  sleep_cpu();
  sleep_disable();
}

void wakeBacklight() {
  lastActivityTime = millis();
  if (!backlightOn) {
    lcd.backlight();
    backlightOn = true;
  }
}

void handleButton(uint8_t code) {
  char digit;
  ButtonAction action = translateButton(code, digit);

  switch (action) {
    case ACTION_DIGIT:
      if (inputLength < PASS_LENGTH) {
        userInput[inputLength] = digit;
        inputLength++;
        userInput[inputLength] = '\0';
        updatePasswordLine();
      }
      break;

    case ACTION_DELETE:
      if (inputLength > 0) {
        inputLength--;
        userInput[inputLength] = '\0';
        updatePasswordLine();
      }
      break;

    case ACTION_CLEAR:
      inputLength = 0;
      userInput[0] = '\0';
      updatePasswordLine();
      break;

    case ACTION_SUBMIT:
      checkPassword();
      break;

    case ACTION_ARM:
      if (!armed) {
        inputLength = 0;
        userInput[0] = '\0';
        startArmingSequence();
        updateStatusLine(false);
        updatePasswordLine();
      }
      break;

    case ACTION_OTHER:
    case ACTION_NONE:
    default:
      break;
  }
}

void checkPassword() {
  bool correct = (inputLength == PASS_LENGTH) &&
                 (strncmp(userInput, PASSWORD, PASS_LENGTH) == 0);

  if (correct) {
    if (armed) {
      armed = false;
      EEPROM.update(EEPROM_ARMED_ADDR, 0);
      updateArmLEDs();
      lcd.setCursor(0, 1);
      lcd.print("DISARMED        ");
      delay(1500);
    } else {
      startArmingSequence();
    }
  } else {
    lcd.setCursor(0, 1);
    lcd.print("ACCESS DENIED   ");
    burglarAlarm(600);
    delay(900);
  }

  inputLength = 0;
  userInput[0] = '\0';
  motionDisplayed = false;
  updateStatusLine(false);
  updatePasswordLine();
}

void startArmingSequence() {
  lcd.setCursor(0, 0);
  lcd.print("ARMING...       ");

  for (int i = 3; i >= 1; i--) {
    char line[17];
    snprintf(line, sizeof(line), "Arming in %d... ", i);
    lcd.setCursor(0, 1);
    lcd.print(line);
    digitalWrite(GREEN_LED_PIN, LOW);
    beep(1000, 150);
    digitalWrite(GREEN_LED_PIN, HIGH);
    delay(850);
  }

  armed = true;
  EEPROM.update(EEPROM_ARMED_ADDR, 1);
  updateArmLEDs();
  lcd.setCursor(0, 1);
  lcd.print("ARMED           ");
  delay(500);
}

void updateStatusLine(bool motion) {
  lcd.setCursor(0, 0);
  if (armed && motion) {
    lcd.print("ARMED - MOTION! ");
  } else if (armed) {
    lcd.print("ALARM: ARMED    ");
  } else {
    lcd.print("ALARM: DISARMED ");
  }
}

void updatePasswordLine() {
  lcd.setCursor(0, 1);
  char display[17] = "                ";
  bool reveal = DEBUG_SHOW_INPUT || peekActive;
  for (int i = 0; i < inputLength; i++) {
    display[i] = reveal ? userInput[i] : '*';
  }
  lcd.print(display);
}

void beep(int freq, int duration) {
  unsigned long start = millis();
  while (millis() - start < (unsigned long)duration) {
    digitalWrite(BUZZER_PIN, HIGH);
    delayMicroseconds(500000 / freq);
    digitalWrite(BUZZER_PIN, LOW);
    delayMicroseconds(500000 / freq);
  }
}

void burglarAlarm(int duration) {
  const unsigned long halfPeriodUs = 50000;
  unsigned long start = millis();
  while (millis() - start < (unsigned long)duration) {
    digitalWrite(BUZZER_PIN, HIGH);
    digitalWrite(RED_LED_PIN, HIGH);
    delayMicroseconds(halfPeriodUs);
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(RED_LED_PIN, LOW);
    delayMicroseconds(halfPeriodUs);
  }
  digitalWrite(RED_LED_PIN, armed ? HIGH : LOW);
}

void motionAlarm() {
  const int TOTAL_BEEPS  = 15;
  const int TOTAL_BLINKS = 2;
  const unsigned long halfPeriodMs = 50;

  for (int i = 0; i < TOTAL_BEEPS; i++) {
    bool doBlink = (i < TOTAL_BLINKS);

    digitalWrite(BUZZER_PIN, HIGH);
    if (doBlink) digitalWrite(RED_LED_PIN, LOW);
    delay(halfPeriodMs);

    digitalWrite(BUZZER_PIN, LOW);
    if (doBlink) digitalWrite(RED_LED_PIN, HIGH);
    delay(halfPeriodMs);
  }

  digitalWrite(RED_LED_PIN, armed ? HIGH : LOW);
}

void updateArmLEDs() {
  digitalWrite(GREEN_LED_PIN, armed ? LOW : HIGH);
  digitalWrite(RED_LED_PIN, armed ? HIGH : LOW);
}
