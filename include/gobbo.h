#pragma once
// ============================================================================
//  ALEXO - "Gobbo" (teleprompter) sul display TFT ST7735 a colori.
//  Il testo scorre dolcemente verso l'alto come un gobbo da studio TV.
//  Gira su un task dedicato sul CORE 0 (unico proprietario del bus SPI HSPI
//  del TFT), cosi' lo scorrimento resta fluido anche mentre il core 1 e'
//  bloccato su STT/Claude/TTS. Il testo arriva da core 1 via coda (thread-safe).
//  Rende su un canvas in RAM e fa il blit a schermo intero -> niente flicker.
// ============================================================================
#include <Arduino.h>
#include <Adafruit_ST7735.h>
#include "ui.h"   // AlexoState (per l'header di stato a colori)

// Avvia il task del gobbo (core 0). Il display dev'essere gia' inizializzato.
// Pulisce subito lo schermo.
void gobboBegin(Adafruit_ST7735 *disp);

// Aggiorna l'indicatore di stato nell'header del TFT (pronto/ascolto/penso/...).
// Thread-safe (scrittura atomica di un byte). Chiamabile da qualsiasi core.
void gobboSetState(AlexoState s);

// Aggiunge una RISPOSTA di Alexo alla chat ("Alexo: ..."), seguita da una riga
// vuota. Parte con una velocita' di scroll di default (leggibile); chiamare
// subito dopo gobboScrollOver() per legarlo alla durata della voce.
void gobboPrint(const String &text);

// Aggiunge una DOMANDA dell'utente alla chat ("Tu: ..."). Mostra l'ultimo
// contenuto (scroll a fondo). Chiamabile da qualsiasi core.
void gobboPrintUser(const String &text);

// Imposta il tempo (ms) entro cui completare lo scroll fino in fondo: serve a
// sincronizzare lo scorrimento con la durata del parlato (chiamata da ttsSpeak
// quando l'audio inizia). Chiamabile da qualsiasi core.
void gobboScrollOver(uint32_t ms);

// Svuota il gobbo e pulisce lo schermo.
void gobboClear();

// Percentuale (0..100) dell'aggiornamento OTA in corso. La disegna la schermata
// OTA dedicata (stile HUD verde) quando lo stato e' ST_OTA. Chiamabile da core 1
// (callback ArduinoOTA): aggiorna solo un valore, il render lo fa il task del gobbo.
void gobboOtaProgress(uint8_t percent);

// Info del brano in onda (metadata ICY della radio): mostrate nella schermata
// dedicata "IN ONDA" quando lo stato e' ST_MUSIC (3 righe size 2 con marquee:
// emittente / titolo / artista). Chiamabile da core 1 (music.cpp).
void gobboNowPlaying(const char *station, const char *title, const char *artist);

// Tipo di aggiornamento OTA in corso: true = filesystem/pagina web ("DATA"),
// false = firmware ("FW"). Mostrato come sottotitolo nella schermata OTA.
void gobboOtaKind(bool isData);

// --- Input dall'encoder (core 0) verso la pipeline (core 1) -----------------
//  L'encoder e' l'UNICO comando: il click avvia/ferma la chat (toggle), il
//  task del gobbo lo interpreta e qui sotto lo espone al loop principale.
//
// true UNA volta (poi si azzera) se l'utente ha chiesto di AVVIARE la chat.
bool gobboTakeTalkRequest();
// true finche' e' pendente una richiesta di FERMARE la registrazione (click
// durante l'ascolto). Usata come condizione di stop in micRecord.
bool gobboStopRequested();
// Azzera la richiesta di stop: chiamare appena prima di iniziare a registrare.
void gobboClearStopRequest();
// Detenti "premuto+giro" accumulati DURANTE la musica (cambio stazione): ritorna
// il delta avanti/indietro e lo azzera. 0 se nessun seek richiesto. Attivo solo
// in ST_MUSIC (fuori dalla musica premuto+giro resta volume).
int32_t gobboTakeMusicSeek();

// --- Chat per il pannello web -----------------------------------------------
// Contatore di revisione: cambia a ogni nuovo messaggio in chat. Il pannello lo
// legge (in /api/live) e ricarica la chat SOLO quando e' cambiato.
uint32_t gobboChatRev();
// Accesso ai messaggi della chat per servirli in streaming (r: 0=Alexo, 1=utente,
// 2=sistema; testo UTF-8). gobboChatCount() = quanti; gobboChatItem() riempie role/text
// (puntatore valido per la durata della richiesta). Da chiamare dal core 1 (webui).
int  gobboChatCount();
bool gobboChatItem(int i, uint8_t *role, const char **text);
