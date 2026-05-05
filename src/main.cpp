#include <WiFi.h>
#include <WebServer.h>
#include "esp_camera.h"
#include "lookup_camera_frame_size.h"
#include "settings.h"
#include "secrets.h"
#include <ArduinoOTA.h>

// ======== USER CONFIG ========
const char* ssid     = DEFAULT_STA_SSID;
const char* password = DEFAULT_STA_PASSWORD;

const char* ap_ssid = WIFI_SSID;
const char* ap_pass = WIFI_PASSWORD;



uint32_t last_ota_time = 0;

WebServer server(80);

static volatile uint32_t stream_frames = 0;
static uint32_t fps_last_ms = 0;

float ledState = 0.0f;  // Current LED state (0.0-1.0)

#if defined(NIGHT_VISION_GPIO_0) && defined(NIGHT_VISION_GPIO_1)
struct NightVisionState {
  bool active = false;
  int pin = -1;
  unsigned long start_ms = 0;
  int state = 0;  // Bistable state: 0=off, 1=on
};

NightVisionState nightVision;
const unsigned long NIGHT_VISION_PULSE_MS = 200;
#endif

#define STREAM_CONTENT_BOUNDARY "frame"

void handleLoop();

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

        // Handle other tasks like night vision timing
        handleLoop();
        printFPS();

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
    }

    Serial.println("Stream client disconnected");
    client.stop();
}

void setupStream()
{
    server.on("/stream", handleStream);
}

// ======== NIGHT VISION HANDLERS ========
#if defined(NIGHT_VISION_GPIO_0) && defined(NIGHT_VISION_GPIO_1)
void handle_night_vision() {
  if (!server.hasArg("state")) {
    server.send(400, "text/plain", "Missing 'state' parameter");
    return;
  }

  int state = server.arg("state").toInt();
  if (state != 0 && state != 1) {
    server.send(400, "text/plain", "Invalid state. Must be 0 or 1");
    return;
  }

  int pin = state == 0 ? NIGHT_VISION_GPIO_0 : NIGHT_VISION_GPIO_1;

  if (nightVision.active && nightVision.pin != pin) {
    digitalWrite(nightVision.pin, LOW);
  }

  nightVision.state = state;  // Update bistable state
  nightVision.active = true;
  nightVision.pin = pin;
  nightVision.start_ms = millis();
  digitalWrite(pin, HIGH);

  Serial.printf("Night vision state %d triggered on GPIO %d\n", state, pin);
  String resp = "OK state=" + String(state);
  server.send(200, "text/plain", resp);
}

void handle_night_vision_state() {
  String resp = String("{\"state\":") + String(nightVision.state) + "}";
  server.send(200, "application/json", resp);
}
#endif

#if defined(IR_LED_PIN)
void handleIRLED()
{
    if (!server.hasArg("state")) {
        server.send(400, "text/plain", "Missing state parameter");
        return;
    }

    float state = server.arg("state").toFloat();

    // constrain to 0.0-1.0
    state = constrain(state, 0.0f, 1.0f);

    ledState = state;

    if (state == 0.0f || state == 1.0f) {
        // DIGITAL MODE
        ledcDetachPin(IR_LED_PIN);  // safe even if not attached
        pinMode(IR_LED_PIN, OUTPUT);
        digitalWrite(IR_LED_PIN, (int)state);
    } else {
        // PWM MODE
        ledcSetup(0, 5000, 8);  // channel 0 for LED
        ledcAttachPin(IR_LED_PIN, 0);
        int duty = (int)(state * 255);
        ledcWrite(0, duty);
    }

    String resp = "OK state=" + String(state);
    server.send(200, "text/plain", resp);
}

void handleIRLEDState()
{
    String resp = "{\"state\":" + String(ledState) + "}";
    server.send(200, "application/json", resp);
}
#endif

// ======== CONTROL ENDPOINTS ========
void setupControl()
{
#if defined(NIGHT_VISION_GPIO_0) && defined(NIGHT_VISION_GPIO_1)
    server.on("/nightvision", handle_night_vision);
    server.on("/nightvision/state", handle_night_vision_state);
#endif

#if defined(IR_LED_PIN)
    server.on("/irled", handleIRLED);
    server.on("/irled/state", handleIRLEDState);
#endif
}

// ======== LOOP HANDLER ========
void handleLoop()
{
#if defined(NIGHT_VISION_GPIO_0) && defined(NIGHT_VISION_GPIO_1)
    if (nightVision.active && millis() - nightVision.start_ms >= NIGHT_VISION_PULSE_MS) {
        digitalWrite(nightVision.pin, LOW);
        nightVision.active = false;
        Serial.printf("Night vision state %d on GPIO %d deactivated after pulse\n", nightVision.state, nightVision.pin);
    }
#endif
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
    handleLoop();
}
