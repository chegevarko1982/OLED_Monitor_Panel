#pragma once

#include "Arduino.h"
#include "OLEDInterface.h"

// address of the multiplexer to change the channels
#define TCA9548A_I2C_ADDRESS  0x70
#define TCA9548A_CHANNEL_EFIS_LEFT  1 //2
#define TCA9548A_CHANNEL_EFIS_RIGHT 6 //7
#define TCA9548A_CHANNEL_FCU_SPD    0 //1
#define TCA9548A_CHANNEL_FCU_HDG    3 //4
#define TCA9548A_CHANNEL_FCU_FPA    4 //5
#define TCA9548A_CHANNEL_FCU_ALT    5 //6
#define TCA9548A_CHANNEL_FCU_VS     7 //8
#define TCA9548A_CHANNEL_Aux        2 //3

// Screen indices used for the dirty-bit mask in OledMonitorPanel::_dirty.
// These are NOT TCA channel numbers - the TCA9548A_CHANNEL_* macros above
// stay the values passed to setTCAChannel().
enum : uint8_t {
    SCR_EFIS_LEFT,
    SCR_EFIS_RIGHT,
    SCR_FCU_SPD,
    SCR_FCU_HDG,
    SCR_FCU_FPA,
    SCR_FCU_ALT,
    SCR_FCU_VS,
    SCR_AUX,
    SCR_COUNT
};

/*
  Animation opt-in, one bit per SCR_* index - deliberately the same bit
  layout as _dirty, so "every screen" is 0xFF and adding a screen later
  costs nothing here. Built by MFCustomDevice::attach() from the Config
  string in the board settings; 0 (an empty Config string) means the panel
  behaves exactly as it did before animation existed.
*/
#define ANIM_MACH       (1 << SCR_EFIS_LEFT)
#define ANIM_VOR_DME    (1 << SCR_EFIS_RIGHT)
#define ANIM_SPD        (1 << SCR_FCU_SPD)
#define ANIM_HDG        (1 << SCR_FCU_HDG)
#define ANIM_RADIO_ALT  (1 << SCR_FCU_FPA)
#define ANIM_ALT        (1 << SCR_FCU_ALT)
#define ANIM_VS         (1 << SCR_FCU_VS)
#define ANIM_CRS        (1 << SCR_AUX)

// Every screen. Safe as a plain 0xFF: slideCells() refuses any screen whose
// cellGeomTable entry has digits == 0, the same way renderCells() does, so a
// screen that grows or loses a cell layout needs no edit here.
#define ANIM_ALL        0xFF

// Frame count bounds accepted from the Config string, and the default when
// it names screens but no FRAMES=.
#define ANIM_FRAMES_MIN     2
#define ANIM_FRAMES_MAX     8
#define ANIM_FRAMES_DEFAULT 4

class OledMonitorPanel
{
public:
    OledMonitorPanel();
    void begin();
    void attach(uint8_t addrI2C, uint8_t animMask = 0, uint8_t frames = ANIM_FRAMES_DEFAULT);
    void detach();
    void set(int16_t messageID, char *message);
    void update();

private:
    bool          _initialised;
    uint8_t       _addrI2C;
    uint8_t       _currentChannel;
    uint8_t       _dirty;
    uint8_t       _animMask;    // ANIM_* bits; 0 = animation off (default)
    uint8_t       _animFrames;  // frames per slide, ANIM_FRAMES_MIN..MAX
    OLEDInterface *oled;

    // Baseline (pixel row) every DSEG7 digit cell on this panel is drawn on.
    // Fixed by the physical layout, not per-call - so it is a constant, not
    // a parameter.
    static const uint8_t DIGIT_BASELINE_Y = 55;

    // Partial-redraw shadow state - what is currently sitting in the
    // framebuffer for each screen, so renderCells() only has to touch the
    // digit cells that actually changed. See renderCells()/commitCells().
    //
    // The invariant is one-directional and easy to break: _shadow describes
    // what is ON THE PANEL, not what the value happens to be. Any branch that
    // paints a screen some other way (light test, managed dashes, a slide in
    // flight) must either commit the new contents or invalidate _shadowSig -
    // leaving a stale entry alone is what makes a screen quietly stop
    // updating, because the next value compares equal to it and nothing is
    // drawn.
    char    _shadow[SCR_COUNT][6];    // digit characters as last drawn, NUL-terminated
    uint8_t _shadowSig[SCR_COUNT];    // layout signature of what is on screen; 0 = unknown

    // Odometer slide state. Exactly one slide runs at a time - that is what
    // holds the per-frame cost inside the measured two-cell budget without
    // needing a scheduler; a second screen changing mid-slide just snaps.
    // See slideCells() for the constants this is costed against.
    uint8_t  _slideScr;      // SCR_* being animated, 0xFF when idle
    uint8_t  _slideMask;     // one bit per moving cell index
    uint8_t  _slideFrame;    // frames drawn so far, 1.._animFrames
    bool     _slideUp;       // true = digits roll upward (value increased)
    char     _slideTo[6];    // target cell string, same shape as _shadow[]
    uint32_t _lastFrameMs;   // millis() at the last slide frame drawn

    void setTCAChannel(byte i);
    void blankAllDisplays(void);
    void renderScreen(uint8_t scr);
    void printCentered(const char *text, int16_t y);
    void renderLabelValue(byte channel,
                           const char *labelText, int16_t labelY, const GFXfont *labelFont,
                           const char *valueText, int16_t valueX, int16_t valueY, const GFXfont *valueFont,
                           bool drawDot, int16_t dotX, int16_t dotY);
    void fastDrawDigit(uint8_t cursorX, uint8_t page0, uint8_t pages,
                        const GFXfont *font, char c, int16_t yShift = 0);
    void clearCell(uint8_t blitX, uint8_t page0, uint8_t pages, uint8_t blitW);
    bool renderCells(uint8_t scr, const char *cells, uint8_t sig);
    void commitCells(uint8_t scr, const char *cells, uint8_t sig);
    bool slideCells(uint8_t scr, const char *cells, uint8_t sig);
    void slideStep(void);
    void abortSlide(void);
    void updateDisplayEfisLeft(void);
    void updateDisplayEfisRight(void);
    void updateDisplayFcuSpd(void);
    void updateDisplayFcuHdg(void);
    void updateDisplayFcuFpa(void);
    void updateDisplayFcuAlt(void);
    void updateDisplayFcuVs(void);
    void updateDisplayAux(void);

};
