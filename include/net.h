#pragma once
// ============================================================================
//  ALEXO - WiFi
// ============================================================================
#include <Arduino.h>

// Connette al WiFi (credenziali in secrets.h). Ritorna true se connesso entro
// timeoutMs. Mostra avanzamento sulla seriale.
bool wifiBegin(uint32_t timeoutMs = 15000);

// true se attualmente connesso.
bool wifiOk();

// Avvia la sincronizzazione dell'orologio via NTP con fuso ITALIANO (ora legale
// automatica). Da chiamare una volta dopo la connessione WiFi. Non blocca: l'ora
// arriva dopo qualche secondo.
void timeBegin();

// Ritorna data e ora correnti in italiano + fuso/UTC, pronte da dare a Claude,
// es. "martedì 1 luglio 2026, ore 21:35 (Europe/Rome, UTC+2)". Stringa VUOTA se
// l'orologio non e' ancora stato sincronizzato (NTP non ancora risposto).
String nowContextString();
