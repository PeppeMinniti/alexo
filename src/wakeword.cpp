// ============================================================================
//  ALEXO - Wake word locale "Alexo" (microWakeWord). Vedi wakeword.h + WAKEWORD.md.
//
//  Catena: PCM 16kHz -> microfrontend (40 feature mel/10ms) -> conversione int8
//  -> modello streaming INT8 (input [1,stride,40], accumula 'stride' frame poi
//  Invoke) -> probabilita' uint8 -> media mobile su WAKE_WINDOW > WAKE_PROB_CUTOFF
//  -> rilevato. Logica e costanti ricalcate da ESPHome micro_wake_word.
//
//  Compilato solo se WAKE_ENABLE o WAKE_TEST (altrimenti stub inerte).
// ============================================================================
#include "wakeword.h"
#include "config.h"

#if WAKE_ENABLE || WAKE_TEST

#include "netlog.h"
#include "mic.h"
#include "settings.h"
#include <math.h>
#include <Chirale_TensorFlowLite.h>
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
#include "wake_model.h"   // alignas(16) const unsigned char g_wake_model[]

#define WAKE_FEATURE_SIZE 40
#define WAKE_ARENA_BYTES  (40 * 1024)   // manifest ~22348; margine in PSRAM

namespace {
  struct FrontendState   fe;
  tflite::MicroInterpreter *interp = nullptr;
  uint8_t *arena = nullptr;
  int      model_stride  = 1;     // input->dims[1]: frame per Invoke
  int      current_step  = 0;
  uint8_t  recent[16];            // finestra mobile delle probabilita'
  int      recent_n = 0, recent_idx = 0;
  uint8_t  last_prob = 0;
  bool     s_ready = false;
}

static void wlogln(const char *s) { Serial.println(s); netlogPrintln(s); }

bool wakeBegin() {
  if (!psramFound()) { wlogln("[wake] PSRAM assente"); return false; }

  // --- Frontend (parametri = preprocessor_settings.h di ESPHome) ---
  struct FrontendConfig cfg;
  FrontendFillConfigWithDefaults(&cfg);
  cfg.window.size_ms = 30;  cfg.window.step_size_ms = 10;
  cfg.filterbank.num_channels = WAKE_FEATURE_SIZE;
  cfg.filterbank.lower_band_limit = 125.0f;
  cfg.filterbank.upper_band_limit = 7500.0f;
  cfg.noise_reduction.smoothing_bits = 10;
  cfg.noise_reduction.even_smoothing = 0.025f;
  cfg.noise_reduction.odd_smoothing = 0.06f;
  cfg.noise_reduction.min_signal_remaining = 0.05f;
  cfg.pcan_gain_control.enable_pcan = 1;
  cfg.pcan_gain_control.strength = 0.95f;
  cfg.pcan_gain_control.offset = 80.0f;
  cfg.pcan_gain_control.gain_bits = 21;
  cfg.log_scale.enable_log = 1;
  cfg.log_scale.scale_shift = 6;
  if (!FrontendPopulateState(&cfg, &fe, 16000)) { wlogln("[wake] frontend init FALLITO"); return false; }

  // --- Modello ---
  const tflite::Model *model = tflite::GetModel(g_wake_model);
  if (model->version() != TFLITE_SCHEMA_VERSION) { wlogln("[wake] schema TFLite incompatibile"); return false; }
  arena = (uint8_t *)ps_malloc(WAKE_ARENA_BYTES);
  if (!arena) { wlogln("[wake] ps_malloc arena fallito"); return false; }
  // Il modello streaming usa RESOURCE VARIABLES (op VAR_HANDLE) per lo stato tra
  // le inferenze: serve un piccolo arena dedicato + MicroResourceVariables passati
  // all'interprete (come fa ESPHome). Senza, AllocateTensors fallisce.
  static alignas(16) uint8_t var_arena[1024];
  tflite::MicroAllocator *ma = tflite::MicroAllocator::Create(var_arena, sizeof(var_arena));
  tflite::MicroResourceVariables *mrv = tflite::MicroResourceVariables::Create(ma, 20);
  static tflite::AllOpsResolver resolver;
  static tflite::MicroInterpreter si(model, resolver, arena, WAKE_ARENA_BYTES, mrv);
  interp = &si;
  if (interp->AllocateTensors() != kTfLiteOk) { wlogln("[wake] AllocateTensors FALLITA"); return false; }

  TfLiteTensor *in  = interp->input(0);
  TfLiteTensor *out = interp->output(0);
  // input atteso [1, stride, 40] int8 ; output [1,1] uint8
  if (in->dims->size != 3 || in->dims->data[2] != WAKE_FEATURE_SIZE || in->type != kTfLiteInt8) {
    char l[120]; snprintf(l, sizeof(l), "[wake] input inatteso: dims=%d d2=%d type=%d", in->dims->size, in->dims->size>=3?in->dims->data[2]:-1, in->type); wlogln(l); return false;
  }
  model_stride = in->dims->data[1];
  char l[140];
  snprintf(l, sizeof(l), "[wake] OK: arena=%u/%d, stride=%d, out.type=%d (cutoff=%d win=%d)",
           (unsigned)interp->arena_used_bytes(), WAKE_ARENA_BYTES, model_stride, out->type, WAKE_PROB_CUTOFF, WAKE_WINDOW);
  wlogln(l);
  s_ready = true;
  return true;
}

bool wakeReady() { return s_ready; }
uint8_t wakeLastProb() { return last_prob; }

void wakeReset() {
  // Azzera lo stato del rilevamento dopo un'interazione: finestra delle
  // probabilita', accumulo delle stride e stato del frontend (noise reduction/
  // PCAN). Cosi' l'audio stantio post-risposta non fa scattare un falso wake.
  recent_n = 0; recent_idx = 0; current_step = 0; last_prob = 0;
  if (s_ready) FrontendReset(&fe);
}

bool wakeFeed(const int16_t *samples, size_t n) {
  if (!s_ready || !samples) return false;
  bool detected = false;
  static int16_t g[2048];
  size_t pos = 0;
  while (pos < n) {
    size_t chunk = n - pos; if (chunk > 2048) chunk = 2048;
    // Applica il guadagno wake (con clamp): alza il livello della voce normale.
    for (size_t j = 0; j < chunk; j++) {
      int32_t v = (int32_t)samples[pos + j] * gSettings.wakeGain;
      g[j] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
    pos += chunk;
    size_t off = 0;
    while (off < chunk) {
    size_t processed = 0;
    struct FrontendOutput fo = FrontendProcessSamples(&fe, g + off, chunk - off, &processed);
    off += processed;
    if (processed == 0) break;
    if (fo.size == 0) continue;   // finestra non ancora completa

    // uint16 frontend -> int8 feature (formula ESPHome: scala 256, div 666, -128)
    TfLiteTensor *in = interp->input(0);
    int8_t *indata = tflite::GetTensorData<int8_t>(in);
    int8_t *slot = indata + WAKE_FEATURE_SIZE * current_step;
    for (int i = 0; i < WAKE_FEATURE_SIZE; i++) {
      int32_t v = ((int32_t)fo.values[i] * 256 + 333) / 666;
      v += -128;
      slot[i] = (int8_t)(v < -128 ? -128 : (v > 127 ? 127 : v));
    }
    if (++current_step < model_stride) continue;   // accumula 'stride' frame
    current_step = 0;

    if (interp->Invoke() != kTfLiteOk) { wlogln("[wake] Invoke FALLITA"); continue; }
    last_prob = interp->output(0)->data.uint8[0];

    // finestra mobile + media > cutoff (parametri runtime dal pannello web)
    const int win = gSettings.wakeWindow;   // gia' clampato a 1..16 in settings
    recent[recent_idx] = last_prob;
    recent_idx = (recent_idx + 1) % win;
    if (recent_n < win) recent_n++;
    if (recent_n >= win) {
      int sum = 0;
      for (int i = 0; i < win; i++) sum += recent[i];
      if (sum > gSettings.wakeProbCutoff * win) {
        detected = true;
        recent_n = 0; recent_idx = 0;   // refrattario: svuota la finestra (cool-off)
      }
    }
    }   // while (off < chunk)
  }     // while (pos < n)
  return detected;
}

// --- Test della catena senza mic --------------------------------------------
#if WAKE_TEST
void wakeSelfTest() {
  static bool tried = false;
  if (!tried) { tried = true; if (!wakeBegin()) wlogln("[wake] init test FALLITO"); }
  if (!s_ready) return;

  // Ascolto CONTINUO: legge un chunk corto (~20 ms) dal mic e lo passa subito
  // alla catena. Va chiamato in un loop STRETTO (niente delay), cosi' il modello
  // streaming riceve un flusso continuo (e non perde gli "Alexa"). Stampa il
  // picco di probabilita' ogni secondo e quando rileva.
  static int16_t buf[320];          // 20 ms a 16 kHz
  static uint8_t maxp = 0;
  static uint32_t lastPrint = 0;
  size_t got = micReadChunk(buf, 320);
  if (got) {
    if (wakeFeed(buf, got)) {
      char l[100];
      snprintf(l, sizeof(l), "[wakeTest] *** ALEXA RILEVATO! *** (picco=%u)", maxp);
      wlogln(l);
      maxp = 0;
    }
    if (last_prob > maxp) maxp = last_prob;
  }
  if (millis() - lastPrint > 1000) {
    lastPrint = millis();
    char l[80];
    snprintf(l, sizeof(l), "[wakeTest] (1s) prob_max=%u/255", maxp);
    wlogln(l);
    maxp = 0;
  }
}
#else
void wakeSelfTest() {}
#endif

#else  // !(WAKE_ENABLE || WAKE_TEST) -- stub inerte

bool    wakeBegin()    { return false; }
bool    wakeReady()    { return false; }
uint8_t wakeLastProb() { return 0; }
bool    wakeFeed(const int16_t *, size_t) { return false; }
void    wakeReset()    {}
void    wakeSelfTest() {}

#endif  // WAKE_ENABLE || WAKE_TEST
