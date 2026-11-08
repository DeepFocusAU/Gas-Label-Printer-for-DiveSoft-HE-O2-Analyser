#ifndef DISPLAY_UI_H
#define DISPLAY_UI_H

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

#define DISPLAY_WIDTH  72
#define DISPLAY_HEIGHT 40

// Knight Rider scanner on the bottom pixel row. It's driven from tick(), so it
// stops moving whenever the main loop is blocked or hung.
#define SCANNER_Y        (DISPLAY_HEIGHT - 1)
#define SCANNER_LENGTH   10
#define SCANNER_TRAIL    8      // Dotted fade behind the moving segment.
#define SCANNER_SWEEP_MS 20000  // One pass from edge to edge.

class DisplayUI {
private:
    U8G2_SSD1306_72X40_ER_F_HW_I2C _u8g2;
    String _lastMainText;
    int16_t _lastScannerPos;

    // Triangle wave from the clock, so speed stays constant however often tick() runs.
    // The bar and its tail run fully off-screen at each end before turning back.
    int16_t scannerPosition(bool* movingRight) const {
        const int32_t start = -(SCANNER_LENGTH + SCANNER_TRAIL);
        const int32_t travel = DISPLAY_WIDTH + SCANNER_TRAIL - start;
        uint32_t phase = millis() % (2 * SCANNER_SWEEP_MS);
        *movingRight = phase < SCANNER_SWEEP_MS;
        uint32_t t = *movingRight ? phase : (2 * SCANNER_SWEEP_MS - phase);
        return (int16_t)(start + (int32_t)(t * travel / SCANNER_SWEEP_MS));
    }

    void drawScanner() {
        bool movingRight;
        int16_t pos = scannerPosition(&movingRight);
        _lastScannerPos = pos;

        _u8g2.setDrawColor(0);
        _u8g2.drawHLine(0, SCANNER_Y, DISPLAY_WIDTH);
        _u8g2.setDrawColor(1);
        // Clip by hand: u8g2 coordinates are unsigned, so a negative x would wrap.
        int16_t x0 = max(pos, (int16_t)0);
        int16_t x1 = min((int16_t)(pos + SCANNER_LENGTH), (int16_t)DISPLAY_WIDTH);
        if (x1 > x0) _u8g2.drawHLine(x0, SCANNER_Y, x1 - x0);

        // Every other pixel behind the segment, to suggest a fading tail.
        for (int16_t i = 2; i <= SCANNER_TRAIL; i += 2) {
            int16_t x = movingRight ? pos - i : pos + SCANNER_LENGTH - 1 + i;
            if (x >= 0 && x < DISPLAY_WIDTH) _u8g2.drawPixel(x, SCANNER_Y);
        }
    }

public:
    DisplayUI() : _u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE), _lastScannerPos(-1) {
        _lastMainText = "";
    }

    void init() {
        Wire.begin(5, 6);
        _u8g2.begin();
        _u8g2.setContrast(0x2F);
        _u8g2.clearBuffer();
        _u8g2.sendBuffer();
    }

    // Call often. Sends only the bottom 8-pixel tile row, and only when the bar moved.
    void tick() {
        bool movingRight;
        if (scannerPosition(&movingRight) == _lastScannerPos) return;
        drawScanner();
        _u8g2.updateDisplayArea(0, SCANNER_Y / 8, DISPLAY_WIDTH / 8, 1);
    }

    void drawInterface(const String& mainText) {
        if (mainText == _lastMainText) {
            return;
        }
        _lastMainText = mainText;

        _u8g2.clearBuffer();

        // Step down to a narrower font when the text won't fit the 72px panel.
        static const uint8_t* fonts[] = {u8g2_font_logisoso18_tf, u8g2_font_logisoso16_tf, u8g2_font_helvB10_tf};
        int16_t textWidth = 0;
        for (const uint8_t* font : fonts) {
            _u8g2.setFont(font);
            textWidth = _u8g2.getStrWidth(mainText.c_str());
            if (textWidth <= 72) break;
        }
        int16_t xStart = (72 - textWidth) / 2;
        if (xStart < 0) xStart = 0;

        // Centre the cap height in the area above the scanner, leaving a 1px gap.
        // Labels are capitals and digits, so the font's ascent (height of 'A') is
        // the visible height.
        const int16_t textAreaHeight = SCANNER_Y - 1;
        int16_t yStart = (textAreaHeight + _u8g2.getAscent()) / 2;

        _u8g2.drawStr(xStart, yStart, mainText.c_str());
        drawScanner();
        _u8g2.sendBuffer();
    }
};

#endif
