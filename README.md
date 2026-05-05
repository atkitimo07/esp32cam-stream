# ESP32CAM-STREAM

Simple HTTP MJPEG streamer for ESP32CAM modules with optional night vision and LED control.

This software turns an ESP32CAM module into a **HTTP Motion JPEG streamer** with optional night vision control and LED management.

Supported protocols:

- HTTP Motion JPEG
  The HTTP JPEG streamer makes it possible to watch the camera stream directly in your browser.
  The URL is http://&lt;ip address&gt;/stream

This software supports the following ESP32-CAM modules:

- AI THINKER ESP32-CAM
- Seeed Studio XIAO ESP32S3 SENSE

The software provides a mDNS server to be easily discoverable on the local network.
It advertises HTTP (port 80).

## Required

- ESP32-CAM module (AI Thinker or Seeed Studio XIAO ESP32S3 Sense),
- USB to Serial (TTL level) converter, piggyback board ESP32-CAM-MB or other way to connect to the device,
- [**PlatformIO**](https://platformio.org/) software (free download)

## Boards

This software currently supports only the following ESP32-CAM modules:

| Board                           | CPU      | PSRAM | Image                     |
|---------------------------------|----------|-------|---------------------------|
| AI-Thinker ESP32-CAM            | ESP32    | 8MB   | ![AI-Thinker](assets/boards/ai_thinker_esp32-cam.jpg) |
| Seeed Studio XIAO ESP32S3 Sense | ESP32-S3 | 8MB   | ![AI-Thinker](assets/boards/seeed-studio-xiao-esp32s3-sense.jpg) |

## Installing and running PlatformIO

PlatformIO is available for all major operating systems: Windows, Linux and MacOS. It is also provided as a plugin to [Visual Studio Code](https://visualstudio.microsoft.com).
More information can be found at: [https://docs.platformio.org/en/latest/installation.html](https://docs.platformio.org/en/latest/installation.html) below the basics.

Install [Visual Studio Code](https://code.visualstudio.com) and install the PlatformIO plugin.

## Putting the ESP32-CAM in download mode

### ESP32-CAM Programming

When using the ESP32-CAM board, press and hold the GP0 button or connect GP0 to GND. Then short press the reset button before releasing the GP0 button.
This will put the ESP32-CAM board in download mode.

Use a USB to serial adapter to connect to the TX and RX pins on the board to upload the program.

## Compiling and deploying the software

Rename /include/secrets.example.cpp to /include/secrets.cpp and add in WiFi, AP, and OTA credentials.

Open a command line or terminal window and clone this repository from GitHub.

```sh
git clone <your-repo-url>
```

go into the folder

```sh
cd esp32cam-stream
```

Next, the firmware has to be build and deployed to the ESP32.
There are two flavours to do this; using the command line or the graphical interface of Visual Studio Code.

### Using the command line

Make sure you have the latest version of the Espressif toolchain.

```sh
pio pkg update -g -p espressif32
```

First the source code has to be compiled to build all targets

```sh
pio run
```

if only a specific target is required, for example the ```esp32cam_ai_thinker``` type:

```sh
pio run -e esp32cam_ai_thinker
```

When finished, firmware has to be uploaded.
Make sure the ESP32-CAM is in download mode (see previous section) and type:

```sh
 pio run -t upload
```

or, again, for a specific target, for example ```esp32cam_ai_thinker```

```sh
pio run -t upload -e esp32cam_ai_thinker
```

When done remove the jumper when using a FTDI adapter or press the reset button on the ESP32-CAM.
To monitor the output, start a terminal using:

```sh
 pio device monitor
```

### Using Visual studio

Open the project in a new window. Run the following tasks using the ```Terminal -> Run Task``` or CTRL+ALT+T command in the menu (or use the icons below on the toolbar). Make sure the ESP32-CAM is in download mode during the uploads.

- PlatformIO: Build (esp32cam)
- PlatformIO: Upload (esp32cam)

To monitor the behavior run the task, run: ```PlatformIO: Monitor (esp32cam)```

## Over-The-Air (OTA) Updates

Once the device is deployed and connected to your network, you can update the firmware wirelessly without needing to connect via USB.

### Using PlatformIO

To upload firmware via OTA using PlatformIO:

```sh
pio run -t upload --upload-port esp32cam-stream.local
```

Or for a specific board environment:

```sh
pio run -e esp32cam_ai_thinker -t upload --upload-port esp32cam-stream.local
```

### Using Arduino IDE

If using Arduino IDE with the ESP32 board support:

1. Select your ESP32 board
2. Go to Tools → Port → Network ports
3. Select `esp32cam-stream.local` (or the IP address if mDNS doesn't work)
4. Upload as normal

### OTA Security

- OTA updates require authentication
- The password is defined in `include/secrets.h` as `OTA_PASSWORD`
- You can change this by modifying the `OTA_PASSWORD` define

### OTA Process

1. The device must be connected to the same network as your development machine
2. OTA uploads happen on port 3232 (ArduinoOTA default)
3. Progress is shown in the serial monitor
4. The device will automatically reboot after successful update
5. If OTA fails, the device continues running the old firmware

## Configuration

The device connects to WiFi using credentials defined in `include/secrets.h`:

- `DEFAULT_STA_SSID`: Your WiFi network name
- `DEFAULT_STA_PASSWORD`: Your WiFi password
- `WIFI_PASSWORD`: Password for the access point mode

If valid WiFi credentials are provided, the device will connect to your network and be accessible at `http://esp32cam-stream.local/stream`.

If WiFi connection fails, the device creates an access point named "ESP32CAM-STREAM" with the password defined in `WIFI_PASSWORD`.

## Connecting to the MJPEG stream

The MJPEG stream is available at: [http://esp32cam-stream.local/stream](http://esp32cam-stream.local/stream)

Open this URL in any web browser to view the live camera feed.

## API

The device provides HTTP endpoints for camera streaming and control:

### GET: /stream

Returns a multipart MJPEG stream of the camera feed. Open this URL in a web browser to view the live stream.

### GET: /nightvision?state=<state>

Triggers a night vision light pulse. The state parameter accepts 0 or 1:
- `state=0`: Pulses GPIO_0 for 200ms (IR filter control)
- `state=1`: Pulses GPIO_1 for 200ms (IR filter control)

Example:
- `http://esp32cam-stream.local/nightvision?state=0`

### GET: /nightvision/state

Returns the current night vision bistable state as JSON.

Example response:
```json
{"state": 0}
```

### GET: /irled?state=<state> (AI-Thinker board only)

Controls the IR LED. The state parameter accepts 0.0 to 1.0:
- 0.0: IR LED off
- 1.0: IR LED on
- Between these values (eg. 0.5): PWM dimming

Example:
- `http://esp32cam-stream.local/irled?state=1` (IR LED on)

### GET: /irled/state (AI-Thinker board only)

Returns the current IR LED state as JSON.

Example response:
```json
{"state": 0.5}
```

## Default WiFi Credentials

You can set default WiFi credentials at compile time by defining these macros in the board JSON's `extra_flags`:

```json
"'-D DEFAULT_STA_SSID="Your-WiFi-SSID"'",
"'-D DEFAULT_STA_PASSWORD="your-password"'",
"'-D DEFAULT_STA_CONNECT_TIMEOUT=10000'"  // optional, in milliseconds
```

When default credentials are configured:
- The device will attempt to connect to the specified WiFi network at startup
- If the connection succeeds within the timeout, the device goes directly online
- If the connection fails, it falls back to AP mode
- If no default credentials are set, the device starts in AP mode as usual

## Troubleshooting

- If the device doesn't connect to WiFi, it will create an access point named "ESP32CAM-STREAM"
- Check the credentials in `include/secrets.h`
- The camera LED indicates connection status (may vary by board)
- If camera fails to initialize, check PSRAM availability and camera module connection
- Use `pio run -t erase` to reset the device if needed

### Power Requirements

Ensure stable 5V power supply. The ESP32 creates its own 3.3V internally.

### Camera Module

Verify the camera ribbon cable is properly seated with the camera lens facing away from the board.

## Credits

esp32cam-stream depends on PlatformIO and the ESP32 camera libraries.

## Notes

This is a simplified ESP32 camera streaming firmware focused on high-performance MJPEG streaming with basic control features. It does not include the full web configuration interface or RTSP streaming found in other ESP32 camera projects.
