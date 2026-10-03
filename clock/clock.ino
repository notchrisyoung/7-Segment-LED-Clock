#include <FS.h> // this needs to be first, or it all crashes and burns...
#include <LittleFS.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>        // Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h> // Local WebServer used to serve the configuration portal
#include <ESP8266HTTPClient.h>
//#include <WiFiClientSecure.h>
#include <ArduinoJson.h> // https://github.com/bblanchon/ArduinoJson/ - v5.x (this sketch uses the v5 API)
#include <ESP8266SSDP.h>
#define FASTLED_INTERRUPT_RETRY_COUNT 0 // https://github.com/FastLED/FastLED/issues/367 decided against disabling FASTLED_ALLOW_INTERRUPTS due to WDT resets
#include <FastLED.h>
#include <DS3231.h>
#include <Wire.h>

#define NUM_LEDS_PER_SEGMENT 3
#define NUM_LEDS_HOUR (14 * NUM_LEDS_PER_SEGMENT) + 1
#define NUM_LEDS_MINUTE (14 * NUM_LEDS_PER_SEGMENT) + 3
#define DATA_PIN_HOUR 2
#define DATA_PIN_MINUTE 13

// LED
CRGB ledsHour[NUM_LEDS_HOUR];
CRGB ledsMinute[NUM_LEDS_MINUTE];
CHSV fromColor;
CHSV toColor = CHSV(128, 255, 255);
CHSV currentColor = CHSV(0, 0, 0);
CHSV savedColor;
unsigned long lastColorChangeTime = 0;
unsigned long colorSaveInterval = 15000; // milliseconds to wait for a color change to trigger a save

uint8_t lerp = 0;
bool fading = false;
int dstOffset = 0;
bool enableDST = true;

// WiFi hotspot the clock creates for its settings page. The name and password
// live in secrets.h (git-ignored); copy secrets.example.h to secrets.h to set them.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#warning "secrets.h not found - using the placeholder hotspot name/password from secrets.example.h"
#include "secrets.example.h"
#endif
const char *ssid = AP_SSID;
const char *password = AP_PASSWORD;

// flag for saving data
bool shouldSaveConfig = false;
DS3231 myRTC;
bool century = false;
bool h12 = false;
bool isTwelveHour = true;
bool PM_time = false;

ESP8266WebServer server(80);


void setRTCTime(int year, int month, int day, int dow, int hour, int minute, int second) {
    Serial.println("Setting RTC time");
    Serial.print("Year: ");
    Serial.println(year);
    Serial.print("Month: ");
    Serial.println(month);
    Serial.print("Day: ");
    Serial.println(day);
    Serial.print("Day of Week: ");
    Serial.println(dow);
    Serial.print("Hour: ");
    Serial.println(hour);
    Serial.print("Minute: ");
    Serial.println(minute);
    Serial.print("Second: ");
    Serial.println(second);
    myRTC.setYear(year);
    myRTC.setMonth(month);
    myRTC.setDate(day);
    myRTC.setDoW(dow);
    myRTC.setHour(hour);
    myRTC.setMinute(minute);
    myRTC.setSecond(second);
}

void checkDST(){
    int currentYear = myRTC.getYear();
    int startMonth = 3; // March
    int startDay = 8 - (myRTC.getDoW() + 5) % 7; // Second Sunday
    int endMonth = 11; // November
    int endDay = 1 - (myRTC.getDoW() + 6) % 7; // First Sunday

    // Check if the current date is within the DST period
    if (((myRTC.getMonth(century) > startMonth && myRTC.getMonth(century) < endMonth) ||
        (myRTC.getMonth(century) == startMonth && myRTC.getDate() >= startDay) ||
        (myRTC.getMonth(century) == endMonth && myRTC.getDate() <= endDay)) && enableDST){ 
        dstOffset = 1; // DST is in effect
    } 
    else {
        dstOffset = 0; // DST is not in effect
    }
}

void setup(){
    Serial.begin(115200);
    Serial.println();

    // initialize and reset LEDs
    delay(1); // leds sometimes not all resetting properly without this for some reason
    FastLED.addLeds<NEOPIXEL, DATA_PIN_HOUR>(ledsHour, NUM_LEDS_HOUR);
    FastLED.addLeds<NEOPIXEL, DATA_PIN_MINUTE>(ledsMinute, NUM_LEDS_MINUTE);
    fill_solid(ledsHour, NUM_LEDS_HOUR, CRGB::Black);
    fill_solid(ledsMinute, NUM_LEDS_MINUTE, CRGB::Black);
    FastLED.show();

    // DS3231 setup
    Wire.begin();

    if (LittleFS.begin()){
        Serial.println("mounted file system");
        Dir dir = LittleFS.openDir("/");
        dir.next();
        Serial.print(dir.fileName());
        Serial.print(" - ");
        Serial.println(dir.fileSize());
        dir.next();
        Serial.print(dir.fileName());
        Serial.print(" - ");
        Serial.println(dir.fileSize());
        if (LittleFS.exists("/config.json")){
            // file exists, reading and loading
            Serial.println("reading config file");
            File configFile = LittleFS.open("/config.json", "r");
            if (configFile){
                Serial.println("opened config file");
                size_t size = configFile.size();
                // Allocate a buffer to store contents of the file.
                std::unique_ptr<char[]> buf(new char[size]);

                configFile.readBytes(buf.get(), size);
                DynamicJsonBuffer jsonBuffer;
                JsonObject &json = jsonBuffer.parseObject(buf.get());
                json.printTo(Serial);
                if (json.success()){
                    Serial.println("\nparsed json");

                    // color
                    JsonVariant hue = json["color"]["h"];
                    JsonVariant sat = json["color"]["s"];
                    JsonVariant val = json["color"]["v"];
                    if (hue.success() && sat.success() && val.success())
                    {
                        // store initial color in toColor... hopefully this is OK
                        toColor = CHSV((hue.as<int>() % 360) * 255 / 360, constrain(sat.as<int>(), 0, 100) * 255 / 100, constrain(val.as<int>(), 0, 100) * 255 / 100);
                    }
                    savedColor = toColor;

                    // clock
                    JsonVariant clk = json["clock"];
                    if (clk.success() && clk.is<int>())
                    {
                        isTwelveHour = clk.as<int>() == 12;
                    }
                }
                else{
                    Serial.println("failed to load json config");
                }
            }
        }
        else{
            Serial.println("config file not found");
        }
    }

    // match the fade in color's hue and saturation
    currentColor = CHSV(toColor.h, toColor.s, 0);

    checkDST();

    // Start ap
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, password);

    server.serveStatic("/", LittleFS, "/index.html");

    server.on("/color", HTTP_PUT, [](){
        String h = server.arg("h");
        String s = server.arg("s");
        String v = server.arg("v");
        if (h != "" && s != "" && v != ""){
        setColor(h.toInt(), s.toInt(), v.toInt());
        server.send(204);
    } });

    server.on("/color", HTTP_GET, [](){
        const size_t bufferSize = JSON_OBJECT_SIZE(3); // https://arduinojson.org/v5/assistant/
        DynamicJsonBuffer jsonBuffer(bufferSize);

        JsonObject& root = jsonBuffer.createObject();
        root["h"] = toColor.hue * 360 / 255;
        root["s"] = toColor.saturation * 100 / 255;
        root["v"] = toColor.value * 100 / 255;

        String jsonString;
        root.printTo(jsonString);
        server.send(200, "application/json", jsonString); 
    });

    server.on("/clock", HTTP_GET, [](){ 
        server.send(200, "text/plain", isTwelveHour ? "12" : "24"); 
    });

    server.on("/clock", HTTP_PUT, [](){
        String body = server.arg("plain");
        Serial.println("Setting clock");
        bool newIsTwelveHour;
        if (body == "12") {
            newIsTwelveHour = true;
            server.send(204);
            if (isTwelveHour != newIsTwelveHour) {
                isTwelveHour = newIsTwelveHour;
                shouldSaveConfig = true;
            }
        } 
        else if (body == "24") {
            newIsTwelveHour = false;
            server.send(204);
            if (isTwelveHour != newIsTwelveHour) {
                isTwelveHour = newIsTwelveHour;
                shouldSaveConfig = true;
            }
        } 
        else {
            server.send(400);
        } 
    });
    server.on("/dst", HTTP_GET, [](){
        server.send(200, "text/plain", enableDST ? "1" : "0");
    });

    server.on("/dst", HTTP_PUT, [](){
        String body = server.arg("plain");
        Serial.println("Setting dst");
        if (body == "1") {
            enableDST = true;
            server.send(204);
            shouldSaveConfig = true;
        } 
        else if (body == "0") {
            enableDST = false;
            server.send(204);
            shouldSaveConfig = true;
        } 
        else {
            server.send(400);
        } 
    });

    server.on("/config", HTTP_GET, [](){
        const size_t bufferSize = JSON_OBJECT_SIZE(3) + 2 * JSON_OBJECT_SIZE(4); // https://arduinojson.org/v5/assistant/
        DynamicJsonBuffer jsonBuffer(bufferSize);

        JsonObject& root = jsonBuffer.createObject();

        JsonObject& color = root.createNestedObject("color");
        color["h"] = toColor.hue * 360 / 255;
        color["s"] = toColor.saturation * 100 / 255;
        color["v"] = toColor.value * 100 / 255;
        root["clock"] = isTwelveHour ? 12 : 24;

        String jsonString;
        root.printTo(jsonString);
        server.send(200, "application/json", jsonString); 
    });

    server.on("/description.xml", HTTP_GET, [](){
        SSDP.schema(server.client());
        // connection hangs for about 2 seconds otherwise
        server.client().stop(); 
    });
  
    server.on("/time", HTTP_PUT, [](){
        Serial.println("Setting time");
        String year = server.arg("year");
        String month = server.arg("month");
        String dow = server.arg("dow");
        String day = server.arg("day");
        String hour = server.arg("hour");
        String minute = server.arg("min");
        String second = server.arg("sec");
        Serial.println("Server Args:");
        Serial.print("Year: ");
        Serial.println(year);
        Serial.print("Month: ");
        Serial.println(month);
        Serial.print("Day of Week: ");
        Serial.println(dow);
        Serial.print("Day: ");
        Serial.println(day);
        Serial.print("Hour: ");
        Serial.println(hour);
        Serial.print("Minute: ");
        Serial.println(minute);
        Serial.print("Second: ");
        Serial.println(second);
        if (hour == "0" && dstOffset == 1 && enableDST){
            hour = "24";
        }
        if (year != "" && month != "" && dow != "" && day != "" && dow != "" && hour != "" && minute != "" && second != ""){
            if (enableDST){
                setRTCTime(year.toInt(), month.toInt(), day.toInt(), dow.toInt(), hour.toInt() - dstOffset, minute.toInt(), second.toInt());
            }
            else{
                setRTCTime(year.toInt(), month.toInt(), day.toInt(), dow.toInt(), hour.toInt(), minute.toInt(), second.toInt());
            }
            
            server.send(204);
        } else {
            server.send(400);
        }
    });

    server.begin();

    // SSDP
    SSDP.setSchemaURL("description.xml");
    SSDP.setHTTPPort(80);
    SSDP.setName("LED Clock");
    SSDP.setSerialNumber(ESP.getChipId());
    SSDP.setURL("");
    SSDP.setModelName("LED Clock");
    SSDP.setModelNumber("LL-7S");
    SSDP.setManufacturer("Chris Young");
    SSDP.setDeviceType("urn:schemas-upnp-org:device:7SegmentClock:1");
    SSDP.begin();

    // save the custom parameters to FS
    if (shouldSaveConfig){
        saveConfig();
    }
}

int currentMinute = 0;
boolean first = true;

void loop(){
    server.handleClient();
    if (first){ // super dramatic fade in effect
        setColor(toColor);
        first = false;
    }
    if (myRTC.getMinute() != currentMinute){
        // check for new offset every hour between midnight and 4AM for DST change
        if (myRTC.getMinute() == 0 && myRTC.getHour(h12,PM_time) <= 4 && enableDST){
            checkDST();
        }
    }
    currentMinute = myRTC.getMinute();
    updateLeds();
    fadeToColor();
    saveColorChange();
    if (shouldSaveConfig){
        saveConfig();
    }
}

// throttle color change induced saving to avoid unnecessary write/erase cycles
void saveColorChange(){
    if (toColor != savedColor && ((millis() - lastColorChangeTime) > colorSaveInterval))
    {
        shouldSaveConfig = true;
    }
}

void saveConfig(){
    Serial.println("saving config");
    shouldSaveConfig = false; // avoid saving again, even if unsuccessful
    DynamicJsonBuffer jsonBuffer;
    JsonObject &json = jsonBuffer.createObject();
    JsonObject &color = json.createNestedObject("color");
    color["h"] = toColor.hue * 360 / 255;
    color["s"] = toColor.saturation * 100 / 255;
    color["v"] = toColor.value * 100 / 255;
    json["clock"] = isTwelveHour ? 12 : 24;
    json["dst"] = enableDST ? 1 : 0;

    File configFile = LittleFS.open("/config.json", "w");
    if (!configFile){
        Serial.println("failed to open config file for writing");
    }

    json.printTo(Serial);
    Serial.println("");
    json.printTo(configFile);
    configFile.close();

    savedColor = toColor;
}

void updateLeds(){
    fill_solid(ledsHour, NUM_LEDS_HOUR, CRGB::Black);
    ledsHour[0] = currentColor; // colon
    int displayHour = myRTC.getHour(h12,PM_time) + dstOffset % 24;
    if (isTwelveHour) {
        if (displayHour == 0) {
            displayHour = 12;
        } else if (displayHour > 12) {
            displayHour -= 12;
        }
    }
    if (displayHour >= 10 || !isTwelveHour){
        displayHourDigitLeft(7 * NUM_LEDS_PER_SEGMENT + 1, displayHour / 10);
    }
    displayHourDigitRight(1, displayHour % 10);
    fill_solid(ledsMinute, NUM_LEDS_MINUTE, CRGB::Black);
    ledsMinute[0] = currentColor; // colon
    int displayMinute = myRTC.getMinute();
    displayMinuteDigitLeft(1, (displayMinute < 10) ? 0 : displayMinute / 10);
    displayMinuteDigitRight(7 * NUM_LEDS_PER_SEGMENT + 1, displayMinute % 10);
    FastLED.show();
}

void fillSegment(struct CRGB *leds, int offset, int segmentIndex){
    fill_solid(leds + offset + segmentIndex * NUM_LEDS_PER_SEGMENT, NUM_LEDS_PER_SEGMENT, currentColor);
}

void displayHourDigitLeft(int offset, int digit){
    switch (digit){
    case 0:
        fillSegment(ledsHour, offset, 0);
        fillSegment(ledsHour, offset, 1);
        fillSegment(ledsHour, offset, 2);
        fillSegment(ledsHour, offset, 4);
        fillSegment(ledsHour, offset, 5);
        fillSegment(ledsHour, offset, 6);
        break;
    case 1:
        fillSegment(ledsHour, offset, 0);
        fillSegment(ledsHour, offset, 4);
        break;
    case 2:
        fillSegment(ledsHour, offset, 5);
        fillSegment(ledsHour, offset, 4);
        fillSegment(ledsHour, offset, 3);
        fillSegment(ledsHour, offset, 2);
        fillSegment(ledsHour, offset, 1);
        break;
    case 3:
        fillSegment(ledsHour, offset, 0);
        fillSegment(ledsHour, offset, 1);
        fillSegment(ledsHour, offset, 3);
        fillSegment(ledsHour, offset, 4);
        fillSegment(ledsHour, offset, 5);
        break;
    case 4:
        fillSegment(ledsHour, offset, 0);
        fillSegment(ledsHour, offset, 6);
        fillSegment(ledsHour, offset, 3);
        fillSegment(ledsHour, offset, 4);
        break;
    case 5:
        fillSegment(ledsHour, offset, 0);
        fillSegment(ledsHour, offset, 1);
        fillSegment(ledsHour, offset, 3);
        fillSegment(ledsHour, offset, 6);
        fillSegment(ledsHour, offset, 5);
        break;
    case 6:
        fillSegment(ledsHour, offset, 1);
        fillSegment(ledsHour, offset, 2);
        fillSegment(ledsHour, offset, 3);
        fillSegment(ledsHour, offset, 0);
        fillSegment(ledsHour, offset, 5);
        fillSegment(ledsHour, offset, 6);
        break;
    case 7:
        fillSegment(ledsHour, offset, 0);
        fillSegment(ledsHour, offset, 5);
        fillSegment(ledsHour, offset, 4);
        break;
    case 8:
        fillSegment(ledsHour, offset, 0);
        fillSegment(ledsHour, offset, 1);
        fillSegment(ledsHour, offset, 2);
        fillSegment(ledsHour, offset, 3);
        fillSegment(ledsHour, offset, 4);
        fillSegment(ledsHour, offset, 5);
        fillSegment(ledsHour, offset, 6);
        break;
    case 9:
        fillSegment(ledsHour, offset, 0);
        fillSegment(ledsHour, offset, 1);
        fillSegment(ledsHour, offset, 3);
        fillSegment(ledsHour, offset, 4);
        fillSegment(ledsHour, offset, 5);
        fillSegment(ledsHour, offset, 6);
        break;
    }
}

void displayHourDigitRight(int offset, int digit){
    switch (digit){
    case 0:
        fillSegment(ledsHour, offset, 0);
        fillSegment(ledsHour, offset, 1);
        fillSegment(ledsHour, offset, 2);
        fillSegment(ledsHour, offset, 4);
        fillSegment(ledsHour, offset, 5);
        fillSegment(ledsHour, offset, 6);
        break;
    case 1:
        fillSegment(ledsHour, offset, 0);
        fillSegment(ledsHour, offset, 4);
        break;
    case 2:
        fillSegment(ledsHour, offset, 0);
        fillSegment(ledsHour, offset, 1);
        fillSegment(ledsHour, offset, 3);
        fillSegment(ledsHour, offset, 5);
        fillSegment(ledsHour, offset, 6);
        break;
    case 3:
        fillSegment(ledsHour, offset, 0);
        fillSegment(ledsHour, offset, 1);
        fillSegment(ledsHour, offset, 3);
        fillSegment(ledsHour, offset, 4);
        fillSegment(ledsHour, offset, 5);
        break;
    case 4:
        fillSegment(ledsHour, offset, 0);
        fillSegment(ledsHour, offset, 2);
        fillSegment(ledsHour, offset, 3);
        fillSegment(ledsHour, offset, 4);
        break;
    case 5:
        fillSegment(ledsHour, offset, 1);
        fillSegment(ledsHour, offset, 2);
        fillSegment(ledsHour, offset, 3);
        fillSegment(ledsHour, offset, 4);
        fillSegment(ledsHour, offset, 5);
        break;
    case 6:
        fillSegment(ledsHour, offset, 1);
        fillSegment(ledsHour, offset, 2);
        fillSegment(ledsHour, offset, 3);
        fillSegment(ledsHour, offset, 4);
        fillSegment(ledsHour, offset, 5);
        fillSegment(ledsHour, offset, 6);
        break;
    case 7:
        fillSegment(ledsHour, offset, 0);
        fillSegment(ledsHour, offset, 1);
        fillSegment(ledsHour, offset, 4);
        break;
    case 8:
        fillSegment(ledsHour, offset, 0);
        fillSegment(ledsHour, offset, 1);
        fillSegment(ledsHour, offset, 2);
        fillSegment(ledsHour, offset, 3);
        fillSegment(ledsHour, offset, 4);
        fillSegment(ledsHour, offset, 5);
        fillSegment(ledsHour, offset, 6);
        break;
    case 9:
        fillSegment(ledsHour, offset, 0);
        fillSegment(ledsHour, offset, 1);
        fillSegment(ledsHour, offset, 2);
        fillSegment(ledsHour, offset, 3);
        fillSegment(ledsHour, offset, 4);
        fillSegment(ledsHour, offset, 5);
        break;
    }
}

void displayMinuteDigitLeft(int offset, int digit){
    switch (digit){
    case 0:
        fillSegment(ledsMinute, offset, 0);
        fillSegment(ledsMinute, offset, 1);
        fillSegment(ledsMinute, offset, 2);
        fillSegment(ledsMinute, offset, 4);
        fillSegment(ledsMinute, offset, 5);
        fillSegment(ledsMinute, offset, 6);
        break;
    case 1:
        fillSegment(ledsMinute, offset, 2);
        fillSegment(ledsMinute, offset, 6);
        break;
    case 2:
        fillSegment(ledsMinute, offset, 1);
        fillSegment(ledsMinute, offset, 2);
        fillSegment(ledsMinute, offset, 3);
        fillSegment(ledsMinute, offset, 4);
        fillSegment(ledsMinute, offset, 5);
        break;
    case 3:
        fillSegment(ledsMinute, offset, 1);
        fillSegment(ledsMinute, offset, 2);
        fillSegment(ledsMinute, offset, 3);
        fillSegment(ledsMinute, offset, 5);
        fillSegment(ledsMinute, offset, 6);
        break;
    case 4:
        fillSegment(ledsMinute, offset, 0);
        fillSegment(ledsMinute, offset, 2);
        fillSegment(ledsMinute, offset, 3);
        fillSegment(ledsMinute, offset, 6);
        break;
    case 5:
        fillSegment(ledsMinute, offset, 0);
        fillSegment(ledsMinute, offset, 1);
        fillSegment(ledsMinute, offset, 3);
        fillSegment(ledsMinute, offset, 5);
        fillSegment(ledsMinute, offset, 6);
        break;
    case 6:
        fillSegment(ledsMinute, offset, 0);
        fillSegment(ledsMinute, offset, 1);
        fillSegment(ledsMinute, offset, 3);
        fillSegment(ledsMinute, offset, 4);
        fillSegment(ledsMinute, offset, 5);
        fillSegment(ledsMinute, offset, 6);
        break;
    case 7:
        fillSegment(ledsMinute, offset, 1);
        fillSegment(ledsMinute, offset, 2);
        fillSegment(ledsMinute, offset, 6);
        break;
    case 8:
        fillSegment(ledsMinute, offset, 0);
        fillSegment(ledsMinute, offset, 1);
        fillSegment(ledsMinute, offset, 2);
        fillSegment(ledsMinute, offset, 3);
        fillSegment(ledsMinute, offset, 4);
        fillSegment(ledsMinute, offset, 5);
        fillSegment(ledsMinute, offset, 6);
        break;
    case 9:
        fillSegment(ledsMinute, offset, 0);
        fillSegment(ledsMinute, offset, 1);
        fillSegment(ledsMinute, offset, 2);
        fillSegment(ledsMinute, offset, 3);
        fillSegment(ledsMinute, offset, 5);
        fillSegment(ledsMinute, offset, 6);
        break;
    }
}

void displayMinuteDigitRight(int offset, int digit){
    switch (digit){
    case 0:
        fillSegment(ledsMinute, offset, 0);
        fillSegment(ledsMinute, offset, 1);
        fillSegment(ledsMinute, offset, 2);
        fillSegment(ledsMinute, offset, 4);
        fillSegment(ledsMinute, offset, 5);
        fillSegment(ledsMinute, offset, 6);
        break;
    case 1:
        fillSegment(ledsMinute, offset, 2);
        fillSegment(ledsMinute, offset, 6);
        break;
    case 2:
        fillSegment(ledsMinute, offset, 0);
        fillSegment(ledsMinute, offset, 1);
        fillSegment(ledsMinute, offset, 3);
        fillSegment(ledsMinute, offset, 5);
        fillSegment(ledsMinute, offset, 6);
        break;
    case 3:
        fillSegment(ledsMinute, offset, 1);
        fillSegment(ledsMinute, offset, 2);
        fillSegment(ledsMinute, offset, 3);
        fillSegment(ledsMinute, offset, 5);
        fillSegment(ledsMinute, offset, 6);
        break;
    case 4:
        fillSegment(ledsMinute, offset, 2);
        fillSegment(ledsMinute, offset, 3);
        fillSegment(ledsMinute, offset, 4);
        fillSegment(ledsMinute, offset, 6);
        break;
    case 5:
        fillSegment(ledsMinute, offset, 1);
        fillSegment(ledsMinute, offset, 2);
        fillSegment(ledsMinute, offset, 3);
        fillSegment(ledsMinute, offset, 4);
        fillSegment(ledsMinute, offset, 5);
        break;
    case 6:
        fillSegment(ledsMinute, offset, 0);
        fillSegment(ledsMinute, offset, 1);
        fillSegment(ledsMinute, offset, 2);
        fillSegment(ledsMinute, offset, 3);
        fillSegment(ledsMinute, offset, 4);
        fillSegment(ledsMinute, offset, 5);
        break;
    case 7:
        fillSegment(ledsMinute, offset, 2);
        fillSegment(ledsMinute, offset, 5);
        fillSegment(ledsMinute, offset, 6);
        break;
    case 8:
        fillSegment(ledsMinute, offset, 0);
        fillSegment(ledsMinute, offset, 1);
        fillSegment(ledsMinute, offset, 2);
        fillSegment(ledsMinute, offset, 3);
        fillSegment(ledsMinute, offset, 4);
        fillSegment(ledsMinute, offset, 5);
        fillSegment(ledsMinute, offset, 6);
        break;
    case 9:
        fillSegment(ledsMinute, offset, 1);
        fillSegment(ledsMinute, offset, 2);
        fillSegment(ledsMinute, offset, 3);
        fillSegment(ledsMinute, offset, 4);
        fillSegment(ledsMinute, offset, 5);
        fillSegment(ledsMinute, offset, 6);
        break;
    }
}

void setColor(int h, int s, int v){
    int hue = (h % 360) * 255 / 360;
    int sat = constrain(s, 0, 100) * 255 / 100;
    int val = constrain(v, 0, 100) * 255 / 100;
    setColor(CHSV(hue, sat, val));
}

void setColor(CHSV chsv){
    lastColorChangeTime = millis();
    fromColor = currentColor;
    toColor = chsv;
    lerp = 0;
    fading = true;
}

void fadeToColor(){
    if (fading){
        if (lerp < 255){
            currentColor = blend(fromColor, toColor, ++lerp);
        }
        else{
            fading = false;
        }
    }
}
