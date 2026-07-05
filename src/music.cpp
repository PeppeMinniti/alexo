// ============================================================================
//  ALEXO - Riproduzione musica (web-radio MP3 -> VS1053). Vedi music.h.
// ============================================================================
#include "music.h"
#include "volume.h"
#include "netlog.h"
#include "webui.h"
#include "mic.h"
#include "ui.h"
#include "settings.h"
#include "gobbo.h"
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <math.h>

// Le stazioni ora sono EDITABILI dal pannello web: vivono in gSettings.musicStations
// (una per riga "chiave | nome | url", default in config.h MUSIC_STATIONS_DEF).
// Vedi musicMatch() piu' sotto.

// --- Livello del ring DURANTE la musica -------------------------------------
//  Stessa logica "perfetta" dell'idle reattivo (micLevelFromChunk): passa-alto +
//  noise-floor auto-calibrante ASIMMETRICO (scende in fretta = si aggancia al
//  SILENZIO, sale lentissimo = i beat NON lo trascinano su) + envelope. Cosi' la
//  baseline resta sul livello tenuto piu' basso e la MUSICA le sporge sopra ->
//  segue il brano ed e' viva. La differenza con l'idle e' solo il GUADAGNO piu'
//  basso (la cassa vicina satura) e un attack/release DEDICATI (macro qui sotto:
//  NON toccano gSettings, quindi gli altri stati restano come li hai tarati).
//  Manopole: MUSIC_LVL_DIV (guadagno; ALZA=piu' tenue), MUSIC_ATTACK (salita sul
//  beat), MUSIC_RELEASE ("sustain": discesa dopo il beat), MUSIC_BASE_MULT/FLOOR
//  (soglia sopra il silenzio). s_musMad/s_musBase esposti in /api/live.
#define MUSIC_BASE_MULT 1.10f   // soglia = baseline*questo + FLOOR
#define MUSIC_FLOOR     80.0f   // margine minimo sopra il silenzio
#define MUSIC_LVL_DIV   10.0f   // scala (mad - soglia) -> 0..255
#define MUSIC_ATTACK    1.00f   // salita ISTANTANEA sul colpo (niente inerzia in salita)
#define MUSIC_RELEASE   0.45f   // discesa ("sustain"): quanto svelto si spegne dopo

// Ring DURANTE la musica: 1 = REATTIVO al mic (default), 0 = "breathing" time-based.
// NB: il mic bloccante NON era la causa della choppiness (Kiss Kiss e
// Virgin sono MP3 identici 128k/48k ma solo la prima suona -> e' il SERVER/rete, non
// il feed). Quindi il reattivo si tiene. Il breathing resta come opzione.
#define MUSIC_RING_REACTIVE 1

// Definito in main.cpp: spegne/accende l'ampli PAM8302A. Qui serve per rispegnerlo
// a volume 0 (muto) MENTRE la radio suona (altrimenti resta il fruscio Class-D).
void ampEnable(bool on);

static volatile float s_musMad = 0, s_musBase = 0;
float musicLastMad()  { return s_musMad; }
float musicLastBase() { return s_musBase; }

#if MUSIC_RING_REACTIVE
static uint8_t musicLevel(const int16_t *s, size_t n) {
  if (!n) return 0;
  // MAD col passa-alto leggero (toglie DC/rumble), come micLevelFromChunk.
  static double hpX = 0, hpY = 0;
  const double hpR = 0.95;
  float acc = 0;
  for (size_t i = 0; i < n; i++) {
    double v = (double)s[i];
    double y = v - hpX + hpR * hpY;
    hpX = v; hpY = y;
    acc += fabsf((float)y);
  }
  float mad = acc / (float)n;
  s_musMad = mad;

  // Noise floor auto-calibrante ASIMMETRICO: scende in fretta (aggancia il
  // silenzio/quiet), sale lentissimo (i beat non lo tirano su).
  static float nf = -1.0f;
  if (nf < 0.0f) nf = mad;
  float k = (mad < nf) ? 0.05f : 0.0005f;
  nf += (mad - nf) * k;
  s_musBase = nf;

  float thresh = nf * MUSIC_BASE_MULT + MUSIC_FLOOR;
  float target = (mad - thresh) / MUSIC_LVL_DIV;
  if (target < 0) target = 0; else if (target > 255) target = 255;

  // Envelope DEDICATO alla musica (macro sopra, non gSettings dell'idle).
  static float env = 0;
  float ek = (target > env) ? MUSIC_ATTACK : MUSIC_RELEASE;
  env += (target - env) * ek;
  int lvl = (int)(env + 0.5f);
  if (lvl < 0) lvl = 0; else if (lvl > 255) lvl = 255;
  return (uint8_t)lvl;
}
#endif  // MUSIC_RING_REACTIVE

static inline bool has(const String &t, const char *w) { return t.indexOf(w) >= 0; }

// true se 'key' compare in 't' come PAROLA INTERA (non incollata ad altre lettere
// o cifre): cosi' "rock" NON scatta dentro "rockettaro" ne' "pop" dentro
// "popolare" -> i casi ambigui cadono a Claude. t e key gia' minuscoli.
static inline bool isWordCh(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}
static bool containsWord(const String &t, const String &key) {
  int kl = key.length();
  if (kl == 0) return false;
  int from = 0;
  while (true) {
    int i = t.indexOf(key, from);
    if (i < 0) return false;
    char before = (i > 0) ? t[i - 1] : ' ';
    char after  = (i + kl < (int)t.length()) ? t[i + kl] : ' ';
    if (!isWordCh(before) && !isWordCh(after)) return true;   // bordi "puliti"
    from = i + 1;                                             // era dentro una parola: cerca oltre
  }
}

// Estrae da una riga "chiave | nome | url" i tre campi (con trim; chiave in
// minuscolo). Ritorna false se la riga e' malformata (manca un campo).
static bool parseStationLine(const String &line, String &key, String &nome, String &url) {
  int p1 = line.indexOf('|'); if (p1 < 0) return false;
  int p2 = line.indexOf('|', p1 + 1); if (p2 < 0) return false;
  key  = line.substring(0, p1);      key.trim();  key.toLowerCase();
  nome = line.substring(p1 + 1, p2); nome.trim();
  url  = line.substring(p2 + 1);     url.trim();
  return key.length() && url.length();
}

// Storage per la stazione trovata: MusicStation punta a queste String, valide
// finche' non si richiama musicMatch (single-thread: solo runInteraction lo usa).
static String s_mUrl, s_mNome;
static MusicStation s_mHit;

const MusicStation *musicMatch(const String &t) {
  // Serve un'INTENZIONE musicale, altrimenti "mi piace il rock" farebbe partire
  // la radio. "strong" = parola inequivocabilmente musicale; "verb" = verbo di
  // riproduzione (accettato solo se accompagnato da un genere).
  bool strong = has(t, "musica") || has(t, "radio") || has(t, "canzon");
  bool verb   = has(t, "metti") || has(t, "suona") || has(t, "riproduci") ||
                has(t, "fai partire") || has(t, "play") || has(t, "ascolt") ||
                has(t, "voglio");
  if (!strong && !verb) return nullptr;

  // Scorre la lista editabile (gSettings.musicStations, una stazione per riga).
  // L'ordine conta: le chiavi piu' specifiche prima (le mette l'utente/il default).
  const String &list = gSettings.musicStations;
  String key, nome, url, firstNome, firstUrl;
  bool haveFirst = false;
  int start = 0;
  while (start < (int)list.length()) {
    int nl = list.indexOf('\n', start);
    if (nl < 0) nl = list.length();
    String line = list.substring(start, nl); line.trim();
    start = nl + 1;
    if (line.length() == 0 || !parseStationLine(line, key, nome, url)) continue;
    if (!haveFirst) { firstNome = nome; firstUrl = url; haveFirst = true; }
    if (containsWord(t, key)) {                 // genere riconosciuto (parola intera)
      s_mUrl = url; s_mNome = nome;
      s_mHit.url = s_mUrl.c_str(); s_mHit.nome = s_mNome.c_str();
      return &s_mHit;
    }
  }

  // Intenzione musicale esplicita ma nessun genere ("metti musica") -> per ora la
  // PRIMA stazione della lista. In FASE 2b qui subentrera' il tool di Claude.
  if (strong && haveFirst) {
    s_mUrl = firstUrl; s_mNome = firstNome;
    s_mHit.url = s_mUrl.c_str(); s_mHit.nome = s_mNome.c_str();
    return &s_mHit;
  }
  return nullptr;   // solo un verbo, senza musica ne' genere: non e' un comando
}

// --- Catalogo interno VERIFICATO (fallback di Claude, fase 2b) ---------------
//  Quando la frase non combacia con la lista del pannello ma e' comunque una
//  richiesta di musica, Claude sceglie un GENERE tra questi (vedi tool in
//  llm.cpp) e qui lo mappiamo alla stazione. URL tutte provate 200/audio-mpeg
//  (niente URL "inventati" da Claude: sceglie solo un genere, l'URL e' nostro).
#define U_181     "http://listen.181fm.com/"
#define U_EAGLE   U_181 "181-eagle_128k.mp3"
#define U_BUZZ    U_181 "181-buzz_128k.mp3"
#define U_POWER   U_181 "181-power_128k.mp3"
#define U_COUNTRY U_181 "181-realcountry_128k.mp3"
#define U_80S     U_181 "181-awesome80s_128k.mp3"
#define U_90S     U_181 "181-star90s_128k.mp3"
#define U_OLDIES  U_181 "181-greatoldies_128k.mp3"
#define U_METAL   U_181 "181-hardrock_128k.mp3"
#define U_JAZZ    U_181 "181-classicaljazz_128k.mp3"
#define U_CHILL   U_181 "181-chilled_128k.mp3"
#define U_DANCE   U_181 "181-energy98_128k.mp3"
#define U_REGGAE  U_181 "181-reggae_128k.mp3"
#define U_CLASSIC U_181 "181-classical_128k.mp3"
#define U_BLUES   U_181 "181-blues_128k.mp3"
#define U_HIPHOP  U_181 "181-thebox_128k.mp3"
#define U_70S     U_181 "181-70s_128k.mp3"
#define U_SALSA   U_181 "181-salsa_128k.mp3"
#define U_INDIE   "http://ice1.somafm.com/indiepop-128-mp3"
#define U_AMBIENT "http://ice1.somafm.com/dronezone-128-mp3"

// La CHIAVE e' cercata "contenuta" nel genere che ritorna Claude (robusto a
// varianti). Le voci PIU' specifiche/ambigue prima (es. "jazz" prima di
// "classic"; "alternativ"/"metal" prima di "rock").
struct CatVoce { const char *key; MusicStation st; };
static const CatVoce CATALOG[] = {
  {"alternativ", {U_BUZZ,    "rock alternativo"}},
  {"metal",      {U_METAL,   "metal"}},
  {"hip",        {U_HIPHOP,  "hip hop"}},
  {"rap",        {U_HIPHOP,  "hip hop"}},
  {"jazz",       {U_JAZZ,    "jazz"}},
  {"loung",      {U_CHILL,   "lounge"}},
  {"chill",      {U_CHILL,   "chill"}},
  {"relax",      {U_CHILL,   "relax"}},
  {"dance",      {U_DANCE,   "dance"}},
  {"reggae",     {U_REGGAE,  "reggae"}},
  {"classic",    {U_CLASSIC, "classica"}},
  {"blues",      {U_BLUES,   "blues"}},
  {"country",    {U_COUNTRY, "country"}},
  {"ottanta",    {U_80S,     "anni 80"}},
  {"anni 80",    {U_80S,     "anni 80"}},
  {"novanta",    {U_90S,     "anni 90"}},
  {"anni 90",    {U_90S,     "anni 90"}},
  {"settanta",   {U_70S,     "anni 70"}},
  {"anni 70",    {U_70S,     "anni 70"}},
  {"oldies",     {U_OLDIES,  "oldies"}},
  {"salsa",      {U_SALSA,   "salsa"}},
  {"indie",      {U_INDIE,   "indie"}},
  {"ambient",    {U_AMBIENT, "ambient"}},
  {"rock",       {U_EAGLE,   "rock"}},
  {"pop",        {U_POWER,   "pop"}},
};

const MusicStation *musicFromGenre(const String &genere) {
  String g = genere; g.toLowerCase();
  for (const CatVoce &v : CATALOG)
    if (g.indexOf(v.key) >= 0) return &v.st;
  return nullptr;   // genere non in catalogo
}

// Elenco dei generi del catalogo, per la descrizione del tool di Claude (llm.cpp).
String musicCatalogList() {
  return F("rock, rock alternativo, metal, pop, anni 70, anni 80, anni 90, country, "
           "jazz, lounge, dance, reggae, salsa, classica, blues, hip hop, indie, "
           "ambient, oldies");
}

// Stop chiesto dal pannello web + stato "sta suonando" (letti dentro musicPlay).
static volatile bool s_stopWeb = false;
static volatile bool s_playing = false;
void musicRequestStop() { s_stopWeb = true; }
bool musicIsPlaying()   { return s_playing; }

// --- Metadata ICY (nome emittente + titolo brano dallo stream) ---------------
static String s_station;      // icy-name (nome emittente)
static String s_nowPlaying;   // StreamTitle corrente ("Artista - Titolo")
String musicStation()    { return s_station; }
String musicNowPlaying() { return s_nowPlaying; }

// Estrae StreamTitle='...' dal blocco metadata; se cambiato aggiorna e lo mostra
// sul TFT (teleprompter). 'meta' e' la stringa del blocco (terminata a 0).
static void parseStreamTitle(const char *meta) {
  const char *p = strstr(meta, "StreamTitle='");
  if (!p) return;
  p += 13;
  const char *e = strstr(p, "';");
  if (!e) return;
  String full;
  for (const char *q = p; q < e; q++) full += *q;
  full.trim();
  if (full.length() == 0 || full == s_nowPlaying) return;
  s_nowPlaying = full;   // completo "Artista - Titolo" (per il pannello /api/live)

  // Split "Artista - Titolo" per la schermata IN ONDA (righe separate).
  String artist = "", title = full;
  int sep = full.indexOf(" - ");
  if (sep >= 0) {
    artist = full.substring(0, sep);   artist.trim();
    title  = full.substring(sep + 3);  title.trim();
  }
  gobboNowPlaying(s_station.c_str(), title.c_str(), artist.c_str());
  Serial.printf("[music] in onda: %s\n", full.c_str());
  netlogPrintln((String("[music] in onda: ") + full).c_str());
}

// Legge un blocco metadata ICY: 1 byte lunghezza (in unita' da 16) + il testo.
// Va SEMPRE consumato per intero per non sfasare l'audio; ne teniamo solo i primi
// ~500 char (StreamTitle e' corto). La gran parte dei blocchi ha lunghezza 0.
static void readIcyMetadata(WiFiClient *stream) {
  uint8_t lenByte = 0;
  if (stream->readBytes(&lenByte, 1) != 1) return;
  int metaLen = (int)lenByte * 16;
  if (metaLen == 0) return;                        // nessun aggiornamento (caso normale)
  static char meta[512];
  int got = 0, remaining = metaLen;
  uint8_t tmp[64];
  while (remaining > 0) {
    int chunk = remaining > (int)sizeof(tmp) ? (int)sizeof(tmp) : remaining;
    int r = stream->readBytes(tmp, chunk);
    if (r <= 0) break;
    for (int i = 0; i < r && got < (int)sizeof(meta) - 1; i++) meta[got++] = (char)tmp[i];
    remaining -= r;
  }
  meta[got] = 0;
  parseStreamTitle(meta);
}

int musicPlay(VS1053 &player, const char *url, bool (*stopRequested)(), int (*seekRequested)()) {
  int seekOut = 0;                 // != 0 = uscita per SEEK (delta stazioni); 0 = stop/fine
  s_stopWeb = false;               // ignora richieste di stop "vecchie"
  // Sorgente HTTP o HTTPS: molte radio italiane (RTL/R101/Deejay/Rai...) sono su
  // https. WiFiClientSecure deriva da WiFiClient -> uso un riferimento polimorfico
  // e TLS "insecure" (senza validare il cert, come STT/Claude/TTS). Le http (es.
  // 181.fm, unitedradio) restano sul client normale.
  bool isHttps = String(url).startsWith("https");
  WiFiClient       clientPlain;
  WiFiClientSecure clientTls;
  if (isHttps) clientTls.setInsecure();
  WiFiClient &client = isHttps ? (WiFiClient&)clientTls : clientPlain;
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, url)) {
    Serial.printf("[music] begin fallito: %s\n", url);
    netlogPrintln((String("[music] BEGIN fallito: ") + url).c_str());
    return 0;
  }
  // Chiedo i metadati ICY inline (nome emittente + titolo brano). Il server li
  // interleava ogni "icy-metaint" byte: li separiamo dall'audio nel loop.
  static const char *ICY_HDRS[] = { "icy-metaint", "icy-name", "Content-Type", "icy-br" };
  http.collectHeaders(ICY_HDRS, 4);
  http.addHeader("Icy-MetaData", "1");
  int code = http.GET();
  if (code != 200) {
    // Diagnostica via Telnet: codice HTTP (o errore negativo di HTTPClient) + tipo.
    // Cosi' si vede PERCHE' una radio non parte (redirect, 403, TLS...).
    Serial.printf("[music] HTTP %d su %s\n", code, url);
    netlogPrintln((String("[music] FAIL HTTP=") + code + " ct=" +
                   http.header("Content-Type") + " url=" + url).c_str());
    http.end();
    return 0;
  }
  int metaint = http.header("icy-metaint").toInt();   // 0 se lo stream non li manda
  s_station    = http.header("icy-name");
  s_nowPlaying = "";
  gobboNowPlaying(s_station.c_str(), "", "");   // emittente subito; titolo/artista col metadata
  WiFiClient *stream = http.getStreamPtr();
  player.setVolume(volumeVsValue());
  s_playing = true;
  // Log Telnet RICCO (per debug da remoto): nome emittente + bitrate + tipo +
  // metaint + url. Il nome emittente (icy-name) c'e' SEMPRE dall'header, anche quando
  // lo stream non manda StreamTitle (prima in Telnet il nome spesso mancava).
  String br = http.header("icy-br");
  String det = String("[music] play: ") + (s_station.length() ? s_station : String("(senza nome)")) +
               (br.length() ? String(" [") + br + "k]" : String("")) +
               " ct=" + http.header("Content-Type") +
               (isHttps ? " https" : " http") +
               " metaint=" + metaint + " url=" + url;
  Serial.println(det);
  netlogPrintln(det.c_str());

  uint8_t buf[512];
  uint32_t idle = millis();
  uint32_t lastLvl = 0;
  bool ampOn = !volumeIsMuted();   // stato ampli: segue il muto (transizioni sotto)
  int bytesToMeta = metaint;   // byte audio fino al prossimo blocco metadata (0 = no demux)
  while (http.connected() || (stream && stream->available())) {
    if (stopRequested && stopRequested()) break;   // click encoder = stop
    if (s_stopWeb) break;                           // pulsante Stop del pannello
    if (seekRequested) {                            // premuto+giro = cambia stazione
      int sd = seekRequested();
      if (sd != 0) { seekOut = sd; break; }
    }
    ArduinoOTA.handle();                            // OTA vivo anche mentre suona
    netlogHandle();
    webuiHandle();                                  // pannello web raggiungibile mentre suona
    volumeApplyPending(player);                     // volume al volo (premuto+giro)

    // Ampli segue il MUTO in tempo reale: a volume 0 lo spengo (via il fruscio
    // Class-D), lo riaccendo appena rialzi. Solo sulle TRANSIZIONI (ampEnable ha un
    // delay(20) in accensione: chiamarlo ogni giro affamerebbe il feed).
    bool wantAmp = !volumeIsMuted();
    if (wantAmp != ampOn) { ampEnable(wantAmp); ampOn = wantAmp; }

    // Ring durante la MUSICA. Aggiornato ogni ~30ms. Il flag "abilita effetto
    // reattivo" del pannello (gSettings.idleReactive) accende/spegne il ring anche
    // qui: se OFF, LED spenti (e si salta pure la lettura mic). Come a muto.
    if (millis() - lastLvl >= 30) {
      lastLvl = millis();
      if (volumeIsMuted() || !gSettings.idleReactive) {
        uiSetLevel(0);                              // muto o effetto reattivo OFF: ring spento
      } else {
#if MUSIC_RING_REACTIVE
        // REATTIVO al mic (⚠️ bloccante -> puo' affamare il feed = audio a scatti).
        static int16_t mbuf[160];
        micFlush();                                 // livello fresco (no lag sull'audio)
        size_t g = micReadChunk(mbuf, 160);
        if (g) uiSetLevel(musicLevel(mbuf, g));
#else
        // BREATHING time-based: niente lettura mic -> il feed del VS1053 non viene
        // MAI affamato (audio liscio). Onda lenta ~2.6s (uso musicLevel() ampiezza).
        float ph = (float)(millis() % 2600) / 2600.0f;
        uiSetLevel((uint8_t)(35.0f + 120.0f * (0.5f - 0.5f * cosf(ph * 6.2831853f))));
#endif
      }
    }

    int avail = stream->available();
    if (avail > 0) {
      int toRead = avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail;
      // Non superare il confine del prossimo blocco metadata: cosi' i byte audio
      // e quelli di metadata non si mescolano.
      if (metaint > 0 && toRead > bytesToMeta) toRead = bytesToMeta;
      int c = stream->readBytes(buf, toRead);
      if (c > 0) {
        player.playChunk(buf, c); idle = millis();   // audio -> VS1053
        if (metaint > 0) {
          bytesToMeta -= c;
          if (bytesToMeta <= 0) { readIcyMetadata(stream); bytesToMeta = metaint; }
        }
      }
    } else {
      if (millis() - idle > 8000) break;            // stream morto/timeout
      delay(2);
    }
  }
  http.end();
  s_playing = false;
  s_nowPlaying = ""; s_station = "";
  uiSetLevel(0);   // spegni il ring: fine musica

  // coda di silenzio per svuotare il decoder (come la TTS)
  memset(buf, 0, sizeof(buf));
  for (int i = 0; i < 4; i++) player.playChunk(buf, sizeof(buf));
  Serial.println("[music] stop");
  netlogPrintln("[music] stop");
  return seekOut;
}

// --- Navigazione della lista stazioni del pannello (seek premuto+giro) --------
//  Scorrono gSettings.musicStations (le stazioni editabili dal pannello web),
//  riusando parseStationLine. Lista piccola: parsing on-demand ad ogni chiamata.
int musicStationCount() {
  const String &list = gSettings.musicStations;
  String key, nome, url;
  int n = 0, start = 0;
  while (start < (int)list.length()) {
    int nl = list.indexOf('\n', start); if (nl < 0) nl = list.length();
    String line = list.substring(start, nl); line.trim();
    start = nl + 1;
    if (line.length() && parseStationLine(line, key, nome, url)) n++;
  }
  return n;
}

bool musicStationGet(int idx, String &outUrl, String &outNome) {
  if (idx < 0) return false;
  const String &list = gSettings.musicStations;
  String key, nome, url;
  int n = 0, start = 0;
  while (start < (int)list.length()) {
    int nl = list.indexOf('\n', start); if (nl < 0) nl = list.length();
    String line = list.substring(start, nl); line.trim();
    start = nl + 1;
    if (line.length() && parseStationLine(line, key, nome, url)) {
      if (n == idx) { outUrl = url; outNome = nome; return true; }
      n++;
    }
  }
  return false;
}

int musicStationIndexOf(const char *url) {
  if (!url) return -1;
  const String &list = gSettings.musicStations;
  String key, nome, u;
  int n = 0, start = 0;
  while (start < (int)list.length()) {
    int nl = list.indexOf('\n', start); if (nl < 0) nl = list.length();
    String line = list.substring(start, nl); line.trim();
    start = nl + 1;
    if (line.length() && parseStationLine(line, key, nome, u)) {
      if (u == url) return n;
      n++;
    }
  }
  return -1;
}
