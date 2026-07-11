// ============================================================================
//  ALEXO - Pannello impostazioni via web (vedi webui.h).
//  Web server sincrono (WebServer di serie) sulla porta 80. La pagina sta in
//  LittleFS (data/index.html). API JSON:
//    GET  /api/settings  -> tutti i parametri correnti (+ volume)
//    POST /api/settings  -> aggiorna i parametri presenti nel body e salva in NVS
//    GET  /api/live      -> valori live del mic (livello/fondo/soglia) + stato
//    POST /api/reset     -> ripristina i default di fabbrica
//  Il server e' sincrono: durante un'interazione (registrazione/rete) il loop e'
//  bloccato e la pagina non risponde per qualche secondo -> normale, il pannello
//  si usa a riposo.
// ============================================================================
#include "webui.h"
#include "config.h"
#include "settings.h"
#include "volume.h"
#include "mic.h"
#include "wakeword.h"
#include "music.h"
#include "gobbo.h"
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>

static WebServer server(80);

// --- Serializza gSettings (+ volume) in un oggetto JSON ---------------------
static void fillSettingsJson(JsonDocument &doc) {
  doc["recSilenceMs"]     = gSettings.recSilenceMs;
  doc["recSilenceMargin"] = gSettings.recSilenceMargin;
  doc["recSilenceFloor"]  = gSettings.recSilenceFloor;
  doc["micLvlMargin"]     = gSettings.micLvlMargin;
  doc["micLvlFloor"]      = gSettings.micLvlFloor;
  doc["micLvlAttack"]     = gSettings.micLvlAttack;
  doc["micLvlRelease"]    = gSettings.micLvlRelease;
  doc["idleReactive"]     = gSettings.idleReactive;
  doc["wakeGain"]         = gSettings.wakeGain;
  doc["wakeProbCutoff"]   = gSettings.wakeProbCutoff;
  doc["wakeWindow"]       = gSettings.wakeWindow;
  doc["voiceId"]          = gSettings.voiceId;
  doc["voiceIdAlt"]       = gSettings.voiceIdAlt;
  doc["voiceTrigger"]     = gSettings.voiceTrigger;
  doc["llmModel"]         = gSettings.llmModel;
  doc["systemPrompt"]     = gSettings.systemPrompt;
  doc["hallucTerms"]      = gSettings.hallucTerms;
  doc["musicStations"]    = gSettings.musicStations;
  doc["replyTrigger"]     = gSettings.replyTrigger;
  doc["replyText"]        = gSettings.replyText;
  doc["volume"]           = volumeGet();
}

static void handleGetSettings() {
  JsonDocument doc;
  fillSettingsJson(doc);
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handlePostSettings() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "json non valido"); return;
  }
  // Aggiorna solo i campi presenti (il pannello puo' mandarne un sottoinsieme).
  if (!doc["recSilenceMs"].isNull())     gSettings.recSilenceMs     = doc["recSilenceMs"].as<uint32_t>();
  if (!doc["recSilenceMargin"].isNull()) gSettings.recSilenceMargin = doc["recSilenceMargin"].as<float>();
  if (!doc["recSilenceFloor"].isNull())  gSettings.recSilenceFloor  = doc["recSilenceFloor"].as<int>();
  if (!doc["micLvlMargin"].isNull())     gSettings.micLvlMargin     = doc["micLvlMargin"].as<float>();
  if (!doc["micLvlFloor"].isNull())      gSettings.micLvlFloor      = doc["micLvlFloor"].as<float>();
  if (!doc["micLvlAttack"].isNull())     gSettings.micLvlAttack     = doc["micLvlAttack"].as<float>();
  if (!doc["micLvlRelease"].isNull())    gSettings.micLvlRelease    = doc["micLvlRelease"].as<float>();
  if (!doc["idleReactive"].isNull())     gSettings.idleReactive     = doc["idleReactive"].as<bool>();
  if (!doc["wakeGain"].isNull())         gSettings.wakeGain         = doc["wakeGain"].as<int>();
  if (!doc["wakeProbCutoff"].isNull())   gSettings.wakeProbCutoff   = doc["wakeProbCutoff"].as<int>();
  if (!doc["wakeWindow"].isNull())       gSettings.wakeWindow       = doc["wakeWindow"].as<int>();
  if (!doc["voiceId"].isNull())          gSettings.voiceId          = doc["voiceId"].as<String>();
  if (!doc["voiceIdAlt"].isNull())       gSettings.voiceIdAlt       = doc["voiceIdAlt"].as<String>();
  if (!doc["voiceTrigger"].isNull())     gSettings.voiceTrigger     = doc["voiceTrigger"].as<String>();
  if (!doc["llmModel"].isNull())         gSettings.llmModel         = doc["llmModel"].as<String>();
  if (!doc["systemPrompt"].isNull())     gSettings.systemPrompt     = doc["systemPrompt"].as<String>();
  if (!doc["hallucTerms"].isNull())      gSettings.hallucTerms      = doc["hallucTerms"].as<String>();
  if (!doc["musicStations"].isNull())    gSettings.musicStations    = doc["musicStations"].as<String>();
  if (!doc["replyTrigger"].isNull())     gSettings.replyTrigger     = doc["replyTrigger"].as<String>();
  if (!doc["replyText"].isNull())        gSettings.replyText        = doc["replyText"].as<String>();
  if (!doc["volume"].isNull())           volumeSet(doc["volume"].as<int>());

  settingsSave();   // clampa e scrive in NVS

  // Rispondi con lo stato aggiornato (cosi' il pannello vede i valori clampati).
  JsonDocument out; fillSettingsJson(out);
  String s; serializeJson(out, s);
  server.send(200, "application/json", s);
}

static void handleLive() {
  uint8_t level = 0; float floorV = 0, thresh = 0;
  micGetLive(&level, &floorV, &thresh);
  JsonDocument doc;
  doc["level"]    = level;                 // livello LED corrente 0..255
  doc["floor"]    = floorV;                // rumore di fondo stimato (LED)
  doc["thresh"]   = thresh;                // soglia di accensione LED
  doc["wakeProb"] = wakeLastProb();        // ultima probabilita' wake 0..255
  doc["heap"]     = ESP.getFreeHeap();
  doc["rssi"]     = WiFi.RSSI();
  doc["music"]    = musicIsPlaying();      // true se una radio sta suonando
  doc["musMad"]   = musicLastMad();        // MAD grezzo musica (per tarare il ring)
  doc["musBase"]  = musicLastBase();       // baseline mobile musica
  doc["station"]  = musicStation();        // nome emittente (metadata ICY)
  doc["nowPlaying"] = musicNowPlaying();   // brano in onda "Artista - Titolo"
  doc["chatRev"]  = gobboChatRev();        // cambia a ogni nuovo msg -> il pannello ricarica la chat
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// GET /api/chat -> ultimi messaggi della chat (quella scrollabile sul TFT) in JSON.
// STREAMING a pezzi (un messaggio alla volta): evita di costruire in RAM una String
// grande quanto tutta la chat (fino a 40 x 2KB). JSON: [{"r":ruolo,"t":"testo"}].
static void handleChat() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");
  int n = gobboChatCount();
  String item;
  for (int i = 0; i < n; i++) {
    uint8_t role; const char *t;
    if (!gobboChatItem(i, &role, &t)) break;
    item = i ? "," : "";
    item += "{\"r\":"; item += (int)role; item += ",\"t\":\"";
    for (const char *p = t; *p; p++) {                 // escape JSON (tiene l'UTF-8)
      unsigned char c = (unsigned char)*p;
      if (c == '"' || c == '\\') { item += '\\'; item += (char)c; }
      else if (c == '\n')        { item += "\\n"; }
      else if (c >= 0x20)        { item += (char)c; }
    }
    item += "\"}";
    server.sendContent(item);
  }
  server.sendContent("]");
  server.sendContent("");   // chiude il chunked transfer
}

// POST /api/music/stop -> ferma la musica in riproduzione (pulsante del pannello).
static void handleMusicStop() {
  musicRequestStop();
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleReset() {
  settingsResetDefaults();
  volumeSet(VOLUME_DEFAULT);
  JsonDocument doc; fillSettingsJson(doc);
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

bool webuiBegin() {
  bool fs = LittleFS.begin(true);   // true = formatta se il mount fallisce
  if (!fs) Serial.println("[web] LittleFS non montato (pagina assente, API ok)");

  // API
  server.on("/api/settings", HTTP_GET,  handleGetSettings);
  server.on("/api/settings", HTTP_POST, handlePostSettings);
  server.on("/api/live",     HTTP_GET,  handleLive);
  server.on("/api/chat",     HTTP_GET,  handleChat);
  server.on("/api/reset",    HTTP_POST, handleReset);
  server.on("/api/music/stop", HTTP_POST, handleMusicStop);

  // Pagina statica da LittleFS (data/index.html).
  server.serveStatic("/", LittleFS, "/index.html");
  server.serveStatic("/index.html", LittleFS, "/index.html");

  server.onNotFound([]() {
    // fallback: se la pagina non c'e' in LittleFS, un messaggio minimo utile.
    if (LittleFS.exists("/index.html")) { server.send(404, "text/plain", "not found"); return; }
    server.send(200, "text/html",
      "<h3>ALEXO</h3><p>Pagina non caricata in LittleFS. Carica il filesystem: "
      "<code>pio run -t uploadfs</code>. L'API JSON e' attiva su /api/settings.</p>");
  });

  server.begin();
  Serial.println("[web] pannello pronto: http://alexo.local/");
  return fs;
}

void webuiHandle() { server.handleClient(); }
