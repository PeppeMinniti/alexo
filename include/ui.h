#pragma once
// ============================================================================
//  ALEXO - UI luminosa: animazioni del ring NeoPixel guidate da uno stato.
//  Gira su un task dedicato sul CORE 0, cosi' le animazioni restano fluide
//  anche mentre il core 1 (loop principale) e' bloccato su STT/Claude/TTS.
// ============================================================================
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

enum AlexoState {
  ST_IDLE,       // a riposo: respiro arcobaleno
  ST_LISTENING,  // ascolto: VU-meter dal microfono
  ST_THINKING,   // elaborazione: cometa che gira
  ST_SPEAKING,   // parla: pulsazione
  ST_ERROR,      // errore: lampeggio rosso
  ST_OTA,        // aggiornamento OTA: cometa verde
  ST_MUSIC       // musica: arcobaleno psichedelico reattivo al livello audio
};

// Avvia il task di animazione (core 0). Il ring dev'essere gia' inizializzato.
void uiBegin(Adafruit_NeoPixel *ring);

// Cambia lo stato animato (thread-safe, non blocca).
void uiSetState(AlexoState s);

// Livello 0..255 per il VU-meter in ascolto.
void uiSetLevel(uint8_t level);
