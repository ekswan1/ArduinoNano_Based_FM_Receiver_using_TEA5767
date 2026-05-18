#include <Wire.h>
#include <TEA5767.h>
#include <LiquidCrystal_I2C.h>

constexpr uint8_t MODE_BTN_PIN   = 3;
constexpr uint8_t ACTION_BTN_PIN = 2;
constexpr uint8_t LOCK_BTN_PIN   = 4;
constexpr uint8_t POT_PIN        = A0;

constexpr uint8_t MAX_STATIONS = 30;

constexpr float    FM_MIN_MHZ     =  87.5f;
constexpr float    FM_MAX_MHZ     = 108.0f;
constexpr uint16_t FM_MIN_INT     =  875;
constexpr uint16_t FM_MAX_INT     = 1080;

constexpr float    SCAN_CEILING   = 107.9f;
constexpr float    WRAP_HIGH_THR  = 100.0f;
constexpr float    WRAP_LOW_THR   =  88.0f;
constexpr float    MIN_STEP_MHZ   =   0.05f;
constexpr float    STUCK_BUMP_MHZ =   0.1f;
constexpr uint8_t  MAX_SCAN_TRIES =  60;

constexpr float FINE_SCALE      = 0.002f;
constexpr int   POT_COARSE_HYST = 3;
constexpr int   POT_FINE_HYST   = 1;

constexpr uint16_t I2C_COOLDOWN_MS  = 150;
constexpr uint16_t DEBOUNCE_MS      = 200;
constexpr uint16_t SCAN_SETTLE_MS   = 400;
constexpr uint16_t ANIM_INTERVAL_MS = 150; 
constexpr uint32_t I2C_CLOCK_HZ     = 400000UL;


const byte animFrames[6][8] = {
  { B00000, B00000, B00100, B00000, B00100, B01010, B01010, B10001 }, 
  { B00000, B00100, B01110, B00100, B00100, B01010, B01010, B10001 }, 
  { B00100, B01110, B11111, B01110, B00100, B01010, B01010, B10001 }, 
  { B01110, B11111, B11111, B11111, B01110, B01010, B01010, B10001 }, 
  { B01110, B11011, B10101, B11011, B01110, B01010, B01010, B10001 }, 
  { B11111, B10001, B10101, B10001, B10101, B01010, B01010, B10001 }  
};


enum Mode : uint8_t { AUTO_MODE, MANUAL_COARSE, MANUAL_FINE };

void radioPrintInfo(const tea5767_info_t *info);
void radioUpdateConfig(tea5767_config_t *config);

TEA5767           radio(&radioPrintInfo, &radioUpdateConfig);
LiquidCrystal_I2C lcd(0x27, 16, 2);

uint16_t foundStations[MAX_STATIONS];
uint8_t  stationCount      = 0;
uint8_t  currentStationIdx = 0;

Mode currentMode      = AUTO_MODE;
bool isLocked         = false;
bool forceUpdate      = true;
bool isScanning       = false;

int lastPotValue      = -1;
float currentManualFreq = FM_MIN_MHZ;
float fineCenterFreq    = FM_MIN_MHZ;
int   fineCenterPot     = 0;

uint16_t      targetFreqInt  = FM_MIN_INT;
uint16_t      actualFreqInt  = 0;
unsigned long lastI2cTime    = 0;

uint8_t       animFrame      = 5; 
unsigned long lastAnimTime   = 0;

int           modeLastState   = HIGH;  unsigned long modeLastTime   = 0;
int           actionLastState = HIGH;  unsigned long actionLastTime = 0;
int           lockLastState   = HIGH;  unsigned long lockLastTime   = 0;

static inline int stablePot() {
  int s = 0;
  for (uint8_t i = 0; i < 4; i++) s += analogRead(POT_PIN);
  return s >> 2;
}

static inline bool buttonPressed(uint8_t pin, int &lastState, unsigned long &lastTime) {
  int r = digitalRead(pin);
  bool fired = (r == LOW && lastState == HIGH && millis() - lastTime > DEBOUNCE_MS);
  if (fired) lastTime = millis();
  lastState = r;
  return fired;
}

static inline uint16_t toInt(float mhz) {
  return (uint16_t)(mhz * 10.0f + 0.5f);
}

static inline float toMHz(uint16_t v) {
  return v / 10.0f;
}

static void lcdLine(uint8_t row, const __FlashStringHelper *text, uint8_t padLen = 16) {
  lcd.setCursor(0, row);
  uint8_t n = 0;
  const char *p = (const char *)text;
  char c;
  while ((c = pgm_read_byte(p++)) != '\0' && n < padLen) {
    lcd.print(c);
    n++;
  }
  while (n < padLen) { lcd.print(' '); n++; } 
}

static void lcdLineStr(uint8_t row, const char *text, uint8_t padLen = 16) {
  lcd.setCursor(0, row);
  uint8_t n = 0;
  while (*text && n < padLen) { lcd.print(*text++); n++; }
  while (n < padLen) { lcd.print(' '); n++; }
}

void updateDisplay() {
  if (currentMode == AUTO_MODE) {
    char buf0[16];
    if (stationCount > 0) {
      snprintf(buf0, sizeof(buf0), "Auto [%2d/%2d]", currentStationIdx + 1, stationCount);
    } else {
      snprintf(buf0, sizeof(buf0), "Auto [--/--]");
    }
    lcdLineStr(0, buf0, 15); 

    char buf1[17];
    if (stationCount > 0) {
      float f = toMHz(foundStations[currentStationIdx]);
      char fBuf[8];
      dtostrf(f, 5, 1, fBuf);
      snprintf(buf1, sizeof(buf1), "Freq: %s MHz ", fBuf);
    } else {
      snprintf(buf1, sizeof(buf1), "No Stations!    ");
    }
    lcdLineStr(1, buf1, 16);
  }
  else {
    if (isLocked) {
      lcdLine(0, F("Freq Set [LOCK] "), 16);
    } else if (currentMode == MANUAL_COARSE) {
      lcdLine(0, F("Manual: 0.1 MHz "), 16);
    } else {
      lcdLine(0, F("Manual: 0.01MHz "), 16);
    }

    char buf[17];
    char fBuf[8];
    dtostrf(currentManualFreq, 6, 2, fBuf);
    snprintf(buf, sizeof(buf), "Freq: %s MHz", fBuf);
    lcdLineStr(1, buf, 16);
  }
}

void handleAnimation() {
  if (!isScanning && currentMode != AUTO_MODE) return;

  if (millis() - lastAnimTime > ANIM_INTERVAL_MS) {
    lastAnimTime = millis();
    
    animFrame = (animFrame == 0) ? 5 : animFrame - 1;
    
    lcd.setCursor(15, 0);
    lcd.write(animFrame);
  }
}

void scanDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    handleAnimation();
  }
}

void handleRadioTuning() {
  if (targetFreqInt != actualFreqInt && millis() - lastI2cTime > I2C_COOLDOWN_MS) {
    lastI2cTime   = millis();
    actualFreqInt = targetFreqInt;
    radio.setFrequency(toMHz(actualFreqInt));
  }
}

void handleModeButton() {
  if (!buttonPressed(MODE_BTN_PIN, modeLastState, modeLastTime)) return;
  currentMode = (currentMode == AUTO_MODE) ? MANUAL_COARSE : AUTO_MODE;
  isLocked    = false;
  forceUpdate = true;
}

void handleActionButton() {
  if (!buttonPressed(ACTION_BTN_PIN, actionLastState, actionLastTime)) return;

  if (currentMode == AUTO_MODE) {
    if (stationCount > 0) {
      if (++currentStationIdx >= stationCount) currentStationIdx = 0;
      forceUpdate = true;
    }
  } else if (currentMode == MANUAL_COARSE) {
    currentMode    = MANUAL_FINE;
    fineCenterFreq = currentManualFreq;
    fineCenterPot  = stablePot();
    isLocked       = false;
    forceUpdate    = true;
  } else { 
    currentMode = MANUAL_COARSE;
    isLocked    = false;
    forceUpdate = true;
  }
}

void handleLockButton() {
  if (!buttonPressed(LOCK_BTN_PIN, lockLastState, lockLastTime)) return;
  if (currentMode != AUTO_MODE) {
    isLocked    = !isLocked;
    forceUpdate = true;
  }
}

void handlePotentiometer() {
  int potValue = stablePot(); 

  if (isLocked) {
    lastPotValue = potValue;
    if (forceUpdate) { forceUpdate = false; updateDisplay(); }
    return;
  }

  if (currentMode == AUTO_MODE) {
    if (forceUpdate) {
      forceUpdate = false;
      if (stationCount > 0) targetFreqInt = foundStations[currentStationIdx];
      updateDisplay();
    }
    return;
  }

  if (currentMode == MANUAL_COARSE) {
    if (abs(potValue - lastPotValue) > POT_COARSE_HYST || forceUpdate) {
      lastPotValue = potValue;
      forceUpdate  = false;
      long mapped   = map(potValue, 0, 1023, FM_MIN_INT, FM_MAX_INT);
      currentManualFreq = mapped / 10.0f;
      targetFreqInt     = (uint16_t)mapped;
      updateDisplay();
    }
  } else { 
    if (abs(potValue - lastPotValue) > POT_FINE_HYST || forceUpdate) {
      lastPotValue = potValue;
      forceUpdate  = false;
      float f = fineCenterFreq + (potValue - fineCenterPot) * FINE_SCALE;
      currentManualFreq = constrain(f, FM_MIN_MHZ, FM_MAX_MHZ);
      targetFreqInt     = toInt(currentManualFreq);
      updateDisplay();
    }
  }
}

void setup() {
  Wire.begin();
  Wire.setClock(I2C_CLOCK_HZ); 

  lcd.init();
  lcd.backlight();

  for(uint8_t i = 0; i < 6; i++) {
    lcd.createChar(i, (uint8_t*)animFrames[i]);
  }

  lcdLine(0, F("TEA5767 Radio   ")); 
  lcdLine(1, F("Initialising... "));

  pinMode(MODE_BTN_PIN,   INPUT_PULLUP);
  pinMode(ACTION_BTN_PIN, INPUT_PULLUP);
  pinMode(LOCK_BTN_PIN,   INPUT_PULLUP);

  radio.awake();
  radio.mute();
  radio.setFrequency(FM_MIN_MHZ);
  delay(500);

  lcdLine(0, F("D3 Btn: Scan    "));
  lcdLine(1, F("D2 Btn: Manual  "));

  bool skipScan = false;
  while (true) {
    if (digitalRead(MODE_BTN_PIN)   == LOW) { delay(200); break; }
    if (digitalRead(ACTION_BTN_PIN) == LOW) { skipScan = true; delay(200); break; }
  }

  if (!skipScan) {
    lcdLine(0, F("Scanning FM...  "));
    lcdLine(1, F("                "));

    isScanning = true; 
    radio.mute();
    radio.setFrequency(FM_MIN_MHZ);
    delay(100);

    float    lastFreq    = 87.0f;
    uint8_t  scanTries   = 0;

    while (stationCount < MAX_STATIONS && scanTries++ < MAX_SCAN_TRIES) {
      radio.searchUp();
      scanDelay(SCAN_SETTLE_MS); 

      tea5767_info_t info;
      radio.getInfo(&info);

      if (info.mhz >= SCAN_CEILING) break;

      if (info.mhz > lastFreq + MIN_STEP_MHZ) {
        foundStations[stationCount++] = toInt(info.mhz);
        lastFreq = info.mhz;

        char buf[17];
        char fBuf[8];
        dtostrf(info.mhz, 5, 1, fBuf);
        snprintf(buf, sizeof(buf), "%s MHz  #%-3d", fBuf, stationCount);
        lcdLineStr(1, buf);
      } else {
        if (info.mhz < WRAP_LOW_THR && lastFreq > WRAP_HIGH_THR) break;
        radio.setFrequency(lastFreq + STUCK_BUMP_MHZ);
        scanDelay(100); 
      }
    }

    isScanning = false; 
    radio.unmute();

    if (stationCount == 0) {
      lcdLine(0, F("No Stations!    "));
      lcdLine(1, F("Going Manual... "));
      delay(2000);
      currentMode = MANUAL_COARSE;
    } else {
      currentMode       = AUTO_MODE;
      currentStationIdx = 0;
      targetFreqInt     = foundStations[0];
    }
  } else {
    stationCount      = 0;
    currentMode       = MANUAL_COARSE;
    currentManualFreq = FM_MIN_MHZ;
    targetFreqInt     = FM_MIN_INT;
    radio.unmute();
  }

  forceUpdate = true;
}

void loop() {
  handleModeButton();
  handleActionButton();
  handleLockButton();
  handlePotentiometer();
  handleRadioTuning(); 
  handleAnimation(); 
}

void radioPrintInfo(const tea5767_info_t *) {}

void radioUpdateConfig(tea5767_config_t *config) {
  config->standby                 = false;
  config->search_stop_level       = isScanning ? 1 : 2;
  
  config->stereo_noise_cancelling = true;
  config->high_cut_control        = true;
  config->soft_mute               = true;
}