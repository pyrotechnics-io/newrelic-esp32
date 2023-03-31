#include <ArduinoJson.h>
#include <WiFi.h>
#include <ESP32Ping.h>
#include <HTTPClient.h>

#include <esp_system.h>
#include <esp_wifi.h>

#ifdef ENABLE_OTA
#include <ArduinoOTA.h>
#endif

#include <ctime>
#include <string>

#define ST(A) #A
#define PARAM(A) ST(A)

#define LED_BUILTIN 2
#define ADC_VREF_mV    3300.0
#define ADC_RESOLUTION 2048.0
#define PIN_LM35       32       // ESP32 pin GIOP36 (ADC0)

constexpr int DEAD_ZONE_START = 22;
constexpr int DEAD_ZONE_END = 7;
bool sleeping = false;
RTC_DATA_ATTR time_t reference_epoch = 0; // For time sanity checks
constexpr int ONE_YEAR = 365 * 24 * 60 * 60;
constexpr int ONE_MONTH = ONE_YEAR / 12;
constexpr int SEC_TO_US = 1000 * 1000;
time_t shift = 0;

static const char *get_chip_id()
{
    static char cid[23] = {'\0'};
    if (cid[0] == '\0')
    {
        uint64_t chipid = ESP.getEfuseMac(); // The chip ID is essentially its
                                             // MAC address(length: 6 bytes).
        uint16_t chip = (uint16_t)(chipid >> 32);
        snprintf(cid, 23, "ESP32-%04X%08X", chip, (uint32_t)chipid);
    }
    return cid;
}

void WiFiStationConnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
    Serial.printf(": ");
}

void WiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info)
{
    Serial.println(WiFi.localIP());
}

void WiFiStationDisconnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
    Serial.printf("Disconnected from WiFi access point: ");
    Serial.println(info.wifi_sta_disconnected.reason);
    if (!sleeping)
        WiFi.begin(PARAM(WIFI_SSID), PARAM(WIFI_PASSWORD));
}

void wifi(bool enable)
{
    if (!enable)
    {
        WiFi.removeEvent(ARDUINO_EVENT_WIFI_STA_CONNECTED);
        WiFi.removeEvent(ARDUINO_EVENT_WIFI_STA_GOT_IP);
        WiFi.removeEvent(ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        return;
    }

    WiFi.disconnect(true);
    Serial.printf("Connecting to WiFi");
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(get_chip_id());
    WiFi.onEvent(WiFiStationConnected, ARDUINO_EVENT_WIFI_STA_CONNECTED);
    WiFi.onEvent(WiFiGotIP, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent(WiFiStationDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    WiFi.begin(PARAM(WIFI_SSID), PARAM(WIFI_PASSWORD));
}

void radio_state(bool enabled)
{
    if (enabled)
        esp_wifi_start();
    else
        esp_wifi_stop();
}

time_t compile_timestamp()
{
#ifdef BUILD_TIME
    return BUILD_TIME;
#else
    const char *time = __DATE__;
    char s_month[5];
    int month, day, year;
    struct tm t = {0};
    static const char month_names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

    sscanf(time, "%s %d %d", s_month, &day, &year);

    month = (strstr(month_names, s_month) - month_names) / 3;

    t.tm_mon = month;
    t.tm_mday = day;
    t.tm_year = year - 1900;
    t.tm_isdst = -1;

    return mktime(&t);
#endif
}

const char *get_time(time_t *pnow = nullptr)
{
    configTime(8 * 60 * 60, 0, "time1.google.com", "sg.pool.ntp.org",
               "time2.google.com"); // get UTC time via NTP
    while (time(pnow) <= 100000)
        delay(100);

    // Check if the time is within a year of the reference epoch
    if (pnow && (std::abs(*pnow - reference_epoch) >= shift))
    {
        Serial.printf(
            "Timestamp is greater than reference shift. Restarting ...");
        ESP.restart();
    }

    std::tm *ptm = std::localtime(pnow);
    if (!ptm)
    {
        Serial.println("Failed to get local time");
        ESP.restart();
    }

    static char buffer[12] = {'\0'};
    std::strftime(buffer, 12, "%H:%M:%S", ptm);
    return buffer;
}

template <typename T>
void telemetry(const std::string metric, const T &value)
{
    String api_ep = "https://";
    api_ep += PARAM(NR_API_ENDPOINT);
    String api_key = PARAM(NR_API_KEY);
    HTTPClient http;
    http.begin(api_ep);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Insert-Key", api_key);
    time_t now = {};
    String now_s = get_time(&now);

    // Construct our event payload
    StaticJsonDocument<200> doc;
    JsonArray data = doc.to<JsonArray>();
    JsonObject item = data.createNestedObject();
    item["eventType"] = "iot";
    item["source"] = get_chip_id();
    item["metric"] = metric;
    item["value"] = std::to_string(value);
    item["timestamp"] = now_s;
    item["epoch"] = static_cast<long int>(now);

    String requestBody;
    serializeJson(doc, requestBody);
    Serial.println(requestBody.c_str());
    int httpResponseCode = http.POST(requestBody);

    if (httpResponseCode > 0)
    {
        String response = http.getString();
        Serial.printf("HTTP Success %d : [%s]\n", httpResponseCode,
                      response.c_str());
    }
    else
    {
        Serial.printf("HTTP POST error: %s\n",
                      http.errorToString(httpResponseCode).c_str());
    }
    http.end();
}

void ota_setup()
{
#ifdef ENABLE_OTA
    // Port defaults to 3232
    // ArduinoOTA.setPort(3232);
    // Hostname defaults to esp3232-[MAC]
    // ArduinoOTA.setHostname("myesp32");
    // No authentication by default
    // ArduinoOTA.setPassword("admin");
    // Password can be set with it's md5 value as well
    // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
    // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");
    Serial.println("Setting up OTA ...");
    ArduinoOTA.setHostname(get_chip_id())
        .onStart([]()
                 {
                     String type;

                     if (ArduinoOTA.getCommand() == U_FLASH)
                         type = "sketch";
                     else // U_SPIFFS
                         type = "filesystem";

                     // NOTE: if updating SPIFFS this would be the place to unmount
                     // SPIFFS using SPIFFS.end()
                     Serial.println("Start updating " + type); })
        .onEnd([]()
               { Serial.println("\nEnd"); })
        .onProgress([](unsigned int progress, unsigned int total)
                    { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); })
        .onError([](ota_error_t error)
                 {
                     Serial.printf("Error[%u]: ", error);

                     if (error == OTA_AUTH_ERROR)
                         Serial.println("Auth Failed");
                     else if (error == OTA_BEGIN_ERROR)
                         Serial.println("Begin Failed");
                     else if (error == OTA_CONNECT_ERROR)
                         Serial.println("Connect Failed");
                     else if (error == OTA_RECEIVE_ERROR)
                         Serial.println("Receive Failed");
                     else if (error == OTA_END_ERROR)
                         Serial.println("End Failed"); });
    ArduinoOTA.begin();
#endif
}

void ota_loop()
{
#ifdef ENABLE_OTA
    ArduinoOTA.handle();
#endif
}

void check_wifi()
{
    for (auto i = 0; i < 30; i++)
    {
        if (WiFi.waitForConnectResult() == WL_CONNECTED)
            return;
        else
        {
            delay(1000);
        }
    }

    ESP.restart();
}

void setup()
{
    Serial.begin(9600);
    pinMode(LED_BUILTIN, OUTPUT);

    sleeping = false;
    radio_state(true);

    wifi(true);
    check_wifi();

    // Setup time
    shift = ONE_YEAR;
    if (!reference_epoch)
    {
        reference_epoch = compile_timestamp();
        Serial.printf("Initialized reference timestamp: %ld\n", reference_epoch);
    }
    else
    {
        Serial.printf("Retrieved reference timestamp: %ld\n", reference_epoch);
    }
    time_t t = {};
    Serial.printf("Time: %s\n", get_time(&t));
    reference_epoch = t;
    shift = ONE_MONTH;
    Serial.printf("Reference time: %ld\n", reference_epoch);

    // Setup OTA (if enabled)
    ota_setup();
    Serial.println();
    Serial.println();
}

float get_sensor_data()
{
    // read the ADC value from the temperature sensor
    int adcVal = analogRead(PIN_LM35);
    float milliVolt = adcVal * (ADC_VREF_mV / ADC_RESOLUTION);
    float tempC = milliVolt / 10;

    // convert the °C to °F
    // float tempF = tempC * 9 / 5 + 32;

    return tempC;
}

void loop()
{
    time_t now = {};
    check_wifi();
    get_time(&now);
    auto delay_time = 200;

    while (true)
    {
        ota_loop();
      
        float t = get_sensor_data();
        telemetry("temperature", t);

        delay(delay_time);
    }
}
