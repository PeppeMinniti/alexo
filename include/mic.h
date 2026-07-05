#pragma once
// ============================================================================
//  ALEXO - Microfono MAX4466 (analogico) campionato su ADC1
//  Registra PCM 16-bit mono a MIC_SAMPLE_RATE in un buffer PSRAM e produce
//  un file WAV pronto da inviare allo speech-to-text.
// ============================================================================
#include <Arduino.h>

// Inizializza l'ADC e alloca i buffer in PSRAM. Ritorna false se la PSRAM
// non e' disponibile o l'allocazione fallisce.
bool micBegin();

// Registra finche' keepGoing() ritorna true, oppure fino a maxMs.
// onLevel(0..255), se passato, viene chiamato ~ogni 20 ms col livello audio
// del momento (utile per un VU-meter su LED/display).
// Ritorna il numero di campioni registrati.
// silenceMs > 0: stop automatico dopo quel tanto di silenzio continuo (dopo aver
// sentito parlare). 0 = disattivato (si ferma solo a keepGoing()==false o a maxMs).
size_t micRecord(uint32_t maxMs, bool (*keepGoing)(), void (*onLevel)(uint8_t) = nullptr, uint32_t silenceMs = 0);

// Svuota il buffer DMA del mic (scarta l'audio accumulato). Da chiamare dopo una
// interazione, prima di riprendere l'ascolto del wake word (evita falsi trigger).
void micFlush();

// Legge fino a 'maxn' campioni PCM 16-bit grezzi dal mic (un o piu' i2s_read,
// bloccante ~maxn/16 ms). PCM "grezzo" (solo shift, NIENTE passa-alto): serve al
// wake word, il cui frontend ha gia' filtri propri (filterbank da 125 Hz).
// Ritorna i campioni effettivamente letti. 0 se non disponibile.
size_t micReadChunk(int16_t *out, size_t maxn);

// Livello sonoro (0..255) per il ring calcolato da un blocco PCM 16-bit gia'
// pronto (passa-alto + noise-floor auto-calibrante + envelope). Lo usa il loop
// del wake word per pilotare il ring con lo STESSO chunk del modello.
uint8_t micLevelFromChunk(const int16_t *s, size_t n);

// Diagnostica del rumore di fondo del mic I2S (Step 0 wake-word). SINGLE-SHOT:
// fa una misura (~600 ms) e stampa una riga sul monitor seriale, poi ritorna.
// Va chiamata in loop alternata a ArduinoOTA.handle() cosi' l'OTA resta vivo.
// Stampa il livello AC e il picco in dominio 24 bit e la proiezione su vari
// I2S_SHIFT. Significativa solo col mic I2S; col MAX4466 stampa una nota.
void micDiag();

// Lettura rapida del livello sonoro ambientale (0..255), per animazioni
// reattive a riposo. Campiona una finestrella brevissima (~2 ms) e ritorna
// l'ampiezza picco-picco scalata. Non interferisce con la registrazione.
uint8_t micPeekLevel();

// Ultimi valori "live" del percorso LED (micLevelFromChunk), per il pannello web:
// livello 0..255, il rumore di fondo stimato e la soglia di accensione correnti.
// Aggiornati a ogni chunk letto a riposo. Utile per tarare i parametri LED/silenzio
// guardandoli in tempo reale dal browser.
void micGetLive(uint8_t *level, float *noiseFloor, float *thresh);

// Accesso ai dati dell'ultima registrazione.
const int16_t *micPcm();         // campioni PCM 16-bit mono
size_t         micSampleCount(); // quanti campioni
uint32_t       micSampleRate();  // frequenza di campionamento (Hz)
int            micLastPeak();    // picco assoluto dell'ultima registrazione (0..32767)
bool           micHeardVoice();  // true se e' stata rilevata voce vera (sopra soglia adattiva)

// Costruisce in PSRAM un WAV completo (header 44 byte + PCM) dall'ultima
// registrazione. Ritorna il puntatore e scrive la lunghezza totale in *len.
// Il buffer resta valido fino alla registrazione successiva.
const uint8_t *micWav(size_t *len);
