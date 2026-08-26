#include "OledMonitorPanel.h"
#include <Fonts/FreeSans18pt7b.h>
#include "Fonts/FreeSans8pt7b.h"
#include "Fonts/FreeSans7pt7b.h"
#include "Fonts/FreeSans6pt7b.h"
#include "Fonts/DSEG7Classic_Regular15pt7b.h"
#include "Fonts/DSEG7Classic_Regular16pt7b.h"
#include "Fonts/DSEG7Classic_Regular18pt7b.h" //https://github.com/keshikan/DSEG and https://rop.nl/truetype2gfx/
// Efis left
static char efisLeftBaroValueHg[6]  = "000"; // MACH

// Efis right
static char efisRightBaroValueHpa[6] = "000"; //VOR DME
static char efisRightBaroValueHg[6]  = "-----"; //Radio Alimeter

// FCU Speed
uint8_t fcuSpeedManagedMode = 0x00;
static char fcuSpeedValue[6]       = "000";
uint8_t fcuSpeedMode        = 0x00;

// FCU Hdg
uint8_t fcuHdgManagedMode = 0x00;
static char fcuHdgValue[6]       = "000";

// FCU Trk Mode
uint8_t fcuTrkMode = 0x00;
// FCU Alt
uint8_t fcuAltManagedMode = 0x00;
static char fcuAltValue[6]       = "00000";

// FCU VS
uint8_t fcuVsManagedMode = 0x00;
static char fcuVsValue[7]       = "00000"; // may carry a leading '-'
static char fcuVsValueFpa[8]    = "77777"; // may get ".0" appended

// light test switch
uint8_t lightTestOn = 0x00;

// new value AUX
static char CRSValue[6] = "000"; // CRS новое значение для 8 экрана

/*
  Bounded copy: copies src into dst (a buffer of dstSize bytes),
  always NUL-terminating, never writing past dst[dstSize-1].
*/
static void copyValue(char *dst, uint8_t dstSize, const char *src)
{
    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
}

/*
  Right-align src into dst, zero-padded on the left, producing exactly
  width chars + NUL. If src is longer than width, the rightmost width
  chars are kept.
*/
static void padLeft(char *dst, uint8_t width, const char *src)
{
    uint8_t len = strlen(src);
    if (len > width) len = width; // keep the RIGHTMOST width chars
    memset(dst, '0', width - len);
    memcpy(dst + width - len, src + strlen(src) - len, len);
    dst[width] = '\0';
}

/*
  Right-pad dst (in place) with fill up to width chars, without exceeding
  dstSize - 1 chars. Used only on local display copies, never on the
  stored globals.
*/

/*
  Copies src into dst (see copyValue) only if it actually differs.
  Returns true if the value changed (and therefore dst was updated).
*/
static bool updateValue(char *dst, uint8_t dstSize, const char *src)
{
    if (strncmp(dst, src, dstSize - 1) == 0) return false;
    copyValue(dst, dstSize, src);
    return true;
}

OledMonitorPanel::OledMonitorPanel()
{
    _initialised = false;
    _currentChannel = 0xFF;
    _dirty = 0;
}

void OledMonitorPanel::attach(uint8_t addrI2C)
{
    _addrI2C = addrI2C;
    _currentChannel = 0xFF;
    Wire.begin();
    Wire.setClock(400000);
    if (!FitInMemory(sizeof(OLEDInterface))) {
        // Error Message to Connector
        cmdMessenger.sendCmd(kStatus, F("Custom Device does not fit in Memory"));
        return;
    }
    if (_addrI2C & 0x01) {
        oled = new (allocateMemory(sizeof(OLEDInterface))) OLEDInterface(SSD1306);
    } else {
        oled = new (allocateMemory(sizeof(OLEDInterface))) OLEDInterface(SH1106);
    }
    _initialised = true;
}

void OledMonitorPanel::begin()
{
    if (!_initialised)
        return;

    //**************************
    // // Efis left
    // //**************************
    setTCAChannel(TCA9548A_CHANNEL_EFIS_LEFT);
    oled->begin(SCREEN_ADDRESS, true); // Address 0x3C default
    // One framebuffer is shared by all eight displays, so this applies to all
    // of them. Without it a glyph that would cross the right edge is moved to
    // the start of the next line instead of being clipped.
    oled->setTextWrap(false);
    oled->display();
    updateDisplayEfisLeft();

    //**************************
    // Efis right
    //**************************
    setTCAChannel(TCA9548A_CHANNEL_EFIS_RIGHT);
    oled->begin(SCREEN_ADDRESS, true); // Address 0x3C default
    oled->display();
    oled->setTextColor(SSD1306_WHITE);
    updateDisplayEfisRight();

    //**********************************************
    // FCU SPD
    //**********************************************
    setTCAChannel(TCA9548A_CHANNEL_FCU_SPD);
    oled->begin(SCREEN_ADDRESS, true); // Address 0x3C default
    updateDisplayFcuSpd();

    //**********************************************
    // FCU HDG
    //**********************************************
    setTCAChannel(TCA9548A_CHANNEL_FCU_HDG);
    oled->begin(SCREEN_ADDRESS, true); // Address 0x3C default
    updateDisplayFcuHdg();

    //**********************************************
    // FCU HDG and V/S or TRK and FPA.
    //**********************************************
    setTCAChannel(TCA9548A_CHANNEL_FCU_FPA);
    oled->begin(SCREEN_ADDRESS, true); // Address 0x3C default
    updateDisplayFcuFpa();

    //**********************************************
    // FCU ALT
    //**********************************************
    setTCAChannel(TCA9548A_CHANNEL_FCU_ALT);
    oled->begin(SCREEN_ADDRESS, true); // Address 0x3C default
    updateDisplayFcuAlt();

    //**********************************************
    // FCU Vs
    //**********************************************
    setTCAChannel(TCA9548A_CHANNEL_FCU_VS);
    oled->begin(SCREEN_ADDRESS, true); // Address 0x3C default
    updateDisplayFcuVs();
    //**********************************************
    // AUX 8
    //**********************************************
   setTCAChannel(TCA9548A_CHANNEL_Aux);
   oled->begin(SCREEN_ADDRESS, true); // Address 0x3C default
    updateDisplayAux();

    // All eight screens were just drawn above - nothing pending yet.
    _dirty = 0;
}

void OledMonitorPanel::detach()
{
    if (!_initialised)
        return;
    _initialised = false;
}

void OledMonitorPanel::set(int16_t messageID, char *message)
{
    /* **********************************************************************************
        Each messageID has it's own value
        check for the messageID and define what to do.
        Important Remark!
        MessageID == -1 will be send from the connector when Mobiflight is closed
        Put in your code to shut down your custom device (e.g. clear a display)
        MessageID == -2 will be send from the connector when PowerSavingMode is entered
        Put in your code to enter this mode (e.g. clear a display)

    ********************************************************************************** */
    // do something according your messageID
    if (!_initialised)
        return;

    switch (messageID) {
    case 2:
        // Efis Left Baro Value Hg
        if (updateValue(efisLeftBaroValueHg, sizeof(efisLeftBaroValueHg), message))
            _dirty |= (1 << SCR_EFIS_LEFT);
        break;

    case 5:
        // Efis Right Baro Value Hpa
        if (updateValue(efisRightBaroValueHpa, sizeof(efisRightBaroValueHpa), message))
            _dirty |= (1 << SCR_EFIS_RIGHT);
        break;

    case 6:
        // Efis Right Baro Value Hg (this is the Radio Altimeter screen)
        if (updateValue(efisRightBaroValueHg, sizeof(efisRightBaroValueHg), message))
            _dirty |= (1 << SCR_FCU_FPA);
        break;

    case 8:
        // Fcu Speed Value
        if (updateValue(fcuSpeedValue, sizeof(fcuSpeedValue), message))
            _dirty |= (1 << SCR_FCU_SPD);
        break;

    case 9: {
        // Fcu Speed Managed
        // 0 = No; 1 = Yes
        // Also read by updateDisplayEfisLeft() - keep both screens in sync.
        uint8_t v = atoi(message);
        if (v != fcuSpeedManagedMode) {
            fcuSpeedManagedMode = v;
            _dirty |= (1 << SCR_FCU_SPD) | (1 << SCR_EFIS_LEFT);
        }
        break;
    }

    case 10:
        // Fcu Hdg Value
        if (updateValue(fcuHdgValue, sizeof(fcuHdgValue), message))
            _dirty |= (1 << SCR_FCU_HDG);
        break;

    case 11: {
        // Fcu Hdg Managed
        // 0 = No; 1 = Yes
        uint8_t v = atoi(message);
        if (v != fcuHdgManagedMode) {
            fcuHdgManagedMode = v;
            _dirty |= (1 << SCR_FCU_HDG);
        }
        break;
    }

    case 12: {
        // Fcu Trk Mode
        // 0 = No; 1 = Yes
        uint8_t v = atoi(message);
        if (v != fcuTrkMode) {
            fcuTrkMode = v;
            _dirty |= (1 << SCR_FCU_HDG) | (1 << SCR_FCU_FPA) | (1 << SCR_FCU_VS);
        }
        break;
    }

    case 13:
        if (updateValue(fcuAltValue, sizeof(fcuAltValue), message))
            _dirty |= (1 << SCR_FCU_ALT);
        break;

    case 14: {
        uint8_t v = atoi(message);
        if (v != fcuAltManagedMode) {
            fcuAltManagedMode = v;
            _dirty |= (1 << SCR_FCU_ALT);
        }
        break;
    }

    case 15:
        if (updateValue(fcuVsValue, sizeof(fcuVsValue), message))
            _dirty |= (1 << SCR_FCU_VS);
        break;

    case 16:
        if (updateValue(fcuVsValueFpa, sizeof(fcuVsValueFpa), message))
            _dirty |= (1 << SCR_FCU_VS);
        break;

    case 17: {
        uint8_t v = atoi(message);
        if (v != fcuVsManagedMode) {
            fcuVsManagedMode = v;
            _dirty |= (1 << SCR_FCU_VS);
        }
        break;
    }

    case 18: {
        // Also read by updateDisplayEfisLeft() - keep both screens in sync.
        uint8_t v = atoi(message);
        if (v != fcuSpeedMode) {
            fcuSpeedMode = v;
            _dirty |= (1 << SCR_FCU_SPD) | (1 << SCR_EFIS_LEFT);
        }
        break;
    }

    case 19: {
        uint8_t v = atoi(message);
        if (v != lightTestOn) {
            lightTestOn = v;
            _dirty = 0xFF; // all SCR_COUNT (8) screens
        }
        break;
    }

    case 20:
        if (updateValue(CRSValue, sizeof(CRSValue), message))
            _dirty |= (1 << SCR_AUX);
        break;

    case -1:
    case -2:
        blankAllDisplays();
        _dirty = 0; // don't let a pending redraw re-light a screen after shutdown
        break;

    default:
        break;
    }
}

void OledMonitorPanel::update()
{
    if (!_initialised)
        return;
    if (!_dirty)
        return;

    // Render at most one screen per call, so a burst of set() calls (e.g.
    // Light Test) spreads its ~25-30 ms-per-screen I2C cost across multiple
    // loop() iterations instead of blocking serial RX for ~240 ms straight.
    uint8_t scr = __builtin_ctz(_dirty); // lowest set bit
    _dirty &= ~((uint8_t)1 << scr);
    renderScreen(scr);
}

/*
  Dispatch a single screen index (SCR_*) to its updateDisplayXxx() renderer.
*/
void OledMonitorPanel::renderScreen(uint8_t scr)
{
    switch (scr) {
    case SCR_EFIS_LEFT:  updateDisplayEfisLeft();  break;
    case SCR_EFIS_RIGHT: updateDisplayEfisRight(); break;
    case SCR_FCU_SPD:    updateDisplayFcuSpd();    break;
    case SCR_FCU_HDG:    updateDisplayFcuHdg();    break;
    case SCR_FCU_FPA:    updateDisplayFcuFpa();    break;
    case SCR_FCU_ALT:    updateDisplayFcuAlt();    break;
    case SCR_FCU_VS:     updateDisplayFcuVs();     break;
    case SCR_AUX:        updateDisplayAux();       break;
    default: break;
    }
}

/* ************************************************************************************************
 ************************************************************************************************
 ************************************************************************************************ */

/*
  switch multiplexer channel
*/
void OledMonitorPanel::setTCAChannel(byte i)
{
    if (_currentChannel == i) return;
    Wire.beginTransmission(_addrI2C);
    Wire.write(1 << i);
    Wire.endTransmission();
    _currentChannel = i;
}

/*
  Blank all eight OLED displays (called on shutdown / power saving)
*/
void OledMonitorPanel::blankAllDisplays(void)
{
    const byte channels[] = {
        TCA9548A_CHANNEL_EFIS_LEFT,
        TCA9548A_CHANNEL_EFIS_RIGHT,
        TCA9548A_CHANNEL_FCU_SPD,
        TCA9548A_CHANNEL_FCU_HDG,
        TCA9548A_CHANNEL_FCU_FPA,
        TCA9548A_CHANNEL_FCU_ALT,
        TCA9548A_CHANNEL_FCU_VS,
        TCA9548A_CHANNEL_Aux
    };

    for (byte i = 0; i < 8; i++) {
        setTCAChannel(channels[i]);
        oled->clearDisplay();
        oled->display();
    }
}

/*******************************************
Has to be redone, only tests

******************************************/

/*
  Shared renderer for the "small label + big DSEG7 value, optional managed
  dot" screen shape (EFIS left/right, FCU SPD/HDG/ALT/FPA, AUX). Does the
  channel switch, clear, label draw, value draw, optional dot, and
  display() - callers only compute which text/cursor/font/dot to pass in
  (including any per-mode label-cursor swap or digit mutation, which stays
  in the caller). FCU VS is the one screen left as a hand-written function:
  its sign handling, dual fonts, and V/S-vs-FPA branching don't reduce to
  this shape without obscuring the logic.
*/
/*
  Draws text horizontally centred on the 128 px wide screen at baseline y,
  using whatever font is currently selected. The label positions in the
  original firmware were hand-tuned for three-character labels (x = 50), so
  anything longer drifted right - "VOR DME" is 63 px wide and ended up hard
  against the right edge, and "RADIO ALT" sat well left of centre. Measuring
  the string means a label can be renamed without re-tuning a magic x.
*/
void OledMonitorPanel::printCentered(const char *text, int16_t y)
{
    int16_t  x1, y1;
    uint16_t w, h;

    oled->getTextBounds(text, 0, y, &x1, &y1, &w, &h);
    int16_t x = ((int16_t)SCREEN_WIDTH - (int16_t)w) / 2 - x1;
    if (x < 0) x = 0;
    oled->setCursor(x, y);
    oled->println(text);
}

void OledMonitorPanel::renderLabelValue(byte channel,
                                 const char *labelText, int16_t labelY, const GFXfont *labelFont,
                                 const char *valueText, int16_t valueX, int16_t valueY, const GFXfont *valueFont,
                                 bool drawDot, int16_t dotX, int16_t dotY)
{
    setTCAChannel(channel);
    oled->clearDisplay();
    oled->setTextColor(SSD1306_WHITE);
    oled->setTextSize(1);

    oled->setFont(labelFont);
    printCentered(labelText, labelY);

    oled->setFont(valueFont);
    oled->setCursor(valueX, valueY);
    oled->println(valueText);

    if (drawDot) {
        oled->fillCircle(dotX, dotY, 3, SSD1306_WHITE);
    }
    oled->display();
}

void OledMonitorPanel::updateDisplayAux(void) // добавил 8й экран
{
    if (lightTestOn == 1) {
        setTCAChannel(TCA9548A_CHANNEL_Aux);
        oled->clearDisplay();
        oled->setTextColor(SSD1306_WHITE); // Draw white text
        oled->setFont(&FreeSans7pt7b);
        oled->setTextSize(1);
        oled->setCursor(20, 20);
        oled->println("CRS");
        oled->setCursor(60, 20);
        oled->println("TRK");
        oled->setCursor(95, 20);
        oled->println("LAT");
        oled->setFont(&DSEG7Classic_Regular15pt7b);
        oled->setCursor(28, 55);
        oled->println("888");
        oled->fillCircle(104, 40, 3, SSD1306_WHITE);
        oled->display();
        return;
    }

    char strHdgValue5[4];
    padLeft(strHdgValue5, 3, CRSValue);
    renderLabelValue(TCA9548A_CHANNEL_Aux,
                      "CRS", 13, &FreeSans7pt7b,
                      strHdgValue5, 20, 55, &DSEG7Classic_Regular18pt7b,
                      false, 0, 0);
}



void OledMonitorPanel::updateDisplayEfisLeft(void)
{
    if (lightTestOn == 1) {
        setTCAChannel(TCA9548A_CHANNEL_EFIS_LEFT);
        oled->clearDisplay();
        oled->setTextColor(SSD1306_WHITE);
        oled->setFont(&FreeSans6pt7b);
        oled->setTextSize(1);
        oled->setCursor(25, 20);
        oled->println("SPD");
        oled->setCursor(65, 20);
        oled->println("MACH");
        oled->setFont(&DSEG7Classic_Regular16pt7b);
        oled->setCursor(28, 55);
        oled->println("888");
        oled->fillCircle(104, 40, 3, SSD1306_WHITE);
        oled->display();
        return;
    }

    char displayValue[6];
    copyValue(displayValue, sizeof(displayValue), efisLeftBaroValueHg);

    const char *labelText;
    if (fcuSpeedMode == 0) {
        labelText = "MACH";
        // MACH is always sent as x.xx (4 characters), so it is shown as-is.
        // The original firmware had a displayValue[4] write here that never had
        // any effect with Arduino String, and would corrupt the buffer if kept.
    } else {
        labelText = "SPEED";
    }

    if (fcuSpeedManagedMode == 1) {
        renderLabelValue(TCA9548A_CHANNEL_EFIS_LEFT,
                          labelText, 13, &FreeSans6pt7b,
                          "---", 28, 55, &DSEG7Classic_Regular16pt7b,
                          true, 104, 40);
    } else {
        renderLabelValue(TCA9548A_CHANNEL_EFIS_LEFT,
                          labelText, 13, &FreeSans6pt7b,
                          displayValue, 20, 55, &DSEG7Classic_Regular18pt7b,
                          false, 0, 0);
    }
} // updateDisplayEfisLeft

void OledMonitorPanel::updateDisplayEfisRight(void)
{
    if (lightTestOn == 1) {
        setTCAChannel(TCA9548A_CHANNEL_EFIS_RIGHT);
        oled->clearDisplay();
        oled->setTextColor(SSD1306_WHITE); // Draw white text
        oled->setFont(&FreeSans7pt7b);
        oled->setTextSize(1);
        oled->setCursor(20, 20);
        oled->println("HDG");
        oled->setCursor(60, 20);
        oled->println("TRK");
        oled->setCursor(95, 20);
        oled->println("LAT");
        oled->setFont(&DSEG7Classic_Regular15pt7b);
        oled->setCursor(28, 55);
        oled->println("888");
        oled->fillCircle(104, 40, 3, SSD1306_WHITE);
        oled->display();
        return;
    }

    char strHdgValue3[4];
    padLeft(strHdgValue3, 3, efisRightBaroValueHpa);
    renderLabelValue(TCA9548A_CHANNEL_EFIS_RIGHT,
                      "VOR DME", 13, &FreeSans7pt7b,
                      strHdgValue3, 20, 55, &DSEG7Classic_Regular18pt7b,
                      false, 0, 0);
} // updateDisplayEfisRight

void OledMonitorPanel::updateDisplayFcuSpd(void)
{
    if (lightTestOn == 1) {
        setTCAChannel(TCA9548A_CHANNEL_FCU_SPD);
        oled->clearDisplay();
        oled->setTextColor(SSD1306_WHITE);
        oled->setFont(&FreeSans6pt7b);
        oled->setTextSize(1);
        oled->setCursor(25, 20);
        oled->println("SPD");
        oled->setCursor(65, 20);
        oled->println("MACH");
        oled->setFont(&DSEG7Classic_Regular16pt7b);
        oled->setCursor(28, 55);
        oled->println("888");
        oled->fillCircle(104, 40, 3, SSD1306_WHITE);
        oled->display();
        return;
    }

    char displayValue[6];
    copyValue(displayValue, sizeof(displayValue), fcuSpeedValue);

    const char *labelText;
    int16_t labelY;
    if (fcuSpeedMode == 1) {
        labelText = "MACH";
        labelY = 20;
        // MACH is always sent as x.xx (4 characters), so it is shown as-is.
        // The original firmware had a displayValue[4] write here that never had
        // any effect with Arduino String, and would corrupt the buffer if kept.
    } else {
        labelText = "SPEED";
        labelY = 13;
    }

    if (fcuSpeedManagedMode == 1) {
        renderLabelValue(TCA9548A_CHANNEL_FCU_SPD,
                          labelText, labelY, &FreeSans6pt7b,
                          "---", 28, 55, &DSEG7Classic_Regular16pt7b,
                          true, 104, 40);
    } else {
        renderLabelValue(TCA9548A_CHANNEL_FCU_SPD,
                          labelText, labelY, &FreeSans6pt7b,
                          displayValue, 28, 55, &DSEG7Classic_Regular18pt7b,
                          false, 0, 0);
    }
}

void OledMonitorPanel::updateDisplayFcuHdg(void)
{
    if (lightTestOn == 1) {
        setTCAChannel(TCA9548A_CHANNEL_FCU_HDG);
        oled->clearDisplay();
        oled->setTextColor(SSD1306_WHITE); // Draw white text
        oled->setFont(&FreeSans7pt7b);
        oled->setTextSize(1);
        oled->setCursor(20, 20);
        oled->println("HDG");
        oled->setCursor(60, 20);
        oled->println("TRK");
        oled->setCursor(95, 20);
        oled->println("LAT");
        oled->setFont(&DSEG7Classic_Regular15pt7b);
        oled->setCursor(28, 55);
        oled->println("888");
        oled->fillCircle(104, 40, 3, SSD1306_WHITE);
        oled->display();
        return;
    }

    if (fcuHdgManagedMode == 1) {
        renderLabelValue(TCA9548A_CHANNEL_FCU_HDG,
                          "HDG", 13, &FreeSans7pt7b,
                          "---", 28, 55, &DSEG7Classic_Regular15pt7b,
                          true, 104, 40);
    } else {
        char strHdgValue[4];
        padLeft(strHdgValue, 3, fcuHdgValue);
        renderLabelValue(TCA9548A_CHANNEL_FCU_HDG,
                          "HDG", 13, &FreeSans7pt7b,
                          strHdgValue, 20, 55, &DSEG7Classic_Regular18pt7b,
                          false, 0, 0);
    }
}

void OledMonitorPanel::updateDisplayFcuFpa(void)
{
    char strAltValue2[6];

    if (lightTestOn == 1) {
        copyValue(strAltValue2, sizeof(strAltValue2), "88888");
    } else {
        padLeft(strAltValue2, 5, efisRightBaroValueHg);
    }

    renderLabelValue(TCA9548A_CHANNEL_FCU_FPA,
                      "RADIO ALT", 16, &FreeSans7pt7b,
                      strAltValue2, 0, 55, &DSEG7Classic_Regular16pt7b,
                      false, 0, 0);
}

void OledMonitorPanel::updateDisplayFcuAlt(void)
{
    char strAltValue[6];
    bool drawDot;

    if (lightTestOn == 1) {
        copyValue(strAltValue, sizeof(strAltValue), "88888");
        drawDot = true;
    } else {
        padLeft(strAltValue, 5, fcuAltValue);
        drawDot = (fcuAltManagedMode == 1);
    }

    renderLabelValue(TCA9548A_CHANNEL_FCU_ALT,
                      "ALT", 15, &FreeSans8pt7b,
                      strAltValue, 1, 55, &DSEG7Classic_Regular16pt7b,
                      drawDot, 124, 39);
} // updateDisplayFcuAlt

void OledMonitorPanel::updateDisplayFcuVs(void)
{
    char strVrValue[8] = "0000";

    setTCAChannel(TCA9548A_CHANNEL_FCU_VS);

    // Clear the buffer
    oled->clearDisplay();
    oled->setTextColor(SSD1306_WHITE);
    oled->setFont(&FreeSans8pt7b);
    oled->setTextSize(1);
    if (lightTestOn == 1) {
        printCentered("V/S", 20);

        oled->setFont(&FreeSans18pt7b);
        oled->setCursor(0, 50);
        oled->print("+");

        strVrValue[0] = '8';
        strVrValue[1] = '8';
        strVrValue[2] = '8';
        strVrValue[3] = '8';
        strVrValue[4] = '\0';

        oled->setFont(&DSEG7Classic_Regular15pt7b);
        oled->setCursor(24, 55);
        oled->print(strVrValue);
    } else {


        printCentered("V/S", 20);

        if (fcuVsManagedMode == 1) {
            oled->setFont(&DSEG7Classic_Regular15pt7b);
            oled->setCursor(0, 55);
            oled->print("-----");
        } else {
            if (fcuTrkMode == 0) {
                if (fcuVsValue[1] == '\0') {
                    oled->setFont(&FreeSans18pt7b);
                    oled->setCursor(0, 50);
                    oled->print("+");

                    strVrValue[0] = '0';
                    strVrValue[1] = '0';
                    strVrValue[2] = '0';
                    strVrValue[3] = '0';
                    strVrValue[4] = '\0';

                    oled->setFont(&DSEG7Classic_Regular15pt7b);
                    oled->setCursor(24, 55);
                    oled->print(strVrValue);
                } else {
                    if (fcuVsValue[0] == '-') {
                        oled->setFont(&DSEG7Classic_Regular15pt7b);
                        oled->setCursor(0, 55);
                        oled->print("-");

                        if (fcuVsValue[4] == '\0') {
                            strVrValue[0] = '0';
                            strVrValue[1] = fcuVsValue[1];
                            strVrValue[2] = fcuVsValue[2];
                            strVrValue[3] = fcuVsValue[3];
                        } else {
                            strVrValue[0] = fcuVsValue[1];
                            strVrValue[1] = fcuVsValue[2];
                            strVrValue[2] = fcuVsValue[3];
                            strVrValue[3] = fcuVsValue[4];
                        }
                        strVrValue[4] = '\0';
                        oled->print(strVrValue);
                    } else {
                        oled->setFont(&FreeSans18pt7b);
                        oled->setCursor(0, 50);
                        oled->print("+");

                        if (fcuVsValue[3] == '\0') {
                            strVrValue[0] = '0';
                            strVrValue[1] = fcuVsValue[0];
                            strVrValue[2] = fcuVsValue[1];
                            strVrValue[3] = fcuVsValue[2];
                        } else {
                            strVrValue[0] = fcuVsValue[0];
                            strVrValue[1] = fcuVsValue[1];
                            strVrValue[2] = fcuVsValue[2];
                            strVrValue[3] = fcuVsValue[3];
                        }
                        strVrValue[4] = '\0';

                        oled->setFont(&DSEG7Classic_Regular15pt7b);
                        oled->setCursor(24, 55);
                        oled->print(strVrValue);
                    }
                }
            } else {
                if (fcuVsValueFpa[0] == '-') {
                    oled->setFont(&DSEG7Classic_Regular15pt7b);
                    oled->setCursor(0, 55);

                    if (fcuVsValueFpa[2] == '\0') {
                        snprintf(strVrValue, sizeof(strVrValue), "%s.0", fcuVsValueFpa);
                    } else {
                        copyValue(strVrValue, sizeof(strVrValue), fcuVsValueFpa);
                    }
                    oled->print(strVrValue);
                } else {
                    oled->setFont(&FreeSans18pt7b);
                    oled->setCursor(0, 50);
                    oled->print("+");

                    if (strcmp(fcuVsValueFpa, "0") == 0) {
                        copyValue(strVrValue, sizeof(strVrValue), "0.0");
                    } else {
                        if (fcuVsValueFpa[1] == '\0') {
                            snprintf(strVrValue, sizeof(strVrValue), "%s.0", fcuVsValueFpa);
                        } else {
                            strVrValue[0] = fcuVsValueFpa[0];
                            strVrValue[1] = fcuVsValueFpa[1];
                            strVrValue[2] = fcuVsValueFpa[2];
                            strVrValue[3] = '\0';
                        }
                    }

                    oled->setFont(&DSEG7Classic_Regular15pt7b);
                    oled->setCursor(24, 55);
                    oled->print(strVrValue);
                }
            }
        }
    }
    oled->display();
}
