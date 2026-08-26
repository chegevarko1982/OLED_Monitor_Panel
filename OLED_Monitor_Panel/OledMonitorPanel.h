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

class OledMonitorPanel
{
public:
    OledMonitorPanel();
    void begin();
    void attach(uint8_t addrI2C);
    void detach();
    void set(int16_t messageID, char *message);
    void update();

private:
    bool          _initialised;
    uint8_t       _addrI2C;
    uint8_t       _currentChannel;
    uint8_t       _dirty;
    OLEDInterface *oled;

    // Baseline (pixel row) every DSEG7 digit cell on this panel is drawn on.
    // Fixed by the physical layout, not per-call - so it is a constant, not
    // a parameter.
    static const uint8_t DIGIT_BASELINE_Y = 55;

    // Partial-redraw shadow state - what is currently sitting in the
    // framebuffer for each screen, so renderCells() only has to touch the
    // digit cells that actually changed. See renderCells()/commitCells().
    char    _shadow[SCR_COUNT][6];    // digit characters as last drawn, NUL-terminated
    uint8_t _shadowSig[SCR_COUNT];    // layout signature of what is on screen; 0 = unknown

    void setTCAChannel(byte i);
    void blankAllDisplays(void);
    void renderScreen(uint8_t scr);
    void printCentered(const char *text, int16_t y);
    void renderLabelValue(byte channel,
                           const char *labelText, int16_t labelY, const GFXfont *labelFont,
                           const char *valueText, int16_t valueX, int16_t valueY, const GFXfont *valueFont,
                           bool drawDot, int16_t dotX, int16_t dotY);
    void fastDrawDigit(uint8_t cursorX, uint8_t page0, uint8_t pages,
                        const GFXfont *font, char c);
    void clearCell(uint8_t blitX, uint8_t page0, uint8_t pages, uint8_t blitW);
    bool renderCells(uint8_t scr, const char *cells, uint8_t sig);
    void commitCells(uint8_t scr, const char *cells, uint8_t sig);
    void updateDisplayEfisLeft(void);
    void updateDisplayEfisRight(void);
    void updateDisplayFcuSpd(void);
    void updateDisplayFcuHdg(void);
    void updateDisplayFcuFpa(void);
    void updateDisplayFcuAlt(void);
    void updateDisplayFcuVs(void);
    void updateDisplayAux(void);

};
