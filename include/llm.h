#pragma once
// ============================================================================
//  ALEXO - Cervello (Claude / Anthropic Messages API)
// ============================================================================
#include <Arduino.h>

// Manda il testo dell'utente a Claude (col contesto della conversazione) e
// ritorna la risposta. Stringa vuota in caso di errore (loggato sulla seriale).
// Se 'musicReq' != nullptr, Claude ha a disposizione il tool "riproduci_musica":
// quando l'utente vuole ascoltare musica, invece di rispondere a voce Claude
// chiama il tool e qui viene messo il GENERE scelto in *musicReq (la risposta
// testo torna vuota). Vuoto = nessuna richiesta musicale.
String llmAsk(const String &userText, String *musicReq = nullptr);

// Azzera la memoria della conversazione (riparte da zero).
void llmReset();
