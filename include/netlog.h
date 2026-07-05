#pragma once
// ============================================================================
//  ALEXO - Log via rete (Telnet, porta 23). Serve a leggere l'output quando
//  l'USB non e' accessibile (sistema assemblato nel case): ci si collega con
//  `telnet alexo.local` (o PuTTY in modalita' Raw/Telnet su alexo.local:23) e si
//  vedono le righe inviate con netlogPrintln().
//
//  E' progettato per NON bloccare e per CONVIVERE con ArduinoOTA: netlogHandle()
//  va chiamata spesso nel loop accanto ad ArduinoOTA.handle(). L'OTA resta sempre
//  prioritario e funzionante.
// ============================================================================
#include <Arduino.h>

// Avvia il server Telnet sulla porta indicata (default 23). Idempotente.
void netlogBegin(uint16_t port = 23);

// Accetta/mantiene il client (non blocca). Chiamala nel loop.
void netlogHandle();

// Invia una riga al client Telnet, se connesso (altrimenti no-op).
void netlogPrintln(const char *s);

// true se c'e' un client Telnet collegato.
bool netlogConnected();
