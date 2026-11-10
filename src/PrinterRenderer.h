#ifndef PRINTER_RENDERER_H
#define PRINTER_RENDERER_H

#include <Arduino.h>
#include <Adafruit_GFX.h>

#include "ThermalPrinterBLE.h"

#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
// Same face as FreeSansBold24pt, digits only, ~4x larger, so the MOD number
// needs little upscaling and keeps smooth edges.
#include "FreeSansBoldDigits.h"

#define PRINT_WIDTH_DOTS  384
#define PRINT_ROW_BYTES   (PRINT_WIDTH_DOTS / 8)
#define PRINT_SIDE_MARGIN 8
#define BLANK_BATCH_ROWS  16

class PrinterRenderer {
private:
    ThermalPrinterBLE* _ble;

    // Raster bytes the printer still expects after the last GS v 0 header. Non-zero
    // only when a print aborted mid-image; over-counting is safe because the NUL
    // padding sent to clear it is ignored outside raster mode.
    uint32_t _rasterBytesOwed;

    const uint8_t ESC_INIT_0 = 0x1B;
    const uint8_t ESC_INIT_1 = 0x40;
    const uint8_t ESC_GAP_0  = 0x1D;
    const uint8_t ESC_GAP_1  = 0x0C;

    static const uint8_t* zeroRows() {
        static const uint8_t zeros[PRINT_ROW_BYTES * BLANK_BATCH_ROWS] = {0};
        return zeros;
    }

    // GS v 0: raster bit image, PRINT_ROW_BYTES wide, lineCount rows tall.
    bool sendRasterHeader(uint16_t lineCount) {
        uint8_t header[] = {0x1D, 0x76, 0x30, 0x00,
                            PRINT_ROW_BYTES, 0x00,
                            (uint8_t)(lineCount & 0xFF), (uint8_t)(lineCount >> 8)};
        // Counted before sending: if the header half-arrives, padding is harmless.
        _rasterBytesOwed = (uint32_t)lineCount * PRINT_ROW_BYTES;
        return _ble->sendCommand(header, sizeof(header));
    }

    bool sendRasterData(const uint8_t* data, size_t size) {
        if (!_ble->sendCommand(data, size)) return false;
        _rasterBytesOwed = (size >= _rasterBytesOwed) ? 0 : _rasterBytesOwed - size;
        return true;
    }

    // Finishes an image left incomplete by an aborted print, so the next print's
    // commands aren't swallowed as pixel data.
    bool resyncPrinter() {
        while (_rasterBytesOwed > 0) {
            Serial.printf("[PRINT] Padding %u raster bytes left by an aborted print.\n", (unsigned)_rasterBytesOwed);
            size_t batch = min((size_t)_rasterBytesOwed, (size_t)(PRINT_ROW_BYTES * BLANK_BATCH_ROWS));
            if (!sendRasterData(zeroRows(), batch)) return false;
        }
        return true;
    }

    bool pushBlankRows(uint16_t lineCount) {
        uint16_t trackingOffset = 0;
        while (trackingOffset < lineCount) {
            uint16_t batchLines = min((uint16_t)(lineCount - trackingOffset), (uint16_t)BLANK_BATCH_ROWS);
            if (!sendRasterHeader(batchLines)) return false;
            if (!sendRasterData(zeroRows(), (size_t)batchLines * PRINT_ROW_BYTES)) return false;
            trackingOffset += batchLines;
            delay(15);
        }
        return true;
    }

    // Thickens every stroke by r dots on each side (square dilation), done on whole
    // bytes so it stays fast. Horizontal pass in place, then vertical from a copy.
    static void emboldenRows(uint8_t* buf, uint16_t rows, uint8_t r) {
        if (r == 0) return;
        if (r > 7) r = 7;
        uint8_t tmp[PRINT_ROW_BYTES];
        for (uint16_t y = 0; y < rows; y++) {
            uint8_t* row = buf + y * PRINT_ROW_BYTES;
            memcpy(tmp, row, PRINT_ROW_BYTES);
            for (uint8_t s = 1; s <= r; s++) {
                for (int i = 0; i < PRINT_ROW_BYTES; i++) {
                    uint8_t towardsRight = (uint8_t)((tmp[i] >> s) | (i > 0 ? tmp[i - 1] << (8 - s) : 0));
                    uint8_t towardsLeft  = (uint8_t)((tmp[i] << s) | (i < PRINT_ROW_BYTES - 1 ? tmp[i + 1] >> (8 - s) : 0));
                    row[i] |= towardsRight | towardsLeft;
                }
            }
        }

        size_t size = (size_t)rows * PRINT_ROW_BYTES;
        uint8_t* copy = (uint8_t*)malloc(size);
        if (copy == nullptr) {
            Serial.println("[PRINT] No memory for vertical embolden; horizontal only.");
            return;
        }
        memcpy(copy, buf, size);
        for (int32_t y = 0; y < rows; y++) {
            uint8_t* row = buf + y * PRINT_ROW_BYTES;
            for (int32_t k = max((int32_t)0, y - r); k <= min((int32_t)rows - 1, y + r); k++) {
                const uint8_t* other = copy + k * PRINT_ROW_BYTES;
                for (int i = 0; i < PRINT_ROW_BYTES; i++) row[i] |= other[i];
            }
        }
        free(copy);
    }

    // Draws text centred in a row at the largest scale up to maxScale that fits.
    // boldDots thickens strokes by that many dots per side; the fit allows for it.
    // GFX's integer setTextSize turns each font pixel into a scale x scale block,
    // which prints as visible stair-steps. Instead the text is drawn once at native
    // size, then resized by a fractional factor with bilinear interpolation
    // thresholded at 50%, which keeps edges about one printer dot fine. At scale 1
    // the font is copied unchanged.
    bool renderSmoothTextRow(const String& text, const GFXfont* fontFace, uint16_t heightLines, float maxScale, int16_t yOffset, uint8_t boldDots = 0) {
        const int16_t pad = 2;

        // Measure at native size; wrap off so the tiny canvas doesn't affect bounds.
        GFXcanvas1 measure(1, 1);
        measure.setFont(fontFace);
        measure.setTextWrap(false);
        int16_t xText, yText;
        uint16_t widthText, heightText;
        measure.getTextBounds(text, 0, 0, &xText, &yText, &widthText, &heightText);
        if (widthText == 0 || heightText == 0) return pushBlankRows(heightLines);

        GFXcanvas1 src(widthText + 2 * pad, heightText + 2 * pad);
        uint8_t* srcBuf = src.getBuffer();
        GFXcanvas1 canvas(PRINT_WIDTH_DOTS, heightLines);
        uint8_t* outBuffer = canvas.getBuffer();
        if (srcBuf == nullptr || outBuffer == nullptr) {
            Serial.println("[PRINT] Out of memory allocating row canvas.");
            return false;
        }
        src.fillScreen(0);
        src.setFont(fontFace);
        src.setTextWrap(false);
        src.setTextColor(1);
        src.setCursor(pad - xText, pad - yText);
        src.print(text);
        canvas.fillScreen(0);

        // Largest scale that fits the row (including the embolden growth), capped at maxScale.
        float scale = maxScale;
        scale = min(scale, (float)(PRINT_WIDTH_DOTS - 2 * PRINT_SIDE_MARGIN - 2 * boldDots) / widthText);
        scale = min(scale, (float)(heightLines - 2 * boldDots) / heightText);

        const int16_t srcW = src.width();
        const int16_t srcH = src.height();
        const int16_t srcStride = (srcW + 7) / 8;
        const int16_t outW = (int16_t)(srcW * scale);
        const int16_t outH = (int16_t)(srcH * scale);
        // Glyph box centred in the row, then shifted down by yOffset, but never so
        // far that the (emboldened) text runs off either edge of the band.
        float inkTop = (heightLines - heightText * scale) / 2 + yOffset;
        float inkTopMax = heightLines - heightText * scale - boldDots;
        inkTop = max((float)boldDots, min(inkTop, inkTopMax));
        const int16_t originX = (int16_t)lroundf((PRINT_WIDTH_DOTS - widthText * scale) / 2 - pad * scale);
        const int16_t originY = (int16_t)lroundf(inkTop - pad * scale);

        auto srcPixel = [&](int32_t x, int32_t y) -> uint32_t {
            if (x < 0 || y < 0 || x >= srcW || y >= srcH) return 0;
            return (srcBuf[y * srcStride + (x >> 3)] >> (7 - (x & 7))) & 1;
        };

        // 16.16 fixed point; the C3 has no FPU.
        const int32_t invScale = (int32_t)(65536.0f / scale);
        const int16_t outStride = PRINT_ROW_BYTES;

        for (int16_t dy = 0; dy < outH; dy++) {
            int16_t py = originY + dy;
            if (py < 0 || py >= (int16_t)heightLines) continue;
            int32_t sy = (((int32_t)(2 * dy + 1) * invScale) >> 1) - 32768;
            int32_t iy = sy >> 16;
            uint32_t fy = (sy >> 8) & 0xFF;

            for (int16_t dx = 0; dx < outW; dx++) {
                int16_t px = originX + dx;
                if (px < 0 || px >= PRINT_WIDTH_DOTS) continue;
                int32_t sx = (((int32_t)(2 * dx + 1) * invScale) >> 1) - 32768;
                int32_t ix = sx >> 16;
                uint32_t fx = (sx >> 8) & 0xFF;

                uint32_t v = srcPixel(ix,     iy)     * (256 - fx) * (256 - fy)
                           + srcPixel(ix + 1, iy)     * fx         * (256 - fy)
                           + srcPixel(ix,     iy + 1) * (256 - fx) * fy
                           + srcPixel(ix + 1, iy + 1) * fx         * fy;
                if (v >= 32768) {
                    outBuffer[py * outStride + (px >> 3)] |= 0x80 >> (px & 7);
                }
            }
        }
        emboldenRows(outBuffer, heightLines, boldDots);

        if (!sendRasterHeader(heightLines)) return false;
        delay(10);
        return sendRasterData(outBuffer, (size_t)heightLines * PRINT_ROW_BYTES);
    }

    bool beginLabel() {
        if (!resyncPrinter()) return false;
        uint8_t initCmd[] = {ESC_INIT_0, ESC_INIT_1};
        if (!_ble->sendCommand(initCmd, sizeof(initCmd))) return false;
        delay(50);
        return true;
    }

    // Feeds to the next label's gap.
    bool endLabel() {
        delay(200);
        uint8_t gapCmd[] = {ESC_GAP_0, ESC_GAP_1};
        return _ble->sendCommand(gapCmd, sizeof(gapCmd));
    }

public:
    PrinterRenderer(ThermalPrinterBLE* bleInstance) : _ble(bleInstance), _rasterBytesOwed(0) {}

    // Max Operating Depth label: only the depth number, as large and heavy as it
    // will go, centred both ways. The 36-dot (4.5mm) blank band at each end keeps
    // the ink clear of the label end, where prints near the edge come out clipped.
    // Returns false if the printer dropped or memory ran out part-way through.
    bool executeModLabelPrint(const String& depthText) {
        const uint16_t endMargin = 36;
        if (!beginLabel()) return false;
        if (!pushBlankRows(endMargin)) return false;
        // No scale cap: the number grows until it fills the width or the band.
        // The digits-only font has no other characters, so the text must be digits.
        if (!renderSmoothTextRow(depthText, &FreeSansBoldDigits, PRINT_WIDTH_DOTS - 2 * endMargin, 20.0f, 0, 3)) return false;
        if (!pushBlankRows(endMargin)) return false;
        return endLabel();
    }

    // Returns false if the printer dropped or memory ran out part-way through.
    bool executeRasterBitmapPrint(const String& r1, const String& r2, const String& r3, const String& r4) {
        if (!beginLabel()) return false;

        if (!pushBlankRows(6)) return false;

        // Rows 1 and 2 have no size cap: each grows until it fills the print width
        // or its band height, then strokes are thickened (last argument, dots per
        // side) for extra weight. Shorter names sit centred in row 1's band. All
        // bands plus blank rows total 384 dots (48mm), the label length.
        // Row 1's band is 132 dots with a 4-dot offset: the text sits where a
        // centred 140-dot band would put it, but rows 2-4 start 1mm higher so row 4
        // isn't clipped at the bottom. The 8 dots are added back as end padding.
        if (!renderSmoothTextRow(r1, &FreeSansBold24pt7b, 132, 10.0f, 4, 2)) return false;
        // Row 2 sits 2mm (16 dots) lower than a plain 80-dot band would put it: the
        // band grows 24 dots (+12 from centring) plus a 4-dot offset. The 24 dots
        // come out of row 3's band, whose offset drops so row 3 doesn't move.
        if (!renderSmoothTextRow(r2, &FreeSansBold24pt7b, 104, 10.0f, 4, 1)) return false;
        // 18pt at native size replaces 9pt x 2: same size, no scaling steps.
        if (!renderSmoothTextRow(r3, &FreeSansBold18pt7b, 54, 1.0f, 11)) return false;
        // No offset: keeps row 4's bottom edge ~4.5mm clear of the label end; closer
        // than ~3.5mm, prints showed shifted/clipped pixels.
        if (!renderSmoothTextRow(r4, &FreeSansBold18pt7b, 78, 1.0f, 0)) return false;

        if (!pushBlankRows(10)) return false;
        return endLabel();
    }
};

#endif
