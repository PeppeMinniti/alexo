#pragma once
// ============================================================================
//  ALEXO - Impostazioni RUNTIME modificabili dal pannello web (webui.cpp).
//  I parametri che prima erano #define fissi (config.h / dentro i .cpp) ora
//  vivono in questa struct, caricata dall'NVS all'avvio (default = config.h) e
//  salvata quando l'utente li cambia dal pannello. Cosi' si tarano senza
//  ricompilare. Il volume resta gestito da volume.cpp (gia' in NVS).
// ============================================================================
#include <Arduino.h>

struct AlexoSettings {
  // --- Mic / stop-al-silenzio / LED ---
  uint32_t recSilenceMs;      // silenzio continuo prima dello stop registrazione
  float    recSilenceMargin;  // soglia voce = noiseFloor * margin + floor
  int      recSilenceFloor;   // margine minimo assoluto (RMS)
  float    micLvlMargin;      // LED reattivi: quanto sopra il fondo per accendere
  float    micLvlFloor;       // LED reattivi: margine minimo assoluto
  float    micLvlAttack;      // LED reattivi: velocita' di salita (reattivita') 0..1
  float    micLvlRelease;     // LED reattivi: velocita' di discesa (permanenza) 0..1
  bool     idleReactive;      // LED "ballano" col suono a riposo (on/off)

  // --- Wake word "Okay Nabu" ---
  int      wakeGain;          // guadagno digitale del percorso wake
  int      wakeProbCutoff;    // soglia probabilita' 0..255
  int      wakeWindow;        // ampiezza finestra mobile (1..16)

  // --- Audio ---
  String   voiceId;           // Voice ID ElevenLabs di default
  String   voiceIdAlt;        // Voice ID alternativo (usato col trigger)
  String   voiceTrigger;      // parola iniziale che attiva la voce alternativa (vuoto = off)

  // --- Cervello (Claude) ---
  String   llmModel;          // es. claude-haiku-4-5 / claude-opus-4-8
  String   systemPrompt;      // "personalita'" di Alexo

  // --- Easter-egg ---
  String   eggTerms;          // termini trigger separati da virgola (vuoto = off)

  // --- Filtro anti-allucinazione Whisper ---
  String   hallucTerms;       // frasi-fantasma da scartare, separate da virgola (vuoto = off)

  // --- Musica (web-radio) ---
  String   musicStations;     // stazioni, una per riga "chiave | nome | url"
};

extern AlexoSettings gSettings;

void settingsBegin();          // carica da NVS (o default di config.h)
void settingsSave();           // scrive TUTTO in NVS (permanente)
void settingsResetDefaults();  // riporta ai default di fabbrica (config.h) e salva
