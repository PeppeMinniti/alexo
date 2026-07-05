#pragma once
// ============================================================================
//  ALEXO - Self-test TFLite Micro (passo 2 wake word, vedi WAKEWORD.md).
//  Verifica che il runtime TFLM compili E giri sull'ESP32-S3, usando il modello
//  di prova "hello_world" (impara sin(x)). Attivo solo con TFL_SELFTEST=1.
// ============================================================================
#include <Arduino.h>

// Esegue una passata del self-test (setup una volta sola + qualche inferenza) e
// stampa i risultati e il tempo di inferenza su Serial e Telnet. Chiamabile in
// loop. No-op se TFL_SELFTEST=0.
void tflSelfTest();
