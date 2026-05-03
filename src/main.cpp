#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "esp_camera.h"
#include "lookup_camera_frame_size.h"
#include "settings.h"
#include <vector>
#include <memory>

// ======== USER CONFIG ========
const char* ssid     = DEFAULT_STA_SSID;
const char* password = DEFAULT_STA_PASSWORD;

const char* ap_ssid = WIFI_SSID;
const char* ap_pass = WIFI_PASSWORD;

std::vector<int> gpioPins;
std::vector<float> gpioStates;

// ======== SERVER SETUP ========

AsyncWebServer server(80);

static volatile uint32_t capture_frames = 0;
static volatile uint32_t stream_frames = 0;
static uint32_t fps_last_ms = 0;

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
void captureFrame()
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return;
    esp_camera_fb_return(fb);
    capture_frames++;
}

void setupStream()
{
    server.on("/stream", HTTP_GET, [=](AsyncWebServerRequest *request) {
        struct StreamState {
            camera_fb_t *fb = nullptr;
            size_t offset = 0;
        };

        auto state = std::make_shared<StreamState>();

        AsyncWebServerResponse *response = request->beginChunkedResponse(
            "multipart/x-mixed-replace; boundary=frame",
            [state](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {

                // Start of new frame - grab latest
                if (state->offset == 0) {
                    if (state->fb) {
                        esp_camera_fb_return(state->fb);
                    }
                    state->fb = esp_camera_fb_get();
                    if (!state->fb) {
                        return RESPONSE_TRY_AGAIN;
                    }
                }

                if (!state->fb) {
                    return RESPONSE_TRY_AGAIN;
                }

                String header = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                                String(state->fb->len) + "\r\n\r\n";

                if (state->offset < header.length()) {
                    size_t n = min(maxLen, header.length() - state->offset);
                    memcpy(buffer, header.c_str() + state->offset, n);
                    state->offset += n;
                    return n;
                }

                size_t img_offset = state->offset - header.length();
                size_t remaining = state->fb->len - img_offset;
                size_t n = min(maxLen, remaining);
                memcpy(buffer, state->fb->buf + img_offset, n);

                state->offset += n;

                if (state->offset >= header.length() + state->fb->len) {
                    state->offset = 0;
                    stream_frames++;
                }

                return n;
            }
        );

        response->addHeader("Cache-Control", "no-cache");
        request->send(response);
    });
}

// ======== SIMPLE CONTROL ENDPOINT ========
#define LED_PIN 4

void setupControl()
{
    server.on("/gpio", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("pin") || !request->hasParam("state")) {
            request->send(400, "text/plain", "Missing pin/state");
            return;
        }

        int pin = request->getParam("pin")->value().toInt();
        float state = request->getParam("state")->value().toFloat();

        // validate pin is allowed
        bool valid = false;
        for (int p : gpioPins) {
            if (p == pin) {
                valid = true;
                break;
            }
        }

        if (!valid) {
            request->send(403, "text/plain", "Pin not allowed");
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
            request->send(500, "text/plain", "Internal error");
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
        request->send(200, "text/plain", resp);
    });
}

// ======== FPS COUNTER (DEBUG) ========
void printFPS()
{
    if (millis() - fps_last_ms < 1000) return;

    fps_last_ms = millis();

    uint32_t cap = capture_frames;
    uint32_t str = stream_frames;

    capture_frames = 0;
    stream_frames = 0;

    Serial.printf("[FPS] capture: %lu | stream: %lu\n",
                  (unsigned long)cap,
                  (unsigned long)str);
}

// ======== SETUP ========
void setup()
{
    Serial.begin(115200);

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
    captureFrame();
    printFPS();
}
