// ============================================================================
//  ALEXO - Encoder rotativo (vedi encoder.h).
//  Backend: libreria Versatile_RotaryEncoder (ruiseixasm) - decodifica robusta
//  a polling + gestione completa degli eventi del pulsante. L'API pubblica resta
//  identica a prima (encoderTake / encoderButtonHeld / ...) cosi' gobbo.cpp e
//  main.cpp non cambiano.
//
//  La libreria e' a POLLING: va interrogata spesso con ReadEncoder(). Per non
//  perdere passi quando il loop principale (core 1) e' bloccato su rete, la
//  interroghiamo da un TASK dedicato ad alta frequenza (~1ms). I callback
//  girano dentro quel task e accumulano in variabili volatili che l'API legge.
// ============================================================================
#include "encoder.h"
#include "config.h"
#include <Versatile_RotaryEncoder.h>

static Versatile_RotaryEncoder *enc = nullptr;

static volatile int32_t encDelta   = 0;   // detenti accumulati (giro, +1/-1 per scatto)
static volatile bool    btnHeld    = false; // true mentre il pulsante e' premuto
static volatile bool    clickEvent = false; // CLICK SINGOLO confermato (differito)
static volatile bool    dblEvent   = false; // DOPPIO click

// Disambiguazione singolo vs doppio: al rilascio di una pressione semplice (senza
// giro) NON emetto subito il singolo, ma avvio un timer; se entro DOUBLE_WINDOW_MS
// arriva un secondo click la libreria chiama onDoublePress (-> annullo il pending
// e emetto un doppio), altrimenti scaduta la finestra confermo il singolo.
#define DOUBLE_WINDOW_MS 350
static volatile bool     rotated       = false;   // ha girato mentre premuto? -> non e' un click
static volatile bool     inDouble      = false;   // questa pressione fa parte di un doppio
static volatile bool     pendingSingle = false;
static volatile uint32_t pressDownAt   = 0;       // istante dell'ULTIMA pressione (riferimento finestra)

// --- Callback della libreria (girano nel task di polling) -------------------
static void onRotate(int8_t r)      { encDelta += r; }                 // giro libero -> scroll
static void onPressRotate(int8_t r) { encDelta += r; rotated = true; } // premuto + giro -> volume
static void onHeldRotate(int8_t r)  { encDelta += r; rotated = true; }

// La finestra del doppio si misura dalla PRESSIONE (come fa la libreria). Al
// rilascio "pulito" (no giro, non e' il 2o di un doppio) armo un singolo PENDENTE
// che il task confermera' solo se la finestra scade senza un 2o click. Se invece
// arriva il doppio (onDoublePress), annullo il pendente e marco inDouble cosi' il
// rilascio successivo NON ri-arma un singolo (era il bug: scattava la chat).
static void onPress()               { btnHeld = true; rotated = false; inDouble = false; pressDownAt = millis(); }
static void onPressRelease()        { btnHeld = false; if (!rotated && !inDouble) pendingSingle = true; rotated = false; inDouble = false; }
static void onLongPressRelease()    { btnHeld = false; if (!rotated && !inDouble) pendingSingle = true; rotated = false; inDouble = false; }
static void onPressRotateRelease()  { btnHeld = false; rotated = false; }               // ha girato -> no click
static void onHeldRotateRelease()   { btnHeld = false; rotated = false; }
static void onDoublePress()         { btnHeld = true; pendingSingle = false; dblEvent = true; inDouble = true; }

// --- Task di polling (interroga la libreria ogni ~1ms) ----------------------
static void encoderTask(void *) {
  for (;;) {
    enc->ReadEncoder();
    // conferma il click singolo se la finestra del doppio e' passata (dalla pressione)
    if (pendingSingle && (millis() - pressDownAt) >= DOUBLE_WINDOW_MS) {
      pendingSingle = false;
      clickEvent = true;
    }
    vTaskDelay(1);   // 1 tick = 1ms (FreeRTOS @1kHz); ReadEncoder si auto-limita a 1ms
  }
}

void encoderBegin() {
  // clk = A (CLK), dt = B (DT), sw = pulsante. Pull-up interni li mette la libreria.
  enc = new Versatile_RotaryEncoder(ENC_A_PIN, ENC_B_PIN, ENC_SW_PIN);

  enc->setHandleRotate(onRotate);
  enc->setHandlePressRotate(onPressRotate);
  enc->setHandleHeldRotate(onHeldRotate);
  enc->setHandlePress(onPress);
  enc->setHandleDoublePress(onDoublePress);
  enc->setHandlePressRelease(onPressRelease);
  enc->setHandleLongPressRelease(onLongPressRelease);
  enc->setHandlePressRotateRelease(onPressRotateRelease);
  enc->setHandleHeldRotateRelease(onHeldRotateRelease);
  enc->setDoublePressDuration(DOUBLE_WINDOW_MS);   // finestra del doppio click

  encDelta = 0; btnHeld = false; clickEvent = false; dblEvent = false; pendingSingle = false; inDouble = false;

  // Task leggero sul core 0 (come ui/gobbo); il loop di rete sta sul core 1.
  xTaskCreatePinnedToCore(encoderTask, "enc", 2048, nullptr, 2, nullptr, 0);
}

int32_t encoderTake() {
  int32_t d = encDelta;
  encDelta -= d;        // sottraggo invece di azzerare: non perdo scatti arrivati nel frattempo
  return d;
}

bool encoderButtonPressed() {
  if (clickEvent) { clickEvent = false; return true; }
  return false;
}

bool encoderDoublePressed() {
  if (dblEvent) { dblEvent = false; return true; }
  return false;
}

bool encoderButtonHeld() {
  return btnHeld;
}
