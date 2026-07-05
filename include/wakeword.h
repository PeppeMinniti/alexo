#pragma once
// ============================================================================
//  ALEXO - Wake word locale "Alexo" (microWakeWord). Vedi WAKEWORD.md.
//
//  Pipeline: I2S 16kHz continuo -> feature frontend (40 mel/10ms) -> modello
//  INT8 streaming (inferenza ogni 20ms) -> probabilita' -> soglia+debounce ->
//  trigger (stesso ingresso del click encoder). Il click resta come fallback.
//
//  STATO: scaffold. L'inferenza TFLite e il modello NON ci sono ancora (work in
//  progress): con WAKE_ENABLE=0 e' tutto inerte e il firmware e' invariato.
//  L'API qui sotto e' il "seam" stabile su cui agganciare TFLite Micro.
// ============================================================================
#include <Arduino.h>

// Inizializza il wake word (alloca arena in PSRAM, carica il modello, prepara il
// frontend). Ritorna false se non disponibile (es. PSRAM mancante o stub).
// No-op che ritorna false finche' l'inferenza non e' implementata.
bool wakeBegin();

// Da chiamare a riposo con un blocco di campioni PCM 16-bit mono a 16 kHz (gli
// STESSI letti per il livello del ring, per non leggere l'I2S due volte).
// Accumula le stride, genera le feature, esegue l'inferenza streaming e
// applica soglia+debounce. Ritorna true SOLO nel frame in cui "Alexo" e'
// riconosciuto (un colpo solo, poi serve un nuovo trigger).
bool wakeFeed(const int16_t *samples, size_t n);

// true se il wake word e' attivo e pronto (modello caricato).
bool wakeReady();

// Azzera lo stato del rilevamento (finestra probabilita' + frontend). Da chiamare
// dopo un'interazione, prima di riprendere l'ascolto, per non auto-ritriggerare.
void wakeReset();

// Ultima probabilita' (0..255) calcolata dall'ultima inferenza, per debug/tuning.
uint8_t wakeLastProb();

// Test della catena senza mic: genera audio sintetico, lo passa a
// wakeFeed e stampa probabilita'/tempi su Serial+Telnet. Chiamabile in loop.
// No-op se ne' WAKE_ENABLE ne' WAKE_TEST.
void wakeSelfTest();
