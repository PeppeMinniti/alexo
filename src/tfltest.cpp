// ============================================================================
//  ALEXO - Self-test TFLite Micro. Vedi tfltest.h + WAKEWORD.md (passo 2).
//  Gira il modello "hello_world" (sin) per provare che TFLM funziona sull'S3.
// ============================================================================
#include "config.h"

#if TFL_SELFTEST

#include "tfltest.h"
#include "netlog.h"
#include <math.h>
#include <Chirale_TensorFlowLite.h>
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tfl_hello_model.h"   // const unsigned char g_model[] (scaricato dal repo Chirale)
// Microfrontend TFLM (lib vendorizzata in lib/microfrontend): genera le 40 feature
// mel che il modello wake-word si aspetta. Qui solo per provarne compilazione+run.
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"

static void logln(const char *s) { Serial.println(s); netlogPrintln(s); }

namespace {
  constexpr int kArenaSize = 4000;                 // hello_world e' minuscolo
  alignas(16) uint8_t tensor_arena[kArenaSize];
  tflite::MicroInterpreter *interp = nullptr;
  TfLiteTensor *in = nullptr, *out = nullptr;
  bool ready = false, tried = false;
}

static void setupOnce() {
  tried = true;
  const tflite::Model *model = tflite::GetModel(g_model);
  if (model->version() != TFLITE_SCHEMA_VERSION) { logln("[tfl] schema TFLite incompatibile"); return; }
  static tflite::AllOpsResolver resolver;
  static tflite::MicroInterpreter staticInterp(model, resolver, tensor_arena, kArenaSize);
  interp = &staticInterp;
  if (interp->AllocateTensors() != kTfLiteOk) { logln("[tfl] AllocateTensors FALLITA"); return; }
  in  = interp->input(0);
  out = interp->output(0);
  char line[160];
  snprintf(line, sizeof(line), "[tfl] runtime OK: arena usata=%u/%d byte, input.type=%d",
           (unsigned)interp->arena_used_bytes(), kArenaSize, (int)in->type);
  logln(line);
  ready = true;
}

// --- Test del microfrontend (40 feature mel, parametri = preprocessor_settings.h) ---
namespace {
  struct FrontendState fe_state;
  bool fe_ready = false, fe_tried = false;
}
static void frontendOnce() {
  fe_tried = true;
  struct FrontendConfig cfg;
  FrontendFillConfigWithDefaults(&cfg);
  cfg.window.size_ms = 30;  cfg.window.step_size_ms = 10;
  cfg.filterbank.num_channels = 40;
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
  if (!FrontendPopulateState(&cfg, &fe_state, 16000)) { logln("[fe] FrontendPopulateState FALLITA"); return; }
  fe_ready = true;
  logln("[fe] frontend init OK (40 mel, finestra 30ms / passo 10ms, 16kHz)");
}

void tflSelfTest() {
  if (!tried) setupOnce();
  if (!ready) return;

  // hello_world: input x in [0, 2pi], output ~ sin(x). Gestisce sia float sia
  // INT8 quantizzato (a seconda di come e' stato esportato il modello).
  for (int i = 0; i <= 4; i++) {
    float x = i * (6.2831853f / 4.0f);
    if (in->type == kTfLiteInt8) {
      in->data.int8[0] = (int8_t)lroundf(x / in->params.scale + in->params.zero_point);
    } else {
      in->data.f[0] = x;
    }
    uint32_t t0 = micros();
    TfLiteStatus st = interp->Invoke();
    uint32_t dt = micros() - t0;
    if (st != kTfLiteOk) { logln("[tfl] Invoke FALLITA"); return; }
    float y = (out->type == kTfLiteInt8)
                ? (out->data.int8[0] - out->params.zero_point) * out->params.scale
                : out->data.f[0];
    char line[160];
    snprintf(line, sizeof(line), "[tfl] x=%.2f  sin(x)~=%+.3f (vero %+.3f)  inferenza=%lu us",
             x, y, sinf(x), (unsigned long)dt);
    logln(line);
  }

  // Frontend: lo configura e lo fa girare su un seno di prova (440 Hz), stampa
  // quante feature produce e le prime. Conferma che il microfrontend compila e gira.
  if (!fe_tried) frontendOnce();
  if (fe_ready) {
    static int16_t pcm[480];
    for (int i = 0; i < 480; i++) pcm[i] = (int16_t)(4000.0f * sinf(2.0f * 3.14159265f * 440.0f * i / 16000.0f));
    size_t read = 0;
    struct FrontendOutput fo = FrontendProcessSamples(&fe_state, pcm, 480, &read);
    char l[160];
    snprintf(l, sizeof(l), "[fe] feature=%u (atteso 40)  f0..f5: %u %u %u %u %u %u",
             (unsigned)fo.size,
             fo.size > 0 ? fo.values[0] : 0, fo.size > 1 ? fo.values[1] : 0,
             fo.size > 2 ? fo.values[2] : 0, fo.size > 3 ? fo.values[3] : 0,
             fo.size > 4 ? fo.values[4] : 0, fo.size > 5 ? fo.values[5] : 0);
    logln(l);
  }
}

#else
void tflSelfTest() {}
#endif  // TFL_SELFTEST
