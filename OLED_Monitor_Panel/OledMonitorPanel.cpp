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
  Right-aligns a numeric value in a field of `width` digits, zero padded, and
  falls back to a field of dashes when the string is not a number at all or
  the value lies outside [lo, hi].

  Two reasons this exists next to padLeft(). MobiFlight sends "-0" for VOR DME
  when no station is tuned; padLeft keeps the rightmost characters of the
  source, so the screen rendered "0-0". And the radio altimeter is only
  meaningful up to 2500 ft - above that the sim keeps sending the true height
  above ground, which has no business on that screen.
*/
/*
  Copies src into dst dropping any '.', giving the digit-cell string for a
  value like MACH. The DSEG7 decimal point has xAdvance 0 - it is drawn
  between two cells and occupies none - so "0.78" is three cells, not four.

  Done by scanning rather than by fixed indices because the two ends of this
  disagree about the format: oled_monitor_panel.device.json describes message
  2 as "value without dot", while the shipped MobiFlight profile formats the
  FCU speed value with one. Either way this yields the same cell string, and
  a short value can no longer read past the end of the source buffer.
*/
static void stripDot(char *dst, uint8_t dstSize, const char *src)
{
    uint8_t n = 0;
    for (; *src && n < dstSize - 1; src++) {
        if (*src != '.') dst[n++] = *src;
    }
    dst[n] = 0;
}

static void padLeftRanged(char *dst, uint8_t width, const char *src, int16_t lo, int16_t hi)
{
    char *end;
    long  v = strtol(src, &end, 10);

    if (end == src || v < lo || v > hi) {
        memset(dst, '-', width);
        dst[width] = '\0';
        return;
    }

    dst[width] = '\0';
    for (int8_t i = width - 1; i >= 0; i--) {
        dst[i] = (char)('0' + (v % 10));
        v /= 10;
    }
}

/*
  Normalises a MACH value into three digit cells and the string to draw.

  MACH always reads x.xx on this panel, but what actually arrives depends on
  how the MobiFlight profile is set up, and the three plausible forms do not
  agree with each other:

      "0.78"   the sim value straight through
      "78"     what oled_monitor_panel.device.json documents for message 2
      "0.5"    the profile rule "value < 100 -> 0.$" applied to a variable
               already scaled by 100 - i.e. mach 0.05

  Taking the decimal point from the string therefore cannot be right for all
  of them. Stripping any '.' and zero-padding the digits to three makes every
  form land on the same cells, and the point is drawn from here instead.

  It also guarantees the three-character cell string renderCells() needs: a
  shorter one fails its length check and silently falls back to a full
  repaint on every single update.
*/
static void machValue(char *cells, char *shown, const char *src)
{
    char digits[8];
    stripDot(digits, sizeof(digits), src);
    padLeft(cells, 3, digits);

    shown[0] = cells[0];
    shown[1] = '.';
    shown[2] = cells[1];
    shown[3] = cells[2];
    shown[4] = 0;
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

/*
  Scratch column buffer for fastDrawDigit(). File-scope rather than a class
  member: it only ever holds one glyph's worth of columns while that
  function runs, so there is nothing to gain from giving every instance its
  own copy, and keeping it off the class shrinks OledMonitorPanel itself.
  32 covers the widest glyph either DSEG7 face at 18pt/16pt uses (22 and 19
  px respectively) with headroom; fastDrawDigit() guards against anything
  wider instead of trusting that.
*/
static uint8_t colbuf[32];

/*
  Per-screen digit-cell geometry for renderCells()/commitCells(). One entry
  per SCR_* index, in PROGMEM since it is read only a handful of times per
  update and RAM is the scarce resource here. Numbers are derived from the
  DSEG7 GFXglyph tables (xAdvance/xOffset/width/yOffset) and the cursor X
  already used by each updateDisplayXxx() call site - see the class header
  comment on renderCells() for the geometry this encodes.

  Constraint that is easy to break by accident: a cell rectangle covers whole
  8-row pages, so on an 18pt screen it starts at row 16 and on the 16pt ALT
  screen at row 24. No label may ink a row at or below that, or redrawing one
  cell will erase part of the label - which a full repaint would then put back,
  so it shows up as a label that decays while a value changes and heals when
  the mode does. Verified margins: 18pt screens +2 rows, RADIO ALT +0 (its
  label ends exactly on row 15), ALT +8.
*/
struct CellGeom {
    uint8_t channel;   // TCA9548A_CHANNEL_* value
    uint8_t x;         // cursor x of digit cell 0
    uint8_t advance;   // cell pitch = the font's xAdvance
    uint8_t blitW;     // width of the pushed rectangle
    uint8_t page0;
    uint8_t pages;
    uint8_t digits;    // number of digit cells
    uint8_t fontIdx;   // 0 = DSEG7 18pt, 1 = DSEG7 16pt
};

static const CellGeom cellGeomTable[] PROGMEM = {
    // SCR_EFIS_LEFT
    { TCA9548A_CHANNEL_EFIS_LEFT,  21, 29, 24, 2, 5, 3, 0 },
    // SCR_EFIS_RIGHT
    { TCA9548A_CHANNEL_EFIS_RIGHT, 21, 29, 24, 2, 5, 3, 0 },
    // SCR_FCU_SPD
    { TCA9548A_CHANNEL_FCU_SPD,    21, 29, 24, 2, 5, 3, 0 },
    // SCR_FCU_HDG
    { TCA9548A_CHANNEL_FCU_HDG,    21, 29, 24, 2, 5, 3, 0 },
    // SCR_FCU_FPA
    { TCA9548A_CHANNEL_FCU_FPA,     6, 29, 24, 2, 5, 4, 0 },
    // SCR_FCU_ALT
    { TCA9548A_CHANNEL_FCU_ALT,     1, 25, 21, 3, 4, 5, 1 },
    // SCR_FCU_VS - no partial path for this screen, digits == 0 means
    // renderCells() always defers to the full repaint.
    { 0, 0, 0, 0, 0, 0, 0, 0 },
    // SCR_AUX
    { TCA9548A_CHANNEL_Aux,        21, 29, 24, 2, 5, 3, 0 },
};

// Catches a SCR_* enum edit that forgets to update the table above, rather
// than letting renderCells() silently read past the end of it.
static_assert(sizeof(cellGeomTable) / sizeof(cellGeomTable[0]) == SCR_COUNT,
              "cellGeomTable must have exactly SCR_COUNT entries");

/*
  Maps a CellGeom::fontIdx to the actual font pointer. Kept out of the
  PROGMEM struct itself - a GFXfont* read back via memcpy_P would still be a
  flash address, so storing it there buys nothing and just duplicates what
  this switch already expresses.
*/
static const GFXfont *fontForIndex(uint8_t fontIdx)
{
    switch (fontIdx) {
    case 1:  return &DSEG7Classic_Regular16pt7b;
    default: return &DSEG7Classic_Regular18pt7b;
    }
}

OledMonitorPanel::OledMonitorPanel()
{
    _initialised = false;
    _currentChannel = 0xFF;
    _dirty = 0;
    // Nothing has been drawn yet, so no shadow describes any screen. The
    // device pool this object is placement-new'd into happens to be zeroed,
    // but renderCells() correctness should not rest on that.
    memset(_shadowSig, 0, sizeof(_shadowSig));
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

    // Every screen above was fully repainted by an updateDisplayXxx() call,
    // not renderCells(), so no shadow reflects what is actually on screen.
    memset(_shadowSig, 0, sizeof(_shadowSig));
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

    // The framebuffer no longer matches any shadow's idea of what is drawn.
    memset(_shadowSig, 0, sizeof(_shadowSig));
}

/*
  Rasterises glyph `c` of `font` straight into the shared page-major
  framebuffer, OR-ing bits in rather than going through Adafruit_GFX's
  drawPixel-per-pixel drawChar(). Measured 1246 us against 4589 us for one
  DSEG7 18pt glyph on the actual board. Restricted to the page range
  [page0, page0 + pages) - the caller is expected to pass the same range it
  intends to push with displayRegion(), so nothing outside that range is
  touched.

  Baseline is always row DIGIT_BASELINE_Y on this panel, so it is not a
  parameter - every digit cell on every screen shares one baseline.
*/
void OledMonitorPanel::fastDrawDigit(uint8_t cursorX, uint8_t page0, uint8_t pages,
                                      const GFXfont *font, char c)
{
    GFXglyph *g  = &(((GFXglyph *)pgm_read_ptr(&font->glyph))[c - pgm_read_byte(&font->first)]);
    uint8_t  *bm = (uint8_t *)pgm_read_ptr(&font->bitmap);
    uint16_t  bo = pgm_read_word(&g->bitmapOffset);
    uint8_t   gw = pgm_read_byte(&g->width);
    uint8_t   gh = pgm_read_byte(&g->height);
    int8_t    xo = pgm_read_byte(&g->xOffset);
    int8_t    yo = pgm_read_byte(&g->yOffset);

    // A glyph wider than colbuf can never happen with the fonts this panel
    // uses, but guard it rather than trust that silently.
    if (gw > sizeof(colbuf)) return;

    // The framebuffer is the only thing between this and the heap. Every
    // caller passes geometry from cellGeomTable, which fits - but a stray
    // cursorX must not be able to write past the end of a page.
    int16_t left = (int16_t)cursorX + xo;
    if (left < 0 || left + gw > SCREEN_WIDTH) return;

    uint8_t *fb = oled->getBuffer();

    int16_t topRow = (int16_t)DIGIT_BASELINE_Y + yo - (int16_t)page0 * 8; // glyph top, relative to page0

    for (uint8_t p = 0; p < pages; p++) {
        memset(colbuf, 0, gw);
        int16_t bandTop = (int16_t)p * 8;
        for (uint8_t b = 0; b < 8; b++) {
            int16_t gr = bandTop + b - topRow;       // row inside the glyph
            if (gr < 0 || gr >= (int16_t)gh) continue;
            uint16_t idx   = (uint16_t)gr * gw;
            uint16_t byteI = bo + (idx >> 3);
            uint8_t  bit   = 0x80 >> (idx & 7);
            uint8_t  bits  = pgm_read_byte(bm + byteI);
            uint8_t  mask  = 1 << b;
            for (uint8_t cx = 0; cx < gw; cx++) {
                if (bits & bit) colbuf[cx] |= mask;
                bit >>= 1;
                if (!bit) { bit = 0x80; bits = pgm_read_byte(bm + (++byteI)); }
            }
        }
        uint8_t *dst = fb + (uint16_t)(page0 + p) * SCREEN_WIDTH + cursorX + xo;
        for (uint8_t cx = 0; cx < gw; cx++) dst[cx] |= colbuf[cx];
    }
}

/*
  Zeroes a blitW x (pages * 8px) rectangle of the framebuffer at column
  blitX, pages [page0, page0 + pages). A plain memset per page measures
  43 us for 5 pages of 24 columns, against 389 us for the equivalent
  fillRect() - fillRect draws pixel by pixel and doesn't know the rectangle
  is page-aligned, so it can't do the memset Adafruit_GFX itself would
  reach for if it exposed one.
*/
void OledMonitorPanel::clearCell(uint8_t blitX, uint8_t page0, uint8_t pages, uint8_t blitW)
{
    uint8_t *fb = oled->getBuffer();
    for (uint8_t p = 0; p < pages; p++) {
        memset(fb + (uint16_t)(page0 + p) * SCREEN_WIDTH + blitX, 0, blitW);
    }
}

/*
  Redraws only the digit cells whose character changed since the last
  renderCells()/commitCells() on this screen, and pushes only those cells'
  rectangles over I2C - the fast path this whole file exists for. Returns
  false when the caller must fall back to a full repaint instead: the
  layout signature `sig` does not match what commitCells() last recorded
  for this screen (mode change, label change, different font - anything
  that moved something renderCells() does not know how to erase), or this
  screen has no cell geometry at all (SCR_FCU_VS).

  `cells` must be a NUL-terminated string of exactly geom.digits characters,
  one per cell, left to right. `sig` must be non-zero - 0 is reserved to
  mean "unknown" in _shadowSig.
*/
bool OledMonitorPanel::renderCells(uint8_t scr, const char *cells, uint8_t sig)
{
    if (!_initialised) return false;
    if (scr >= SCR_COUNT) return false;

    CellGeom geom;
    memcpy_P(&geom, &cellGeomTable[scr], sizeof(CellGeom));

    if (geom.digits == 0) return false;        // this screen has no partial path (SCR_FCU_VS)
    if (sig == 0) return false;
    if (_shadowSig[scr] != sig) return false;   // layout on screen does not match - need a full repaint
    if (strlen(cells) != geom.digits) return false;

    const GFXfont *font = fontForIndex(geom.fontIdx);

    setTCAChannel(geom.channel);
    for (uint8_t i = 0; i < geom.digits; i++) {
        if (cells[i] == _shadow[scr][i]) continue; // unchanged cell - zero cost

        uint8_t cursorX = geom.x + i * geom.advance;
        uint8_t blitX   = cursorX + 2;

        clearCell(blitX, geom.page0, geom.pages, geom.blitW);
        fastDrawDigit(cursorX, geom.page0, geom.pages, font, cells[i]);
        oled->displayRegion(blitX, geom.page0, geom.blitW, geom.pages);

        _shadow[scr][i] = cells[i];
    }
    return true;
}

/*
  Records what a full repaint just put on `scr`, so the next update for
  that screen can go through renderCells() instead of a full repaint. Call
  this at the end of a full repaint (i.e. an updateDisplayXxx() body), with
  the same `cells`/`sig` a following renderCells() call would use.
*/
void OledMonitorPanel::commitCells(uint8_t scr, const char *cells, uint8_t sig)
{
    if (!_initialised) return;
    if (scr >= SCR_COUNT) return;

    uint8_t len = strlen(cells);
    if (len > sizeof(_shadow[0]) - 1) len = sizeof(_shadow[0]) - 1;
    memcpy(_shadow[scr], cells, len);
    _shadow[scr][len] = '\0';
    _shadowSig[scr] = sig;
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

  Deliberately NOT used for the DSEG7 values. That font is monospaced but
  its ink is not: '1' is 3 px wide and sits at the right of its 29 px cell,
  while '0' is 22 px wide. Measuring the ink would centre "100" 19 px away
  from where it centres "000", so the digits would jump sideways as the
  value changed. Value x stays a per-screen constant, derived once from
  cell width x digit count.
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

    // Nothing besides lightTestOn changes this screen's layout - no managed
    // mode, no label switch - so the signature carries no extra bits.
    uint8_t sig = 0x80;

    if (renderCells(SCR_AUX, strHdgValue5, sig)) return;

    renderLabelValue(TCA9548A_CHANNEL_Aux,
                      "CRS", 13, &FreeSans7pt7b,
                      strHdgValue5, 21, 55, &DSEG7Classic_Regular18pt7b,
                      false, 0, 0);
    commitCells(SCR_AUX, strHdgValue5, sig);
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

    char        displayValue[6];
    char        cells[4];
    const char *labelText;

    if (fcuSpeedMode == 0) {
        labelText = "MACH";
        machValue(cells, displayValue, efisLeftBaroValueHg);
    } else {
        labelText = "SPEED";
        copyValue(displayValue, sizeof(displayValue), efisLeftBaroValueHg);
        stripDot(cells, sizeof(cells), displayValue);
    }

    if (fcuSpeedManagedMode == 1) {
        // Managed dashes use a different font/x (and a dot) - not the cell
        // layout, so this always takes the full repaint and never touches
        // the shadow.
        renderLabelValue(TCA9548A_CHANNEL_EFIS_LEFT,
                          labelText, 13, &FreeSans6pt7b,
                          "---", 26, 55, &DSEG7Classic_Regular16pt7b,
                          true, 104, 40);
        return;
    }

    // bit0: fcuSpeedMode - the label text (MACH vs SPEED) this screen draws.
    // The MACH decimal point is not in sig: it sits on columns 48..51 and the
    // second cell's rectangle starts at 52, so no cell redraw can erase it.
    uint8_t sig = 0x80 | (fcuSpeedMode ? 0x01 : 0x00);

    if (renderCells(SCR_EFIS_LEFT, cells, sig)) return;

    renderLabelValue(TCA9548A_CHANNEL_EFIS_LEFT,
                      labelText, 13, &FreeSans6pt7b,
                      displayValue, 21, 55, &DSEG7Classic_Regular18pt7b,
                      false, 0, 0);
    commitCells(SCR_EFIS_LEFT, cells, sig);
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
    // No station tuned reads as 0 (or "-0") - show dashes, not a distance.
    // Dashes are drawn in the same font at the same x as real digits, so
    // this is still an ordinary cell string - no extra sig bit needed.
    padLeftRanged(strHdgValue3, 3, efisRightBaroValueHpa, 1, 999);

    // Nothing besides lightTestOn changes this screen's layout.
    uint8_t sig = 0x80;

    if (renderCells(SCR_EFIS_RIGHT, strHdgValue3, sig)) return;

    renderLabelValue(TCA9548A_CHANNEL_EFIS_RIGHT,
                      "VOR DME", 13, &FreeSans7pt7b,
                      strHdgValue3, 21, 55, &DSEG7Classic_Regular18pt7b,
                      false, 0, 0);
    commitCells(SCR_EFIS_RIGHT, strHdgValue3, sig);
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

    char        displayValue[6];
    char        cells[4];
    const char *labelText;
    int16_t     labelY;

    if (fcuSpeedMode == 1) {
        labelText = "MACH";
        machValue(cells, displayValue, fcuSpeedValue);
        // Was baseline 20, inherited from the original firmware. At 20 the
        // label inks rows 12..20, and a digit cell's rectangle starts at
        // row 16 - so redrawing the middle cell on its own cut a notch out
        // of "MACH". 13 puts it on rows 5..13, clear of row 16, and lines it
        // up with SPEED. No label baseline on an 18pt screen may pass 15.
        labelY = 13;
    } else {
        labelText = "SPEED";
        labelY = 13;
        copyValue(displayValue, sizeof(displayValue), fcuSpeedValue);
        stripDot(cells, sizeof(cells), displayValue);
    }

    if (fcuSpeedManagedMode == 1) {
        // Managed dashes use a different font/x (and a dot) - not the cell
        // layout, so this always takes the full repaint and never touches
        // the shadow.
        renderLabelValue(TCA9548A_CHANNEL_FCU_SPD,
                          labelText, labelY, &FreeSans6pt7b,
                          "---", 26, 55, &DSEG7Classic_Regular16pt7b,
                          true, 104, 40);
        return;
    }

    // bit0: fcuSpeedMode - the label text (SPEED vs MACH) this screen draws.
    // The MACH decimal point is not in sig: it inks columns 48..51 and the
    // second cell's rectangle starts at 52, so no cell redraw can erase it.
    uint8_t sig = 0x80 | (fcuSpeedMode ? 0x01 : 0x00);

    if (renderCells(SCR_FCU_SPD, cells, sig)) return;

    renderLabelValue(TCA9548A_CHANNEL_FCU_SPD,
                      labelText, labelY, &FreeSans6pt7b,
                      displayValue, 21, 55, &DSEG7Classic_Regular18pt7b,
                      false, 0, 0);
    commitCells(SCR_FCU_SPD, cells, sig);
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
        // Managed dashes use a different font/x (and a dot) - not the cell
        // layout, so this always takes the full repaint and never touches
        // the shadow.
        renderLabelValue(TCA9548A_CHANNEL_FCU_HDG,
                          "HDG", 13, &FreeSans7pt7b,
                          "---", 28, 55, &DSEG7Classic_Regular15pt7b,
                          true, 104, 40);
        return;
    }

    char strHdgValue[4];
    padLeft(strHdgValue, 3, fcuHdgValue);

    // Only the managed branch above changes this screen's layout, and it
    // already returned - nothing else to fold into the signature.
    uint8_t sig = 0x80;

    if (renderCells(SCR_FCU_HDG, strHdgValue, sig)) return;

    renderLabelValue(TCA9548A_CHANNEL_FCU_HDG,
                      "HDG", 13, &FreeSans7pt7b,
                      strHdgValue, 21, 55, &DSEG7Classic_Regular18pt7b,
                      false, 0, 0);
    commitCells(SCR_FCU_HDG, strHdgValue, sig);
}

void OledMonitorPanel::updateDisplayFcuFpa(void)
{
    // Four digits, not five: the range tops out at 2500 ft, so a fifth digit
    // could only ever be a leading zero. Dropping it leaves room for the same
    // 18pt face the other numeric screens use, at x = 6 (ink 9..117).
    char strAltValue2[5];

    if (lightTestOn == 1) {
        // Light test reuses this screen's normal layout (only the digits
        // differ), but every other screen skips the partial path during
        // light test, so do the same here for consistency and leave the
        // shadow invalid.
        copyValue(strAltValue2, sizeof(strAltValue2), "8888");
        renderLabelValue(TCA9548A_CHANNEL_FCU_FPA,
                          "RADIO ALT", 15, &FreeSans7pt7b,
                          strAltValue2, 6, 55, &DSEG7Classic_Regular18pt7b,
                          false, 0, 0);
        return;
    }

    // Radio altimeter reads 0..2500 ft; anything above is out of range and
    // shown as dashes - same font/x as real digits, so still an ordinary
    // cell string. No other branch in this screen, so sig carries no bits.
    padLeftRanged(strAltValue2, 4, efisRightBaroValueHg, 0, 2500);
    uint8_t sig = 0x80;

    if (renderCells(SCR_FCU_FPA, strAltValue2, sig)) return;

    // Baseline 15, not 16: at 16 the label inked row 16, which is the first
    // row of every digit cell rectangle on this screen. See cellGeomTable.
    renderLabelValue(TCA9548A_CHANNEL_FCU_FPA,
                      "RADIO ALT", 15, &FreeSans7pt7b,
                      strAltValue2, 6, 55, &DSEG7Classic_Regular18pt7b,
                      false, 0, 0);
    commitCells(SCR_FCU_FPA, strAltValue2, sig);
}

void OledMonitorPanel::updateDisplayFcuAlt(void)
{
    char strAltValue[6];

    if (lightTestOn == 1) {
        copyValue(strAltValue, sizeof(strAltValue), "88888");
        renderLabelValue(TCA9548A_CHANNEL_FCU_ALT,
                          "ALT", 15, &FreeSans8pt7b,
                          strAltValue, 1, 55, &DSEG7Classic_Regular16pt7b,
                          true, 124, 39);
        return;
    }

    padLeft(strAltValue, 5, fcuAltValue);
    bool drawDot = (fcuAltManagedMode == 1);

    // bit0: the managed dot at (124, 39). Unlike the other screens, managed
    // mode here does not move the digits to a different font/x - it only
    // adds the dot, which sits outside every cell blit rectangle (the last
    // one ends at column 123). renderCells() would never redraw that dot on
    // its own, so its state has to live in sig to force a full repaint when
    // it toggles.
    uint8_t sig = 0x80 | (drawDot ? 0x01 : 0x00);

    if (renderCells(SCR_FCU_ALT, strAltValue, sig)) return;

    renderLabelValue(TCA9548A_CHANNEL_FCU_ALT,
                      "ALT", 15, &FreeSans8pt7b,
                      strAltValue, 1, 55, &DSEG7Classic_Regular16pt7b,
                      drawDot, 124, 39);
    commitCells(SCR_FCU_ALT, strAltValue, sig);
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
