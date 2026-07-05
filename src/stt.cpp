// ============================================================================
//  ALEXO - Speech-to-Text (OpenAI Whisper)
//  Costruisce un body multipart/form-data (in PSRAM) con il WAV e lo invia a
//  https://api.openai.com/v1/audio/transcriptions, poi estrae il campo "text".
// ============================================================================
#include "stt.h"
#include "secrets.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// --- Provider STT (Groq, gratis, API compatibile OpenAI) --------------------
// Per passare a OpenAI: URL "https://api.openai.com/v1/audio/transcriptions",
// MODEL "whisper-1", KEY OPENAI_API_KEY.
#define STT_URL     "https://api.groq.com/openai/v1/audio/transcriptions"
#define STT_MODEL   "whisper-large-v3-turbo"
#define STT_API_KEY GROQ_API_KEY

String sttTranscribe(const uint8_t *wav, size_t wavLen, const char *lang) {
  if (!wav || wavLen == 0) return "";

  const String boundary = "----alexoBoundary7MA4YWxkTrZu0gW";

  // Parti del corpo multipart prima e dopo i byte del file
  String pre;
  pre  = "--" + boundary + "\r\n";
  pre += "Content-Disposition: form-data; name=\"model\"\r\n\r\n" STT_MODEL "\r\n";
  pre += "--" + boundary + "\r\n";
  pre += "Content-Disposition: form-data; name=\"language\"\r\n\r\n" + String(lang) + "\r\n";
  pre += "--" + boundary + "\r\n";
  pre += "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
  pre += "Content-Type: audio/wav\r\n\r\n";
  const String post = "\r\n--" + boundary + "--\r\n";

  const size_t bodyLen = pre.length() + wavLen + post.length();
  uint8_t *body = (uint8_t *)ps_malloc(bodyLen);
  if (!body) {
    Serial.println("[stt] allocazione PSRAM del body fallita");
    return "";
  }
  size_t o = 0;
  memcpy(body + o, pre.c_str(), pre.length());  o += pre.length();
  memcpy(body + o, wav, wavLen);                o += wavLen;
  memcpy(body + o, post.c_str(), post.length());

  WiFiClientSecure client;
  client.setInsecure();              // niente verifica certificato (ok per hobby)
  client.setTimeout(20000);

  HTTPClient http;
  http.setTimeout(25000);
  http.begin(client, STT_URL);
  http.addHeader("Authorization", String("Bearer ") + STT_API_KEY);
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  Serial.printf("[stt] invio %u byte a Whisper (%s)...\n", (unsigned)bodyLen, STT_MODEL);
  uint32_t t0 = millis();
  int code = http.POST(body, bodyLen);
  String resp = http.getString();
  http.end();
  free(body);
  Serial.printf("[stt] risposta HTTP %d in %lu ms\n", code, (unsigned long)(millis() - t0));

  if (code != 200) {
    Serial.printf("[stt] errore: %s\n", resp.c_str());
    return "";
  }

  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, resp);
  if (e) {
    Serial.printf("[stt] JSON non valido: %s\n", e.c_str());
    return "";
  }
  String text = doc["text"] | "";
  text.trim();
  return text;
}
