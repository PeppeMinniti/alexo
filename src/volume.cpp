// ============================================================================
//  ALEXO - Controllo volume del VS1053 (vedi volume.h).
// ============================================================================
#include "volume.h"
#include "config.h"
#include <Preferences.h>

// pendingDelta e' scritto dal core 0 (task encoder) e letto/azzerato dal core 1.
// E' un int32 allineato -> letture/scritture atomiche sull'Xtensa; l'unica
// "perdita" possibile e' uno scatto in caso di collisione esatta, irrilevante
// per una manopola del volume.
static volatile int32_t pendingDelta = 0;
static volatile int32_t pendingAbs   = -1;   // set assoluto dal pannello web (-1 = niente)
static uint8_t          curVol = VOLUME_DEFAULT;
static Preferences      prefs;

// Volume utente (0..100) -> valore VS1053. Il VS1053 e' logaritmico: la sua parte
// bassa e' quasi muta, per questo rimappiamo 1..100 nella zona udibile
// VOLUME_VS_MIN..100 (tutta la corsa dello slider diventa utile). 0 = muto.
uint8_t volumeVsValue() {
  if (curVol == 0) return 0;
  return (uint8_t)map(curVol, 1, 100, VOLUME_VS_MIN, 100);
}
bool volumeIsMuted() { return curVol == 0; }

void volumeBegin(VS1053 &player) {
  prefs.begin("alexo", false);                 // namespace NVS
  curVol = prefs.getUChar("vol", VOLUME_DEFAULT);
  if (curVol > 100)        curVol = 100;
  if (curVol < VOLUME_MIN) curVol = VOLUME_MIN;
  player.setVolume(volumeVsValue());
  Serial.printf("[vol] volume iniziale: %u%% (VS1053 %u)\n", curVol, volumeVsValue());
}

void volumeRequest(int32_t detents) {
  pendingDelta += detents;                     // core 0: solo accumulo
}

void volumeSet(int percent) {
  if (percent < VOLUME_MIN) percent = VOLUME_MIN;
  if (percent > 100)        percent = 100;
  pendingAbs = percent;                        // applicato dal core 1 (vedi sotto)
}

bool volumeApplyPending(VS1053 &player) {
  // Prima un eventuale set ASSOLUTO dal pannello web.
  int32_t abs = pendingAbs;
  if (abs >= 0) {
    pendingAbs = -1;
    if ((uint8_t)abs != curVol) {
      curVol = (uint8_t)abs;
      player.setVolume(volumeVsValue());
      prefs.putUChar("vol", curVol);
      Serial.printf("[vol] volume (web): %u%% (VS1053 %u)\n", curVol, volumeVsValue());
      return true;
    }
    return false;
  }

  int32_t d = pendingDelta;
  if (d == 0) return false;
  pendingDelta -= d;                           // consuma esattamente quanto letto

  int32_t v = (int32_t)curVol + d * VOLUME_STEP;
  if (v < VOLUME_MIN) v = VOLUME_MIN;
  if (v > 100)        v = 100;
  if ((uint8_t)v == curVol) return false;

  curVol = (uint8_t)v;
  player.setVolume(volumeVsValue());
  prefs.putUChar("vol", curVol);               // salva (NVS fa wear-leveling)
  Serial.printf("[vol] volume: %u%% (VS1053 %u)\n", curVol, volumeVsValue());
  return true;
}

uint8_t volumeGet() { return curVol; }
