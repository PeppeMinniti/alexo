// ============================================================================
//  ALEXO - Cattura microfono. DUE backend selezionabili in config.h:
//    MIC_USE_I2S = 0  -> MAX4466 analogico su ADC1 (campionamento ritmato)
//    MIC_USE_I2S = 1  -> mic I2S digitale ICS-43434/INMP441 (DMA hardware)
//  L'API pubblica (mic.h) e' identica nei due casi: il resto del firmware non
//  cambia. Per passare da uno all'altro: cambia MIC_USE_I2S e ricompila.
// ============================================================================
#include "mic.h"
#include "config.h"
#include "settings.h"
#include "netlog.h"
#include <math.h>

// Stampa una riga sia su Serial sia sul log di rete (Telnet): cosi' la
// diagnostica si legge anche col sistema chiuso nel case (no USB).
static void micLogln(const char *s) {
  Serial.println(s);
  netlogPrintln(s);
}

#if MIC_USE_I2S
  #include "driver/i2s.h"
#else
  #include "driver/adc.h"
  #include "esp_adc_cal.h"
#endif

// --- Parametri comuni -------------------------------------------------------
#define MIC_MAX_SECONDS 20   // buffer PSRAM: tetto registrazione (vedi REC_MAX_MS)
static const size_t MIC_MAX_SAMPLES = (size_t)MIC_SAMPLE_RATE * MIC_MAX_SECONDS;

// --- Buffer comuni (PSRAM) --------------------------------------------------
static int16_t *g_pcm   = nullptr;   // PCM 16-bit mono
static uint8_t *g_wav   = nullptr;   // header WAV + copia PCM
static size_t   g_count = 0;         // campioni nell'ultima registrazione
static int      g_peak  = 0;         // picco assoluto ultima registrazione
static bool     g_heard = false;     // voce vera rilevata (sopra soglia) nell'ultima reg.

// ============================================================================
//                    BACKEND I2S (ICS-43434 / INMP441)
// ============================================================================
#if MIC_USE_I2S

// Idle reattivo AUTO-CALIBRANTE + ENVELOPE FOLLOWER (anti-impulso, anti-flash).
// Il rumore di fondo dell'I2S e' IMPULSIVO: a riposo il MAD sta basso ma fa picchi
// sporadici molto alti (anche dopo il passa-alto). Strategia: (1) un "noise floor"
// insegue da solo la base; (2) invece di un gate binario (che o ammazzava la
// sensibilita' o lasciava passare flash a raffica), la luminosita' segue un
// ENVELOPE con ATTACCO LENTO: un impulso isolato (1 frame) non fa in tempo a
// salire -> niente flash; il suono vero, che dura piu' frame, accende pieno.
//  MIC_LVL_MARGIN e MIC_LVL_FLOOR sono ora RUNTIME (gSettings.micLvlMargin/Floor,
//  modificabili dal pannello web); i default di fabbrica stanno in config.h.
#define MIC_LVL_GAIN    1.0f   // guadagno idle reattivo (luminosita' vs suono)
//  ATTACK (salita/reattivita') e RELEASE (discesa/permanenza) sono ora RUNTIME
//  (gSettings.micLvlAttack/Release, dal pannello web); default in config.h.
#define MIC_DEBUG       0

static const i2s_port_t I2S_PORT = I2S_NUM_0;

// Snapshot "live" per il pannello web (aggiornati da micLevelFromChunk).
static volatile uint8_t g_liveLevel  = 0;
static volatile float   g_liveFloor  = 0;
static volatile float   g_liveThresh = 0;

bool micBegin() {
  if (!psramFound()) { Serial.println("[mic] PSRAM non disponibile!"); return false; }
  if (!g_pcm) g_pcm = (int16_t *)ps_malloc(MIC_MAX_SAMPLES * sizeof(int16_t));
  if (!g_wav) g_wav = (uint8_t *)ps_malloc(44 + MIC_MAX_SAMPLES * sizeof(int16_t));
  if (!g_pcm || !g_wav) { Serial.println("[mic] allocazione PSRAM fallita!"); return false; }

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = MIC_SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;   // INMP441: 24 bit in 32
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;    // L/R a GND -> canale LEFT
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = I2S_SCK_PIN;
  pins.ws_io_num    = I2S_WS_PIN;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = I2S_SD_PIN;

  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) {
    Serial.println("[mic] i2s_driver_install fallita!");
    return false;
  }
  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.printf("[mic] ICS-43434 I2S pronto (BCLK=%d WS=%d SD=%d), %d Hz\n",
                I2S_SCK_PIN, I2S_WS_PIN, I2S_SD_PIN, MIC_SAMPLE_RATE);
  return true;
}

size_t micRecord(uint32_t maxMs, bool (*keepGoing)(), void (*onLevel)(uint8_t), uint32_t silenceMs) {
  if (!g_pcm) return 0;
  const size_t maxSamples =
      min(MIC_MAX_SAMPLES, (size_t)((uint64_t)MIC_SAMPLE_RATE * maxMs / 1000));

  static int32_t buf[256];
  size_t   n = 0;
  int      peak = 0, chunkPeak = 0;
  double   chunkSumSq = 0;    // somma dei quadrati nel chunk -> energia media (RMS)
  long     chunkCount = 0;    // campioni nel chunk (per la media)
  float    silLevel   = 0;    // livello "silenzio" smussato (envelope anti-raffica)
  float    noiseFloor = -1;   // fondo di rumore auto-adattivo (ventilatore/vento)
  uint32_t levelTimer = millis();
  // Stop automatico al silenzio: dopo aver sentito parlare (heard), se restano
  // silenceMs di silenzio continuo si chiude. Se non si sente nulla, chiude dopo
  // una attesa di cortesia (grazia + silenzio + 1s).
  const uint32_t recStart = millis();
  uint32_t lastSound = recStart;
  bool     heard = false;

  // Passa-alto a un polo (~120 Hz) sul dominio a 24 bit: rimuove il DC e la deriva
  // a bassa frequenza che, misurata in diagnostica (Step 0), erano il grosso del
  // rumore di fondo dell'ICS-43434 (la voce sopra i 120 Hz passa intatta). Stato
  // azzerato a ogni registrazione (piccolo transitorio iniziale, trascurabile).
  //   y[n] = x[n] - x[n-1] + R*y[n-1]
  double hpX = 0, hpY = 0;
  const double hpR = 0.95;
  const int    shift24 = I2S_SHIFT - 8;          // shift dal dominio 24 bit a 16

  while (n < maxSamples) {
    if (keepGoing && !keepGoing()) break;
    size_t br = 0;
    i2s_read(I2S_PORT, buf, sizeof(buf), &br, portMAX_DELAY);
    int got = br / 4;
    for (int i = 0; i < got && n < maxSamples; i++) {
      double v = (double)(buf[i] >> 8);           // campione a 24 bit
      double y = v - hpX + hpR * hpY;             // passa-alto (toglie DC/rumble)
      hpX = v; hpY = y;
      int32_t s = (int32_t)y >> shift24;          // 24->16 bit col guadagno scelto
      if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
      g_pcm[n++] = (int16_t)s;
      int a = s < 0 ? -s : s;
      if (a > peak) peak = a;
      if (a > chunkPeak) chunkPeak = a;
      chunkSumSq += (double)s * s;   // energia -> RMS per lo stop-al-silenzio
      chunkCount++;
    }
    if ((millis() - levelTimer) >= 20) {
      int l = (int)((long)chunkPeak * 255 / 4000);   // livello PICCO -> LED (0..255)
      if (l > 255) l = 255;
      if (onLevel) onLevel((uint8_t)l);
      // --- Stop automatico al silenzio, con SOGLIA AUTO-ADATTIVA -------------
      //  Invece di un livello fisso, la "voce" e' RELATIVA al rumore di fondo.
      //  Il fondo (ventilatore/vento) e' stimato in continuo da noiseFloor, che
      //  scende in fretta (si aggancia al silenzio/rumore costante) e sale
      //  pochissimo (la voce non lo trascina su -> resta il rumore, non la voce).
      //  Uso l'ENERGIA MEDIA (RMS) del chunk, non il picco: il vento sul mic fa
      //  picchi altissimi ma isolati (il picco li vede, la media no), quindi la
      //  media separa molto meglio "vento" da "voce". La soglia e':
      //      soglia = noiseFloor * REC_SILENCE_MARGIN + REC_SILENCE_FLOOR
      //  cosi' si adatta da solo se cambia il rumore, senza ritarare a mano.
      if (silenceMs) {
        float rms = (chunkCount > 0) ? sqrtf((float)(chunkSumSq / chunkCount)) : 0.0f;
        if (noiseFloor < 0) noiseFloor = rms;                 // seed col primo chunk
        float k = (rms < noiseFloor) ? 0.30f : 0.02f;         // giu' veloce, su lento
        noiseFloor += (rms - noiseFloor) * k;
        silLevel += (rms - silLevel) * 0.40f;                 // envelope anti-raffica
        float soglia = noiseFloor * REC_SILENCE_MARGIN + REC_SILENCE_FLOOR;
        uint32_t now = millis();
        if (silLevel >= soglia) { lastSound = now; heard = true; }
        bool stopSilenzio  = heard && (now - lastSound) >= silenceMs;
        bool stopNienteVoce = !heard && (now - recStart) >= (uint32_t)(REC_MIN_MS + silenceMs + 1000);
        if (stopSilenzio || stopNienteVoce) { chunkPeak = 0; break; }
      }
      levelTimer = millis();
      chunkPeak = 0;
      chunkSumSq = 0; chunkCount = 0;
    }
  }
  g_count = n;
  g_peak  = peak;
  // Se lo stop-al-silenzio e' attivo, 'heard' dice se abbiamo sentito voce vera
  // (sopra la soglia adattiva). Se disattivato (silenceMs==0) non possiamo saperlo:
  // consideriamo "sentito" per non scartare registrazioni buone.
  g_heard = silenceMs ? heard : true;
  return n;
}

// Diagnostica del rumore di fondo (Step 0 wake-word). SINGLE-SHOT: fa UNA misura
// (~600 ms) e stampa UNA riga, poi RITORNA. Va chiamata ripetutamente dal loop di
// setup, alternata a ArduinoOTA.handle(), cosi' l'OTA resta vivo e si puo'
// riflashare per disattivarla (NIENTE loop infinito che murerebbe l'OTA).
// Misura i campioni a 24 bit PIENI (buf>>8: il dato dell'ICS-43434 e' giustificato
// a sinistra in 32 bit) e proietta cosa darebbe ciascun I2S_SHIFT
// (out16 = v24 >> (SHIFT-8)): a riposo si vuole AC16 basso (~10-30), parlando si
// vuole PEAK16 sano (qualche migliaio, senza saturare a 32767).
void micDiag() {
  static bool intro = false;
  if (!intro) {
    micLogln("[micDiag] === Diagnostica mic I2S (Step 0 wake-word) ===");
    micLogln("[micDiag] AC=rumore RAW  AC_HP=rumore dopo passa-alto ~120Hz  PEAK=picco  DC=offset (24bit)");
    micLogln("[micDiag] Se AC_HP << AC -> rumore a bassa freq (fix software). Se resta alto -> EMI/massa (HW).");
    micLogln("[micDiag] PEAK16/AC16 @ shift 15 (candidato). 1)SILENZIO 2)di' \"Alexo\". OTA+Telnet attivi.");
    intro = true;
  }
  // Passa-alto a un polo (DC-blocker / anti-rumble), CONTINUO tra le finestre:
  //   y[n] = x[n] - x[n-1] + R*y[n-1]   con R=0.95 -> taglio ~120 Hz a 16 kHz.
  // La voce (formanti 300-3000 Hz) passa, la deriva sotto i ~120 Hz viene tolta.
  static double hpPrevX = 0, hpPrevY = 0;
  const double R = 0.95;

  static int32_t buf[256];
  double sum = 0, sumSq = 0, sumHp2 = 0;
  int32_t peak = 0, peakHp = 0;
  long n = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 600) {
    size_t br = 0;
    i2s_read(I2S_PORT, buf, sizeof(buf), &br, portMAX_DELAY);
    int got = br / 4;
    for (int i = 0; i < got; i++) {
      double v = (double)(buf[i] >> 8);        // campione a 24 bit con segno
      sum += v; sumSq += v * v;
      double a = v < 0 ? -v : v;
      if (a > peak) peak = (int32_t)a;
      double y = v - hpPrevX + R * hpPrevY;    // uscita passa-alto
      hpPrevX = v; hpPrevY = y;
      sumHp2 += y * y;
      double ay = y < 0 ? -y : y;
      if (ay > peakHp) peakHp = (int32_t)ay;
      n++;
    }
  }
  if (n < 1) return;
  double mean = sum / n;
  double var  = sumSq / n - mean * mean; if (var < 0) var = 0;
  double sd   = sqrt(var);                    // rumore RAW (dominio 24 bit)
  double sdHp = sqrt(sumHp2 / n);             // rumore dopo passa-alto (~0 mean)
  const double f15 = (double)(1L << (15 - 8));// proiezione su shift 15
  long pk16   = (long)(peak   / f15); if (pk16   > 32767) pk16   = 32767;
  long pkHp16 = (long)(peakHp / f15); if (pkHp16 > 32767) pkHp16 = 32767;
  char line[256];
  snprintf(line, sizeof(line),
           "[micDiag] AC=%.0f AC_HP=%.0f PEAK=%ld DC=%.0f | @sh15: AC16=%.1f AC_HP16=%.1f PEAK16=%ld PEAK_HP16=%ld",
           sd, sdHp, (long)peak, mean, sd / f15, sdHp / f15, pk16, pkHp16);
  micLogln(line);
}

size_t micReadChunk(int16_t *out, size_t maxn) {
  static int32_t buf[256];
  size_t total = 0;
  while (total < maxn) {
    size_t br = 0;
    i2s_read(I2S_PORT, buf, sizeof(buf), &br, portMAX_DELAY);
    int got = br / 4;
    if (got <= 0) break;
    for (int i = 0; i < got && total < maxn; i++) {
      int32_t s = buf[i] >> I2S_SHIFT;                 // grezzo, niente passa-alto
      if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
      out[total++] = (int16_t)s;
    }
  }
  return total;
}

// Calcola il livello (0..255) per il ring da un blocco PCM 16-bit GIA' pronto.
// Passa-alto ~120 Hz (toglie DC/rumble -> niente sfarfallio) + noise-floor
// auto-calibrante + envelope follower. Lo usa sia micPeekLevel sia il loop del
// wake word, che gli passa lo STESSO chunk del modello (cosi' non si legge l'I2S
// due volte). Stato statico = un solo chiamante attivo per volta.
void micFlush() {
  // Svuota (scarta) tutto l'audio accumulato nel DMA dell'I2S. Da chiamare dopo
  // una interazione (registrazione+risposta dura alcuni secondi, durante i quali
  // il DMA si riempie): senza, il wake word ingoierebbe quell'audio stantio in una
  // raffica e farebbe un falso trigger appena finisce la voce.
  static int32_t tmp[256];
  size_t br;
  do { br = 0; i2s_read(I2S_PORT, tmp, sizeof(tmp), &br, 0); } while (br > 0);
}

uint8_t micLevelFromChunk(const int16_t *s, size_t n) {
  if (n == 0) return 0;
  static double hpX = 0, hpY = 0;
  const double hpR = 0.95;
  float acc = 0;
  for (size_t i = 0; i < n; i++) {
    double v = (double)s[i];
    double y = v - hpX + hpR * hpY;                // passa-alto (toglie DC/rumble)
    hpX = v; hpY = y;
    acc += fabsf((float)y);
  }
  float mad = acc / n;

  // Noise floor auto-calibrante: scende in fretta (si aggancia al silenzio), sale
  // pochissimo (la voce non lo trascina su) -> baseline sul rumore di fondo.
  static float noiseFloor = -1.0f;
  if (noiseFloor < 0.0f) noiseFloor = mad;
  float k = (mad < noiseFloor) ? 0.05f : 0.0005f;
  noiseFloor += (mad - noiseFloor) * k;

  float thresh = noiseFloor * gSettings.micLvlMargin + gSettings.micLvlFloor;
  float target = (mad - thresh) * MIC_LVL_GAIN;
  if (target < 0) target = 0; else if (target > 255) target = 255;

  // Envelope follower: attacco lento (un impulso isolato non fa in tempo a salire),
  // rilascio piu' rapido. Niente sfarfallio, resta reattivo al suono vero.
  static float env = 0;
  float ek = (target > env) ? gSettings.micLvlAttack : gSettings.micLvlRelease;
  env += (target - env) * ek;
  int lvl = (int)(env + 0.5f);
  if (lvl < 0) lvl = 0; else if (lvl > 255) lvl = 255;
  // Esporta i valori correnti per il pannello web (lettura live).
  g_liveLevel  = (uint8_t)lvl;
  g_liveFloor  = noiseFloor;
  g_liveThresh = thresh;
  return (uint8_t)lvl;
}

// Snapshot dei valori live (aggiornati da micLevelFromChunk a ogni chunk a riposo).
void micGetLive(uint8_t *level, float *noiseFloor, float *thresh) {
  if (level)      *level      = g_liveLevel;
  if (noiseFloor) *noiseFloor = g_liveFloor;
  if (thresh)     *thresh     = g_liveThresh;
}

uint8_t micPeekLevel() {
  static int32_t buf[256];
  size_t br = 0;
  i2s_read(I2S_PORT, buf, sizeof(buf), &br, portMAX_DELAY);
  int n = br / 4;
  if (n <= 0) return 0;
  static int16_t pcm[256];
  for (int i = 0; i < n; i++) {
    int32_t v = buf[i] >> I2S_SHIFT;                // grezzo (il passa-alto e' dentro)
    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
    pcm[i] = (int16_t)v;
  }
  return micLevelFromChunk(pcm, n);
}

// ============================================================================
//                       BACKEND ANALOGICO (MAX4466)
// ============================================================================
#else  // !MIC_USE_I2S

#define MIC_REC_GAIN  16      // guadagno digitale in registrazione (raw->int16)
#define MIC_LVL_NOISE 20.0f   // soglia idle reattivo (MAD)
#define MIC_LVL_GAIN  14.0f   // guadagno idle reattivo
#define MIC_DEBUG     0

// Su ESP32-S3 ADC1: GPIO1->CH0, GPIO2->CH1, ... quindi canale = pin - 1.
static const adc1_channel_t MIC_CH = (adc1_channel_t)(MIC_ADC_PIN - 1);

bool micBegin() {
  if (!psramFound()) { Serial.println("[mic] PSRAM non disponibile!"); return false; }
  if (!g_pcm) g_pcm = (int16_t *)ps_malloc(MIC_MAX_SAMPLES * sizeof(int16_t));
  if (!g_wav) g_wav = (uint8_t *)ps_malloc(44 + MIC_MAX_SAMPLES * sizeof(int16_t));
  if (!g_pcm || !g_wav) { Serial.println("[mic] allocazione PSRAM fallita!"); return false; }

  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(MIC_CH, ADC_ATTEN_DB_12);  // ex DB_11 (deprecato, stesso comportamento)
  (void)adc1_get_raw(MIC_CH);   // primo read a vuoto per stabilizzare
  Serial.printf("[mic] MAX4466 pronto: GPIO%d (ADC1_CH%d), %d Hz, max %d s\n",
                MIC_ADC_PIN, (int)MIC_CH, MIC_SAMPLE_RATE, MIC_MAX_SECONDS);
  return true;
}

size_t micRecord(uint32_t maxMs, bool (*keepGoing)(), void (*onLevel)(uint8_t), uint32_t silenceMs) {
  (void)silenceMs;   // stop-al-silenzio non implementato sul backend analogico
  if (!g_pcm) return 0;
  const uint32_t period_us = 1000000UL / MIC_SAMPLE_RATE;
  const size_t   maxSamples =
      min(MIC_MAX_SAMPLES, (size_t)((uint64_t)MIC_SAMPLE_RATE * maxMs / 1000));

  size_t   n = 0;
  uint64_t sum = 0;                  // per stimare l'offset DC
  uint32_t levelTimer = millis();
  uint16_t chunkMin = 4095, chunkMax = 0;

  uint32_t tNext = micros();
  while (n < maxSamples) {
    if (keepGoing && !keepGoing()) break;
    while ((int32_t)(micros() - tNext) < 0) { }
    tNext += period_us;

    uint16_t raw = adc1_get_raw(MIC_CH);
    g_pcm[n++] = (int16_t)raw;       // grezzo, riscalato dopo
    sum += raw;
    if (raw < chunkMin) chunkMin = raw;
    if (raw > chunkMax) chunkMax = raw;

    if (onLevel && (millis() - levelTimer) >= 20) {
      uint16_t amp = (chunkMax - chunkMin);
      uint8_t lvl = (uint8_t)min<uint32_t>(255, (uint32_t)amp * 255 / 4095);
      onLevel(lvl);
      levelTimer = millis();
      chunkMin = 4095; chunkMax = 0;
    }
  }

  g_count = n;
  if (n == 0) { g_peak = 0; g_heard = false; return 0; }
  g_heard = true;   // backend analogico: nessun rilevatore di voce, assumiamo sentito

  // rimuovo l'offset DC e riscalo a int16, calcolando il picco
  int32_t dc = (int32_t)(sum / n);
  int peak = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t s = ((int32_t)g_pcm[i] - dc) * MIC_REC_GAIN;
    if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
    g_pcm[i] = (int16_t)s;
    int a = s < 0 ? -s : s;
    if (a > peak) peak = a;
  }
  g_peak = peak;
  return n;
}

void micDiag() {
  Serial.println("[micDiag] Diagnostica disponibile solo col mic I2S (MIC_USE_I2S=1).");
}

size_t micReadChunk(int16_t *out, size_t maxn) {
  // Backend analogico: non usato dal wake word (che vuole il mic I2S).
  (void)out; (void)maxn; return 0;
}

uint8_t micLevelFromChunk(const int16_t *s, size_t n) {
  (void)s; (void)n; return 0;   // non usato col backend analogico
}

void micGetLive(uint8_t *level, float *noiseFloor, float *thresh) {
  if (level) *level = 0; if (noiseFloor) *noiseFloor = 0; if (thresh) *thresh = 0;
}

void micFlush() { /* niente DMA sul backend analogico */ }

uint8_t micPeekLevel() {
  const int N = 256;
  const uint32_t period = 1000000UL / MIC_SAMPLE_RATE;
  uint32_t sum = 0;
  static uint16_t raw[N];
  uint32_t tNext = micros();
  for (int i = 0; i < N; i++) {
    while ((int32_t)(micros() - tNext) < 0) { }
    tNext += period;
    uint16_t r = adc1_get_raw(MIC_CH);
    raw[i] = r;
    sum += r;
  }
  float mean = (float)sum / N;
  float acc = 0;
  for (int i = 0; i < N; i++) acc += fabsf((float)raw[i] - mean);   // MAD: robusta agli spike
  float mad = acc / N;

  int lvl = (int)((mad - MIC_LVL_NOISE) * MIC_LVL_GAIN);
  if (lvl < 0) lvl = 0; if (lvl > 255) lvl = 255;
#if MIC_DEBUG
  static uint32_t lastDbg = 0;
  if (millis() - lastDbg > 500) { Serial.printf("[mic] mad=%.1f lvl=%d\n", mad, lvl); lastDbg = millis(); }
#endif
  return (uint8_t)lvl;
}

#endif  // MIC_USE_I2S

// ============================================================================
//                         PARTE COMUNE (WAV / accessori)
// ============================================================================
const int16_t *micPcm()         { return g_pcm; }
size_t         micSampleCount() { return g_count; }
uint32_t       micSampleRate()  { return MIC_SAMPLE_RATE; }
int            micLastPeak()    { return g_peak; }
bool           micHeardVoice()  { return g_heard; }

static void wr32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void wr16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }

const uint8_t *micWav(size_t *len) {
  if (!g_wav || g_count == 0) { if (len) *len = 0; return nullptr; }
  const uint32_t sr      = MIC_SAMPLE_RATE;
  const uint32_t dataLen = (uint32_t)g_count * sizeof(int16_t);
  uint8_t *h = g_wav;
  memcpy(h + 0,  "RIFF", 4);  wr32(h + 4, 36 + dataLen);
  memcpy(h + 8,  "WAVE", 4);  memcpy(h + 12, "fmt ", 4);
  wr32(h + 16, 16); wr16(h + 20, 1); wr16(h + 22, 1);
  wr32(h + 24, sr); wr32(h + 28, sr * 2); wr16(h + 32, 2); wr16(h + 34, 16);
  memcpy(h + 36, "data", 4); wr32(h + 40, dataLen);
  memcpy(h + 44, g_pcm, dataLen);
  if (len) *len = 44 + dataLen;
  return g_wav;
}
