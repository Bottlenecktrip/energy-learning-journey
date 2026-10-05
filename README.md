# Energy Learning Journey

Interactive power grid exhibit I built during my internship with SP's School of Electrical & Electronic Engineering, for SP Group, CHINT and Siemens.

The idea: you pick a zone, Generation, Transmission, or Distribution, using a joystick, a button, or your own phone, and the exhibit reacts two ways at once. A 3D model of the grid flies over to that zone in the browser, and on the physical diorama next to it, the matching LED lights up at the same moment. A buzzer beeps to confirm it too, so picking a zone always feels certain.

**Try it:** https://bottlenecktrip.github.io/energy-learning-journey/

## How it works

The 3D model (`index.html`) is built entirely in Three.js. Every tower, cable and building is just boxes, cylinders and curved tubes, assembled and lit by hand instead of dropped in from a modelling tool. All seven stations are clickable, each with its own camera angle.

Behind the scenes, an ESP32 (`firmware/hub`) runs a small Wi-Fi server that keeps the physical model and this web page talking to each other. Trigger a zone from either side and the other one follows within about a second. A second ESP32, built into a touchscreen remote (`firmware/remote`), talks to the hub wirelessly over ESP-NOW, so a presenter can drive the whole exhibit from across the room with no wire at all.

Then there's the physical model itself, an actual scale build with a stand-in for Jurong Power Station, solar panels and hydrogen fuel cells, a transmission run of towers and transformers, and a distribution zone of lit HDB blocks down to an EV bay and battery unit. `docs/WIRING.txt` has the full pin map if you want to see how it's put together.

## Repo layout

```
index.html            the 3D model, no build step, just open it
firmware/hub/          ESP32 hub: joystick, button, buzzer, zone LEDs, Wi-Fi API
firmware/remote/       ESP32 + touchscreen remote: talks to the hub over ESP-NOW
docs/WIRING.txt        pin map for the physical build
```

## Flashing it yourself

Board is an ESP32 Dev Module, and it needs the "Huge APP (3MB No OTA/1MB SPIFFS)" partition scheme or the sketch won't fit. Before uploading, swap in your own network at the top of both sketches:

```cpp
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
```

The remote needs its display driver settings passed in at compile time instead of through the Arduino IDE. There's a comment block at the top of `firmware/remote/remote.ino` explaining how.
