#pragma once
// ============================================================================
//  ALEXO - Riproduzione musica (web-radio MP3 -> VS1053).
//  Le stazioni sono stream MP3 in HTTP semplice (181.fm): il VS1053/VS1003
//  decodifica l'MP3 in hardware, quindi si riusa lo stesso feed a pezzetti
//  della TTS (tts.cpp), solo che la sorgente e' uno stream INFINITO.
//  Comando vocale: "metti/suona <genere>" (rock/pop/country/anni 80/anni 90).
//  Fase 1 (semplice): stazioni FISSE qui sotto. Fase 2 (elegante): editabili
//  dal pannello web + tool di Claude.
// ============================================================================
#include <Arduino.h>
#include <VS1053.h>

// Una stazione: URL dello stream MP3 + etichetta da mostrare sul gobbo.
struct MusicStation { const char *url; const char *nome; };

// Riconosce un comando musicale nel testo (GIA' minuscolo) confrontandolo con la
// lista editabile del pannello (gSettings.musicStations). Ritorna la stazione da
// suonare, oppure nullptr se non e' un match locale.
const MusicStation *musicMatch(const String &testoLower);

// Fallback fase 2b: mappa un GENERE scelto da Claude (tool riproduci_musica) a una
// stazione del catalogo interno verificato. nullptr se il genere non e' in catalogo.
const MusicStation *musicFromGenre(const String &genere);
// Elenco dei generi del catalogo, per la descrizione del tool di Claude.
String musicCatalogList();

// Riproduce lo stream MP3 sul VS1053 finche' stopRequested() non ritorna true
// (click encoder), musicRequestStop() non viene chiamata (pulsante del pannello
// web) o lo stream cade. Se seekRequested() ritorna un delta != 0 (premuto+giro
// in musica), interrompe e RITORNA quel delta, cosi' il chiamante passa a un'altra
// stazione; ritorna 0 se la riproduzione e' finita o fermata. Blocca il core 1 per
// tutta la durata, ma tiene VIVI OTA, Telnet e il pannello web (flash solo via OTA).
int musicPlay(VS1053 &player, const char *url, bool (*stopRequested)(), int (*seekRequested)());

// --- Navigazione della lista stazioni del pannello (per il seek premuto+giro) --
// Numero di stazioni valide in gSettings.musicStations.
int  musicStationCount();
// Riempie url/nome con la stazione all'indice idx (0-based). false se fuori range.
bool musicStationGet(int idx, String &url, String &nome);
// Indice della stazione con quell'URL nella lista, oppure -1 se non presente.
int  musicStationIndexOf(const char *url);

// Chiede lo stop della riproduzione (chiamata dal pannello web, thread-safe).
void musicRequestStop();
// true mentre uno stream sta suonando (per lo stato nel pannello web).
bool musicIsPlaying();
// MAD grezzo e baseline dell'ultimo chunk musicale (per tarare la sensibilita'
// del ring: campi musMad/musBase in /api/live). 0 se non sta suonando.
float musicLastMad();
float musicLastBase();

// Metadata ICY dello stream in corso: nome emittente (icy-name) e brano corrente
// ("Artista - Titolo" da StreamTitle). Stringa vuota se assenti / non in musica.
String musicStation();
String musicNowPlaying();
