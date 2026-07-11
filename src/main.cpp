// ============================================================================
//  ALEXO - Assistente vocale (firmware principale)
//
//  Flusso: tieni premuto -> ASCOLTO (registra) -> PENSO (Whisper + Claude)
//          -> PARLO (ElevenLabs -> VS1053) -> riposo.
//
//  Due core: il loop (core 1) fa la pipeline; le animazioni del ring girano
//  su un task dedicato (core 0, vedi ui.cpp), cosi' restano fluide anche
//  mentre il core 1 e' bloccato sulle chiamate di rete.
// ============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <ArduinoOTA.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_NeoPixel.h>
#include <VS1053.h>

#include "config.h"
#include "mic.h"
#include "net.h"
#include "stt.h"
#include "llm.h"
#include "tts.h"
#include "ui.h"
#include "sound.h"
#include "gobbo.h"
#include "encoder.h"
#include "volume.h"
#include "netlog.h"
#include "wakeword.h"
#include "tfltest.h"
#include "settings.h"
#include "webui.h"
#include "music.h"

// --- Voce alternativa -------------------------------------------------------
// Se la tua frase INIZIA con la parola-trigger, Alexo risponde con la voce
// alternativa invece di quella di default (vedi tts.cpp). Ora sono RUNTIME:
// gSettings.voiceIdAlt / gSettings.voiceTrigger (modificabili dal pannello web);
// i default di fabbrica stanno in config.h (ELEVEN_VOICE_ALT_DEF/VOICE_TRIGGER_DEF).

// --- Periferiche ------------------------------------------------------------
//  Il TFT sta su un bus SPI DEDICATO (HSPI), separato dal VS1053 (bus globale
//  SPI = FSPI): cosi' lo scroll della chat (core 0) non litiga col feed audio
//  (core 1). MISO non serve al display (sola scrittura) -> -1.
SPIClass tftSPI(HSPI);
Adafruit_ST7735 display(&tftSPI, TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);
Adafruit_NeoPixel ring(LED_RING_COUNT, LED_RING_PIN, NEO_GRB + NEO_KHZ800);

// Sottoclasse diagnostica: nella libreria `read_register` e' protected. La
// esponiamo per leggere i registri SCI del VS1053 e mostrarli sul display
// (cosi' si diagnostica senza la seriale). VS1053Diag E' un VS1053 a tutti gli
// effetti -> nessun'altra parte del codice cambia.
class VS1053Diag : public VS1053 {
public:
  VS1053Diag(uint8_t cs, uint8_t dcs, uint8_t dreq) : VS1053(cs, dcs, dreq) {}
  uint16_t readReg(uint8_t r) { return read_register(r); }
  // Legge un registro SCI SENZA attendere DREQ (read_register fa busy-wait su
  // DREQ e si impiccherebbe). Serve a diagnosticare quando DREQ e' basso: se il
  // chip risponde lo stesso, e' VIVO e alimentato -> il guasto e' SOLO la linea
  // DREQ. Se torna 0000/FFFF, il chip non risponde (alimentazione/reset).
  uint16_t readRegNoWait(uint8_t r) {
    control_mode_on();
    SPI.write(3); SPI.write(r);
    uint16_t v = ((uint16_t)SPI.transfer(0xFF) << 8) | SPI.transfer(0xFF);
    control_mode_off();
    return v;
  }
};
VS1053Diag player(VS1053_XCS_PIN, VS1053_XDCS_PIN, VS1053_DREQ_PIN);

// Istante (millis) in cui e' finito l'ultimo AUDIO dall'altoparlante (musica O
// voce/TTS). Subito dopo, la coda acustica della cassa rientra nel mic e puo'
// innescare un avvio spurio (falso wake o trigger che sfugge): per
// ~AUDIO_COOLDOWN_MS ignoriamo OGNI avvio (wake e click).
static uint32_t g_audioEndMs = 0;
static const uint32_t AUDIO_COOLDOWN_MS = 1500;

// --- Stato ------------------------------------------------------------------
bool tftOk = false;
bool vsOk  = false;
bool micOk = false;

// Condizione di stop della registrazione: registra finche' NON arriva un click
// di stop dall'encoder (toggle). Domani il wake-word/silenzio prenderanno il
// posto di questo predicato senza toccare il resto della pipeline.
static bool recKeepGoing() { return !gobboStopRequested(); }

// Cambia stato: aggiorna SIA il ring (animazioni) SIA l'header del TFT.
static inline void setState(AlexoState s) { uiSetState(s); gobboSetState(s); }

// Allucinazioni tipiche di Whisper sul silenzio/rumore (italiano): quando la
// registrazione non contiene voce, Whisper "inventa" queste frasi. Le scartiamo
// per non far ripartire una chat col fantasma "Grazie". La lista e' editabile dal
// pannello web (gSettings.hallucTerms, separata da virgola). Confronto sul testo
// ripulito (minuscolo, senza punteggiatura/spazi ai bordi), match ESATTO su una
// delle frasi in lista (o testo vuoto).
static bool isAllucinazione(const String &testo) {
  String s = testo; s.toLowerCase(); s.trim();
  while (s.length() && strchr(".!?,;:- ", s[s.length() - 1])) s.remove(s.length() - 1);
  s.trim();
  if (s.length() == 0) return true;
  const String &csv = gSettings.hallucTerms;
  int start = 0;
  while (start <= (int)csv.length()) {
    int comma = csv.indexOf(',', start);
    if (comma < 0) comma = csv.length();
    String term = csv.substring(start, comma);
    term.trim(); term.toLowerCase();
    if (term.length() > 0 && s.equals(term)) return true;
    start = comma + 1;
  }
  return false;
}

// Confronta 't' (GIA' minuscolo) con un elenco di termini separati da virgola
// (es. "bene, ok, ciao"). contains=false -> match se t INIZIA con un termine
// (voce alternativa); contains=true -> match se t CONTIENE un termine (easter-egg).
// Termini vuoti ignorati; elenco vuoto -> nessun match (funzione disattivata).
static bool matchAnyTerm(const String &t, const String &csv, bool contains) {
  int start = 0;
  while (start <= (int)csv.length()) {
    int comma = csv.indexOf(',', start);
    if (comma < 0) comma = csv.length();
    String term = csv.substring(start, comma);
    term.trim(); term.toLowerCase();
    if (term.length() > 0 && (contains ? (t.indexOf(term) >= 0) : t.startsWith(term)))
      return true;
    start = comma + 1;
  }
  return false;
}

// RISPOSTA PERSONALIZZATA: se il trigger (gSettings.replyTrigger) e' valorizzato e la
// domanda lo CONTIENE (confronto minuscolo), torna il testo fisso (gSettings.replyText)
// e si salta l'AI. Trigger vuoto = disattivato (risponde l'AI). Per scherzi/riprese.
static String customReplyMatch(const String &testo) {
  String trig = gSettings.replyTrigger; trig.trim(); trig.toLowerCase();
  if (trig.isEmpty()) return "";
  String tl = testo; tl.toLowerCase();
  if (tl.indexOf(trig) >= 0) return gSettings.replyText;
  return "";
}

// --- Display ----------------------------------------------------------------
//  Splash di stato durante il boot (prima che il gobbo prenda il controllo del
//  TFT). Disegno diretto sul TFT: qui siamo ancora single-task (solo setup()).
void tftStatus(const char *line1, const char *line2 = nullptr) {
  if (!tftOk) return;
  display.fillScreen(ST77XX_BLACK);
  display.setTextSize(2);
  display.setTextColor(ST77XX_CYAN);
  display.setCursor(0, 0);
  display.println("ALEXO");
  display.setTextSize(1);
  display.setTextColor(ST77XX_WHITE);
  display.setCursor(0, 30);
  display.println(line1);
  if (line2) { display.setCursor(0, 42); display.println(line2); }
}

static void setRingSolid(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < LED_RING_COUNT; i++) ring.setPixelColor(i, r, g, b);
  ring.show();
}

// --- Amplificatore PAM8302A (SD attivo basso: HIGH = acceso, LOW = muto) -----
//  Acceso solo quando c'e' audio (bip + voce); muto a riposo -> niente fruscio
//  Class-D. Lo accendiamo un attimo PRIMA dell'audio (il pop cade nel silenzio).
//  NON static: lo chiama anche music.cpp per rispegnerlo a volume 0 mentre suona.
void ampEnable(bool on) {
#if AMP_SD_PIN >= 0
  digitalWrite(AMP_SD_PIN, on ? HIGH : LOW);
  if (on) delay(20);   // breve assestamento dell'ampli prima di mandare audio
#endif
}

// --- Gestione errore (feedback + ritorno a riposo) --------------------------
static void fail(const char *msg) {
  Serial.printf(">> %s\n", msg);
  setState(ST_ERROR);
  gobboPrint(String("[") + msg + "]");
  if (vsOk) { ampEnable(true); soundError(player); delay(50); ampEnable(false); }
  delay(1200);
  setState(ST_IDLE);
}

// --- Splash futuristico all'accensione (TFT HUD + ring "carica") ------------
#if SPLASH_BOOT
static void bootSplash() {
  if (!tftOk) return;
  const int W = TFT_WIDTH, H = TFT_HEIGHT;            // 128 x 160
  const uint16_t BLK = ST77XX_BLACK;
  const uint16_t CY  = display.color565(0, 255, 255); // cyan acceso
  const uint16_t CYd = display.color565(0, 70, 85);   // cyan tenue (griglia)
  const uint16_t CYm = display.color565(0, 150, 170); // cyan medio
  const uint16_t MAG = display.color565(255, 0, 170); // accento magenta

  display.fillScreen(BLK);

  // 1) Scanline che scorre dall'alto in basso lasciando una griglia tenue
  for (int y = 0; y < H; y += 4) {
    display.drawFastHLine(0, y, W, CYd);
    display.drawFastHLine(0, y + 2, W, CY);
    delay(6);
    display.drawFastHLine(0, y + 2, W, BLK);
  }
  for (int x = 0; x <= W; x += 16) display.drawFastVLine(x, 0, H, CYd);

  // 2) Parentesi angolari stile HUD
  const int b = 12;
  display.drawFastHLine(2, 2, b, CY);          display.drawFastVLine(2, 2, b, CY);
  display.drawFastHLine(W - 2 - b, 2, b, CY);  display.drawFastVLine(W - 3, 2, b, CY);
  display.drawFastHLine(2, H - 3, b, CY);      display.drawFastVLine(2, H - 3 - b, b, CY);
  display.drawFastHLine(W - 2 - b, H - 3, b, CY); display.drawFastVLine(W - 3, H - 3 - b, b, CY);

  // 3) "Reattore": cerchi concentrici che si espandono + punto che gira sul ring
  const int cx = W / 2, cy = 52;
  for (int r = 3; r <= 36; r += 3) {
    display.drawCircle(cx, cy, r, (r % 6 == 0) ? CY : CYd);
    int idx = (r / 3) % LED_RING_COUNT;
    ring.clear();
    ring.setPixelColor(idx, 0, 45, 55);
    ring.setPixelColor((idx + 1) % LED_RING_COUNT, 0, 14, 18);
    ring.show();
    delay(28);
  }
  display.fillCircle(cx, cy, 6, CY);
  display.drawCircle(cx, cy, 10, CYm);

  // 4) "ALEXO" lettera per lettera (con ombra), poi sottotitolo
  display.setTextSize(3);
  const char *name = "ALEXO";
  const int chW = 18, tw = 5 * chW, tx = (W - tw) / 2, ty = 96;
  for (int i = 0; i < 5; i++) {
    display.setTextColor(CYd); display.setCursor(tx + i * chW + 1, ty + 1); display.write(name[i]);
    display.setTextColor(CY);  display.setCursor(tx + i * chW,     ty);     display.write(name[i]);
    ring.clear();
    for (int k = 0; k <= i * 2 && k < LED_RING_COUNT; k++) ring.setPixelColor(k, 0, 38, 48);
    ring.show();
    delay(130);
  }
  display.setTextSize(1);
  display.setTextColor(CYm);
  const char *sub = "ASSISTENTE VOCALE";
  display.setCursor((W - (int)strlen(sub) * 6) / 2, ty + 26);
  display.print(sub);

  // 5) Barra di avanzamento + ring che "carica" in proporzione
  const int bx = 12, by = 144, bw = W - 24, bh = 7;
  display.drawRect(bx, by, bw, bh, CYm);
  display.setTextColor(ST77XX_WHITE);
  display.setCursor(bx + 14, by - 11); display.print("AVVIO SISTEMA");
  for (int p = 0; p <= bw - 4; p += 3) {
    display.fillRect(bx + 2, by + 2, p, bh - 4, CY);
    int lit = (p * LED_RING_COUNT) / (bw - 4);
    ring.clear();
    for (int k = 0; k < lit && k < LED_RING_COUNT; k++) ring.setPixelColor(k, 0, 32, 42);
    ring.show();
    delay(10);
  }

  // 6) "PRONTO" + pulse del ring che svanisce
  display.setTextColor(MAG);
  //display.setCursor(bx + bw - 42, by - 11); display.print("PRONTO");
  display.setCursor(bx + bw - 42, by - 11); display.print("");
  for (int v = 60; v >= 0; v -= 6) {
    for (int k = 0; k < LED_RING_COUNT; k++) ring.setPixelColor(k, 0, v, (uint8_t)(v * 1.2f));
    ring.show();
    delay(22);
  }
  ring.clear(); ring.show();
  delay(250);
}
#endif

// Avvia una stazione radio: mostra il nome, stato ST_MUSIC (ring reattivo), suona
// finche' non si ferma, poi ritorno PULITO a riposo (mute ampli, flush anti
// falso-wake, finestra di raffreddamento). Usata sia dal match locale (lista del
// pannello) sia dal fallback di Claude (catalogo). L'ampli e' gia' acceso (viene
// dalla fase di ascolto di runInteraction).
static void playStation(const MusicStation *st) {
  // Copio url/nome in String locali: la sorgente (s_mHit / catalogo) verrebbe
  // riusata da musicStationGet durante il seek.
  String url = st->url, nome = st->nome;
  // Indice nella lista del pannello per il seek premuto+giro; -1 se la stazione
  // non e' in lista (es. scelta da Claude dal catalogo): il primo seek entrera'
  // allora dagli estremi della lista.
  int idx = musicStationIndexOf(url.c_str());

  // A volume MUTO (0) spengo l'ampli: la musica e' comunque silenziosa e cosi'
  // sparisce il "macinio"/ronzio Class-D che il mic sentirebbe. Se non muto,
  // l'ampli e' gia' acceso dalla fase di ascolto.
  ampEnable(!volumeIsMuted());
  setState(ST_MUSIC);
  gobboClearStopRequest();

  // Loop di stazioni: musicPlay ritorna 0 = stop/fine, oppure il delta del seek
  // (premuto+giro) -> passo alla stazione prec./succ. della lista e riparto.
  for (;;) {
    Serial.printf(">> MUSICA: %s (%s)\n", nome.c_str(), url.c_str());
    gobboPrint(String("\xE2\x99\xAA ") + nome);   // "♪ <nome>"
    int seek = musicPlay(player, url.c_str(),
                         []() { return gobboTakeTalkRequest(); },       // click = stop
                         []() { return (int)gobboTakeMusicSeek(); });   // premuto+giro = cambia
    if (seek == 0) break;                          // stop / fine stream
    int n = musicStationCount();
    if (n <= 0) break;                             // lista vuota: esci
    if (idx < 0) idx = (seek > 0) ? 0 : n - 1;     // fuori lista: entra dagli estremi
    else { idx = (idx + seek) % n; if (idx < 0) idx += n; }   // ciclico avanti/indietro
    if (!musicStationGet(idx, url, nome)) break;
  }

  ampEnable(false);
  for (int i = 0; i < 10; i++) { micFlush(); delay(40); }
  wakeReset();
  gobboTakeTalkRequest();        // scarta un eventuale click/talk accumulato
  gobboTakeMusicSeek();          // scarta seek residui (niente salto alla prossima musica)
  gobboClearStopRequest();
  g_audioEndMs = millis();       // finestra di raffreddamento (loop)
  setState(ST_IDLE);
}

// --- Una interazione completa (ascolto -> pensa -> parla) -------------------
static void runInteraction() {
  // 1) ASCOLTO
  gobboClearStopRequest();   // ignora click "vecchi": si ferma solo col prossimo
  setState(ST_LISTENING);    // ora un click dell'encoder = stop registrazione
  if (vsOk) ampEnable(true); // accendi l'ampli: bip e voce passano, poi si muta
  if (vsOk) soundStart(player);

  uint32_t t0 = millis();
  size_t   n  = micRecord(REC_MAX_MS, recKeepGoing, uiSetLevel, gSettings.recSilenceMs);
  if (vsOk) soundStop(player);

  size_t wavLen = 0;
  const uint8_t *wav = micWav(&wavLen);
  float secs = (float)n / (float)micSampleRate();
  Serial.printf("   campioni=%u  durata=%.2fs (%lums)  picco=%d/32767  WAV=%u byte\n",
                (unsigned)n, secs, (unsigned long)(millis() - t0), micLastPeak(),
                (unsigned)wavLen);

  // troppo corto: probabilmente premuto per sbaglio
  if (n < (size_t)(micSampleRate() / 4)) {
    if (vsOk) ampEnable(false);   // muta l'ampli prima di tornare a riposo
    setState(ST_IDLE);
    return;
  }
  // Nessuna voce VERA rilevata (solo silenzio/rumore): probabile falso avvio
  // (wake fantasma / click). NON mandare a Whisper (allucinerebbe "Grazie" e
  // farebbe ripartire una chat). Torna a riposo in silenzio.
  if (!micHeardVoice()) {
    Serial.println(">> nessuna voce rilevata: ignoro (niente Whisper)");
    if (vsOk) ampEnable(false);
    setState(ST_IDLE);
    return;
  }
  if (!wifiOk()) { fail("No WiFi"); return; }

  // 2) PENSO - trascrizione
  setState(ST_THINKING);
  String testo = sttTranscribe(wav, wavLen, "it");
  if (testo.isEmpty()) { fail("Non ho capito"); return; }
  // Filtro anti-allucinazione di Whisper (il "Grazie" fantasma sul silenzio):
  // scarta in silenzio, senza rispondere ne' far ripartire nulla.
  if (isAllucinazione(testo)) {
    Serial.printf(">> scartata allucinazione STT: \"%s\"\n", testo.c_str());
    if (vsOk) ampEnable(false);
    setState(ST_IDLE);
    return;
  }
  Serial.printf(">> TESTO: \"%s\"\n", testo.c_str());
  gobboPrintUser(testo);   // mostra "Tu: ..." nella chat

  // 2-bis) RISPOSTA PERSONALIZZATA: se la domanda contiene il trigger configurato
  // (gSettings.replyTrigger) uso il testo fisso (replyText) e SALTO musica + Claude.
  String risposta = customReplyMatch(testo);
  bool scripted = risposta.length() > 0;

  if (!scripted) {
    // MUSICA (match LOCALE dalla lista del pannello) -> suona subito, veloce,
    // senza disturbare Claude. Lo stop (click/pannello) e il ritorno pulito a
    // riposo li gestisce playStation().
    {
      String tLowerMus = testo; tLowerMus.toLowerCase();
      const MusicStation *st = musicMatch(tLowerMus);
      if (st && vsOk) { playStation(st); return; }
    }

    // 3) PENSO - cervello. Gli do il tool musica (solo se il VS1053 c'e'): per una
    // richiesta di ASCOLTO non capita dal match locale, Claude sceglie una radio dal
    // catalogo (musicGenre) invece di rispondere a voce -> non "cade" in chat.
    String musicGenre;
    risposta = llmAsk(testo, vsOk ? &musicGenre : nullptr);

    // Claude ha deciso di mettere musica?
    if (vsOk && musicGenre.length()) {
      const MusicStation *cs = musicFromGenre(musicGenre);
      if (cs) { playStation(cs); return; }
      // catalogo senza quel genere: lo dice a voce invece di tacere
      Serial.printf(">> musica: genere \"%s\" non in catalogo\n", musicGenre.c_str());
      risposta = String("Non ho una stazione per ") + musicGenre + ", mi dispiace.";
    }
  }

  if (risposta.isEmpty()) { fail("Errore cervello"); return; }

  Serial.printf(">> ALEXO: \"%s\"\n", risposta.c_str());
  gobboPrint(risposta);   // "Alexo: ..." nella chat (scorre con la voce)

  // 4) PARLO
  if (vsOk) {
    setState(ST_SPEAKING);
    // Se LA TUA frase inizia con la parola-trigger -> voce alternativa.
    String t = testo; t.trim(); t.toLowerCase();
    bool vocaltra = matchAnyTerm(t, gSettings.voiceTrigger, false);  // startsWith uno dei termini
    Serial.printf("[tts] voce: %s\n", vocaltra ? "alternativa" : "default");
    ttsSpeak(player, risposta, vocaltra ? gSettings.voiceIdAlt : String(""));
    delay(50); ampEnable(false);   // lascia sfumare la coda di silenzio, poi muta
    // La coda della VOCE rientra nel mic: raffreddamento come per la musica, cosi'
    // non parte una chat fantasma subito dopo la risposta.
    g_audioEndMs = millis();
  } else {
    Serial.println(">> VS1053 non collegato: salto la voce");
  }
  setState(ST_IDLE);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== ALEXO ===");
  Serial.printf("PSRAM: %u bytes (%s)  Flash: %u bytes\n",
                (unsigned)ESP.getPsramSize(), ESP.getPsramSize() > 0 ? "OK" : "NO",
                (unsigned)ESP.getFlashChipSize());

  // Impostazioni runtime (dal pannello web) caricate dall'NVS, default = config.h.
  // Va PRIMA dei moduli che leggono gSettings (mic, wake, llm, tts).
  settingsBegin();

  // Backlight del display su GPIO (acceso subito = splash di boot visibile).
  // Da qui in poi la sua accensione/spegnimento a riposo la gestisce il gobbo.
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);

  // TFT ST7735 su bus SPI dedicato (HSPI). MISO non serve (sola scrittura).
  tftSPI.begin(TFT_SCLK_PIN, -1, TFT_MOSI_PIN, TFT_CS_PIN);
  display.initR(TFT_INITR);
  display.setSPISpeed(TFT_SPI_HZ);   // clock basso = meno EMI sul ring (vedi config.h)
  display.setRotation(0);            // 0 = ritratto 128x160
  display.cp437(true);
  display.setTextWrap(false);
  tftOk = true;                      // l'ST7735 non ha un "isConnected": assumiamo OK
  display.fillScreen(ST77XX_BLACK);  // schermo nero fino allo splash (niente garbage)
  Serial.println("[OK ] TFT ST7735");

  // Ring: brightness PIENA (255) per non quantizzare il fading; la luminosita'
  // effettiva e' tenuta bassa direttamente nei valori delle animazioni (ui.cpp).
  ring.begin();
  ring.setBrightness(255);
  setRingSolid(0, 0, 20);
  Serial.println("[OK ] Ring NeoPixel");

#if SPLASH_BOOT
  bootSplash();                      // splash futuristico (TFT HUD + ring "carica")
#endif

  // Nessun bottone dedicato: l'encoder e' l'unico comando (click = avvia/ferma
  // la chat, premuto+giro = volume, giro = scroll). GPIO14 resta libero.

  // VS1053
  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);

  // --- Init VS1053/VS1003 ROBUSTO + diagnostica AFFIDABILE -------------------
  //  Configuriamo SUBITO i pin di controllo come OUTPUT. begin() lo farebbe, ma
  //  se per un boot non chiamiamo begin() le letture SCI darebbero 0000 FASULLI
  //  (il chip non viene selezionato perche' CS non e' pilotato). Con i CS gia'
  //  impostati, la lettura grezza readRegNoWait() (che NON aspetta il DREQ) e'
  //  SEMPRE attendibile: ci dice davvero se il chip risponde o no.
  pinMode(VS1053_XCS_PIN,  OUTPUT); digitalWrite(VS1053_XCS_PIN,  HIGH);
  pinMode(VS1053_XDCS_PIN, OUTPUT); digitalWrite(VS1053_XDCS_PIN, HIGH);
  pinMode(VS1053_DREQ_PIN, INPUT);
#if VS1053_XRST_PIN >= 0
  pinMode(VS1053_XRST_PIN, OUTPUT);
#endif

  delay(300);                        // stabilizza l'alimentazione del chip al cold-boot
  vsOk = false;
  uint16_t vsSt = 0, vsMode = 0;
  for (int i = 0; i < 8 && !vsOk; i++) {
#if VS1053_XRST_PIN >= 0
    digitalWrite(VS1053_XRST_PIN, LOW);  delay(60);   // reset hardware lungo (XRST)
    digitalWrite(VS1053_XRST_PIN, HIGH);
#endif
    // Dai tempo al chip di uscire dal reset e avviarsi: poll dello SCI_STATUS
    // (RAW, no DREQ) fino a 400ms. Al COLD-boot a volte ci mette di piu' a
    // svegliarsi (alimentazione/oscillatore del modulo).
    uint32_t t = millis();
    do { vsSt = player.readRegNoWait(0x1); delay(5); }
    while ((vsSt == 0x0000 || vsSt == 0xFFFF) && (millis() - t) < 400);
    bool responds = !(vsSt == 0x0000 || vsSt == 0xFFFF);
    if (responds) {
      player.begin();                 // chip presente e vivo -> init completa
      delay(20);
      vsMode = player.readRegNoWait(0x0);
      vsOk = (vsMode == 0x4800);
    } else {
      delay(150);                     // non risponde: aspetta e ritenta col reset
    }
    Serial.printf("[vs1053] tentativo %d: ST=%04X MODE=%04X %s\n",
                  i + 1, (unsigned)vsSt, (unsigned)vsMode,
                  vsOk ? "OK" : (responds ? "(reinit)" : "(chip non risponde)"));
  }
  if (vsOk) volumeBegin(player);   // volume salvato (o VOLUME_DEFAULT) -> VS1053
  Serial.printf("[%s] VS1053\n", vsOk ? "OK " : "ERR");

  // Ampli PAM8302A muto al boot (SD attivo basso) -> niente pop ne' fruscio.
#if AMP_SD_PIN >= 0
  pinMode(AMP_SD_PIN, OUTPUT);
  digitalWrite(AMP_SD_PIN, LOW);
#endif

  // Microfono
  micOk = micBegin();
  Serial.printf("[%s] Microfono %s\n", micOk ? "OK " : "ERR",
                MIC_USE_I2S ? "I2S (ICS-43434)" : "MAX4466");

#if WAKE_ENABLE
  // Wake word locale "Alexo" (work in progress, vedi WAKEWORD.md). Lo stub
  // ritorna false finche' l'inferenza TFLite non e' implementata.
  bool wakeOk = wakeBegin();
  Serial.printf("[%s] Wake word\n", wakeOk ? "OK " : "off");
#endif

  // WiFi
  tftStatus("WiFi...", "connessione");
  bool wifi = wifiBegin();
  Serial.printf("[%s] WiFi\n", wifi ? "OK " : "ERR");

  // OTA: aggiornamento firmware via WiFi (hostname "alexo" -> alexo.local)
  if (wifi) {
    timeBegin();   // orologio via NTP: serve a Claude per rispondere sull'ora
    ArduinoOTA.setHostname("alexo");
    // ArduinoOTA.setPassword("...");   // opzionale: protezione con password
    ArduinoOTA.onStart([]() {                        // schermata OTA HUD verde
      gobboOtaKind(ArduinoOTA.getCommand() == U_SPIFFS);   // FW (firmware) o DATA (filesystem)
      gobboOtaProgress(0);
      setState(ST_OTA);
    });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
      gobboOtaProgress(t ? (uint8_t)((uint64_t)p * 100 / t) : 0);          // % ben visibile al centro
    });
    ArduinoOTA.onEnd([]()    { gobboOtaProgress(100); });                  // poi il device si riavvia
    ArduinoOTA.onError([](ota_error_t){ setState(ST_IDLE); gobboPrint("[OTA errore]"); });
    ArduinoOTA.begin();
    Serial.println("[OK ] OTA pronto (hostname: alexo.local)");
    // Log via rete (Telnet porta 23): per leggere l'output senza USB.
    // Vedi netlog.h. Collegarsi con `telnet alexo.local`.
    netlogBegin();
    Serial.println("[OK ] netlog Telnet pronto (telnet alexo.local)");
    // Pannello impostazioni web (LittleFS + API). http://alexo.local/
    webuiBegin();
  }

  // Step 0 wake-word: diagnostica rumore mic. Se MIC_DIAG=1 il firmware si ferma
  // QUI in un loop che alterna la misura del mic, ArduinoOTA.handle() e
  // netlogHandle(): NON prosegue (niente chat) ma l'OTA resta VIVO e l'output va
  // anche su Telnet. Per uscire: rimetti MIC_DIAG=0 e riflasha via OTA.
#if TFL_SELFTEST
  // Self-test TFLite Micro (passo 2 wake word): gira il modello hello_world in
  // loop e stampa su Telnet, con OTA vivo. Rimetti TFL_SELFTEST=0 e riflasha OTA.
  Serial.println("[tfl] SELF-TEST TFLite Micro ATTIVO (TFL_SELFTEST=1). OTA+Telnet attivi.");
  for (;;) {
    ArduinoOTA.handle();
    netlogHandle();
    tflSelfTest();
    delay(1500);
  }
#endif

#if WAKE_TEST
  // Test catena wake senza mic: frontend->modello->prob su audio
  // sintetico, senza mic. OTA vivo. Rimetti WAKE_TEST=0 e riflasha per uscire.
  Serial.println("[wakeTest] TEST CATENA WAKE ATTIVO (WAKE_TEST=1). OTA+Telnet attivi.");
  for (;;) {
    ArduinoOTA.handle();
    netlogHandle();
    wakeSelfTest();   // ascolto CONTINUO: niente delay (il modello e' streaming)
  }
#endif

#if MIC_DIAG
  Serial.println("[micDiag] MODALITA' DIAGNOSTICA ATTIVA (MIC_DIAG=1). OTA+Telnet attivi.");
  for (;;) {
    ArduinoOTA.handle();
    netlogHandle();
    if (micOk) micDiag();
    else { Serial.println("[micDiag] mic non inizializzato");
           netlogPrintln("[micDiag] mic non inizializzato"); delay(1000); }
  }
#endif

  // Connessione fatta: pulisci il display e avvia il "gobbo" (chat scrollabile)
  // che da qui in poi e' l'unico a scrivere sul TFT (task su core 0).
  if (tftOk) gobboBegin(&display);

  // Encoder rotativo per scorrere la chat (su/giu' = storico, pulsante = live)
  encoderBegin();

  // Avvia le animazioni del ring (task sul core 0) ed entra in riposo
  uiBegin(&ring);
  setState(ST_IDLE);
}

void loop() {
  static uint32_t lastInteraction = 0;
  static bool     convActive = false;

  if (wifiOk()) { ArduinoOTA.handle();   // ascolta richieste di aggiornamento OTA
                  netlogHandle();        // mantiene il client Telnet (log via rete)
                  webuiHandle(); }       // serve il pannello impostazioni web

  // Applica eventuali cambi di volume chiesti dall'encoder (premuto + giro).
  // Qui siamo a riposo; durante il parlato ci pensa ttsSpeak (stesso core/bus).
  if (vsOk) volumeApplyPending(player);

  // DOPPIO click dell'encoder = accende/spegne il ring reattivo al suono (toggle
  // runtime). Lo stato vive in gSettings.idleReactive (default = config.h), cosi'
  // e' condiviso col pannello web e salvato in NVS.
  if (encoderDoublePressed()) {
    gSettings.idleReactive = !gSettings.idleReactive;
    settingsSave();
    Serial.printf("[ring] reattivo al suono: %s\n", gSettings.idleReactive ? "ON" : "OFF");
    netlogPrintln(gSettings.idleReactive ? "[ring] reattivo ON" : "[ring] reattivo OFF");
    for (int b = 0; b < 2; b++) { uiSetLevel(150); delay(80); uiSetLevel(0); delay(80); }  // blink di conferma
    if (micOk) micFlush();   // scarta l'audio letto durante il blink (no falso wake)
  }

  // Click dell'encoder a riposo = avvia la chat (toggle: un altro click ferma
  // la registrazione, gestito dentro runInteraction via recKeepGoing).
  // (il && consuma comunque il talk-request; se siamo nel raffreddamento post
  // musica lo scartiamo per non riavviare una chat a vuoto)
  if (gobboTakeTalkRequest() && (millis() - g_audioEndMs) > AUDIO_COOLDOWN_MS) {
    if (!micOk) {
      fail("Mic non pronto");
    } else {
      runInteraction();
      micFlush(); wakeReset();   // scarta l'audio accumulato, niente falso wake
      lastInteraction = millis();
      convActive = true;
    }
  }

  // Avvio chat: il click encoder (sopra) resta SEMPRE attivo in parallelo.
#if WAKE_ENABLE
  // Wake word "Okay Nabu": ascolto CONTINUO del mic (chunk ~20ms). Al
  // riconoscimento avvia la chat come un click encoder. Lo STESSO chunk pilota
  // anche il livello del ring: niente doppia lettura I2S (che ruberebbe meta'
  // dell'audio al modello streaming).
  if (micOk && wakeReady()) {
    static int16_t wbuf[320];
    size_t got = micReadChunk(wbuf, 320);
    if (got) {
      if (wakeFeed(wbuf, got) && (millis() - g_audioEndMs) > AUDIO_COOLDOWN_MS) {
        Serial.println("[wake] *** WAKE WORD! avvio chat ***");
        netlogPrintln("[wake] *** WAKE WORD! avvio chat ***");
        runInteraction();
        micFlush(); wakeReset();   // scarta l'audio della risposta, niente auto-wake
        lastInteraction = millis();
        convActive = true;
      }
  #if IDLE_REACTIVE
      // Livello del ring dallo STESSO chunk (algoritmo buono: passa-alto +
      // auto-floor + envelope). Spento se il toggle (doppio click) e' OFF.
      uiSetLevel(gSettings.idleReactive ? micLevelFromChunk(wbuf, got) : 0);
  #endif
    }
  }
#elif IDLE_REACTIVE
  // Idle reattivo (senza wake): il ring "balla" col suono. micPeekLevel legge
  // l'I2S col passa-alto; va aggiornato OGNI giro o g_level resta "congelato".
  if (micOk) uiSetLevel(gSettings.idleReactive ? micPeekLevel() : 0);
#endif

  // Dopo 2 minuti di inattivita' azzera la memoria: nuova conversazione
  if (convActive && (millis() - lastInteraction) > 120000) {
    llmReset();
    convActive = false;
    Serial.println("[mem] conversazione azzerata (inattivita')");
  }

  delay(8);
}
