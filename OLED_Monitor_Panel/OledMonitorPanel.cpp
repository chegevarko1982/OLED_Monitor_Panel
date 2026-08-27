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

  MACH reads x.xx on this panel, but what arrives depends on how the profile
  is configured - with a point ("0.78") or without one ("78", which is what
  oled_monitor_panel.device.json documents for message 2). Taking the point
  from the string is therefore not reliable, so it is drawn from here.

  The digits are read as a decimal, not as a raw count: whatever follows the
  point is the fraction and is padded on the RIGHT to two places, so "0.5" is
  0.50. With no point at all the digits are taken to be the fraction, since
  that is the format device.json describes - "78" is 0.78.

  This also guarantees the three-character cell string renderCells() needs; a
  shorter one fails its length check and silently falls back to a full repaint
  on every update.
*/
static void machValue(char *cells, char *shown, const char *src)
{
    while (*src == ' ' || *src == '+') src++;

    const char *dot    = strchr(src, '.');
    const char *intEnd = dot ? dot : src + strlen(src);

    // The value is read as an ordinary decimal number: "1" is one, not one
    // tenth. The device.json description used to call this a value "without
    // dot", inherited from the original project, and reading a dotless "1"
    // as the fraction is what made Mach 1 show as 0.10.
    const char *intStart = src;
    while (intStart < intEnd && *intStart == '0') intStart++;   // leading zeros

    bool ok = (intEnd - intStart) <= 1;                          // X.YZ has room for one
    for (const char *p = src; ok && p < intEnd; p++)
        if (*p < '0' || *p > '9') ok = false;

    if (!ok) {
        // Two or more integer digits cannot be drawn here at all. Dashes say
        // so, in the same font at the same x as real digits, rather than
        // quietly showing one digit of a number that is not the value.
        cells[0] = cells[1] = cells[2] = '-';
    } else {
        cells[0] = (intStart < intEnd) ? *intStart : '0';

        // Fraction, padded on the right to exactly two places.
        const char *frac = dot ? dot + 1 : "";
        cells[1] = '0';
        cells[2] = '0';
        if (frac[0] >= '0' && frac[0] <= '9') {
            cells[1] = frac[0];
            if (frac[1] >= '0' && frac[1] <= '9') cells[2] = frac[1];
        }
    }
    cells[3] = 0;

    shown[0] = cells[0];
    shown[1] = '.';
    shown[2] = cells[1];
    shown[3] = cells[2];
    shown[4] = 0;
}


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
  Drum tuning, off the bench numbers.

  One cell's frame - clear the rectangle, rasterise the two visible glyphs
  into it, push it once - measured 7.1 ms on this board. drumStep() advances
  exactly one cell per call, so 7.1 ms is also the longest stretch this
  firmware blocks serial for, whatever else is turning. At 115200 baud the
  256-byte receive buffer holds 22 ms, so that leaves a wide margin.

  The 12 ms period follows: 7.1 of 12 ms is 59 % duty, and a one-digit turn
  lands in roughly 8 steps, so about 96 ms - the roll duration the panel was
  tuned to, in steps a quarter the size of the old four-frame slide.

  DRUM_DECEL_SHIFT gives the wheel its feel: each step closes a quarter of
  what is left, so it leaves fast and settles softly, the way a drum on a
  shaft does. DRUM_MIN_STEP stops that tail from crawling - without it the
  last few 1/256ths would take as long as the whole turn. There is no easing
  table and no frame counter: a wheel that is retargeted mid-turn simply has
  further to go, which is why fast input spins it instead of stuttering.
*/
#define ANIM_FRAME_MS      12
#define DRUM_DECEL_SHIFT   2

/*
  Ceiling on how far a wheel may turn in one step, in 1/256ths of a digit.
  Re-aiming a wheel that is already turning makes the remaining distance
  larger, and the proportional step larger with it - which is what makes a
  spun knob spin the drum instead of stuttering. Without a ceiling a big jump
  would move most of a digit in one 12 ms step and read as a teleport rather
  than as motion. 128 is half a digit per step, so a whole revolution takes
  about 20 steps to catch up. It never binds on the ordinary one- or
  two-digit change, whose first step is 64 and 128.
*/
#define DRUM_MAX_STEP      128

// A digit wheel carries 0..9; positions are Q8 in digit units.
#define DRUM_DIGITS 10
#define DRUM_SPAN   (DRUM_DIGITS * 256)

/*
  How many digits a single transition may move and still be turned rather than
  snapped.

  Three, because a carry is exactly what needs it. Counting a 3-digit screen
  one step at a time, the hundreds digit NEVER changes on its own: 099 -> 100
  and 199 -> 200 move all three at once. At two, the leading digit could not
  animate at all - not as a policy but as an accident of arithmetic, which is
  what it looked like on the panel.

  Above three it stops being a step and becomes a jump: 09900 -> 10000 on the
  altitude screen has nothing to roll through and should click over.
*/
#define ANIM_MAX_CELLS 3

/*
  How many digit cells may be turning at once across the whole panel.

  Arithmetic, not taste. One cell step costs 7.1 ms and drumStep() does one per
  frame period, so N cells turning share the period between them: a one-digit
  turn takes ~96 ms alone, ~192 ms with two in flight and ~290 ms with three.

  Three so that a carry - which needs all of ANIM_MAX_CELLS at once - is never
  blocked by the panel-wide budget, since a cap that admits a transition only
  to have the next check reject it would be the same defect twice. Beyond three
  the wheels visibly lag the value they are chasing, so those screens snap
  through renderCells() instead.
*/
#define ANIM_CELLS_IN_FLIGHT 3

/*
  Marker written into _shadow[] for a cell whose content is no longer known -
  an aborted slide left half a glyph there. Chosen because it can never equal
  a digit or the '-' padLeftRanged() produces, so renderCells() is guaranteed
  to clear and redraw exactly that cell.
*/
static const char CELL_UNKNOWN = (char)0xFF;

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
    // SCR_FCU_VS - only the four digits are cells; the sign sits at columns
    // 0..18, outside cell 0's rectangle (which starts at 26), so a cell
    // redraw can never touch it. Its state rides in sig instead, the same way
    // the ALT managed dot does. Measured: 15pt digits ink rows 27..55 inside
    // the page-aligned 24..55, and the "V/S" label ends on row 20.
    { TCA9548A_CHANNEL_FCU_VS,     24, 24, 20, 3, 4, 4, 2 },
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
    case 2:  return &DSEG7Classic_Regular15pt7b;
    default: return &DSEG7Classic_Regular18pt7b;
    }
}

OledMonitorPanel::OledMonitorPanel()
{
    _initialised = false;
    _currentChannel = 0xFF;
    _dirty = 0;
    _animMask = 0;
    _animFrames = ANIM_FRAMES_DEFAULT;
    _drumActive = 0;
    _drumCursor = 0;
    _drumUp     = 0;
    memset(_drumMoving, 0, sizeof(_drumMoving));
    // Nothing has been drawn yet, so no shadow describes any screen. The
    // device pool this object is placement-new'd into happens to be zeroed,
    // but renderCells() correctness should not rest on that.
    memset(_shadowSig, 0, sizeof(_shadowSig));
}

void OledMonitorPanel::attach(uint8_t addrI2C, uint8_t animMask, uint8_t frames)
{
    _addrI2C = addrI2C;
    _currentChannel = 0xFF;
    _animMask   = animMask;
    // Clamped here rather than trusted: the value comes from a free-text
    // Config string, and 0 would divide by zero in drumStep().
    _animFrames = frames < ANIM_FRAMES_MIN ? ANIM_FRAMES_MIN
                : frames > ANIM_FRAMES_MAX ? ANIM_FRAMES_MAX
                                           : frames;
    _drumActive = 0;
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
    _dirty      = 0;
    _drumActive = 0;
    memset(_drumMoving, 0, sizeof(_drumMoving));

    // Every screen above was fully repainted by an updateDisplayXxx() call,
    // not renderCells(), so no shadow reflects what is actually on screen.
    memset(_shadowSig, 0, sizeof(_shadowSig));
}

void OledMonitorPanel::detach()
{
    if (!_initialised)
        return;
    _initialised = false;
    _drumActive  = 0;
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

    // A new value for a screen that is already turning needs no special case
    // here at all: it goes through renderScreen() like any other, and
    // slideCells() moves the wheel's target instead of starting over. That is
    // the difference between a wheel and the fixed-length slide this replaced,
    // which had to abort - and an aborted roll is what a snapped digit is.
    if (_drumActive) {
        // Frame clock, deliberately NOT MF_CUSTOMDEVICE_POLL_MS. That define
        // throttles the whole of update(), so it would also delay ordinary
        // repaints of the unanimated screens by up to a frame period and
        // stretch a Light Test burst of eight screens to eight frame periods
        // of wall clock. Gating only the slide leaves every other path exactly
        // as it was.
        //
        // Clocked from now rather than from _lastFrameMs + ANIM_FRAME_MS: if
        // update() was starved (a full repaint is 55 ms, more than four frame
        // periods) the catch-up form would fire several cells back to back,
        // which is precisely the burst this budget exists to prevent. Losing a
        // little smoothness beats losing the margin.
        if ((uint32_t)(millis() - _lastFrameMs) >= ANIM_FRAME_MS) {
            _lastFrameMs = millis();
            drumStep();
            return; // one cell is the whole budget for this call
        }
    }

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

    // The multiplexer needs to settle before the next transaction reaches the
    // panel behind it. Without this the bus occasionally locks up on the very
    // first switch at boot, and Wire on AVR busy-waits with no timeout, so the
    // board hangs before it ever answers the connector - it shows up in
    // MobiFlight as a nameless "Compatible" module with no serial.
    //
    // The original firmware used delay(5) here. That was removed as
    // copy-paste, which left the margin thin; holding the bus at 400 kHz for
    // our own transactions (see OLEDInterface) then cut it further, and from
    // there whether a given build survived depended on its exact instruction
    // timing. 100 us was measured to be enough, twice the value that already
    // worked, and costs 0.8 ms across all eight screens at boot - against
    // 55 ms for a single full repaint.
    delayMicroseconds(100);
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

    // Stop every wheel too. set() clears _dirty on shutdown, so nothing else
    // would, and the drum would happily keep turning digits onto screens that
    // were just blanked.
    _drumActive = 0;
    memset(_drumMoving, 0, sizeof(_drumMoving));
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

  `yShift` moves the glyph down (positive) or up (negative) from that
  baseline, which is all the slide animation needs. Clipping to the cell is
  free and needs no extra work: the page loop below only ever runs over
  [page0, page0 + pages), and a glyph row that falls outside the shifted band
  simply fails the `gr` range test, so a glyph shifted a whole cell height
  writes nothing at all. Defaulted, so every existing call site is unchanged.
*/
void OledMonitorPanel::fastDrawDigit(uint8_t cursorX, uint8_t page0, uint8_t pages,
                                      const GFXfont *font, char c, int16_t yShift)
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

    int16_t topRow = (int16_t)DIGIT_BASELINE_Y + yo - (int16_t)page0 * 8 + yShift; // glyph top, relative to page0

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
  screen has no cell geometry at all.

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

    if (geom.digits == 0) return false;        // this screen has no partial path
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

/*
  Advances one digit wheel by one step and redraws it - the whole budget for
  this call, and the reason several cells can turn at once.

  Motion is a proportional approach: each step closes DRUM_DECEL_SHIFT worth of
  the remaining distance, with a floor so the tail cannot crawl. That gives a
  quick departure and a soft landing without an easing table, and - the part
  that matters - it has no notion of "frame 3 of 8", so a wheel whose target
  moves mid-turn just has further to travel. Nothing is ever restarted.

  Wheels are taken in turn from _drumCursor, so a screen the sim drives hard
  cannot starve the others.

  Rendering is two glyphs straight into the framebuffer, no canvas: the digit
  the wheel is leaving and the one after it, one cell height apart. Which pair
  that is falls out of the position, so it works the same turning up or down.
  Clipping is free - fastDrawDigit() only ever writes pages [page0, page0 +
  pages), so the parts that have rotated out of the cell cost nothing to hide.
*/
void OledMonitorPanel::drumStep(void)
{
    if (!_drumActive) return;

    // Next screen with a wheel still turning, starting after the last served.
    uint8_t scr = SCR_COUNT;
    for (uint8_t n = 0; n < SCR_COUNT; n++) {
        uint8_t i = (uint8_t)((_drumCursor + 1 + n) % SCR_COUNT);
        if (_drumMoving[i]) { scr = i; break; }
    }
    if (scr == SCR_COUNT) { _drumActive = 0; return; }
    _drumCursor = scr;

    uint8_t cell = __builtin_ctz(_drumMoving[scr]);

    CellGeom geom;
    memcpy_P(&geom, &cellGeomTable[scr], sizeof(CellGeom));
    const GFXfont *font = fontForIndex(geom.fontIdx);

    bool     up     = (_drumUp & ((uint8_t)1 << scr)) != 0;
    uint16_t pos    = _drumPos[scr][cell];
    uint16_t target = (uint16_t)_drumTarget[scr][cell] * 256;

    // Distance still to travel in the direction of turn, wrapping through 9-0.
    uint16_t dist = up ? (uint16_t)((target - pos + DRUM_SPAN) % DRUM_SPAN)
                       : (uint16_t)((pos - target + DRUM_SPAN) % DRUM_SPAN);

    // _animFrames keeps its documented meaning - more frames, slower wheel -
    // by setting the floor rather than a frame count. 8 gives 24, 2 gives 96.
    uint8_t  minStep = (uint8_t)(192 / _animFrames);
    uint16_t step    = dist >> DRUM_DECEL_SHIFT;
    if (step < minStep)      step = minStep;
    if (step > DRUM_MAX_STEP) step = DRUM_MAX_STEP;

    bool settling = (dist == 0 || step >= dist);
    if (settling) {
        pos = target;
    } else {
        pos = up ? (uint16_t)((pos + step) % DRUM_SPAN)
                 : (uint16_t)((pos + DRUM_SPAN - step) % DRUM_SPAN);
    }
    _drumPos[scr][cell] = pos;

    uint8_t h    = geom.pages * 8;
    uint8_t d0   = (uint8_t)(pos >> 8);              // digit the wheel is leaving
    uint8_t frac = (uint8_t)(pos & 0x00FF);
    uint8_t off  = (uint8_t)(((uint16_t)frac * h) >> 8);

    uint8_t cursorX = geom.x + cell * geom.advance;
    uint8_t blitX   = cursorX + 2;

    setTCAChannel(geom.channel);
    clearCell(blitX, geom.page0, geom.pages, geom.blitW);
    fastDrawDigit(cursorX, geom.page0, geom.pages, font, (char)('0' + d0), -(int16_t)off);
    // At frac == 0 the follower sits a whole cell height away and would write
    // nothing, so skip its ~1.2 ms rather than rasterise it into the clip test.
    if (frac)
        fastDrawDigit(cursorX, geom.page0, geom.pages, font,
                      (char)('0' + (d0 + 1) % DRUM_DIGITS), (int16_t)(h - off));
    oled->displayRegion(blitX, geom.page0, geom.blitW, geom.pages);

    if (settling) {
        // The cell now holds exactly one digit again, so the shadow describes
        // the panel and the next value for this screen can take the ordinary
        // partial path.
        _shadow[scr][cell] = (char)('0' + _drumTarget[scr][cell]);
        _drumMoving[scr] &= ~((uint8_t)1 << cell);
        if (_drumMoving[scr] == 0) _drumActive &= ~((uint8_t)1 << scr);
    }
}

/*
  Stops every wheel on a screen without letting it arrive, leaving the shadow
  honest about what that did to the panel. Only the refusal paths in
  slideCells() use this - an ordinary new value retargets instead.
*/
void OledMonitorPanel::abortDrum(uint8_t scr)
{
    if (scr >= SCR_COUNT) return;
    if (!(_drumActive & ((uint8_t)1 << scr))) return;

    // Mid-turn those cells hold halves of two glyphs, which no single
    // character describes - so poison them rather than pretend they still hold
    // the old digit. The alternative, dropping _shadowSig, would force a 55 ms
    // full repaint; this costs one ordinary cell redraw each, because
    // CELL_UNKNOWN can never compare equal to the incoming character.
    for (uint8_t i = 0; i < MAX_DIGIT_CELLS; i++) {
        if (_drumMoving[scr] & ((uint8_t)1 << i)) _shadow[scr][i] = CELL_UNKNOWN;
    }
    _drumMoving[scr] = 0;
    _drumActive &= ~((uint8_t)1 << scr);
}

/*
  Points this screen's digit wheels at `cells`, starting them if they are at
  rest and simply re-aiming them if they are already turning. Returns true when
  it took the screen over - the caller must then do nothing else this update.
  Returns false for everything else, which drops the caller back onto the
  ordinary renderCells() or full-repaint path with nothing left half-done.

  Call it immediately before renderCells(), with the same cells/sig.
*/
bool OledMonitorPanel::slideCells(uint8_t scr, const char *cells, uint8_t sig)
{
    if (!(_animMask & (uint8_t)(1 << scr))) return false; // opt-in, and off by default
    if (!_initialised || scr >= SCR_COUNT) return false;

    CellGeom geom;
    memcpy_P(&geom, &cellGeomTable[scr], sizeof(CellGeom));

    bool refuse = false;

    // Everything renderCells() would refuse on has to be refused here too: a
    // wheel starts from _shadow, so it is only meaningful when the shadow is
    // known to describe the panel.
    if (lightTestOn == 1) refuse = true;
    else if (geom.digits == 0 || geom.digits > MAX_DIGIT_CELLS) refuse = true;
    else if (sig == 0 || _shadowSig[scr] != sig) refuse = true;
    else if (strlen(cells) != geom.digits) refuse = true;

    if (!refuse) {
        for (uint8_t i = 0; i < geom.digits; i++) {
            // The dash fields padLeftRanged() produces (RADIO ALT above 2500
            // ft, VOR DME with no station tuned) are not positions on a wheel,
            // so there is nothing to turn to.
            if (cells[i] < '0' || cells[i] > '9') { refuse = true; break; }
            // A cell at rest has to start from a digit; one already turning
            // carries its own position and needs no shadow.
            if (!(_drumMoving[scr] & ((uint8_t)1 << i))
                && (_shadow[scr][i] < '0' || _shadow[scr][i] > '9')) { refuse = true; break; }
        }
    }

    if (refuse) {
        // Hand the screen back intact: a wheel left turning would fight the
        // repaint that is about to happen.
        abortDrum(scr);
        return false;
    }

    // Where the screen is logically headed right now - the target for a cell
    // that is turning, the settled digit for one that is not. Direction is
    // taken from this rather than from _shadow alone, so re-aiming a wheel
    // mid-turn follows the new change and not the one it started on.
    char cur[MAX_DIGIT_CELLS + 1];
    for (uint8_t i = 0; i < geom.digits; i++) {
        cur[i] = (_drumMoving[scr] & ((uint8_t)1 << i))
                     ? (char)('0' + _drumTarget[scr][i])
                     : _shadow[scr][i];
    }
    cur[geom.digits] = 0x00;

    int8_t cmp = (int8_t)memcmp(cells, cur, geom.digits);
    if (cmp == 0) return false; // nothing to aim at - let renderCells() no-op

    // Cells that would have to start from rest, and the panel-wide budget they
    // have to fit into. Cells already turning cost nothing extra: re-aiming a
    // wheel is free, it is the turning itself that spends the frame period.
    uint8_t starting = 0, alreadyTurning = 0;
    for (uint8_t i = 0; i < geom.digits; i++) {
        if (_drumMoving[scr] & ((uint8_t)1 << i)) alreadyTurning++;
        else if (cells[i] != _shadow[scr][i]) starting++;
    }

    if (starting) {
        // A step this big clicks over instead of crawling.
        if (starting + alreadyTurning > ANIM_MAX_CELLS) { abortDrum(scr); return false; }

        uint8_t inFlight = 0;
        for (uint8_t i = 0; i < SCR_COUNT; i++) {
            uint8_t m = _drumMoving[i];
            while (m) { inFlight++; m &= (uint8_t)(m - 1); }
        }
        if (inFlight + starting > ANIM_CELLS_IN_FLIGHT) { abortDrum(scr); return false; }
    }

    // Direction is taken from the value as a whole, never per cell: across a
    // carry (350 -> 349) the units rise while the tens fall, and turning the
    // two opposite ways at once looks like a fault rather than a counter. Both
    // strings are the same length and all digits by now, so the byte compare
    // IS the numeric compare - no strtol, no overflow to think about.
    if (cmp > 0) _drumUp |= ((uint8_t)1 << scr);
    else         _drumUp &= ~((uint8_t)1 << scr);

    for (uint8_t i = 0; i < geom.digits; i++) {
        uint8_t to = (uint8_t)(cells[i] - '0');
        if (_drumMoving[scr] & ((uint8_t)1 << i)) {
            _drumTarget[scr][i] = to; // already turning - just re-aim it
        } else if (cells[i] != _shadow[scr][i]) {
            _drumPos[scr][i]    = (uint16_t)(_shadow[scr][i] - '0') * 256;
            _drumTarget[scr][i] = to;
            _drumMoving[scr] |= ((uint8_t)1 << i);
        }
    }
    _drumActive |= ((uint8_t)1 << scr);

    // First step now, not up to a frame period from now: the value has already
    // changed and the screen should start moving on the same update() an
    // unanimated screen would have snapped on.
    _lastFrameMs = millis();
    drumStep();
    return true;
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
        // The panel now shows the test pattern, which no shadow describes.
        // Leaving the old entry in place is not neutral: when light test goes
        // off again the value is usually unchanged, so every cell would
        // compare equal to the shadow, renderCells() would draw nothing and
        // report success, and the 888s would stay on the panel until the
        // value next moved. Every branch that paints a screen without a
        // matching commitCells() does this - see the _shadowSig comment in
        // the header.
        _shadowSig[SCR_AUX] = 0;
        return;
    }

    char strHdgValue5[4];
    padLeft(strHdgValue5, 3, CRSValue);

    // Nothing besides lightTestOn changes this screen's layout - no managed
    // mode, no label switch - so the signature carries no extra bits.
    uint8_t sig = 0x80;

    if (slideCells(SCR_AUX, strHdgValue5, sig)) return;
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
        _shadowSig[SCR_EFIS_LEFT] = 0; // test pattern on screen - see updateDisplayAux()
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
        // layout, so this always takes the full repaint.
        _shadowSig[SCR_EFIS_LEFT] = 0; // dashes on screen - see updateDisplayAux()
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

    if (slideCells(SCR_EFIS_LEFT, cells, sig)) return;
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
        _shadowSig[SCR_EFIS_RIGHT] = 0; // test pattern on screen - see updateDisplayAux()
        return;
    }

    char strHdgValue3[4];
    // No station tuned reads as 0 (or "-0") - show dashes, not a distance.
    // Dashes are drawn in the same font at the same x as real digits, so
    // this is still an ordinary cell string - no extra sig bit needed.
    padLeftRanged(strHdgValue3, 3, efisRightBaroValueHpa, 1, 999);

    // Nothing besides lightTestOn changes this screen's layout.
    uint8_t sig = 0x80;

    // The sim drives this one, not the pilot's hand, so it is one of the two
    // screens the odometer slide is offered on. Off unless the Config string
    // asked for it - see slideCells().
    if (slideCells(SCR_EFIS_RIGHT, strHdgValue3, sig)) return;
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
        _shadowSig[SCR_FCU_SPD] = 0; // test pattern on screen - see updateDisplayAux()
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
        // layout, so this always takes the full repaint.
        _shadowSig[SCR_FCU_SPD] = 0; // dashes on screen - see updateDisplayAux()
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

    if (slideCells(SCR_FCU_SPD, cells, sig)) return;
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
        _shadowSig[SCR_FCU_HDG] = 0; // test pattern on screen - see updateDisplayAux()
        return;
    }

    if (fcuHdgManagedMode == 1) {
        // Managed dashes use a different font/x (and a dot) - not the cell
        // layout, so this always takes the full repaint.
        _shadowSig[SCR_FCU_HDG] = 0; // dashes on screen - see updateDisplayAux()
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

    if (slideCells(SCR_FCU_HDG, strHdgValue, sig)) return;
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
        // light test, so do the same here for consistency.
        copyValue(strAltValue2, sizeof(strAltValue2), "8888");
        _shadowSig[SCR_FCU_FPA] = 0; // test pattern on screen - see updateDisplayAux()
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

    // Counting down on approach is the case the odometer slide was written
    // for. Off unless the Config string asked for it - see slideCells().
    if (slideCells(SCR_FCU_FPA, strAltValue2, sig)) return;
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
        _shadowSig[SCR_FCU_ALT] = 0; // test pattern on screen - see updateDisplayAux()
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

    if (slideCells(SCR_FCU_ALT, strAltValue, sig)) return;
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

    /*
      In V/S mode with a selected value this screen is four digit cells at a
      fixed pitch, so it takes the same partial path as every other screen.
      FPA mode, managed dashes and light test each lay it out differently and
      keep the full repaint - vsCells says which of the two this call is.

      The digits are drawn at x = 24 under both signs. That is not a change:
      the negative branch used to print "-" at x = 0 and let the cursor carry
      on, and the 15pt '-' has an xAdvance of exactly 24. Setting the cursor
      explicitly makes the two branches share one geometry deliberately
      instead of by coincidence.
    */
    bool    vsCells = (lightTestOn != 1 && fcuVsManagedMode != 1 && fcuTrkMode == 0);
    bool    neg     = false;
    uint8_t sig     = 0;
    char    cells[5];

    if (vsCells) {
        // A lone "-" is not a value; treat it as the zero the old code did.
        neg = (fcuVsValue[0] == '-' && fcuVsValue[1] != 0x00);
        padLeft(cells, 4, neg ? fcuVsValue + 1 : fcuVsValue);

        // bit0: the sign. It lies outside every cell rectangle, so a change of
        // sign has to force the full repaint from here.
        sig = 0x80 | (neg ? 0x01 : 0x00);

        if (slideCells(SCR_FCU_VS, cells, sig)) return;
        if (renderCells(SCR_FCU_VS, cells, sig)) return;
    }

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
                if (neg) {
                    oled->setFont(&DSEG7Classic_Regular15pt7b);
                    oled->setCursor(0, 55);
                    oled->print("-");
                } else {
                    oled->setFont(&FreeSans18pt7b);
                    oled->setCursor(0, 50);
                    oled->print("+");
                }
                oled->setFont(&DSEG7Classic_Regular15pt7b);
                oled->setCursor(24, 55);
                oled->print(cells);
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

    if (vsCells)
        commitCells(SCR_FCU_VS, cells, sig);
    else
        _shadowSig[SCR_FCU_VS] = 0; // light test / managed / FPA - see updateDisplayAux()
}
