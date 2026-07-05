#pragma once
// ============================================================================
//  ALEXO - Encoder rotativo per scorrere la chat sul display.
//  Decodifica in quadratura via interrupt (conta anche mentre il loop di core 1
//  e' bloccato su rete). I pin sono in config.h.
// ============================================================================
#include <Arduino.h>

// Inizializza i pin e gli interrupt dell'encoder.
void encoderBegin();

// Detenti accumulati dall'ultima chiamata: >0 in un verso, <0 nell'altro, 0 fermo.
int32_t encoderTake();

// true UNA volta per un CLICK SINGOLO confermato. NB: e' "differito" di ~280ms
// per disambiguare dal doppio click (vedi encoderDoublePressed): se entro quella
// finestra arriva un secondo click, NON viene emesso un singolo (ma un doppio).
// I click con giro (premuto+giro = volume) non contano come click.
bool encoderButtonPressed();

// true UNA volta per un DOPPIO click (due pressioni rapide). Usato come toggle.
bool encoderDoublePressed();

// true finche' il pulsante e' tenuto premuto (lettura istantanea, pull-up:
// premuto = LOW). Serve a distinguere il giro libero dal "premuto + giro".
bool encoderButtonHeld();
