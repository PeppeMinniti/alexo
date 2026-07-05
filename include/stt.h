#pragma once
// ============================================================================
//  ALEXO - Speech-to-Text (OpenAI Whisper)
// ============================================================================
#include <Arduino.h>

// Invia un WAV (header+PCM) all'endpoint Whisper di OpenAI e ritorna il testo
// trascritto. Stringa vuota in caso di errore (loggato sulla seriale).
// lang = codice ISO ("it", "en", ...) per migliorare la trascrizione.
String sttTranscribe(const uint8_t *wav, size_t wavLen, const char *lang = "it");
