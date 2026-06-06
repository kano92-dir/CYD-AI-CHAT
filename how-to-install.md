## Required Libraries

Install the following libraries through Arduino IDE Library Manager before uploading the firmware:

| Library             | Author                 |
| ------------------- | ---------------------- |
| TFT_eSPI            | Bodmer                 |
| XPT2046_Touchscreen | Paul Stoffregen        |
| ArduinoJson         | Benoit Blanchon        |
| Preferences         | Built-in ESP32 Library |
| WiFi                | Built-in ESP32 Library |
| HTTPClient          | Built-in ESP32 Library |
| WiFiClientSecure    | Built-in ESP32 Library |
| SPI                 | Built-in ESP32 Library |
| NTPClient           | Fabrice Weinberg       |
| WiFiUdp             | Built-in ESP32 Library |

---

## Installation

### 1. Install Arduino IDE

Download and install Arduino IDE:

https://www.arduino.cc/en/software

### 2. Install ESP32 Board Package

Open:

Tools → Board → Boards Manager

Search for:

ESP32

Install:

ESP32 by Espressif Systems

---

### 3. Install Required Libraries

Open:

Sketch → Include Library → Manage Libraries

Install all libraries listed above.

---

### 4. Configure TFT_eSPI

Open:

Documents/Arduino/libraries/TFT_eSPI/User_Setup.h

Configure the display according to your Cheap Yellow Display model.

---

### 5. Add Your OpenRouter API Key

Locate:

```cpp
static const char* API_KEY = "YOUR_OPENROUTER_API_KEY";
```

Replace it with your own OpenRouter API key.

---

### 6. Select Board

Tools → Board

Select:

ESP32 Dev Module

(or the board recommended for your CYD model)

---

### 7. Upload

Connect the ESP32 using USB.

Press Upload.

After flashing, configure Wi-Fi and start chatting.

---

## Supported Hardware

* ESP32-2432S028R (Cheap Yellow Display)

---

## Firmware Version

Current Release: v2.0.0
