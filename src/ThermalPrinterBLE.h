#ifndef THERMAL_PRINTER_BLE_H
#define THERMAL_PRINTER_BLE_H

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <atomic>

// Upper bound on bytes per BLE write. The printer has no flow control, so larger
// chunks may overrun its buffer; drop this back to 20 if prints come out garbled.
#define BLE_MAX_CHUNK 128
#define BLE_REQUESTED_MTU 247
// Average send rate. Writes go out without response, so nothing stops data
// arriving faster than the head prints; dense black rows print slowest. If the
// buffer overflows, bytes are lost and the printer hangs mid-image waiting for
// them. Lower this if prints still stall.
#define BLE_MAX_BYTES_PER_SEC 4000
#define BLE_MIN_CHUNK_GAP_MS  12

// Transmit power. The printer sits centimetres away in the cradle, so the
// default +9 dBm is wasted. Raise it (ESP_PWR_LVL_N6, _N0, _P3) if the link is
// unreliable with the printer further away.
#define BLE_TX_POWER ESP_PWR_LVL_N0

// A connect attempt blocks loop(); NimBLE's default is 30 s.
#define BLE_CONNECT_TIMEOUT_S 5

// Scanning while disconnected: back-to-back scans for the first two minutes
// after the link drops (or a button press), so the printer is picked up as soon
// as it advertises again, then a short scan every few seconds to save power.
#define BLE_SCAN_SECONDS         1
#define BLE_SCAN_PERIOD_FAST_MS  0
#define BLE_SCAN_PERIOD_SLOW_MS  5000
#define BLE_SCAN_FAST_FOR_MS     120000
// Once the printer's address is known, scans are passive (no scan requests sent)
// and match on the address. Every Nth scan is active, in case the name only
// appears in the scan response and the address has changed.
#define BLE_ACTIVE_SCAN_EVERY 4

class ThermalPrinterBLE : public NimBLEAdvertisedDeviceCallbacks, public NimBLEClientCallbacks {
private:
    String _targetName;
    // Written from the NimBLE host task, read from loop().
    std::atomic<bool> _doConnect;
    std::atomic<bool> _connected;
    std::atomic<uint32_t> _disconnectedSince;
    NimBLEClient* _pClient;
    NimBLERemoteCharacteristic* _pRemoteCharacteristic;
    NimBLEAddress _targetAddress;
    // Address saved in flash from the last successful connection, if any.
    bool _haveSavedAddress;
    NimBLEAddress _savedAddress;
    unsigned long _lastScanTime;
    uint32_t _scanCount;
    bool _scanNow;
    size_t _chunkSize;
    // Called between write chunks so the UI keeps animating during a long print.
    void (*_idleHook)();

    // Services that never carry printer data.
    static bool isStandardService(NimBLEUUID uuid) {
        return uuid == NimBLEUUID((uint16_t)0x1800) ||   // Generic Access
               uuid == NimBLEUUID((uint16_t)0x1801) ||   // Generic Attribute
               uuid == NimBLEUUID((uint16_t)0x180A);     // Device Information
    }

    // Data channels used by common BLE thermal printers, tried before falling back
    // to the first writable characteristic on a non-standard service.
    NimBLERemoteCharacteristic* findKnownPrinterChannel() {
        static const char* knownPairs[][2] = {
            {"49535343-fe7d-4ae5-8fa9-9fafd205e455", "49535343-8841-43f4-a8d4-ecbe34729bb3"},
            {"000018f0-0000-1000-8000-00805f9b34fb", "00002af1-0000-1000-8000-00805f9b34fb"},
            {"0000ff00-0000-1000-8000-00805f9b34fb", "0000ff02-0000-1000-8000-00805f9b34fb"},
        };
        for (auto& pair : knownPairs) {
            NimBLERemoteService* pService = _pClient->getService(NimBLEUUID(pair[0]));
            if (pService == nullptr) continue;
            NimBLERemoteCharacteristic* pChar = pService->getCharacteristic(NimBLEUUID(pair[1]));
            if (pChar != nullptr && (pChar->canWrite() || pChar->canWriteNoResponse())) {
                return pChar;
            }
        }
        return nullptr;
    }

    NimBLERemoteCharacteristic* findFallbackChannel() {
        std::vector<NimBLERemoteService*>* services = _pClient->getServices(true);
        if (services == nullptr) return nullptr;

        for (auto* pService : *services) {
            if (isStandardService(pService->getUUID())) continue;
            std::vector<NimBLERemoteCharacteristic*>* characteristics = pService->getCharacteristics(true);
            if (characteristics == nullptr) continue;

            for (auto* pChar : *characteristics) {
                if (pChar->canWrite() || pChar->canWriteNoResponse()) {
                    return pChar;
                }
            }
        }
        return nullptr;
    }

    void releaseClient() {
        _pRemoteCharacteristic = nullptr;
        if (_pClient != nullptr) {
            if (_pClient->isConnected()) _pClient->disconnect();
            NimBLEDevice::deleteClient(_pClient);
            _pClient = nullptr;
        }
    }

    void loadSavedAddress() {
        Preferences prefs;
        if (!prefs.begin("printer", true)) return;
        String name = prefs.getString("name", "");
        String addr = prefs.getString("addr", "");
        uint8_t type = prefs.getUChar("type", BLE_ADDR_PUBLIC);
        prefs.end();
        // Ignore an address saved for a different printer name.
        if (addr.length() == 0 || name != _targetName) return;
        _savedAddress = NimBLEAddress(std::string(addr.c_str()), type);
        _haveSavedAddress = true;
        Serial.printf("[BLE] Saved printer address: %s\n", addr.c_str());
    }

    void saveAddress(const NimBLEAddress& address) {
        if (_haveSavedAddress && _savedAddress == address) return;
        Preferences prefs;
        if (!prefs.begin("printer", false)) return;
        prefs.putString("name", _targetName);
        prefs.putString("addr", address.toString().c_str());
        prefs.putUChar("type", address.getType());
        prefs.end();
        _savedAddress = address;
        _haveSavedAddress = true;
        Serial.printf("[BLE] Saved printer address %s\n", address.toString().c_str());
    }

    void startScan() {
        _scanNow = false;
        NimBLEScan* pBLEScan = NimBLEDevice::getScan();
        bool active = !_haveSavedAddress || (_scanCount % BLE_ACTIVE_SCAN_EVERY) == BLE_ACTIVE_SCAN_EVERY - 1;
        _scanCount++;
        pBLEScan->setActiveScan(active);
        pBLEScan->start(BLE_SCAN_SECONDS, nullptr, false);
        _lastScanTime = millis();
    }

public:
    ThermalPrinterBLE(const char* targetName) :
        _targetName(targetName), _doConnect(false), _connected(false), _disconnectedSince(0),
        _pClient(nullptr), _pRemoteCharacteristic(nullptr), _targetAddress(""),
        _haveSavedAddress(false), _savedAddress(""),
        _lastScanTime(0), _scanCount(0), _scanNow(false), _chunkSize(20), _idleHook(nullptr) {}

    void setIdleHook(void (*hook)()) { _idleHook = hook; }

    void init(const char* localBLEName) {
        NimBLEDevice::init(localBLEName);
        NimBLEDevice::setPower(BLE_TX_POWER);
        NimBLEDevice::setMTU(BLE_REQUESTED_MTU);
        NimBLEScan* pBLEScan = NimBLEDevice::getScan();
        pBLEScan->setAdvertisedDeviceCallbacks(this);
        pBLEScan->setInterval(100);
        pBLEScan->setWindow(50);

        _disconnectedSince = millis();
        loadSavedAddress();
        if (_haveSavedAddress) {
            // Try the known printer straight away, without scanning first. If it's
            // off, the connect times out and normal scanning takes over.
            _targetAddress = _savedAddress;
            _doConnect = true;
        } else {
            startScan();
        }
    }

    void keepScanAlive() {
        if (_connected || _doConnect) return;
        if (NimBLEDevice::getScan()->isScanning()) return;
        unsigned long now = millis();
        uint32_t period = (now - _disconnectedSince < BLE_SCAN_FAST_FOR_MS)
                              ? BLE_SCAN_PERIOD_FAST_MS : BLE_SCAN_PERIOD_SLOW_MS;
        if (_scanNow || now - _lastScanTime >= period) startScan();
    }

    // Scan on the next loop() and go back to fast scanning, e.g. after a button
    // press while the printer isn't connected.
    void requestScanNow() {
        if (_connected || _doConnect) return;
        _scanNow = true;
        _disconnectedSince = millis();
    }

    bool checkScanTrigger() { return _doConnect; }
    void clearScanTrigger() { _doConnect = false; }
    bool isConnected() const {
        return _connected && _pClient != nullptr && _pClient->isConnected();
    }

    bool connectAndFindWriteChannel() {
        // A client left over from a dropped connection can't be deleted inside the
        // disconnect callback, so it is cleaned up here.
        releaseClient();
        // onResult stops the scan from the BLE task; connecting before it has
        // fully stopped can fail straight away.
        NimBLEScan* pBLEScan = NimBLEDevice::getScan();
        if (pBLEScan->isScanning()) pBLEScan->stop();
        for (int i = 0; i < 50 && pBLEScan->isScanning(); i++) delay(10);

        _pClient = NimBLEDevice::createClient();
        _pClient->setClientCallbacks(this, false);
        _pClient->setConnectTimeout(BLE_CONNECT_TIMEOUT_S);

        Serial.printf("[BLE] Handshaking directly with target address: %s\n", _targetAddress.toString().c_str());
        uint32_t started = millis();
        if (!_pClient->connect(_targetAddress)) {
            Serial.printf("[BLE] Connect failed after %lu ms.\n", (unsigned long)(millis() - started));
            releaseClient();
            return false;
        }

        NimBLERemoteCharacteristic* pChar = findKnownPrinterChannel();
        if (pChar == nullptr) pChar = findFallbackChannel();
        if (pChar == nullptr) {
            Serial.println("[BLE] No writable printer characteristic found.");
            releaseClient();
            _connected = false;
            return false;
        }

        Serial.printf("[BLE] Using characteristic %s\n", pChar->getUUID().toString().c_str());
        uint16_t mtu = _pClient->getMTU();
        _chunkSize = (mtu > 3) ? min((size_t)(mtu - 3), (size_t)BLE_MAX_CHUNK) : 20;
        Serial.printf("[BLE] MTU %u, write chunk %u bytes\n", mtu, (unsigned)_chunkSize);

        // The link can drop during service discovery, after onDisconnect has run.
        if (!_pClient->isConnected()) {
            Serial.println("[BLE] Printer dropped during discovery.");
            releaseClient();
            _connected = false;
            return false;
        }

        _pRemoteCharacteristic = pChar;
        _connected = true;
        saveAddress(_targetAddress);
        return true;
    }

    // Before sleeping: drops the link and stops the BLE stack entirely.
    void shutdown() {
        NimBLEScan* pBLEScan = NimBLEDevice::getScan();
        if (pBLEScan->isScanning()) pBLEScan->stop();
        _doConnect = false;
        _connected = false;
        releaseClient();
        NimBLEDevice::deinit(true);
    }

    bool sendCommand(const uint8_t* cmd, size_t size) {
        // The characteristic is owned by _pClient, which is only freed from loop(),
        // so the local copy stays valid even if the link drops mid-send.
        NimBLERemoteCharacteristic* pChar = _pRemoteCharacteristic;
        if (!_connected || pChar == nullptr) return false;

        // Only use write-without-response when the characteristic supports it.
        bool withResponse = !pChar->canWriteNoResponse();
        size_t offset = 0;
        while (offset < size) {
            if (!_connected) return false;
            size_t chunkSize = (size - offset > _chunkSize) ? _chunkSize : (size - offset);
            if (!pChar->writeValue(cmd + offset, chunkSize, withResponse)) {
                Serial.println("[BLE] Write failed.");
                return false;
            }
            offset += chunkSize;
            uint32_t gapMs = max((uint32_t)BLE_MIN_CHUNK_GAP_MS,
                                 (uint32_t)(chunkSize * 1000 / BLE_MAX_BYTES_PER_SEC));
            delay(gapMs);
            if (_idleHook != nullptr) _idleHook();
        }
        return true;
    }

protected:
    void onDisconnect(NimBLEClient* pClient) override {
        Serial.println("[BLE] Printer disconnected.");
        _connected = false;
        _disconnectedSince = millis();
    }

    void onResult(NimBLEAdvertisedDevice* advertisedDevice) override {
        if (_doConnect || _connected) return;

        bool nameMatch = advertisedDevice->haveName() &&
                         String(advertisedDevice->getName().c_str()) == _targetName;
        bool addressMatch = _haveSavedAddress && advertisedDevice->getAddress() == _savedAddress;
        if (!nameMatch && !addressMatch) return;

        Serial.printf("[MATCH] Target printer found: %s\n", advertisedDevice->getAddress().toString().c_str());
        NimBLEDevice::getScan()->stop();
        _targetAddress = advertisedDevice->getAddress();
        _doConnect = true;
    }
};

#endif
