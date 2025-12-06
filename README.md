# Environment Vigilance & Nature Data Monitor (ESP32)

**Purpose:** collect environmental and nature-related data (air quality, PM2.5, VOCs, temp/humidity, pressure, light) on an ESP32 DevKit, compute a simple risk score, show non-medical advisories tailored toward pregnancy-aware concerns, and upload data to a cloud endpoint for satellite fusion and clinical processing.

**Important:** This project **does NOT** give medical diagnoses or prescriptions. The on-device advisories are general non-medical recommendations only. For medical advice contact a healthcare professional.

## Files
- `env_vigilance_esp32.ino` — main Arduino IDE sketch (ESP32)

## Hardware (suggested)
- ESP32 DevKit (e.g., ESP32 DEVKIT V1)
- DHT22 (Temp & Humidity) — data pin to GPIO4
- BMP280 (I2C) — SCL GPIO22, SDA GPIO21
- PMS5003 / PMS7003 particulate sensor — UART to GPIO16 (RX) and GPIO17 (TX)
- MQ-135 gas sensor — analog to GPIO34 (with divider / conditioning)
- BH1750 ambient light sensor — I2C (optional)
- Power supply: ensure separate supply for sensors/relays if needed.
- Common ground.

## Software / Libraries
Install these via Arduino Library Manager:
- Adafruit BMP280
- DHT sensor library
- Adafruit Unified Sensor
- BH1750
- ArduinoJson (for JSON building)

Board package: Install `esp32` board support in Arduino IDE (Espressif Systems).

## Configuration
- Edit `WIFI_SSID` and `WIFI_PASS` in the `.ino`.
- Set `CLOUD_ENDPOINT`, `CLOUD_PORT`, and `CLOUD_PATH` to your ingestion server for satellite & clinical analysis.
- Adjust thresholds (PM2.5 limits, VOC scales) in code if required.

## How it works
1. Reads sensors periodically (every ~7s).
2. Parses PMS5003 frames to extract PM values.
3. Scales MQ-135 ADC to a VOC index (heuristic — calibrate for your circuit).
4. Computes a combined risk score (0–100) and a non-medical advice string.
5. Stores recent samples in RAM (ring buffer) and serves them on a password-protected web UI.
6. Posts JSON payloads to your cloud endpoint for further processing (satellite fusion, clinical logic).

## Extending for satellite analysis & clinical recommendations
- The device uploads raw + derived sensor data to your server. Implement server-side logic to:
  - Query satellite APIs (e.g., air quality satellite products, aerosol optical depth) — note: these APIs require web access and possibly paid keys.
  - Merge satellite data with ground sensors to improve spatial coverage.
  - Run clinical triage or generate prescriptions only by qualified medical services — DO NOT do this on-device.
- If you want, I can help generate a simple Node.js or Python Flask ingestion service that:
  - Accepts device JSON,
  - Queries a satellite product API,
  - Computes more sophisticated exposure metrics,
  - Pushes notifications to users or clinicians.

## Safety & calibration notes
- MQ-135 & PMS sensors require calibration. Use certified reference monitors or calibration gases where possible.
- For pregnancy-relevant monitoring, consult environmental health experts to define actionable thresholds and clinical workflows.
- Always direct users to consult healthcare professionals for any health concerns.

## License
MIT
