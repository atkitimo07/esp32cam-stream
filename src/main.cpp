#include <WiFi.h>
#include <WebServer.h>
#include "esp_camera.h"
#include "lookup_camera_frame_size.h"
#include "settings.h"
#include "secrets.h"
#include <vector>
#include <ArduinoOTA.h>

// ======== USER CONFIG ========
const char* ssid     = DEFAULT_STA_SSID;
const char* password = DEFAULT_STA_PASSWORD;

const char* ap_ssid = WIFI_SSID;
const char* ap_pass = WIFI_PASSWORD;

std::vector<int> gpioPins;
std::vector<float> gpioStates;

uint32_t last_ota_time = 0;


WebServer server(80);

static volatile uint32_t capture_frames = 0;
static volatile uint32_t stream_frames = 0;
static uint32_t fps_last_ms = 0;

#define STREAM_CONTENT_BOUNDARY "frame"

// ======== FPS COUNTER (DEBUG) ========
void printFPS()
{
    if (millis() - fps_last_ms < 1000) return;
    fps_last_ms = millis();
    Serial.printf("[FPS] stream: %lu\n", (unsigned long)stream_frames);
    stream_frames = 0;
}

// ======== WIFI SETUP ========
void setupWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    Serial.print("Connecting to WiFi");

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nConnected!");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\nFailed. Starting AP...");
        WiFi.mode(WIFI_AP);
        WiFi.softAP(ap_ssid, ap_pass);
        Serial.println(WiFi.softAPIP());
    }
}

// ======== OTA SETUP ========
void setupOTA()
{
    ArduinoOTA
    .onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) {
            type = "sketch";
        } else {  // U_SPIFFS
            type = "filesystem";
        }

        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        Serial.println("Start updating " + type);
    })
    .onEnd([]() {
        Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
        if (millis() - last_ota_time > 500) {
            Serial.printf("Progress: %u%%\n", (progress / (total / 100)));
            last_ota_time = millis();
        }
    })
    .onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) {
            Serial.println("Auth Failed");
        } else if (error == OTA_BEGIN_ERROR) {
            Serial.println("Begin Failed");
        } else if (error == OTA_CONNECT_ERROR) {
            Serial.println("Connect Failed");
        } else if (error == OTA_RECEIVE_ERROR) {
            Serial.println("Receive Failed");
        } else if (error == OTA_END_ERROR) {
            Serial.println("End Failed");
        }
    });

    ArduinoOTA.begin();
}

// ======== GPIO SETUP ========
std::vector<float> parseCSV(const char* str)
{
    std::vector<float> result;
    String s = String(str);

    int start = 0;
    while (true) {
        int comma = s.indexOf(',', start);
        if (comma == -1) {
            result.push_back(s.substring(start).toFloat());
            break;
        }
        result.push_back(s.substring(start, comma).toFloat());
        start = comma + 1;
    }
    return result;
}

void setupGPIOs()
{
#ifdef GPIO_AVAILABLE_PINS_STR
#ifdef GPIO_INITIAL_STATES_STR

    auto pins = parseCSV(GPIO_AVAILABLE_PINS_STR);
    auto states = parseCSV(GPIO_INITIAL_STATES_STR);

    if (pins.size() != states.size()) {
        Serial.println("GPIO config mismatch, must be same length!");
        return;
    }

    for (size_t i = 0; i < pins.size(); i++) {
        int pin = (int)pins[i];
        float state = states[i];

        pinMode(pin, OUTPUT);

        if (state == 0.0f || state == 1.0f) {
            digitalWrite(pin, (int)state);
        } else {
            // initialise PWM
            int channel = i; // simple mapping
            ledcSetup(channel, 5000, 8);
            ledcAttachPin(pin, channel);
            ledcWrite(channel, (int)(state * 255));
        }

        gpioPins.push_back(pin);
        gpioStates.push_back(state);

        Serial.printf("GPIO %d init -> %.2f\n", pin, state);
    }

#endif
#endif
}

// ======== CAMERA INIT ========
void initCamera()
{
    camera_config_t config;
    config.ledc_channel = CAMERA_CONFIG_LEDC_CHANNEL;
    config.ledc_timer   = CAMERA_CONFIG_LEDC_TIMER;
    config.pin_d0 = CAMERA_CONFIG_PIN_Y2;
    config.pin_d1 = CAMERA_CONFIG_PIN_Y3;
    config.pin_d2 = CAMERA_CONFIG_PIN_Y4;
    config.pin_d3 = CAMERA_CONFIG_PIN_Y5;
    config.pin_d4 = CAMERA_CONFIG_PIN_Y6;
    config.pin_d5 = CAMERA_CONFIG_PIN_Y7;
    config.pin_d6 = CAMERA_CONFIG_PIN_Y8;
    config.pin_d7 = CAMERA_CONFIG_PIN_Y9;
    config.pin_xclk = CAMERA_CONFIG_PIN_XCLK;
    config.pin_pclk = CAMERA_CONFIG_PIN_PCLK;
    config.pin_vsync = CAMERA_CONFIG_PIN_VSYNC;
    config.pin_href = CAMERA_CONFIG_PIN_HREF;
    config.pin_sccb_sda = CAMERA_CONFIG_PIN_SCCB_SDA;
    config.pin_sccb_scl = CAMERA_CONFIG_PIN_SCCB_SCL;
    config.pin_pwdn = CAMERA_CONFIG_PIN_PWDN;
    config.pin_reset = CAMERA_CONFIG_PIN_RESET;

    config.xclk_freq_hz = CAMERA_CONFIG_CLK_FREQ_HZ;
    config.pixel_format = PIXFORMAT_JPEG;

    config.frame_size = lookup_frame_size(DEFAULT_FRAME_SIZE);
    config.jpeg_quality = DEFAULT_JPEG_QUALITY;
    config.fb_count = CAMERA_CONFIG_FB_COUNT;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed: 0x%x\n", err);
        return;
    }
    Serial.printf("Camera init successful with frame size %s and JPEG quality %d\n", DEFAULT_FRAME_SIZE, DEFAULT_JPEG_QUALITY);
}

// ======== MJPEG STREAM HANDLER ========
void handleStream()
{
    WiFiClient client = server.client();
    char size_buf[16];

    // Send HTTP headers
    client.write("HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: multipart/x-mixed-replace; boundary=" STREAM_CONTENT_BOUNDARY "\r\n\r\n");

    while (client.connected()) {
        // Yield to allow WebServer to handle other requests
        server.handleClient();

        // Boundary and content-type header
        client.write("\r\n--" STREAM_CONTENT_BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: ");

        // Capture frame
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            delay(10);
            continue;
        }

        // Send size
        snprintf(size_buf, sizeof(size_buf), "%zu\r\n\r\n", fb->len);
        client.write(size_buf);

        // Send JPEG data
        client.write(fb->buf, fb->len);

        esp_camera_fb_return(fb);
        stream_frames++;
        printFPS();
    }

    Serial.println("Stream client disconnected");
    client.stop();
}

void setupStream()
{
    server.on("/stream", handleStream);
}

// ======== SIMPLE CONTROL ENDPOINT ========
#define LED_PIN 4

void setupControl()
{
    server.on("/gpio", [](void) {
        if (!server.hasArg("pin") || !server.hasArg("state")) {
            server.send(400, "text/plain", "Missing pin/state");
            return;
        }

        int pin = server.arg("pin").toInt();
        float state = server.arg("state").toFloat();

        // validate pin is allowed
        bool valid = false;
        for (int p : gpioPins) {
            if (p == pin) {
                valid = true;
                break;
            }
        }

        if (!valid) {
            server.send(403, "text/plain", "Pin not allowed");
            return;
        }

        // find index
        int idx = -1;
        for (size_t i = 0; i < gpioPins.size(); i++) {
            if (gpioPins[i] == pin) {
                idx = i;
                break;
            }
        }

        if (idx == -1) {
            server.send(500, "text/plain", "Internal error");
            return;
        }

        // ===== MODE SWITCHING =====

        if (state == 0.0f || state == 1.0f) {
            // DIGITAL MODE

            ledcDetachPin(pin);  // safe even if not attached
            pinMode(pin, OUTPUT);
            digitalWrite(pin, (int)state);

        } else {
            // PWM MODE

            int channel = idx; // stable mapping
            ledcSetup(channel, 5000, 8);
            ledcAttachPin(pin, channel);

            int duty = constrain((int)(state * 255), 0, 255);
            ledcWrite(channel, duty);
        }

        gpioStates[idx] = state;

        String resp = "OK pin=" + String(pin) + " state=" + String(state);
        server.send(200, "text/plain", resp);
    });
}

// ======== SETUP ========
void setup()
{
    Serial.begin(921600);

    setupWiFi();
    initCamera();
    setupStream();
    setupControl();

    server.begin();

    Serial.println("Server started");
}

// ======== LOOP ========
void loop()
{
    ArduinoOTA.handle();
    server.handleClient();
}
