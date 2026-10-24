ESP32 Wi-Fi temperature sensors, shared settings and temp.local dashboard
====================================================================

Files:
  config.h                          SINGLE shared Wi-Fi config
  sensor_node/sensor_node.ino       battery sensor (SHT40, GPIO4/5)
  base_station/base_station.ino     base receiver / logger / web server
  base_station/dashboard_page.h    embedded dashboard source (generated)
  dashboard.html                    browser page source (also embedded)

1. Edit root config.h: SSID, password, SAMPLE_SECONDS and optional fallback IPv4.
2. Configure LittleFS partitions for the sensor and base. NOTE: neither sketch
   auto-formats an unmounted filesystem. Provision new boards deliberately.
3. For each sensor choose a unique DEVICE_ID in sensor_node.ino.
4. Upload base_station/base_station.ino and then sensor_node/sensor_node.ino.
5. Open http://temp.local/ in a browser on your home Wi-Fi. The base advertises
   this name using mDNS; the sensor resolves 'temp' with MDNS.queryHost().
6. The base connects to NTP for time. Sensor measurements persist in LittleFS
   and are uploaded in batches when Wi-Fi/base station become available.

SHARED HEADER / ARDUINO IDE NOTE:
Both sketches use #include "../config.h" (one master configuration).
This works when the sketch build preserves the sibling directory structure,
but some Arduino IDE/CLI build flows copy the sketch into a temporary directory,
so a parent-relative include MAY FAIL (file not found). If that happens, use
"config.h" and place a copy in each sketch folder, or use a proper shared
Arduino library header. Do not silently reconfigure with different settings.

DASHBOARD:
Base serves the page and API on the same origin: http://temp.local/.
Browser history is saved in IndexedDB for the current browser/profile.
Dated CSV DOWNLOAD works over http://temp.local but does not prove that a file
was durably saved to the intended folder, and DOES NOT automatically delete the
base records. This is deliberate data protection.

A web browser cannot use showDirectoryPicker() from ordinary http://temp.local,
because it is not a secure context. To enable chosen-folder direct writes and
archive acknowledgement, load the included dashboard.html from localhost
(python3 -m http.server 8080) or host via HTTPS, set Base URL to
http://temp.local (browser private-network/CORS permissions may apply), then
Choose archive folder -> Save snapshot + acknowledge.

If the browser says "blocked mixed content" or restricts private-network
requests, use an HTTPS reverse proxy on your LAN that also proxies the API.

IMPORTANT SECURITY AND DATA LIMITATIONS:
* Trusted local network ONLY: HTTP archive-ack endpoint is unauthenticated and
  can delete data if invoked maliciously. Do not port-forward or expose online.
* Firmware has not been compiled/tested on your board. Confirm mDNS across your
  mesh; fallback IP is provided in config.h when multicast discovery fails.
* LittleFS logs can fill. Base compaction requires space for a second file.
* RTC estimates drift and reset after battery change; un-timed readings are
  approximately backfilled using sequence differences.
* Tests recommended: send/ack failure, interrupted compaction, battery swap,
  and archive ACK after a verified local file save.
* If changing dashboard.html, regenerate dashboard_page.h from the HTML.

DASHBOARD STYLING:
Sensor cards now use the same distinct colour as their graph lines and legend.
The dashboard has more spacing between controls, cards, and graph.
When changing dashboard.html, regenerate base_station/dashboard_page.h before flashing;
this package already contains the matching generated header.

RESET FEATURE
-------------
Open http://temp.local and use "Reset all readings…" at the bottom.
This removes base /measurements.csv and compaction files, clears deduplication
metadata in base Preferences/NVS, and clears IndexedDB history plus archive
tracking in the current browser. Existing CSV downloads on your computer
are NOT deleted. Other browsers detect a changed dataset ID on refresh and
clear their old history too. Sensors are not erased and continue uploading.

The /api/reset endpoint uses a confirmation phrase, NOT authentication.
Keep this device on a trusted LAN. The base's reset does not secure-erase
physical flash.

If the reset succeeds on the base but fails in the browser, refreshing the
dashboard will clear stale history when the dataset identifier changes.
