// ============================================================================
//  ALEXO - "Gobbo" + chat scrollabile sul display TFT ST7735 a colori. Vedi gobbo.h.
//
//  - Il TFT mostra la chat ("Tu: ..." / "Alexo: ...") come uno storico in
//    PSRAM (migliaia di righe). Il task gira su CORE 0 ed e' l'unico a toccare
//    il bus SPI del TFT (HSPI); il testo arriva da core 1 via coda FreeRTOS.
//  - Per evitare il flicker (il TFT non ha framebuffer come l'OLED), si rende
//    tutto su un canvas 16bit in RAM e poi si fa UN solo blit a schermo intero.
//  - Colori per ruolo: "Tu:" giallo, "Alexo:" bianco, messaggi di sistema rossi.
//  - Due modalita':
//      AUTO   : segue la voce (la riga "letta" resta a READ_ANCHOR), oppure
//               sta in fondo (live) quando non c'e' parlato.
//      MANUALE: l'ENCODER comanda lo scroll su/giu' nello storico. Si entra
//               girando l'encoder; il pulsante riporta in AUTO (live).
//    L'encoder e' letto QUI sul task (core 0): funziona anche mentre core 1
//    e' bloccato dentro ttsSpeak.
// ============================================================================
#include "gobbo.h"
#include "encoder.h"
#include "volume.h"
#include "config.h"
#include <Adafruit_GFX.h>

static Adafruit_ST7735 *D  = nullptr;
static GFXcanvas16     *CV = nullptr;   // framebuffer in RAM, poi blittato sul TFT

#define COLS         21      // caratteri per riga (128px / 6px per char)
#define LINEH         8      // altezza riga in px (font size 1)
#define VIEWW   TFT_WIDTH    // 128
#define VIEWH   TFT_HEIGHT   // 160
#define HEADER_H     14      // barra fissa in alto: titolo + stato
#define CHATH   (VIEWH - HEADER_H)        // area scrollabile della chat (px)
#define MAXLINES   2000      // righe di storico chat (in PSRAM)
#define READ_ANCHOR  ((CHATH * 3) / 5)    // riga "letta" tenuta a ~3/5 dell'area chat
#define MS_PER_LINE_DEFAULT 700          // pace leggibile senza voce (ms/riga)

// --- Stato per l'header (scritto da qualsiasi core, letto dal task) ----------
static volatile uint8_t g_state = ST_IDLE;   // byte: scrittura/lettura atomica
static volatile uint8_t g_otaPct = 0;        // % avanzamento OTA (schermata dedicata)
static volatile bool    g_otaData = false;   // true = OTA del filesystem (DATA), false = firmware (FW)
// Richieste dall'encoder (core 0) verso la pipeline (core 1): click di avvio /
// click di stop registrazione. Bool allineati -> accesso atomico sull'Xtensa.
static volatile bool g_talkReq = false;
static volatile bool g_stopReq = false;
static volatile int32_t g_musSeek = 0;   // detenti "premuto+giro" in musica -> cambio stazione
// Etichetta + colore per ogni AlexoState (indice = valore enum in ui.h).
static const char *ST_LABEL[] = { "pronto","ascolto","penso","parlo","errore","OTA","musica" };
static const uint16_t ST_COL[] = { ST77XX_BLUE, ST77XX_GREEN, ST77XX_YELLOW,
                                   ST77XX_CYAN, ST77XX_RED, ST77XX_GREEN, ST77XX_MAGENTA };

// Ruolo di una riga -> colore. Lo storico wrappa le frasi su piu' righe, quindi
// ogni riga si porta dietro il suo ruolo (la prima ha il prefisso "Tu:"/"Alexo:").
enum { ROLE_ALEXO = 0, ROLE_USER = 1, ROLE_SYS = 2 };
// ROLE_SYS in GIALLO (il rosso e' poco leggibile su questo pannello ST7735).
static const uint16_t ROLE_COLOR[3] = { ST77XX_WHITE, ST77XX_YELLOW, ST77XX_YELLOW };

// Buffer righe circolare in PSRAM (testo + ruolo paralleli)
static char    *buf      = nullptr;   // MAXLINES * (COLS+1)
static uint8_t *rolebuf  = nullptr;   // MAXLINES
static int      startIdx = 0;         // indice della riga piu' vecchia
static int      nLines   = 0;         // righe valide
static uint8_t  curRole  = ROLE_ALEXO;// ruolo applicato dalle pushLine correnti

// --- Copia leggera della chat per il PANNELLO WEB ---------------------------
//  Ultimi WEBCHAT_MAX messaggi INTERI in UTF-8 (NON i frammenti a 21 col in CP437
//  del TFT): popolata da gobboPrint/gobboPrintUser, che girano sul CORE 1 come
//  webuiHandle -> nessun lock. g_chatRev cambia a ogni messaggio, cosi' il pannello
//  ricarica la chat SOLO quando ne arriva uno nuovo (refresh guidato dagli eventi).
#define WEBCHAT_MAX 40
#define WEBCHAT_LEN 2048          // = GobboMsg.text: stessa capienza del TFT (niente troncamento)
struct WebMsg { uint8_t role; char text[WEBCHAT_LEN]; };
static WebMsg  *webChat  = nullptr;
static int      webStart = 0, webCount = 0;
static volatile uint32_t g_chatRev = 0;

static void webChatPush(uint8_t role, const char *utf8) {
  if (!webChat) return;
  int idx = (webStart + webCount) % WEBCHAT_MAX;
  if (webCount < WEBCHAT_MAX) webCount++;
  else                        webStart = (webStart + 1) % WEBCHAT_MAX;
  webChat[idx].role = role;
  strncpy(webChat[idx].text, utf8, WEBCHAT_LEN - 1);
  webChat[idx].text[WEBCHAT_LEN - 1] = 0;
  g_chatRev++;
}

static float    posY  = 0;         // offset verticale corrente (px)
static uint32_t voiceStart = 0;    // millis inizio scroll auto
static uint32_t voiceDur   = 0;    // durata scroll auto (0 = vai a fondo/live)
static int      respStart  = 0;    // prima riga della risposta corrente
static int      respLines  = 0;    // n. righe della risposta corrente

enum { MODE_AUTO, MODE_MANUAL };
static int mode = MODE_AUTO;

static QueueHandle_t q = nullptr;

// Messaggio in coda.
struct GobboMsg {
  uint8_t  kind;        // 0=alexo  1=clear  2=scrollOver  3=user
  uint32_t ms;          // per kind==2: durata totale dello scroll
  char     text[2048];  // per kind 0/3
};

// --- UTF-8 -> CP437 ---------------------------------------------------------
// Il testo arriva in UTF-8 (es. "è" = 0xC3 0xA8) ma il font del display usa
// CP437 (1 byte/glifo). Convertiamo i caratteri accentati italiani + la
// punteggiatura "tipografica" di Claude nel byte CP437 giusto; il resto -> '?'.
static uint8_t cpFromUnicode(uint32_t u) {
  if (u < 0x80) return (uint8_t)u;
  switch (u) {
    case 0x00E0: return 0x85;  // à
    case 0x00E1: return 0xA0;  // á
    case 0x00E8: return 0x8A;  // è
    case 0x00E9: return 0x82;  // é
    case 0x00EC: return 0x8D;  // ì
    case 0x00ED: return 0xA1;  // í
    case 0x00F2: return 0x95;  // ò
    case 0x00F3: return 0xA2;  // ó
    case 0x00F9: return 0x97;  // ù
    case 0x00FA: return 0xA3;  // ú
    case 0x00E7: return 0x87;  // ç
    case 0x00F1: return 0xA4;  // ñ
    case 0x00C9: return 0x90;  // É
    case 0x00B0: return 0xF8;  // °
    // maiuscole accentate non presenti in CP437 -> lettera base
    case 0x00C0: case 0x00C1: return 'A';   // À Á
    case 0x00C8: case 0x00CA: return 'E';   // È Ê
    case 0x00CC: case 0x00CD: return 'I';   // Ì Í
    case 0x00D2: case 0x00D3: return 'O';   // Ò Ó
    case 0x00D9: case 0x00DA: return 'U';   // Ù Ú
    // punteggiatura tipografica
    case 0x2018: case 0x2019: return '\'';  // ' '
    case 0x201C: case 0x201D: return '"';   // " "
    case 0x2013: case 0x2014: return '-';   // - -
    case 0x2026: return '.';                // ...
    default: return '?';
  }
}

// Converte la stringa UTF-8 in CP437 SUL POSTO (output sempre <= input -> sicuro).
static void utf8ToCp437(char *s) {
  uint8_t *r = (uint8_t *)s, *w = (uint8_t *)s;
  while (*r) {
    uint8_t c = *r; uint32_t u;
    if (c < 0x80)                                  { u = c; r += 1; }
    else if ((c & 0xE0) == 0xC0 && r[1])           { u = ((c & 0x1F) << 6) | (r[1] & 0x3F); r += 2; }
    else if ((c & 0xF0) == 0xE0 && r[1] && r[2])   { u = ((uint32_t)(c & 0x0F) << 12) | ((r[1] & 0x3F) << 6) | (r[2] & 0x3F); r += 3; }
    else if ((c & 0xF8) == 0xF0 && r[1] && r[2] && r[3]) { u = 0xFFFD; r += 4; }
    else                                           { u = c; r += 1; }
    *w++ = cpFromUnicode(u);
  }
  *w = 0;
}

// --- Buffer righe (solo task, core 0) ---------------------------------------
static inline char    *slot(int i)     { return buf + ((startIdx + i) % MAXLINES) * (COLS + 1); }
static inline uint8_t &roleAt(int i)    { return rolebuf[(startIdx + i) % MAXLINES]; }

static void pushLine(const char *s) {
  if (nLines < MAXLINES) {
    strncpy(slot(nLines), s, COLS); slot(nLines)[COLS] = 0;
    roleAt(nLines) = curRole;
    nLines++;
  } else {
    // pieno: sovrascrive la piu' vecchia e avanza -> la nuova diventa l'ultima
    strncpy(slot(0), s, COLS); slot(0)[COLS] = 0;
    roleAt(0) = curRole;
    startIdx = (startIdx + 1) % MAXLINES;
    if (posY >= LINEH) posY -= LINEH;
    if (respStart > 0)  respStart--;
  }
}

// Manda a capo 'text' a parole in righe da COLS e le accoda.
static void addText(const char *text) {
  char word[COLS + 1]; int wl = 0;
  char line[COLS + 1]; int ll = 0;
  line[0] = 0;

  auto flushWord = [&]() {
    if (wl == 0) return;
    word[wl] = 0;
    if (ll == 0)                         { strcpy(line, word); ll = wl; }
    else if (ll + 1 + wl <= COLS)        { line[ll++] = ' '; memcpy(line + ll, word, wl); ll += wl; line[ll] = 0; }
    else                                 { pushLine(line); strcpy(line, word); ll = wl; line[ll] = 0; }
    wl = 0;
  };

  for (const char *p = text; *p; p++) {
    char c = *p;
    if (c == '\n')      { flushWord(); pushLine(line); line[0] = 0; ll = 0; }
    else if (c == ' ')  { flushWord(); }
    else if (wl < COLS) { word[wl++] = c; }     // parole >COLS: troncate
  }
  flushWord();
  if (ll > 0) pushLine(line);
}

// --- Render (solo task) -----------------------------------------------------
//  Disegna sul canvas in RAM e fa UN blit -> niente flicker. Per risparmiare
//  CPU/bus si ridisegna solo quando qualcosa e' cambiato (posY o contenuto).
// Header fisso in alto: "ALEXO" a sinistra, etichetta di stato (colorata) a
// destra, riga di separazione del colore dello stato. Disegnato DOPO la chat
// cosi' copre eventuali righe che sconfinano sotto l'header.
static void drawHeader() {
  uint8_t s = g_state; if (s > ST_MUSIC) s = ST_IDLE;
  CV->fillRect(0, 0, VIEWW, HEADER_H, ST77XX_BLACK);
  CV->setTextColor(ST77XX_WHITE);
  CV->setCursor(2, 3);
  CV->print("ALEXO");
  const char *lbl = ST_LABEL[s];
  int x = VIEWW - (int)strlen(lbl) * 6 - 2;   // 6px per glifo (font size 1)
  CV->setTextColor(ST_COL[s]);
  CV->setCursor(x, 3);
  CV->print(lbl);
  CV->drawFastHLine(0, HEADER_H - 1, VIEWW, ST_COL[s]);
}

static void render() {
  if (!CV) return;
  CV->fillScreen(ST77XX_BLACK);
  CV->setTextSize(1);   // la schermata IN ONDA usa size 2: qui la chat/header sono size 1
  int sy = (int)(posY + 0.5f);
  for (int i = 0; i < nLines; i++) {
    int y = HEADER_H + i * LINEH - sy;       // la chat vive sotto l'header
    if (y >= VIEWH) break;
    if (y <= HEADER_H - LINEH) continue;     // sopra l'header: lo copre comunque
    CV->setTextColor(ROLE_COLOR[roleAt(i)]);
    CV->setCursor(0, y);
    CV->print(slot(i));
  }
  drawHeader();
  D->drawRGBBitmap(0, 0, CV->getBuffer(), VIEWW, VIEWH);
}

// --- Schermata OTA dedicata (stile HUD, tema VERDE) -------------------------
//  Stessa "vibe" dello splash di boot ma verde e con la percentuale GRANDE al
//  centro. Disegnata sul canvas + un solo blit (niente flicker). La chiama SOLO
//  il task del gobbo (unico proprietario del bus TFT) mentre g_state == ST_OTA.
static void renderOtaScreen(uint8_t pct) {
  if (!CV) return;
  const int W = VIEWW, H = VIEWH;                 // 128 x 160
  const uint16_t BLK  = ST77XX_BLACK;
  const uint16_t GRN  = D->color565(0, 255, 90); // verde acceso (come il ring OTA)
  const uint16_t GRNd = D->color565(0, 70, 32);  // verde tenue (griglia)
  const uint16_t GRNm = D->color565(0, 210, 95); // verde medio (piu' luminoso)

  CV->fillScreen(BLK);

  // griglia tenue di sfondo
  for (int y = 0; y < H; y += 8) CV->drawFastHLine(0, y, W, GRNd);
  for (int x = 0; x <= W; x += 16) CV->drawFastVLine(x, 0, H, GRNd);

  // parentesi angolari stile HUD
  const int b = 12;
  CV->drawFastHLine(2, 2, b, GRN);            CV->drawFastVLine(2, 2, b, GRN);
  CV->drawFastHLine(W - 2 - b, 2, b, GRN);    CV->drawFastVLine(W - 3, 2, b, GRN);
  CV->drawFastHLine(2, H - 3, b, GRN);        CV->drawFastVLine(2, H - 3 - b, b, GRN);
  CV->drawFastHLine(W - 2 - b, H - 3, b, GRN);CV->drawFastVLine(W - 3, H - 3 - b, b, GRN);

  // titolo sulla stessa riga (verde acceso, centrato)
  CV->setTextColor(GRN); CV->setTextSize(2);
  // Tipo di update: firmware ("FW") o filesystem/pagina web ("DATA"). Stesso font
  // (size 1); la centratura usa strlen -> resta centrato qualunque sia il testo.
  const char *sub = g_otaData ? "DATA" : "FW";
  CV->setCursor((W - (4+(int)strlen(sub)) * 12) / 2, 15); CV->print("OTA "); CV->print(sub);

  // percentuale GRANDE al centro (font size 4 = 24px per cifra)
  if (pct > 100) pct = 100;
  char num[8]; snprintf(num, sizeof(num), "%u%%", (unsigned)pct);
  CV->setTextSize(4);
  int nw = (int)strlen(num) * 24;
  CV->setTextColor(GRN);
  CV->setCursor((W - nw) / 2, 58); CV->print(num);

  // barra di avanzamento (alzata: la scritta sotto usciva dallo schermo)
  const int bx = 12, by = 104, bw = W - 24, bh = 14;
  CV->drawRect(bx, by, bw, bh, GRNm);
  int fill = (int)((long)(bw - 4) * pct / 100);
  if (fill < 0) fill = 0; else if (fill > bw - 4) fill = bw - 4;
  CV->fillRect(bx + 2, by + 2, fill, bh - 4, GRN);

  // avviso (verde acceso, ben dentro lo schermo)
  CV->setTextColor(GRN); CV->setTextSize(1);
  const char *warn = "NON SPEGNERE";
  CV->setCursor((W - (int)strlen(warn) * 6) / 2, by + 24); CV->print(warn);

  D->drawRGBBitmap(0, 0, CV->getBuffer(), VIEWW, VIEWH);
}

// --- Schermata "IN ONDA" (brano radio in corso) -----------------------------
//  Sfondo pulito, "IN ONDA" in alto, poi 3 righe size 2 distanziate: emittente,
//  titolo, artista. Se una riga e' piu' larga dello schermo scorre in marquee
//  (dx->sx). Le stringhe (gia' in CP437) le aggiorna il task da un messaggio
//  kind==4; np_changeMs si azzera a ogni cambio brano -> il marquee riparte e la
//  schermata si "ripulisce". Disegnata solo dal task (unico sul bus TFT).
static char     np_station[96] = "";
static char     np_title[96]   = "";
static char     np_artist[96]  = "";
static uint32_t np_changeMs    = 0;

// Velocità del marquee (px/s), tarabile: la scelta è in base a QUANTO il testo
// sporge dallo schermo, non alla lunghezza assoluta. Poco più largo dello schermo
// -> velocità ~ NP_SPEED_MIN (molto lento); più sporge, più accelera (NP_SPEED_K
// px/s per ogni px in eccesso), fino a NP_SPEED_MAX (tetto per restare leggibile).
#define NP_SPEED_MIN  10.0f    // px/s quando il testo supera di poco lo schermo
#define NP_SPEED_K     0.14f   // px/s in più per ogni px che sporge oltre lo schermo
#define NP_SPEED_MAX  45.0f    // px/s massimi (oltre non si legge)

// Una riga size 2: centrata se ci sta, altrimenti marquee (con una seconda copia
// dopo un gap per il loop continuo). La velocità la calcola qui in base al testo.
static void drawNpLine(int y, const char *s, uint16_t col) {
  CV->setTextSize(2);
  CV->setTextColor(col);
  int textW = (int)strlen(s) * 12;              // 6px * size 2
  if (textW == 0) return;
  if (textW <= VIEWW - 4) {
    CV->setCursor((VIEWW - textW) / 2, y); CV->print(s);   // ci sta: centrata, ferma
    return;
  }
  // Marquee. Velocità proporzionale a quanto il testo sporge (textW - schermo):
  // ~0 sporgenza -> NP_SPEED_MIN (lento); tanta sporgenza -> più veloce, con tetto.
  const int GAP = 34;
  int loopW = textW + GAP;
  float overflow = (float)(textW - VIEWW);
  if (overflow < 0) overflow = 0;
  float pxPerSec = NP_SPEED_MIN + overflow * NP_SPEED_K;
  if (pxPerSec > NP_SPEED_MAX) pxPerSec = NP_SPEED_MAX;
  int ox = (int)((float)(millis() - np_changeMs) * pxPerSec / 1000.0f) % loopW;
  CV->setCursor(2 - ox, y);           CV->print(s);
  CV->setCursor(2 - ox + loopW, y);   CV->print(s);        // seconda copia -> loop senza buchi
}

static void renderNowPlaying() {
  if (!CV) return;
  const uint16_t CY = D->color565(0, 229, 255);
  const uint16_t CYd = D->color565(0, 70, 85);
  const uint16_t WH = ST77XX_WHITE;
  const uint16_t YE = ST77XX_YELLOW;
  CV->fillScreen(ST77XX_BLACK);

  // header "IN ONDA"
  CV->setTextSize(3); CV->setTextColor(CY);
  const char *h = "IN ONDA";
  CV->setCursor((VIEWW - (int)strlen(h) * 18) / 2, 4); CV->print(h);
  CV->drawFastHLine(0, 26, VIEWW, CYd);

  drawNpLine(54,  np_station, YE);   // 1) emittente (marquee: velocità calcolata dentro)
  drawNpLine(94,  np_title,   WH);   // 2) titolo
  drawNpLine(124, np_artist,  WH);   // 3) artista

  /*
  CV->setTextSize(1); CV->setTextColor(CY);
  CV->setCursor(1,40); CV->print((int)strlen(np_station));
  CV->setCursor(45,40); CV->print((int)strlen(np_title));
  CV->setCursor(90,40); CV->print((int)strlen(np_artist));
  */

  D->drawRGBBitmap(0, 0, CV->getBuffer(), VIEWW, VIEWH);
}

// --- Task (core 0) ----------------------------------------------------------
static void gobboTask(void *) {
  const uint32_t FRAME_MS = 30;
  char tmp[2048 + 8];
  float   lastPosY  = -1.0f;
  int     lastN     = -1;
  uint8_t lastState = 0xFF;
  // Risparmio energetico del display: si spegne il backlight dopo
  // DISPLAY_SLEEP_MS senza interventi, si riaccende al primo intervento.
  uint32_t lastActivity = millis();
  bool     blOn = true;          // acceso al boot (vedi setup() in main.cpp)
  for (;;) {
    // OTA in corso: schermata dedicata HUD verde con la % al centro. La chat e
    // l'encoder sono sospesi; il backlight resta acceso. Il core 1 aggiorna solo
    // g_otaPct via gobboOtaProgress(); qui la disegniamo.
    if (g_state == (uint8_t)ST_OTA) {
      if (!blOn) { digitalWrite(TFT_BL_PIN, HIGH); blOn = true; }
      renderOtaScreen(g_otaPct);
      lastActivity = millis();
      lastState = 0xFF;          // forza un redraw completo all'uscita dall'OTA
      vTaskDelay(pdMS_TO_TICKS(80));
      continue;
    }

    bool activity = false;       // un qualunque "intervento" in questo frame?

    // 1) messaggi in coda (nuova domanda/risposta/stato = intervento)
    GobboMsg *m;
    while (q && xQueueReceive(q, &m, 0) == pdTRUE) {
      activity = true;
      if (m->kind == 1) {                         // clear
        nLines = 0; startIdx = 0; posY = 0;
        respStart = 0; respLines = 0; voiceDur = 0; mode = MODE_AUTO;
      } else if (m->kind == 2) {                  // scrollOver: timing voce
        voiceStart = millis(); voiceDur = m->ms; mode = MODE_AUTO;
      } else if (m->kind == 4) {                  // now playing: "emittente\ntitolo\nartista"
        char *s1 = m->text;
        char *s2 = strchr(s1, '\n'); if (s2) *s2++ = 0; else s2 = s1 + strlen(s1);
        char *s3 = strchr(s2, '\n'); if (s3) *s3++ = 0; else s3 = s2 + strlen(s2);
        strncpy(np_station, s1, sizeof(np_station) - 1); np_station[sizeof(np_station) - 1] = 0;
        strncpy(np_title,   s2, sizeof(np_title)   - 1); np_title[sizeof(np_title)   - 1] = 0;
        strncpy(np_artist,  s3, sizeof(np_artist)  - 1); np_artist[sizeof(np_artist)  - 1] = 0;
        utf8ToCp437(np_station); utf8ToCp437(np_title); utf8ToCp437(np_artist);
        np_changeMs = millis();                   // cambio brano -> marquee riparte, schermata pulita
      } else if (m->kind == 3) {                  // domanda utente
        curRole = ROLE_USER;
        snprintf(tmp, sizeof(tmp), "Tu: %s", m->text);
        utf8ToCp437(tmp);
        addText(tmp); pushLine("");
        voiceDur = 0; mode = MODE_AUTO;           // mostra in fondo
      } else {                                    // risposta Alexo
        respStart = nLines;
        // "[...]" = messaggio di sistema (errori, OTA) -> rosso; altrimenti Alexo.
        curRole = (m->text[0] == '[') ? ROLE_SYS : ROLE_ALEXO;
        snprintf(tmp, sizeof(tmp), "Alexo: %s", m->text);
        utf8ToCp437(tmp);
        addText(tmp); pushLine("");
        respLines  = nLines - respStart;
        voiceStart = millis();
        voiceDur   = (uint32_t)respLines * MS_PER_LINE_DEFAULT;  // poi sovrascritta
        mode = MODE_AUTO;
      }
      free(m);
    }

    // 2) encoder = unico input. Tre gesti:
    //    - giro libero        -> scroll manuale della chat
    //    - PREMUTO + giro      -> volume (orario = su, antiorario = giu')
    //    - click secco (premi/rilascia senza girare):
    //          a riposo        -> AVVIA la chat (registra)
    //          mentre registra -> FERMA e invia  (toggle)
    //  Qui non si tocca l'hardware audio (il VS1053/mic stanno sul core 1): si
    //  accumulano solo richieste (volumeRequest / talk / stop) che applica il
    //  core 1. Il "torna live" non serve: ogni nuovo messaggio rimette in AUTO.
    // Debounce del pulsante: filtra i glitch. Col pin SW flottante (encoder non
    // ancora cablato) i disturbi EMI generano "click fantasma" -> registrazioni
    // di silenzio -> Whisper allucina ("Grazie"). Pretendiamo 3 frame coerenti
    // (~90ms) prima di accettare un cambio di stato del pulsante.
    static uint8_t dbCount = 0;
    static bool    dbHeld  = false;
    bool rawHeld = encoderButtonHeld();
    if (rawHeld != dbHeld) { if (++dbCount >= 3) { dbHeld = rawHeld; dbCount = 0; } }
    else                     dbCount = 0;
    bool    held = dbHeld;
    int32_t d    = encoderTake();

    // Durante la MUSICA il giro (nudo O premuto) regola il VOLUME come la manopola
    // di una radio. Il cambio stazione NON e' piu' sul premuto+giro (scomodo) ma sul
    // CLICK singolo (vedi sotto). Fuori dalla musica: premuto+giro=volume, giro=scroll.
    if (g_state == (uint8_t)ST_MUSIC) {
      if (d != 0) volumeRequest(d);                          // in musica: qualsiasi giro -> volume
    } else if (held) {
      if (d != 0) volumeRequest(d);                          // premuto+giro -> volume
    } else if (d != 0) {
      mode = MODE_MANUAL; posY += (float)d * LINEH;          // giro libero -> scroll
    }
    // Click SINGOLO confermato (gia' disambiguato dal doppio, escluso se si e' girato):
    //   in MUSICA  -> stazione SUCCESSIVA (avanti, ciclico)
    //   in ascolto -> ferma la registrazione
    //   altrimenti -> avvia la chat
    if (encoderButtonPressed()) {
      if      (g_state == (uint8_t)ST_MUSIC)     g_musSeek += 1;   // click = radio successiva
      else if (g_state == (uint8_t)ST_LISTENING) g_stopReq = true; // ferma registrazione
      else                                       g_talkReq = true; // avvia chat
    }
    // DOPPIO click in MUSICA = esci dalla radio e torna alla chat (musicPlay legge
    // gobboTakeTalkRequest come STOP). Fuori dalla musica il doppio click lo gestisce
    // main.cpp (toggle ring reattivo): la' il core 1 non e' bloccato.
    if (g_state == (uint8_t)ST_MUSIC && encoderDoublePressed()) {
      g_talkReq = true;
    }

    // Qualunque uso dell'encoder (giro o pulsante) e' un intervento; cosi' lo e'
    // anche Alexo "occupato" (ascolto/penso/parlo/OTA) -> il display resta acceso.
    if (d != 0 || held) activity = true;
    if (g_state != (uint8_t)ST_IDLE) activity = true;

    // 3) MUSICA: schermata dedicata "IN ONDA" (marquee -> redraw continuo).
    //    L'encoder qui sopra resta attivo (giro = volume). Altrimenti: chat.
    if (g_state == (uint8_t)ST_MUSIC) {
      renderNowPlaying();
      lastState = 0xFF;   // forza un redraw completo della chat all'uscita dalla musica
    } else {
      // 3b) calcola posizione (l'area scrollabile e' CHATH, sotto l'header)
      float maxScroll = (nLines * LINEH > CHATH) ? (float)(nLines * LINEH - CHATH) : 0.0f;
      if (mode == MODE_MANUAL) {
        if (posY < 0) posY = 0; else if (posY > maxScroll) posY = maxScroll;
      } else {  // AUTO
        if (voiceDur > 0 && maxScroll > 0) {
          float f = (float)(millis() - voiceStart) / (float)voiceDur;
          if (f < 0) f = 0; else if (f > 1) f = 1;
          float readPx = (float)(respStart * LINEH) + f * (respLines * LINEH);
          float tgt = readPx - READ_ANCHOR;
          if (tgt < 0) tgt = 0; else if (tgt > maxScroll) tgt = maxScroll;
          posY = tgt;
        } else {
          posY = maxScroll;   // live: in fondo
        }
      }

      // 4) ridisegna solo se serve (evita blit inutili a schermo fermo)
      if (posY != lastPosY || nLines != lastN || g_state != lastState) {
        render();
        lastPosY = posY; lastN = nLines; lastState = g_state;
      }
    }

    // 5) backlight: sveglia al primo intervento, spegni dopo l'inattivita'.
    //    (digitalWrite e' su un GPIO, non sul bus SPI: nessuna contesa.)
#if DISPLAY_SLEEP_MS > 0
    if (activity) {
      lastActivity = millis();
      if (!blOn) { digitalWrite(TFT_BL_PIN, HIGH); blOn = true; }
    } else if (blOn && (millis() - lastActivity) > DISPLAY_SLEEP_MS) {
      digitalWrite(TFT_BL_PIN, LOW); blOn = false;
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
  }
}

// --- API --------------------------------------------------------------------
void gobboBegin(Adafruit_ST7735 *disp) {
  D = disp;
  buf     = (char *)ps_malloc((size_t)MAXLINES * (COLS + 1));
  if (!buf) buf = (char *)malloc((size_t)MAXLINES * (COLS + 1));  // fallback
  rolebuf = (uint8_t *)ps_malloc(MAXLINES);
  if (!rolebuf) rolebuf = (uint8_t *)malloc(MAXLINES);
  webChat = (WebMsg *)ps_malloc(sizeof(WebMsg) * WEBCHAT_MAX);   // copia chat per il web
  if (!webChat) webChat = (WebMsg *)malloc(sizeof(WebMsg) * WEBCHAT_MAX);

  // Canvas in RAM (128x160x2 = 40KB di SRAM interna): GFXcanvas16 alloca con
  // malloc. 40KB sull'S3 ci stanno comodi; la storia chat invece e' in PSRAM.
  CV = new GFXcanvas16(VIEWW, VIEWH);
  if (CV) {
    CV->setTextSize(1);
    CV->setTextWrap(false);
    CV->cp437(true);              // mappa corretta dei glifi accentati (CP437)
    CV->fillScreen(ST77XX_BLACK);
  }
  D->fillScreen(ST77XX_BLACK);

  q = xQueueCreate(8, sizeof(GobboMsg *));
  xTaskCreatePinnedToCore(gobboTask, "gobbo", 6144, nullptr, 1, nullptr, 0);
}

static void send(uint8_t kind, const String &text, uint32_t ms) {
  if (!q) return;
  GobboMsg *m = (GobboMsg *)malloc(sizeof(GobboMsg));
  if (!m) return;
  m->kind = kind; m->ms = ms;
  strncpy(m->text, text.c_str(), sizeof(m->text) - 1);
  m->text[sizeof(m->text) - 1] = 0;
  if (xQueueSend(q, &m, 0) != pdTRUE) free(m);
}

void gobboSetState(AlexoState s)        { g_state = (uint8_t)s; }

// Click di avvio: ritorna true UNA volta (e azzera) se c'e' una richiesta.
bool gobboTakeTalkRequest() { bool r = g_talkReq; g_talkReq = false; return r; }
// Click di stop durante la registrazione (letto dal predicato di micRecord).
bool gobboStopRequested()   { return g_stopReq; }
// Azzera lo stop pendente: il core 1 lo chiama appena prima di registrare.
void gobboClearStopRequest(){ g_stopReq = false; }
int32_t gobboTakeMusicSeek() { int32_t d = g_musSeek; g_musSeek -= d; return d; }
void gobboPrint(const String &text)     { if (!text.isEmpty()) { webChatPush(text[0] == '[' ? ROLE_SYS : ROLE_ALEXO, text.c_str()); send(0, text, 0); } }
void gobboPrintUser(const String &text) { if (!text.isEmpty()) { webChatPush(ROLE_USER, text.c_str()); send(3, text, 0); } }

// Contatore di revisione della chat: cambia a ogni nuovo messaggio (il pannello lo
// legge in /api/live e ricarica /api/chat solo quando e' cambiato).
uint32_t gobboChatRev() { return g_chatRev; }

// Accessori per servire la chat in STREAMING (il webui manda un messaggio alla volta,
// senza costruire una String enorme in RAM). Il puntatore resta valido per la durata
// della richiesta (webui e gobboPrint girano entrambi sul core 1 -> sequenziali).
int gobboChatCount() { return webCount; }
bool gobboChatItem(int i, uint8_t *role, const char **text) {
  if (i < 0 || i >= webCount) return false;
  int idx = (webStart + i) % WEBCHAT_MAX;
  *role = webChat[idx].role;
  *text = webChat[idx].text;
  return true;
}
void gobboScrollOver(uint32_t ms)       { send(2, "", ms); }
void gobboClear()                       { send(1, "", 0); }
void gobboOtaProgress(uint8_t percent)  { g_otaPct = percent > 100 ? 100 : percent; }
void gobboOtaKind(bool isData)          { g_otaData = isData; }
void gobboNowPlaying(const char *station, const char *title, const char *artist) {
  // impacchetta le 3 righe separate da '\n' (il task le divide, vedi kind==4)
  String p = String(station ? station : "") + "\n" +
             String(title   ? title   : "") + "\n" +
             String(artist  ? artist  : "");
  send(4, p, 0);
}
