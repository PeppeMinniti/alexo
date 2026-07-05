#pragma once
// ============================================================================
//  ALEXO - Controllo volume del VS1053 (scala 0..100, salvato in NVS).
//
//  Due core: l'encoder gira sul TASK del gobbo (core 0) e accumula le richieste
//  con volumeRequest(); il VS1053 sta sul bus SPI del core 1, quindi e' SEMPRE
//  il core 1 ad applicarle con volumeApplyPending() (nel loop a riposo e dentro
//  lo streaming TTS mentre Alexo parla). Cosi' non si litiga sul bus.
// ============================================================================
#include <Arduino.h>
#include <VS1053.h>

// Carica il volume salvato (o VOLUME_DEFAULT) e lo applica al player.
void volumeBegin(VS1053 &player);

// Chiamata dal task encoder (core 0): accumula "detents" (>0 / <0). Non tocca
// l'hardware, si limita a registrare la richiesta.
void volumeRequest(int32_t detents);

// Imposta un valore ASSOLUTO 0..100 (dal pannello web, core 0/altro task): non
// tocca l'hardware, lascia che sia il core 1 ad applicarlo con volumeApplyPending.
void volumeSet(int percent);

// Chiamata dal core 1: se c'e' una richiesta in sospeso aggiorna il volume,
// lo scrive sul VS1053 e lo salva. Ritorna true se il volume e' cambiato.
bool volumeApplyPending(VS1053 &player);

// Volume corrente "utente" (0..100), per display/pannello.
uint8_t volumeGet();

// Valore da passare a player.setVolume(): il volume utente 1..100 rimappato nella
// zona UDIBILE del VS1053 (VOLUME_VS_MIN..100), 0 se muto. Da usare in TUTTE le
// chiamate a setVolume (tts/music/volume) al posto di volumeGet().
uint8_t volumeVsValue();

// true se il volume utente e' 0 (muto vero: silenzio + ampli da spegnere).
bool volumeIsMuted();
