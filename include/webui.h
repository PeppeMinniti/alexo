#pragma once
// ============================================================================
//  ALEXO - Pannello impostazioni via web (http://alexo.local/).
//  Web server sulla porta 80 (WebServer di serie, sincrono): serve la pagina da
//  LittleFS (cartella data/) ed espone una piccola API JSON per leggere/scrivere
//  i parametri (gSettings + volume) e per la lettura LIVE del microfono.
//  webuiHandle() va chiamato spesso dal loop (accanto a ArduinoOTA.handle()).
// ============================================================================
#include <Arduino.h>

// Monta LittleFS e avvia il web server. Ritorna false se LittleFS non monta
// (in quel caso la pagina non c'e', ma l'API JSON funziona lo stesso).
bool webuiBegin();

// Da chiamare nel loop: gestisce le richieste HTTP in arrivo.
void webuiHandle();
