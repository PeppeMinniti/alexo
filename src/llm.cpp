// ============================================================================
//  ALEXO - Cervello: Anthropic Messages API (Claude Haiku 4.5)
//  + memoria della conversazione + ricerca web (web_search server-side).
// ============================================================================
#include "llm.h"
#include "secrets.h"
#include "settings.h"
#include "net.h"
#include "music.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

//  Modello e personalita' (system prompt) sono ora RUNTIME (gSettings.llmModel /
//  systemPrompt, modificabili dal pannello web); i default stanno in config.h.
#define LLM_MAX_TOKENS 1024            // headroom per ragionamento + ricerca
#define WEB_MAX_USES   3               // max ricerche per richiesta (limita i costi)

// --- Allocatore ArduinoJson su PSRAM ----------------------------------------
struct PsramAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return ps_malloc(n); }
  void  deallocate(void *p) override { free(p); }
  void *reallocate(void *p, size_t n) override { return ps_realloc(p, n); }
};
static PsramAllocator psramAlloc;

// --- Memoria della conversazione (ultimi N messaggi) ------------------------
#define MAX_HISTORY 8   // messaggi totali = 4 scambi user/assistant
static String histRole[MAX_HISTORY];
static String histText[MAX_HISTORY];
static int    histN = 0;

static void histPush(const char *role, const String &text) {
  if (histN >= MAX_HISTORY) {
    for (int i = 2; i < histN; i++) {
      histRole[i - 2] = histRole[i];
      histText[i - 2] = histText[i];
    }
    histN -= 2;
  }
  histRole[histN] = role;
  histText[histN] = text;
  histN++;
}

void llmReset() { histN = 0; }

String llmAsk(const String &userText, String *musicReq) {
  if (userText.isEmpty()) return "";

  histPush("user", userText);

  // --- corpo JSON della richiesta (doc su PSRAM) ---
  JsonDocument req(&psramAlloc);
  req["model"]      = gSettings.llmModel;
  req["max_tokens"] = LLM_MAX_TOKENS;

  // System prompt + data/ora attuali (via NTP): senza, Claude non sa "quando" e'
  // adesso e sbaglia sistematicamente ora e fusi. Se l'orologio non e' ancora
  // sincronizzato, nowContextString() torna vuoto e non aggiungiamo nulla.
  String sys = gSettings.systemPrompt;
  String nowStr = nowContextString();
  if (nowStr.length()) {
    sys += "\n\nData e ora attuali: " + nowStr +
           ". Usa QUESTE per le domande sull'ora corrente; per altre citta' calcola "
           "l'ora dall'UTC qui sopra applicando il loro fuso. Non usare la ricerca web per l'ora.";
  }
  req["system"]     = sys;

  // strumento di ricerca web (server-side, variante base per Haiku 4.5)
  JsonObject tool = req["tools"].add<JsonObject>();
  tool["type"]     = "web_search_20250305";
  tool["name"]     = "web_search";
  tool["max_uses"] = WEB_MAX_USES;

  // tool MUSICA (client-side): se richiesto (musicReq != nullptr) Claude puo'
  // avviare la musica invece di rispondere a voce. Sceglie un genere del catalogo
  // interno (music.cpp); l'URL lo mettiamo noi (niente URL inventati).
  if (musicReq) {
    JsonObject mt = req["tools"].add<JsonObject>();
    mt["name"] = "riproduci_musica";
    mt["description"] = String(
        "Avvia la riproduzione di musica/radio quando l'utente vuole ASCOLTARE "
        "musica, anche con richieste vaghe o per umore (es. \"metti qualcosa di "
        "rilassante\", \"musica allegra\", \"un po' di jazz\"). NON usarlo per "
        "domande informative o conversazione normale. Scegli il 'genere' PIU' "
        "adatto tra questi: ") + musicCatalogList() + ".";
    JsonObject sch = mt["input_schema"].to<JsonObject>();
    sch["type"] = "object";
    JsonObject props = sch["properties"].to<JsonObject>();
    JsonObject gp = props["genere"].to<JsonObject>();
    gp["type"] = "string";
    gp["description"] = "il genere o mood scelto tra quelli elencati";
    JsonArray rq = sch["required"].to<JsonArray>();
    rq.add("genere");
  }

  JsonArray msgs = req["messages"].to<JsonArray>();
  for (int i = 0; i < histN; i++) {
    JsonObject m = msgs.add<JsonObject>();
    m["role"]    = histRole[i];
    m["content"] = histText[i];
  }

  String body;
  serializeJson(req, body);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(25000);

  HTTPClient http;
  http.setTimeout(30000);
  http.begin(client, "https://api.anthropic.com/v1/messages");
  http.addHeader("content-type", "application/json");
  http.addHeader("x-api-key", ANTHROPIC_API_KEY);
  http.addHeader("anthropic-version", "2023-06-01");

  Serial.printf("[llm] chiedo a Claude (%s, +web)...\n", gSettings.llmModel.c_str());
  uint32_t t0 = millis();
  int code = http.POST(body);
  String resp = http.getString();
  http.end();
  Serial.printf("[llm] risposta HTTP %d in %lu ms\n", code, (unsigned long)(millis() - t0));

  if (code != 200) {
    Serial.printf("[llm] errore: %s\n", resp.c_str());
    if (histN > 0) histN--;
    return "";
  }

  // --- parsing con FILTRO: estraggo solo i blocchi di testo e stop_reason,
  //     ignorando i risultati di ricerca (voluminosi) -> poca memoria ---
  JsonDocument filter;
  filter["stop_reason"]          = true;
  filter["content"][0]["type"]   = true;
  filter["content"][0]["text"]   = true;
  filter["content"][0]["name"]   = true;   // tool_use: nome del tool
  filter["content"][0]["input"]  = true;   // tool_use: argomenti (genere)

  JsonDocument doc(&psramAlloc);
  DeserializationError e =
      deserializeJson(doc, resp, DeserializationOption::Filter(filter));
  if (e) {
    Serial.printf("[llm] JSON non valido: %s\n", e.c_str());
    if (histN > 0) histN--;
    return "";
  }

  const char *stop = doc["stop_reason"] | "";
  String out;
  for (JsonObject block : doc["content"].as<JsonArray>()) {
    const char *bt = block["type"] | "";
    if (strcmp(bt, "text") == 0) {
      out += (const char *)(block["text"] | "");
    } else if (musicReq && strcmp(bt, "tool_use") == 0 &&
               strcmp(block["name"] | "", "riproduci_musica") == 0) {
      *musicReq = (const char *)(block["input"]["genere"] | "");
    }
  }
  out.trim();
  Serial.printf("[llm] stop_reason=%s\n", stop);

  // Claude ha scelto la musica: niente risposta a voce, torna il genere via
  // musicReq. Salvo comunque un turno assistant (per l'alternanza in memoria).
  if (musicReq && musicReq->length()) {
    histPush("assistant", String("(avviata la musica: ") + *musicReq + ")");
    return "";
  }

  if (out.isEmpty()) { if (histN > 0) histN--; return ""; }
  histPush("assistant", out);
  return out;
}
