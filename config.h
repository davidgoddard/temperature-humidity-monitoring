#pragma once
// SINGLE shared configuration, included by both Arduino sketches.
// Change these values for your home network.
#define WIFI_SSID       "artDeco"
#define WIFI_PASSWORD   "homeDeco1234"
#define BASE_HOST       "temp"          // mDNS host label, without .local
#define BASE_PORT       80
#define SAMPLE_SECONDS  120
// Optional: when mDNS is blocked by the mesh, enter reserved IP here.
// Leave empty to use temp.local exclusively.
#define BASE_FALLBACK_IP ""
