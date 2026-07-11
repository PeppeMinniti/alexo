// ============================================================================
//  ALEXO - Impostazioni runtime (vedi settings.h). Backend: Preferences (NVS),
//  namespace "cfg" (separato da "alexo" del volume). Ogni parametro ha una
//  chiave breve (<=15 char, limite NVS). Se una chiave non esiste ancora, si usa
//  il default di config.h -> il primo avvio parte coi valori di fabbrica.
// ============================================================================
#include "settings.h"
#include "config.h"
#include <Preferences.h>

AlexoSettings gSettings;

static Preferences prefs;

// Applica i valori di FABBRICA (macro di config.h) alla struct in RAM.
static void loadDefaults() {
  gSettings.recSilenceMs     = REC_SILENCE_MS;
  gSettings.recSilenceMargin = REC_SILENCE_MARGIN;
  gSettings.recSilenceFloor  = REC_SILENCE_FLOOR;
  gSettings.micLvlMargin     = MIC_LVL_MARGIN_DEF;
  gSettings.micLvlFloor      = MIC_LVL_FLOOR_DEF;
  gSettings.micLvlAttack     = MIC_LVL_ATTACK_DEF;
  gSettings.micLvlRelease    = MIC_LVL_RELEASE_DEF;
  gSettings.idleReactive     = (IDLE_REACTIVE != 0);
  gSettings.wakeGain         = WAKE_GAIN;
  gSettings.wakeProbCutoff   = WAKE_PROB_CUTOFF;
  gSettings.wakeWindow       = WAKE_WINDOW;
  gSettings.voiceId          = ELEVEN_VOICE_DEF;
  gSettings.voiceIdAlt       = ELEVEN_VOICE_ALT_DEF;
  gSettings.voiceTrigger     = VOICE_TRIGGER_DEF;
  gSettings.llmModel         = LLM_MODEL_DEF;
  gSettings.systemPrompt     = SYSTEM_PROMPT_DEF;
  gSettings.hallucTerms      = HALLUC_TERMS_DEF;
  gSettings.musicStations    = MUSIC_STATIONS_DEF;
  gSettings.replyTrigger     = REPLY_TRIGGER_DEF;
  gSettings.replyText        = REPLY_TEXT_DEF;
}

// Vincoli di sicurezza (evita valori che romperebbero il firmware).
static void clamp() {
  if (gSettings.recSilenceMs > 10000)      gSettings.recSilenceMs = 10000;
  if (gSettings.recSilenceMargin < 1.0f)   gSettings.recSilenceMargin = 1.0f;
  if (gSettings.recSilenceMargin > 6.0f)   gSettings.recSilenceMargin = 6.0f;
  if (gSettings.recSilenceFloor < 0)       gSettings.recSilenceFloor = 0;
  if (gSettings.recSilenceFloor > 4000)    gSettings.recSilenceFloor = 4000;
  if (gSettings.micLvlMargin < 1.0f)       gSettings.micLvlMargin = 1.0f;
  if (gSettings.micLvlMargin > 8.0f)       gSettings.micLvlMargin = 8.0f;
  if (gSettings.micLvlFloor < 0)           gSettings.micLvlFloor = 0;
  if (gSettings.micLvlFloor > 500)         gSettings.micLvlFloor = 500;
  if (gSettings.micLvlAttack < 0.02f)      gSettings.micLvlAttack = 0.02f;
  if (gSettings.micLvlAttack > 1.0f)       gSettings.micLvlAttack = 1.0f;
  if (gSettings.micLvlRelease < 0.02f)     gSettings.micLvlRelease = 0.02f;
  if (gSettings.micLvlRelease > 1.0f)      gSettings.micLvlRelease = 1.0f;
  if (gSettings.wakeGain < 1)              gSettings.wakeGain = 1;
  if (gSettings.wakeGain > 12)             gSettings.wakeGain = 12;
  if (gSettings.wakeProbCutoff < 100)      gSettings.wakeProbCutoff = 100;
  if (gSettings.wakeProbCutoff > 255)      gSettings.wakeProbCutoff = 255;
  if (gSettings.wakeWindow < 1)            gSettings.wakeWindow = 1;
  if (gSettings.wakeWindow > 16)           gSettings.wakeWindow = 16;   // recent[16]
}

void settingsBegin() {
  loadDefaults();                     // base = valori di fabbrica
  prefs.begin("cfg", true);           // sola lettura: leggo solo cio' che esiste
  gSettings.recSilenceMs     = prefs.getUInt ("recMs",   gSettings.recSilenceMs);
  gSettings.recSilenceMargin = prefs.getFloat("recMg",   gSettings.recSilenceMargin);
  gSettings.recSilenceFloor  = prefs.getInt  ("recFl",   gSettings.recSilenceFloor);
  gSettings.micLvlMargin     = prefs.getFloat("lvlMg",   gSettings.micLvlMargin);
  gSettings.micLvlFloor      = prefs.getFloat("lvlFl",   gSettings.micLvlFloor);
  gSettings.micLvlAttack     = prefs.getFloat("lvlAt",   gSettings.micLvlAttack);
  gSettings.micLvlRelease    = prefs.getFloat("lvlRe",   gSettings.micLvlRelease);
  gSettings.idleReactive     = prefs.getBool ("idle",    gSettings.idleReactive);
  gSettings.wakeGain         = prefs.getInt  ("wkGain",  gSettings.wakeGain);
  gSettings.wakeProbCutoff   = prefs.getInt  ("wkCut",   gSettings.wakeProbCutoff);
  gSettings.wakeWindow       = prefs.getInt  ("wkWin",   gSettings.wakeWindow);
  gSettings.voiceId          = prefs.getString("voice",  gSettings.voiceId);
  gSettings.voiceIdAlt       = prefs.getString("voiceA", gSettings.voiceIdAlt);
  gSettings.voiceTrigger     = prefs.getString("vtrig",  gSettings.voiceTrigger);
  gSettings.llmModel         = prefs.getString("model",  gSettings.llmModel);
  gSettings.systemPrompt     = prefs.getString("prompt", gSettings.systemPrompt);
  gSettings.hallucTerms      = prefs.getString("hall",   gSettings.hallucTerms);
  gSettings.musicStations    = prefs.getString("music",  gSettings.musicStations);
  gSettings.replyTrigger     = prefs.getString("rtrig",  gSettings.replyTrigger);
  gSettings.replyText        = prefs.getString("rtext",  gSettings.replyText);
  prefs.end();
  clamp();
  Serial.println("[set] impostazioni caricate da NVS (default se assenti)");
}

void settingsSave() {
  clamp();
  prefs.begin("cfg", false);          // lettura/scrittura
  prefs.putUInt  ("recMs",  gSettings.recSilenceMs);
  prefs.putFloat ("recMg",  gSettings.recSilenceMargin);
  prefs.putInt   ("recFl",  gSettings.recSilenceFloor);
  prefs.putFloat ("lvlMg",  gSettings.micLvlMargin);
  prefs.putFloat ("lvlFl",  gSettings.micLvlFloor);
  prefs.putFloat ("lvlAt",  gSettings.micLvlAttack);
  prefs.putFloat ("lvlRe",  gSettings.micLvlRelease);
  prefs.putBool  ("idle",   gSettings.idleReactive);
  prefs.putInt   ("wkGain", gSettings.wakeGain);
  prefs.putInt   ("wkCut",  gSettings.wakeProbCutoff);
  prefs.putInt   ("wkWin",  gSettings.wakeWindow);
  prefs.putString("voice",  gSettings.voiceId);
  prefs.putString("voiceA", gSettings.voiceIdAlt);
  prefs.putString("vtrig",  gSettings.voiceTrigger);
  prefs.putString("model",  gSettings.llmModel);
  prefs.putString("prompt", gSettings.systemPrompt);
  prefs.putString("hall",   gSettings.hallucTerms);
  prefs.putString("music",  gSettings.musicStations);
  prefs.putString("rtrig",  gSettings.replyTrigger);
  prefs.putString("rtext",  gSettings.replyText);
  prefs.end();
  Serial.println("[set] impostazioni salvate in NVS");
}

void settingsResetDefaults() {
  loadDefaults();
  settingsSave();
}
