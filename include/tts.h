#pragma once
// ============================================================================
//  ALEXO - Text-to-Speech (ElevenLabs) con riproduzione sul VS1053
// ============================================================================
#include <Arduino.h>
#include <VS1053.h>

// Sintetizza "text" con ElevenLabs e lo riproduce in streaming sul VS1053.
// Ritorna true se ha riprodotto audio. Il player dev'essere gia' inizializzato.
// voiceId: Voice ID ElevenLabs da usare. Se vuoto ("") usa la voce di default.
bool ttsSpeak(VS1053 &player, const String &text, const String &voiceId = "");
