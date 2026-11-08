#ifndef THERMAL_PRINTER_BLE_H
#define THERMAL_PRINTER_BLE_H

#include <Arduino.h>
#include <NimBLEDevice.h>
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

class ThermalPrinterBLE : public NimBLEAdvertisedDeviceCallbacks, public NimBLEClientCallbacks {
private:
    String _targetName;
    // Written from the NimBLE host task, read from loop().
    std::atomic<bool> _doConnect;
    std::atomic<bool> _connected;
    NimBLEClient* _pClient;
    NimBLERemoteCharacteristic* _pRemoteCharacteristic;
    NimBLEAddress _targetAddress;
    unsigned long _lastScanTime;
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

public:
    ThermalPrinterBLE(const char* targetName) :
        _targetName(targetName), _doConnect(false), _connected(false),
        _pClient(nullptr), _pRemoteCharacteristic(nullptr), _targetAddress(""),
        _lastScanTime(0), _chunkSize(20), _idleHook(nullptr) {}

    void setIdleHook(void (*hook)()) { _idleHook = hook; }

    void init(const char* localBLEName) {
        NimBLEDevice::init(localBLEName);
        NimBLEDevice::setMTU(BLE_REQUESTED_MTU);
        NimBLEScan* pBLEScan = NimBLEDevice::getScan();
        pBLEScan->setAdvertisedDeviceCallbacks(this);
        pBLEScan->setInterval(100);
        pBLEScan->setWindow(50);
        pBLEScan->setActiveScan(true);

        pBLEScan->start(2, nullptr, false);
        _lastScanTime = millis();
    }

    void keepScanAlive() {
        if (!_connected && !_doConnect) {
            if (millis() - _lastScanTime > 4000) {
                NimBLEDevice::getScan()->start(2, nullptr, false);
                _lastScanTime = millis();
            }
        }
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
        _pClient = NimBLEDevice::createClient();
        _pClient->setClientCallbacks(this, false);

        Serial.printf("[BLE] Handshaking directly with target address: %s\n", _targetAddress.toString().c_str());
        if (!_pClient->connect(_targetAddress)) {
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
        return true;
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
    }

    void onResult(NimBLEAdvertisedDevice* advertisedDevice) override {
        if (_doConnect || _connected) return;
        if (!advertisedDevice->haveName()) return;

        String deviceName = advertisedDevice->getName().c_str();
        if (deviceName == _targetName) {
            Serial.printf("[MATCH] Target printer found: %s\n", deviceName.c_str());
            NimBLEDevice::getScan()->stop();
            _targetAddress = advertisedDevice->getAddress();
            _doConnect = true;
        }
    }
};

#endif
