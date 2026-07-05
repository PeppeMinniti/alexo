#pragma once
// ============================================================================
//  ALEXO - Suoni di feedback (toni generati al volo, riprodotti dal VS1053)
// ============================================================================
#include <VS1053.h>

void soundStart(VS1053 &player);   // bip acuto: inizio ascolto
void soundStop(VS1053 &player);    // bip grave: fine ascolto
void soundError(VS1053 &player);   // doppio bip basso: errore
