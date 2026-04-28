/*
 * NodeMCU ESP8266 - Multi-sensor Oregon THN128 transmitter
 *
 * - Fetches and transmits each sensor in sequence
 * - Sensor readings are cached, API called only when cache expires
 * - Transmits last known value even if API call fails
 *
 * Wiring:
 *   FS1000A DATA -> NodeMCU D6 (GPIO12)
 *   FS1000A VCC  -> NodeMCU VIN (5V when USB powered)
 *   FS1000A GND  -> NodeMCU GND
 *
 * Requires:
 *   - ESP8266 board support
 *   - ErriezOregonTHN128 library (installed from ZIP)
 *   - ArduinoJson library (v7.x)
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>
#include <ErriezOregonTHN128Transmit.h>

// --- WiFi credentials ---
const char* WIFI_SSID     = "SSID";
const char* WIFI_PASSWORD = "PASSWORD";

// --- API ---
const char* API_BASE_URL  = "https://API_ID.execute-api.REGION.amazonaws.com/v1";

// --- Minimum time between API calls per sensor ---
#define CACHE_TTL_MS  60000UL  // 60 seconds

// --- Transmission parameters ---
#define RETRANSMIT 3         // Repeat each sensor X times
#define RETRANSMIT_GAP 50    // Delay between repeating transmissinon (ms)
#define LOOP_DELAY 10000     // Wait before next loop after sending all sensors (ms)


// --- Sensor configuration ---
struct SensorConfig {
    const char* tagId;
    uint8_t     channel;        // Oregon channel 1, 2 or 3
    uint8_t     rollingAddress; // Unique per sensor 0..7
    const char* label;
};

static const SensorConfig SENSORS[] = {
    { "C1",  1,  0,  "Balcony"     },
    { "C2",  2,  2,  "Outside (S)" },
    { "C3",  3,  4,  "Outside (N)" },
};
static const int SENSOR_COUNT = sizeof(SENSORS) / sizeof(SENSORS[0]);


// --- Cached readings ---
struct SensorCache {
    int16_t        temperature;   // tenths of degrees
    bool           lowBattery;
    bool           valid;         // false until first successful fetch
    unsigned long  fetchedAt;     // millis() when last fetched
};

static SensorCache cache[SENSOR_COUNT];

// --- RF TX pin (NodeMCU D6 = GPIO12) ---
#define RF_TX_PIN  12

// --- Transmit a single cached reading ---
void transmitReading(int index) {
    if (!cache[index].valid) {
        Serial.printf("[%s] No data yet, skipping TX\n", SENSORS[index].label);
        return;
    }

    OregonTHN128Data_t data = {
        .rawData        = 0,
        .rollingAddress = SENSORS[index].rollingAddress,
        .channel        = SENSORS[index].channel,
        .temperature    = cache[index].temperature,
        .lowBattery     = cache[index].lowBattery,
    };
    data.rawData = OregonTHN128_DataToRaw(&data);

    char tempStr[10];
    OregonTHN128_TempToString(tempStr, sizeof(tempStr), cache[index].temperature);
    Serial.printf("[%s] TX Ch%d temp=%s C lowBatt=%s age=%lus\n",
        SENSORS[index].label, SENSORS[index].channel,
        tempStr, cache[index].lowBattery ? "YES" : "no",
        (millis() - cache[index].fetchedAt) / 1000);

    for (int r = 0; r < RETRANSMIT; r++) {
        noInterrupts();
        OregonTHN128_Transmit(&data);
        interrupts();
        delay(RETRANSMIT_GAP);
    }
}

// --- Fetch one sensor from API, update cache ---
// Returns true if fetch succeeded
bool fetchSensor(int index) {
    std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
    client->setInsecure();

    HTTPClient http;
    String url = String(API_BASE_URL) + "/tags/" + SENSORS[index].tagId;
    http.begin(*client, url);
    http.setTimeout(3000);

    int statusCode = http.GET();

    if (statusCode != 200) {
        Serial.printf("[%s] HTTP error: %d\n", SENSORS[index].label, statusCode);
        http.end();
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();

    if (err) {
        Serial.printf("[%s] JSON error: %s\n", SENSORS[index].label, err.c_str());
        return false;
    }

    float temperature = doc["temperature"];
    float voltage     = doc["voltage"];

    cache[index].temperature = (int16_t)roundf(temperature * 10.0f);
    cache[index].lowBattery  = (voltage < 2.5f);
    cache[index].valid       = true;
    cache[index].fetchedAt   = millis();

    char tempStr[10];
    OregonTHN128_TempToString(tempStr, sizeof(tempStr), cache[index].temperature);
    Serial.printf("[%s] Fetched temp=%s C voltage=%.3f\n",
        SENSORS[index].label, tempStr, voltage);

    return true;
}

// --- Fetch if cache expired, then transmit ---
void processSensor(int index) {
    unsigned long age = millis() - cache[index].fetchedAt;
    bool expired = !cache[index].valid || (age >= CACHE_TTL_MS);

    if (expired) {
        Serial.printf("[%s] Cache expired (%lus), fetching...\n",
            SENSORS[index].label, age / 1000);
        fetchSensor(index);  // transmit cached value even if this fails
    }

    transmitReading(index);
}

// --- WiFi connect ---
void connectWiFi() {
    Serial.print(F("Connecting to WiFi"));
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(F("."));
    }
    Serial.println();
    Serial.print(F("Connected, IP: "));
    Serial.println(WiFi.localIP());
}

void setup() {
    Serial.begin(115200);
    Serial.println(F("\nNodeMCU Oregon THN128 transmitter"));

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);  // active LOW on NodeMCU

    // Mark all cache entries as invalid
    for (int i = 0; i < SENSOR_COUNT; i++) {
        cache[i] = { 0, false, false, 0 };
    }

    OregonTHN128_TxBegin(RF_TX_PIN);
    connectWiFi();
}

void loop() {
    // Reconnect WiFi if dropped
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("WiFi lost, reconnecting..."));
        connectWiFi();
    }

    // Process each sensor — fetch if cache expired, then transmit
    for (int i = 0; i < SENSOR_COUNT; i++) {
        processSensor(i);
    }
    delay(LOOP_DELAY);
}

