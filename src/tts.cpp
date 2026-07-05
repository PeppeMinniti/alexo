// ============================================================================
//  ALEXO - Text-to-Speech: ElevenLabs -> streaming MP3 sul VS1053
//  POST a api.elevenlabs.io, l'MP3 ricevuto viene dato a pezzetti al VS1053
//  man mano che arriva (bassa latenza, niente buffer enorme).
// ============================================================================
#include "tts.h"
#include "secrets.h"
#include "gobbo.h"
#include "volume.h"
#include "settings.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// La voce di DEFAULT e' ora RUNTIME (gSettings.voiceId, dal pannello web); il
// default di fabbrica sta in config.h (ELEVEN_VOICE_DEF).
#define ELEVEN_MODEL    "eleven_flash_v2_5"       // veloce, 0,5 crediti/carattere
#define TTS_MS_PER_CHAR 65        // stima durata parlato: ms per carattere
                                  // (per sync gobbo<->voce; ritocca se serve)

// Normalizza il testo per la PRONUNCIA (solo per la TTS, NON per il display:
// a video restano "36°C" e "19,5"). Espande i simboli che ElevenLabs legge male
// nella loro forma parlata italiana. Per aggiungere casi: una nuova condizione
// nel loop. Il testo in ingresso e' UTF-8 (es. "°" = 0xC2 0xB0).
// Prova a leggere un ORARIO "H:MM" o "HH:MM" a partire da in[i]. Se combacia,
// accoda a 'out' la forma parlata e ritorna quanti caratteri ha consumato;
// altrimenti ritorna 0 (e 'out' non viene toccato). Regole:
//   00:00 -> "mezzanotte"    12:00 -> "mezzogiorno"
//   HH:00 -> "HH in punto"   HH:MM -> "HH e MM"   (ora 0 -> "zero")
// Vincoli: ore 0-23, minuti a 2 cifre 0-59; non deve seguire un'altra cifra o ':'
// (cosi' non trasforma numeri lunghi o orari con i secondi HH:MM:SS).
static int leggiOrario(const String &in, int i, int n, String &out) {
  int j = i, oreDigits = 0;
  while (j < n && isDigit((uint8_t)in[j]) && oreDigits < 2) { j++; oreDigits++; }
  if (oreDigits < 1) return 0;
  int colon = j;
  if (colon >= n || in[colon] != ':') return 0;
  if (colon + 2 >= n) return 0;
  if (!isDigit((uint8_t)in[colon + 1]) || !isDigit((uint8_t)in[colon + 2])) return 0;
  int after = colon + 3;
  if (after < n && (isDigit((uint8_t)in[after]) || in[after] == ':')) return 0;

  int H = in.substring(i, colon).toInt();
  int M = ((uint8_t)in[colon + 1] - '0') * 10 + ((uint8_t)in[colon + 2] - '0');
  if (H > 23 || M > 59) return 0;

  if      (H == 0  && M == 0) out += "mezzanotte";
  else if (H == 12 && M == 0) out += "mezzogiorno";
  else if (M == 0)            { out += String(H); out += " in punto"; }   // HH:00
  else if (H == 0)            { out += "zero ";   out += String(M); }     // 00:MM -> "zero MM"
  else                        { out += String(H); out += " e "; out += String(M); }  // HH:MM
  return after - i;   // caratteri consumati (le ore + ':' + i 2 minuti)
}

static String normalizzaPerVoce(const String &in) {
  String out;
  out.reserve(in.length() + 16);
  int n = in.length();
  for (int i = 0; i < n; i++) {
    uint8_t c = (uint8_t)in[i];

    // orario "HH:MM" -> forma parlata. Solo all'INIZIO di un numero (il carattere
    // prima non e' una cifra), cosi' non spezza numeri piu' lunghi.
    if (isDigit(c) && (i == 0 || !isDigit((uint8_t)in[i - 1]))) {
      int consumed = leggiOrario(in, i, n, out);
      if (consumed > 0) { i += consumed - 1; continue; }
    }

    // grado "°" (UTF-8 0xC2 0xB0), eventualmente seguito da C/F
    if (c == 0xC2 && i + 1 < n && (uint8_t)in[i + 1] == 0xB0) {
      char next = (i + 2 < n) ? in[i + 2] : 0;
      if      (next == 'C' || next == 'c') { out += " gradi centigradi"; i += 2; }
      else if (next == 'F' || next == 'f') { out += " gradi Fahrenheit"; i += 2; }
      else                                 { out += " gradi";            i += 1; }
      continue;
    }

    // virgola decimale tra cifre: "19,5" -> "19 virgola 5"
    if (c == ',' && i > 0 && i + 1 < n &&
        isDigit((uint8_t)in[i - 1]) && isDigit((uint8_t)in[i + 1])) {
      out += " virgola ";
      continue;
    }

    // percentuale: "100%" -> "100 per cento"
    if (c == '%') { out += " per cento"; continue; }

    // frazione tra cifre: "10/3" -> "10 fratto 3"
    // NB: tocca solo cifra/cifra; attenzione alle date "10/3/2026" -> diventano
    // "10 fratto 3 fratto 2026" (Claude in genere scrive "10 marzo", raro).
    if (c == '/' && i > 0 && i + 1 < n &&
        isDigit((uint8_t)in[i - 1]) && isDigit((uint8_t)in[i + 1])) {
      out += " fratto ";
      continue;
    }

    out += (char)c;
  }
  return out;
}

bool ttsSpeak(VS1053 &player, const String &text, const String &voiceId) {
  if (text.isEmpty()) return false;

  // Testo "parlato": uguale a quello a video ma coi simboli espansi (vedi sopra).
  String parlato = normalizzaPerVoce(text);

  // Voce: usa quella passata, altrimenti la default (dal pannello web).
  String voce = voiceId.isEmpty() ? gSettings.voiceId : voiceId;

  // Corpo JSON
  JsonDocument req;
  req["text"]     = parlato;
  req["model_id"] = ELEVEN_MODEL;
  String body;
  serializeJson(req, body);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20000);

  HTTPClient http;
  http.setTimeout(25000);
  String url = String("https://api.elevenlabs.io/v1/text-to-speech/")
             + voce + "?output_format=mp3_44100_128";
  http.begin(client, url);
  http.addHeader("xi-api-key", ELEVENLABS_API_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "audio/mpeg");

  Serial.printf("[tts] sintesi di %u caratteri (%s)...\n",
                (unsigned)parlato.length(), ELEVEN_MODEL);
  uint32_t t0 = millis();
  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("[tts] errore HTTP %d: %s\n", code, http.getString().c_str());
    http.end();
    return false;
  }

  // --- streaming dell'MP3 verso il VS1053 ---
  int len = http.getSize();             // -1 se sconosciuto (chunked)
  WiFiClient *stream = http.getStreamPtr();
  player.setVolume(volumeVsValue());   // volume utente rimappato nella zona udibile

  // L'audio sta per partire: lega lo scroll del gobbo alla DURATA dell'audio.
  // mp3_44100_128 = 128 kbps CBR -> durata(ms) = byte * 8 / 128000 = byte / 16.
  // Se ElevenLabs manda Content-Length (len>0) la durata e' ESATTA; se la
  // risposta e' chunked (len<0) ricado sulla stima da lunghezza testo.
  uint32_t durMs = (len > 0) ? (uint32_t)len / 16
                             : (uint32_t)parlato.length() * TTS_MS_PER_CHAR;
  gobboScrollOver(durMs);
  Serial.printf("[tts] durata audio: %lu ms (%s)\n", (unsigned long)durMs,
                len > 0 ? "esatta da Content-Length" : "stimata da testo");

  uint8_t buf[512];
  size_t total = 0;
  uint32_t idle = millis();
  while (http.connected() || (stream && stream->available())) {
    volumeApplyPending(player);   // regola il volume al volo MENTRE Alexo parla
    int avail = stream->available();
    if (avail > 0) {
      int toRead = avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail;
      int c = stream->readBytes(buf, toRead);
      if (c > 0) {
        player.playChunk(buf, c);
        total += c;
        idle = millis();
        if (len > 0) { len -= c; if (len == 0) break; }
      }
    } else {
      if (millis() - idle > 2000) break;   // stream finito/timeout
      delay(1);
    }
  }
  http.end();

  // svuota il decoder con un po' di silenzio
  memset(buf, 0, sizeof(buf));
  for (int i = 0; i < 4; i++) player.playChunk(buf, sizeof(buf));

  Serial.printf("[tts] riprodotti %u byte MP3 in %lu ms\n",
                (unsigned)total, (unsigned long)(millis() - t0));
  return total > 0;
}
