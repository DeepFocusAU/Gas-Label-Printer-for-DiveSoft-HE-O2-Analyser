#include <Arduino.h>
#include <Wire.h>
#include <esp_task_wdt.h>

#include "ThermalPrinterBLE.h"
#include "PrinterRenderer.h"
#include "DisplayUI.h"

#define BTN_PIN  9
// GPIO20/21 are the C3's native UART pins (silkscreen RX/TX) and aren't strapping
// pins. They're free because Serial runs over USB CDC. Avoid GPIO2, 8 and 9
// (strapping), and 5/6 (OLED I2C). The ROM bootloader writes its boot log on
// GPIO21 at reset, so the analyser's RX line may see a burst of text on power-up.
#define RX1_PIN 20
#define TX1_PIN 21

// Set to true to ignore Serial1 and instead feed a fake analyser line for each
// standard gas in turn, changing every DEBUG_GAS_INTERVAL_MS.
const boolean DEBUG_MODE = false;
const uint32_t DEBUG_GAS_INTERVAL_MS = 3000;
const char* const DEBUG_DATETIME = "2026-08-25 06:54:00";

// Set to true to echo every analyser line to the USB serial monitor.
const boolean SERIAL_DIAG = true;
// How often to print a UART receive status line to the monitor.
const uint32_t UART_STATUS_INTERVAL_MS = 15000;
#define UART_MAX_LINE 128
// With no valid analyser line for this long the display shows NO SIG and the last
// reading is dropped, so a stale gas can't be printed. Only a parsed line counts:
// noise on the RX pin produces bytes too, and must not keep a reading alive.
const uint32_t NO_SIGNAL_TIMEOUT_MS = 5000;
// A partial line with no new bytes for this long will never complete, so it's
// discarded rather than being joined onto the start of the next line.
const uint32_t PARTIAL_LINE_TIMEOUT_MS = 500;

// Any byte at all; used for diagnostics only.
uint32_t analyserLastRxMs = 0;
boolean analyserEverReceived = false;
// Last line that parsed cleanly; this is what decides whether there's a signal.
uint32_t analyserLastValidMs = 0;
boolean analyserEverValid = false;

boolean analyserHasSignal() {
    return analyserEverValid && millis() - analyserLastValidMs < NO_SIGNAL_TIMEOUT_MS;
}

struct SystemState {
    String row1Text;
    String row2Text;
    String row3Text;
    String row4Text;
    // Only true while the most recent analyser line parsed cleanly.
    boolean hasValidReading;
};

SystemState state = {"00/00", "MOD --M", "O2: 0.0%; He: 0.0%", "", false};

DisplayUI display;
ThermalPrinterBLE printer("DP230S-Y512423506");
PrinterRenderer renderer(&printer);

struct StandardGasMix {
    float stdO2;
    float stdHe;
    const char* displayName;
    int modDepth;
};

// OXYGEN must stay at OXYGEN_INDEX; lookupGasIndex returns it directly.
const int OXYGEN_INDEX = 0;
const StandardGasMix standardMixes[] = {
    {98.0,  0.0, "OXYGEN",     6},
    {21.0,  0.0, "AIR",       66},
    {32.0,  0.0, "EAN32",     40},
    {36.0,  0.0, "EAN36",     34},
    {38.0,  0.0, "EAN38",     32},
    {40.0,  0.0, "EAN40",     30},
    {50.0,  0.0, "EAN50",     21},
    {60.0,  0.0, "EAN60",     18},
    {80.0,  0.0, "EAN80",     10},
    {10.0, 70.0, "10/70",    150},
    {15.0, 55.0, "15/55",     96},
    {18.0, 45.0, "18/45",     78},
    {21.0, 35.0, "21/35",     66},
    {10.0, 50.0, "10/50",    150}
};
const size_t numStandardMixes = sizeof(standardMixes) / sizeof(standardMixes[0]);

int lookupGasIndex(float measuredO2, float fHe) {
    if (fHe <= 1.5f && measuredO2 >= 94.0f) {
        return OXYGEN_INDEX;
    }

    int bestIndex = -1;
    float minDistance = 9999.0;

    for (size_t i = 0; i < numStandardMixes; i++) {
        float diffO2 = measuredO2 - standardMixes[i].stdO2;
        float diffHe = fHe - standardMixes[i].stdHe;

        if (abs(diffO2) <= 2.0f && abs(diffHe) <= 5.0f) {
            float distance = sqrt((diffO2 * diffO2) + (diffHe * diffHe));
            if (distance < minDistance) {
                minDistance = distance;
                bestIndex = (int)i;
            }
        }
    }
    return bestIndex;
}

int calculateMOD(float fO2Percentage) {
    if (fO2Percentage <= 0.0f) return 0;
    float fractionO2 = fO2Percentage / 100.0f;
    float maxAtmospheresAbsolute = 1.6f / fractionO2;
    float depthMeters = (maxAtmospheresAbsolute - 1.0f) * 10.0f;
    if (depthMeters < 0.0f) depthMeters = 0.0f;
    return (int)(depthMeters + 0.5f);
}

// What the OLED should show when idle: PAIR while no printer is connected, NO SIG
// while the analyser is silent, then the gas name, or dashes if its data won't parse.
String idleScreenText() {
    if (!printer.isConnected()) return String("PAIR");
    if (!analyserHasSignal()) return String("NO SIG");
    return state.hasValidReading ? state.row1Text : String("----");
}

// Returns true only if the line parsed as a valid reading.
boolean parseGasString(String line) {
    line.trim();
    if (line.length() == 0) return false;

    float fHe = 0.0;
    float fO2 = 0.0;
    float fTi = 0.0;

    char dateBuf[32] = {0};
    char timeBuf[32] = {0};

    String processingLine = line;
    int errIndex = processingLine.indexOf("He ***.*");
    if (errIndex != -1) {
        processingLine.replace("***.*", "0.0");
    }

    int parsedFields = sscanf(processingLine.c_str(), "He %f %% O2 %f %% Ti %f ~C %31s %31s", &fHe, &fO2, &fTi, dateBuf, timeBuf);
    if (parsedFields < 5) {
        // Don't leave the previous gas on screen ready to print.
        Serial.print("[PARSE] Rejected line: "); Serial.println(line);
        state.hasValidReading = false;
        display.drawInterface(idleScreenText());
        return false;
    }

    String gasDateTime = String(dateBuf) + " " + String(timeBuf);

    Serial.print("He: "); Serial.print(fHe, 1);
    Serial.print(" %, O2: "); Serial.print(fO2, 1);
    Serial.print(" %, Time: "); Serial.println(gasDateTime);

    int gasIdx = lookupGasIndex(fO2, fHe);
    int finalMOD = 0;

    if (gasIdx != -1) {
        state.row1Text = String(standardMixes[gasIdx].displayName);
        // The measured mix may be richer than the standard it snapped to, so never
        // print a MOD deeper than the measured O2 allows.
        finalMOD = min(standardMixes[gasIdx].modDepth, calculateMOD(fO2));
    } else {
        char fallbackBuffer[32] = {0};
        snprintf(fallbackBuffer, sizeof(fallbackBuffer), "%.0f/%.0f", fO2, fHe);
        state.row1Text = String(fallbackBuffer);
        finalMOD = calculateMOD(fO2);
    }

    Serial.print("[DIAG] Row 1 String Value: "); Serial.println(state.row1Text);

    char row2Buffer[32] = {0};
    snprintf(row2Buffer, sizeof(row2Buffer), "MOD %dM", finalMOD);
    state.row2Text = String(row2Buffer);

    char row3Buffer[64] = {0};
    snprintf(row3Buffer, sizeof(row3Buffer), "O2: %.1f%%; He: %.1f%%", fO2, fHe);
    state.row3Text = String(row3Buffer);
    state.row4Text = gasDateTime;
    state.hasValidReading = true;
    analyserLastValidMs = millis();
    analyserEverValid = true;

    display.drawInterface(idleScreenText());
    return true;
}

// Prints a received line as-is, with control and non-ASCII bytes shown as <0xNN>
// so wiring or baud-rate faults are visible.
void logEscaped(const String& text) {
    for (size_t i = 0; i < text.length(); i++) {
        uint8_t b = (uint8_t)text[i];
        if (b >= 0x20 && b < 0x7F) {
            Serial.print((char)b);
        } else {
            Serial.printf("<0x%02X>", b);
        }
    }
}

void logAnalyserLine(const String& line) {
    if (!SERIAL_DIAG) return;
    Serial.printf("[UART RX] (%u bytes) \"", (unsigned)line.length());
    logEscaped(line);
    Serial.println("\"");
}

void checkSerialHardwareInput() {
    static String asyncBuffer = "";
    // Counts since the last status line.
    static uint32_t bytesSinceStatus = 0;
    static uint32_t linesSinceStatus = 0;
    static uint32_t validSinceStatus = 0;
    static uint32_t junkSinceStatus = 0;
    static String junkSample = "";
    static uint32_t lastStatusTime = 0;

    // Bytes that can't be part of a line; the latest are kept for the status log.
    auto discardBuffer = []() {
        junkSinceStatus += asyncBuffer.length();
        junkSample = asyncBuffer;
        asyncBuffer = "";
    };

    if (DEBUG_MODE) {
        static size_t debugGasIndex = 0;
        static uint32_t lastDebugFeed = 0;
        static boolean debugStarted = false;

        if (debugStarted && millis() - lastDebugFeed < DEBUG_GAS_INTERVAL_MS) return;
        debugStarted = true;
        lastDebugFeed = millis();

        // Built in the analyser's own format so it goes through the real parser.
        const StandardGasMix& mix = standardMixes[debugGasIndex];
        char line[UART_MAX_LINE];
        snprintf(line, sizeof(line), "He %04.1f %% O2 %04.1f %% Ti 22.4 ~C %s",
                 mix.stdHe, mix.stdO2, DEBUG_DATETIME);
        Serial.printf("[DEBUG] Simulated gas %u/%u: %s\n", (unsigned)(debugGasIndex + 1),
                      (unsigned)numStandardMixes, mix.displayName);
        analyserLastRxMs = millis();
        analyserEverReceived = true;
        logAnalyserLine(String(line));
        parseGasString(String(line));

        debugGasIndex = (debugGasIndex + 1) % numStandardMixes;
        return;
    }

    while (Serial1.available() > 0) {
        char c = Serial1.read();
        analyserLastRxMs = millis();
        analyserEverReceived = true;
        bytesSinceStatus++;

        if (c == '\n') {
            logAnalyserLine(asyncBuffer);
            if (parseGasString(asyncBuffer)) validSinceStatus++;
            linesSinceStatus++;
            asyncBuffer = "";
        } else if (c != '\r') {
            // No real line is this long, so what's buffered is noise.
            if (asyncBuffer.length() >= UART_MAX_LINE) discardBuffer();
            asyncBuffer += c;
            // Every analyser line starts "He ", so anything before it is noise.
            size_t len = asyncBuffer.length();
            if (len > 3 && asyncBuffer.endsWith("He ")) {
                asyncBuffer.remove(len - 3);
                discardBuffer();
                asyncBuffer = "He ";
            }
        }
    }

    // Checked only once the UART is drained: a print can block loop() for seconds
    // with the rest of the line still waiting in the receive buffer.
    if (asyncBuffer.length() > 0 && millis() - analyserLastRxMs >= PARTIAL_LINE_TIMEOUT_MS) {
        discardBuffer();
    }

    if (SERIAL_DIAG && millis() - lastStatusTime >= UART_STATUS_INTERVAL_MS) {
        lastStatusTime = millis();
        unsigned intervalS = (unsigned)(UART_STATUS_INTERVAL_MS / 1000);

        if (bytesSinceStatus > 0) {
            Serial.printf("[UART] Status: RECEIVING - %u bytes, %u lines (%u valid) in last %u s.\n",
                          (unsigned)bytesSinceStatus, (unsigned)linesSinceStatus,
                          (unsigned)validSinceStatus, intervalS);
        } else if (analyserEverReceived) {
            Serial.printf("[UART] Status: NO DATA - nothing in last %u s; last byte %lu s ago.\n",
                          intervalS, (unsigned long)((millis() - analyserLastRxMs) / 1000));
        } else {
            Serial.printf("[UART] Status: NO DATA - nothing received since boot (RX pin %d, 115200 baud).\n",
                          RX1_PIN);
        }
        if (bytesSinceStatus > 0 && linesSinceStatus == 0) {
            Serial.println("[UART] Bytes arrived but no newline; check line ending or baud rate.");
        }
        if (junkSinceStatus > 0) {
            Serial.printf("[UART] Discarded %u noise bytes; latest: \"", (unsigned)junkSinceStatus);
            logEscaped(junkSample);
            Serial.println("\"");
        }

        bytesSinceStatus = 0;
        linesSinceStatus = 0;
        validSinceStatus = 0;
        junkSinceStatus = 0;
    }
}

void handlePrintButton() {
    // Re-armed only once the button is released, so holding it never reprints.
    static boolean buttonArmed = true;

    if (digitalRead(BTN_PIN) == HIGH) {
        buttonArmed = true;
        return;
    }
    if (!buttonArmed) return;

    delay(50);
    if (digitalRead(BTN_PIN) != LOW) return;
    buttonArmed = false;

    Serial.println("[EVENT] Normal print action triggered.");

    if (!state.hasValidReading || !analyserHasSignal()) {
        Serial.println("[REJECTED] No valid analyser reading.");
        display.drawInterface("WAIT");
        delay(1200);
        display.drawInterface(idleScreenText());
        return;
    }

    if (!printer.isConnected()) {
        Serial.println("[REJECTED] Device is currently DISCONNECTED.");
        display.drawInterface("ERR");
        delay(1200);
        display.drawInterface(idleScreenText());
        return;
    }

    display.drawInterface("PRNT");
    if (!renderer.executeRasterBitmapPrint(state.row1Text, state.row2Text, state.row3Text, state.row4Text)) {
        Serial.println("[PRINT] Print aborted.");
        display.drawInterface("ERR");
        delay(1200);
    }
    display.drawInterface(idleScreenText());
}

void setup() {
    Serial.begin(115200);
    delay(10);
    Serial.println("\n=== ESP32-C3 GAS LABEL PRINTER ===");

    pinMode(BTN_PIN, INPUT_PULLUP);

    display.init();
    display.drawInterface("BOOT");

    printer.init("ESP32_C3_Printer");
    // Printing blocks loop() for a couple of seconds; keep the scanner moving.
    printer.setIdleHook([]() { display.tick(); });

    // The UART isn't read during a print, so leave room for several analyser lines.
    Serial1.setRxBufferSize(1024);
    Serial1.begin(115200, SERIAL_8N1, RX1_PIN, TX1_PIN);

    display.drawInterface(idleScreenText());

    Serial.println("[SUCCESS] System initialized.");
}

void loop() {
    // CONFIG_ESP_TASK_WDT_EN is IDF 5.x; CONFIG_ESP_TASK_WDT is IDF 4.x.
    #if defined(CONFIG_ESP_TASK_WDT_EN) || defined(CONFIG_ESP_TASK_WDT)
    esp_task_wdt_reset();
    #endif

    printer.keepScanAlive();
    checkSerialHardwareInput();

    // Redraw when the printer link comes up or drops, so a lost printer shows PAIR.
    static boolean wasConnected = false;
    boolean connectedNow = printer.isConnected();
    if (connectedNow != wasConnected) {
        wasConnected = connectedNow;
        display.drawInterface(idleScreenText());
    }

    // Same for the analyser: on losing signal, drop the reading and show NO SIG.
    static boolean hadSignal = false;
    boolean signalNow = analyserHasSignal();
    if (signalNow != hadSignal) {
        hadSignal = signalNow;
        if (!signalNow) {
            Serial.println("[UART] Analyser signal lost; reading cleared.");
            state.hasValidReading = false;
        }
        display.drawInterface(idleScreenText());
    }

    if (printer.checkScanTrigger()) {
        display.drawInterface("PAIR");
        if (printer.connectAndFindWriteChannel()) {
            display.drawInterface(idleScreenText());
        } else {
            display.drawInterface("RETRY");
        }
        printer.clearScanTrigger();
    }

    handlePrintButton();

    // Heartbeat: freezes if loop() stalls, and during the ERR pauses.
    display.tick();

    delay(2);
}
