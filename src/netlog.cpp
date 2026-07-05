// ============================================================================
//  ALEXO - Log via rete (Telnet). Vedi netlog.h.
//  Un solo client alla volta: se ne arriva un altro, sostituisce il precedente.
//  Tutte le operazioni sono non bloccanti per non disturbare il loop OTA.
// ============================================================================
#include "netlog.h"
#include <WiFi.h>

static WiFiServer *s_server = nullptr;
static WiFiClient  s_client;

void netlogBegin(uint16_t port) {
  if (s_server) return;                 // gia' avviato
  s_server = new WiFiServer(port);
  s_server->begin();
  s_server->setNoDelay(true);
}

void netlogHandle() {
  if (!s_server) return;
  // Nuovo client in attesa? Accettalo (sostituendo l'eventuale precedente).
  if (s_server->hasClient()) {
    WiFiClient nc = s_server->available();
    if (s_client && s_client.connected()) s_client.stop();
    s_client = nc;
    s_client.setNoDelay(true);
    s_client.println("[netlog] connesso ad Alexo");
  }
  // Scarta l'eventuale input del client (non lo usiamo, ma va svuotato).
  if (s_client && s_client.connected()) {
    while (s_client.available()) s_client.read();
  }
}

void netlogPrintln(const char *s) {
  if (s_client && s_client.connected()) s_client.println(s);
}

bool netlogConnected() {
  return s_client && s_client.connected();
}
