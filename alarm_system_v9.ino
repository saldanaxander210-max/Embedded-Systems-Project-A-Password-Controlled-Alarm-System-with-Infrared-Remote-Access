/*
  Alarm System with IR Remote Passcode
  --------------------------------------
  Hardware:
    - 16x2 I2C LCD (HD44780 + I2C backpack)   -> A4 (SDA), A5 (SCL)
    - HX1838 IR receiver module               -> D7 (signal/"blank" pin)
    - Grove Piezo Buzzer (active buzzer)      -> D5 (signal)
    - SR501 PIR motion sensor                 -> D6 (OUT)

  Libraries required (install both via Library Manager):
    - LiquidCrystal_I2C   (by Frank de Brabander)
    - IRremote            (by Armin Joachimsmeyer)
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <IRremote.h>
#include <EEPROM.h>
#include <avr/sleep.h>

// ---------- LCD ----------
// Most I2C backpacks are address 0x27; some are 0x3F.
// If the LCD shows nothing/garbage, try changing 0x27 to 0x3F.
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------- Pins ----------
const int IR_RECEIVE_PIN = 6;
const int PIR_PIN        = 5;
const int BUZZER_PIN     = 4;

// ---------- LEDs ----------
// Each LED: long leg (anode) -> ~220-330 ohm resistor -> Arduino pin
//           short leg (cathode) -> GND rail
const int GREEN_LED_PIN = 2; // lit while DISARMED
const int RED_LED_PIN   = 3; // lit solid while ARMED; flashes with buzzer when motion is detected

// ---------- Password ----------
// Change this to whatever 6-digit code you want.
const char PASSWORD[] = "123456";
const int  PASS_LENGTH = 6;

char userInput[7];       // up to 6 digits + null terminator
int  inputLength = 0;

// ---------- Debug ----------
// Set to true to ALWAYS show the actual digits instead of asterisks.
// Leave false for normal secure use -- you can still peek by holding UP (see below).
const bool DEBUG_SHOW_INPUT = false;

// ---------- Hold-UP-to-peek ----------
// While the UP button is held, the remote continuously sends "repeat" signals.
// We use that to keep peek mode active, and time out shortly after they stop
// (i.e. as soon as you release the button).
bool peekActive = false;
unsigned long lastUpSignalTime = 0;
const unsigned long PEEK_TIMEOUT_MS = 200; // tune down if it feels laggy, up if it flickers

// ---------- Alarm state ----------
bool armed = false;

// ---------- PIR debounce ----------
int pirState = LOW;
bool motionDisplayed = false; // tracks whether LCD is currently showing the motion warning

// ---------- Persistent state (survives power loss) ----------
const int EEPROM_ARMED_ADDR = 0; // one byte: 1 = armed, 0 = disarmed

// ---------- Energy-saving (backlight) ----------
const unsigned long INACTIVITY_TIMEOUT_MS = 15000; // turn off backlight after 15s of no button presses
unsigned long lastActivityTime = 0;
bool backlightOn = true;

// =====================================================================
//  TRANSLATION LAYER
//  Maps the raw hex codes your remote sends (from IrReceiver.decodedIRData.command)
//  into meaningful button actions for this program.
// =====================================================================
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
#define BTN_DEL   0x16   // first "*"  -> delete last digit
#define BTN_CLEAR 0x0D   // second "*" -> clear whole input
#define BTN_UP    0x18
#define BTN_DOWN  0x52
#define BTN_LEFT  0x08
#define BTN_RIGHT 0x5A
#define BTN_OK    0x1C

// Action codes returned by translateButton()
enum ButtonAction {
  ACTION_NONE = 0,
  ACTION_DIGIT,
  ACTION_DELETE,
  ACTION_CLEAR,
  ACTION_SUBMIT,
  ACTION_ARM,
  ACTION_OTHER
};

// Translates a raw IR hex code into an action + digit (if applicable).
// Returns the action type; if ACTION_DIGIT, 'outDigit' is set to '0'-'9'.
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
    // DOWN arms the system directly (only acted on while disarmed -- see handleButton()).
    case BTN_DOWN:
      return ACTION_ARM;
    // Left/Right recognized but unused by the alarm logic for now.
    // (Up is intercepted earlier in loop() for hold-to-peek, so it never reaches here.)
    case BTN_LEFT:
    case BTN_RIGHT:
      return ACTION_OTHER;
    default:
      return ACTION_NONE; // unrecognized code (e.g. repeat signal, noise)
  }
}

// =====================================================================
//  SETUP
// =====================================================================
void setup() {
  lcd.init();
  lcd.backlight();

  IrReceiver.begin(IR_RECEIVE_PIN);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(PIR_PIN, INPUT);

  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);

  digitalWrite(BUZZER_PIN, LOW);

  // Restore last known arm state in case power was cut while armed/disarmed
  // (NOTE: armed only ever changes here on boot, or in startArmingSequence()/
  // checkPassword() -- there is no timer anywhere that auto-disarms.)
  armed = (EEPROM.read(EEPROM_ARMED_ADDR) == 1);
  updateArmLEDs();

  lastActivityTime = millis();

  updateStatusLine(false);
  updatePasswordLine();
}

// =====================================================================
//  MAIN LOOP
// =====================================================================
void loop() {
  // ---- Motion detection (only matters while armed) ----
  pirState = digitalRead(PIR_PIN);

  if (armed) {
    bool motionNow = (pirState == HIGH);
    if (motionNow != motionDisplayed) {
      motionDisplayed = motionNow;
      updateStatusLine(motionDisplayed);
    }
    if (motionNow) {
      wakeBacklight();  // make sure the LCD is lit up during a motion alarm
      motionAlarm();    // 7 fast buzzes, but red LED only blinks 3 of those times
    }
  } else if (motionDisplayed) {
    // Just disarmed while the warning was showing -- clear it
    motionDisplayed = false;
    updateStatusLine(false);
  }

  // ---- Read IR remote ----
  if (IrReceiver.decode()) {
    uint8_t code = IrReceiver.decodedIRData.command;
    bool isRepeat = IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT;

    // Any real button signal counts as activity -> wake the backlight
    wakeBacklight();

    if (code == BTN_UP) {
      // UP pressed or held (repeat) -> keep peek mode on and reset its timer
      peekActive = true;
      lastUpSignalTime = millis();
      updatePasswordLine();
    } else if (!isRepeat) {
      // Any other (non-repeat) button press acts normally
      handleButton(code);
    }
    IrReceiver.resume();
  }

  // ---- Turn off peek mode once UP stops sending signals (i.e. released) ----
  if (peekActive && (millis() - lastUpSignalTime > PEEK_TIMEOUT_MS)) {
    peekActive = false;
    updatePasswordLine();
  }

  // ---- Energy saving: dim backlight after inactivity (armed or disarmed) ----
  if (backlightOn && (millis() - lastActivityTime > INACTIVITY_TIMEOUT_MS)) {
    lcd.noBacklight();
    backlightOn = false;
  }

  // ---- Energy saving: let the CPU idle briefly between cycles.
  // SLEEP_MODE_IDLE keeps all timers/interrupts running (so the IR receiver's
  // demodulation timer and millis() keep working normally) -- it only pauses
  // the CPU core itself until the next interrupt, which happens very often.
  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_enable();
  sleep_cpu();
  sleep_disable();
}

// Turns the LCD backlight back on and resets the inactivity timer.
// Called whenever the user does something with the remote.
void wakeBacklight() {
  lastActivityTime = millis();
  if (!backlightOn) {
    lcd.backlight();
    backlightOn = true;
  }
}

// =====================================================================
//  BUTTON HANDLING
// =====================================================================
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
      // If already at max length (6 digits), extra digits are ignored.
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
      // DOWN button: only arms the system when it's currently disarmed.
      // (Ignored while already armed -- disarming still requires the password.)
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
      // Nothing to do
      break;
  }
}

// =====================================================================
//  PASSWORD CHECK
// =====================================================================
void checkPassword() {
  bool correct = (inputLength == PASS_LENGTH) &&
                 (strncmp(userInput, PASSWORD, PASS_LENGTH) == 0);

  if (correct) {
    if (armed) {
      // Disarm immediately -- no countdown needed for disarming.
      armed = false;
      EEPROM.update(EEPROM_ARMED_ADDR, 0); // survives power loss (EEPROM.update only writes if changed)
      updateArmLEDs();
      lcd.setCursor(0, 1);
      lcd.print("DISARMED        ");
      delay(1500);
    } else {
      // Correct password while disarmed -- arm, but only after the
      // 3-second countdown (same sequence the DOWN button triggers).
      startArmingSequence();
    }
  } else {
    lcd.setCursor(0, 1);
    lcd.print("ACCESS DENIED   ");
    burglarAlarm(600); // fast-interval burglar-alarm buzz on wrong password
    delay(900);
  }

  // Reset entry and redraw normal screen
  inputLength = 0;
  userInput[0] = '\0';
  motionDisplayed = false;
  updateStatusLine(false);
  updatePasswordLine();
}

// =====================================================================
//  ARMING SEQUENCE
//  Shows a 3-second countdown with a buzz each second, then sets armed=true.
//  The green LED blinks once per second in sync with each buzz (3 blinks
//  total over the countdown). Used both when a correct password is entered
//  while disarmed, and when the DOWN button is pressed while disarmed.
// =====================================================================
void startArmingSequence() {
  lcd.setCursor(0, 0);
  lcd.print("ARMING...       ");

  for (int i = 3; i >= 1; i--) {
    char line[17];
    snprintf(line, sizeof(line), "Arming in %d... ", i);
    lcd.setCursor(0, 1);
    lcd.print(line);
    digitalWrite(GREEN_LED_PIN, LOW);  // blink off in sync with the buzz
    beep(1000, 150);                   // one buzz per second of the countdown
    digitalWrite(GREEN_LED_PIN, HIGH); // blink back on
    delay(850);                        // ~850ms remaining of the 1s tick (150ms already spent beeping)
  }

  armed = true;
  EEPROM.update(EEPROM_ARMED_ADDR, 1); // survives power loss (EEPROM.update only writes if changed)
  updateArmLEDs();
  lcd.setCursor(0, 1);
  lcd.print("ARMED           ");
  delay(500);
}

// =====================================================================
//  LCD HELPERS
// =====================================================================
void updateStatusLine(bool motion) {
  lcd.setCursor(0, 0);
  if (armed && motion) {
    lcd.print("ARMED - MOTION! "); // 16 chars
  } else if (armed) {
    lcd.print("ALARM: ARMED    ");
  } else {
    lcd.print("ALARM: DISARMED ");
  }
}

void updatePasswordLine() {
  lcd.setCursor(0, 1);
  char display[17] = "                "; // 16 spaces
  bool reveal = DEBUG_SHOW_INPUT || peekActive;
  for (int i = 0; i < inputLength; i++) {
    display[i] = reveal ? userInput[i] : '*';
  }
  lcd.print(display);
}

// =====================================================================
//  BUZZER (custom, avoids tone() which conflicts with IRremote's timer)
// =====================================================================
void beep(int freq, int duration) {
  unsigned long start = millis();
  while (millis() - start < (unsigned long)duration) {
    digitalWrite(BUZZER_PIN, HIGH);
    delayMicroseconds(500000 / freq);
    digitalWrite(BUZZER_PIN, LOW);
    delayMicroseconds(500000 / freq);
  }
}

// Fast-interval "burglar alarm" buzz -- rapid on/off (~10 Hz) toggling, with
// the red LED flashing in sync with the buzzer. Used for wrong-password entry.
void burglarAlarm(int duration) {
  const unsigned long halfPeriodUs = 50000; // ~10 Hz: fast, urgent-sounding interval
  unsigned long start = millis();
  while (millis() - start < (unsigned long)duration) {
    digitalWrite(BUZZER_PIN, HIGH);
    digitalWrite(RED_LED_PIN, HIGH);
    delayMicroseconds(halfPeriodUs);
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(RED_LED_PIN, LOW);
    delayMicroseconds(halfPeriodUs);
  }
  digitalWrite(RED_LED_PIN, armed ? HIGH : LOW); // return to steady "armed" state after flashing
}

// Motion-detected alarm: buzzes 7 fast times (louder/more insistent, like a
// burglar alarm), but the red LED only blinks for the first 3 of those
// buzzes -- after that it just stays lit (it's already solid ON while
// armed) for the remaining 4 buzzes.
void motionAlarm() {
  const int TOTAL_BEEPS  = 15;
  const int TOTAL_BLINKS = 2;
  const unsigned long halfPeriodMs = 50; // ~10 Hz: fast, urgent-sounding interval

  for (int i = 0; i < TOTAL_BEEPS; i++) {
    bool doBlink = (i < TOTAL_BLINKS);

    digitalWrite(BUZZER_PIN, HIGH);
    if (doBlink) digitalWrite(RED_LED_PIN, LOW); // dip = one blink
    delay(halfPeriodMs);

    digitalWrite(BUZZER_PIN, LOW);
    if (doBlink) digitalWrite(RED_LED_PIN, HIGH); // back on
    delay(halfPeriodMs);
  }

  digitalWrite(RED_LED_PIN, armed ? HIGH : LOW); // make sure it ends steady "armed"
}

// =====================================================================
//  LEDs
// =====================================================================
void updateArmLEDs() {
  digitalWrite(GREEN_LED_PIN, armed ? LOW : HIGH); // green = disarmed
  digitalWrite(RED_LED_PIN, armed ? HIGH : LOW);   // red = armed (blinks during motionAlarm on motion)
}
