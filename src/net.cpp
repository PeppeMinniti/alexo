// ============================================================================
//  ALEXO - WiFi
// ============================================================================
#include "net.h"
#include "secrets.h"
#include <WiFi.h>
#include <time.h>

bool wifiBegin(uint32_t timeoutMs) {
  Serial.printf("[wifi] connessione a \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeoutMs) {
    delay(250);
    Serial.print('.');
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[wifi] OK - IP %s, RSSI %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.println("\n[wifi] FALLITA (SSID/password? rete 2.4GHz?)");
  return false;
}

bool wifiOk() { return WiFi.status() == WL_CONNECTED; }

// --- Orologio via NTP (fuso Italia con ora legale automatica) ----------------
static const char *GIORNI[] = { "domenica", "lunedì", "martedì", "mercoledì",
                                "giovedì", "venerdì", "sabato" };
static const char *MESI[]   = { "gennaio", "febbraio", "marzo", "aprile", "maggio",
                                "giugno", "luglio", "agosto", "settembre",
                                "ottobre", "novembre", "dicembre" };

void timeBegin() {
  // TZ Europe/Rome: CET (UTC+1), CEST (UTC+2) con passaggio ora legale automatico.
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3",
               "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  Serial.println("[time] sincronizzazione NTP avviata (fuso Europe/Rome)");
}

String nowContextString() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "";   // orologio non ancora sincronizzato (< nov 2023)
  struct tm lt, gt;
  localtime_r(&now, &lt);             // ora locale (Europe/Rome, con ora legale)
  gmtime_r(&now, &gt);               // ora UTC
  char buf[200];
  // Diamo SIA l'ora locale SIA l'UTC: Claude calcola gli altri fusi dall'UTC.
  snprintf(buf, sizeof(buf),
           "%s %d %s %d, ore locali %02d:%02d (Europe/Rome); nello stesso istante in UTC sono le %02d:%02d",
           GIORNI[lt.tm_wday], lt.tm_mday, MESI[lt.tm_mon], lt.tm_year + 1900,
           lt.tm_hour, lt.tm_min, gt.tm_hour, gt.tm_min);
  return String(buf);
}
