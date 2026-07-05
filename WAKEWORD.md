# ALEXO — Wake word locale (microWakeWord)

Wake word **offline** sull'ESP32-S3 per avviare la chat a voce, in parallelo al
click encoder. Tutto in locale (nessun cloud per il wake).

> Stato: ✅ **FUNZIONANTE** — wake word **"Okay Nabu"** integrato e testato.
> Tutti i passi sotto sono FATTI. Implementazione in
> `src/wakeword.cpp` (+ `src/wake_model.h`, `lib/microfrontend/`), attiva con
> `WAKE_ENABLE 1` in `config.h`.
>
> **Parola attuale = "Okay Nabu"** (modello pre-addestrato): si pronuncia
> bene ed è preciso. Scartati "hey jarvis" (non colto per l'accento) e "alexa"
> (scattava su qualsiasi "-xa"). **"Alexo" CUSTOM** = unico upgrade rimasto (serve
> allenare un modello microWakeWord via Colab; nessuna registrazione, voci
> sintetiche; ~30-60 min con guida). Drop-in: sostituire `wake_model.h` + i `WAKE_*`.
>
> **Tuning finale**: WAKE_GAIN=4, WAKE_PROB_CUTOFF=240, WAKE_WINDOW=5. Lezioni:
> il modello è STREAMING → vuole flusso CONTINUO (niente delay tra le letture, o
> non rileva); troppo guadagno SATURA e peggiora; dopo l'interazione servono
> `micFlush()`+`wakeReset()` o riparte da solo sull'audio stantio.

## Architettura target

```
🎤 I2S 16kHz (continuo) → [feature frontend: 40 mel ogni 10ms]
   → modello microWakeWord INT8 (MixNet streaming, inferenza ogni 20ms, <10ms su S3)
   → probabilità wake → soglia + debounce (N inferenze consecutive sopra soglia)
   → TRIGGER: stesso ingresso del click encoder (gobboTakeTalkRequest)
              → pipeline cloud già pronta (Groq → Claude → ElevenLabs)
```

Il wake **sostituisce solo l'avvio**; tutto il resto della pipeline è invariato.
Il click encoder resta come avvio manuale e come stop.

## Dati tecnici (verificati giu 2026)

- Modello: **INT8 TFLite ~200–240 KB**, architettura MixNet streaming (stato interno).
- Frontend: **40 feature spettrali ogni 10 ms** (tipo mel/micro_speech, con
  noise-suppression + AGC nel preprocessore).
- Inferenza ogni **20 ms** sull'ultima stride; **<10 ms** su ESP32-S3 (con esp-nn).
- RAM: **~350 KB** (tensor arena) → allocare in **PSRAM** (`ps_malloc`), ne abbiamo 8 MB.
- Flash: il modello (~240 KB) come array `const` in `.rodata` (16 MB di flash, ampio).

## Toolchain TFLite Micro — LA decisione critica (da validare con un build)

Il progetto è Arduino/PlatformIO; TFLite Micro nasce ESP-IDF. Due strade:

1. **`esp-tflite-micro`** (Espressif): massima performance (kernel ottimizzati
   `esp-nn` sulle istruzioni vettoriali dell'S3). È un componente ESP-IDF: in
   PlatformIO-Arduino va aggiunto come componente, integrazione più delicata.
2. **Libreria Arduino che impacchetta TFLM + microfrontend** (es. EdgeNeuron, o un
   port `tflite-micro` per Arduino): più facile da mettere in `lib_deps`, ma può
   non avere l'accelerazione esp-nn e usare una versione TF più vecchia.

> **Azione**: provare a buildare un'inferenza MINIMALE con (1) e, se troppo
> ostico in Arduino, ripiegare su (2). Questo è il primo vero esperimento: finché
> non compila e gira una `Invoke()` su un tensore fittizio, il resto è teoria.

## Passi (in ordine, ciascuno verificabile via OTA)

1. **[FATTO] Step 0 — mic pulito.** Passa-alto + shift 15 + diagnostica `MIC_DIAG`.
2. **TFLite Micro compila e gira.** Aggiungere la lib scelta, far girare una
   `Invoke()` banale on-device (anche su input finto). Verifica: nessun crash,
   tempi di inferenza stampati su Telnet (`netlog`). ← de-risk della toolchain.
3. **Frontend feature.** Generare le 40 feature/10 ms dallo stream I2S (libreria
   microfrontend di TFLM). Verifica: dump dei valori su Telnet con voce vs silenzio.
4. **Modello PRE-ADDESTRATO.** Scaricare un modello pronto da
   `esphome/micro-wake-word-models` (es. "hey jarvis"), incorporarlo come array,
   collegarlo frontend→modello, stampare la probabilità su Telnet. Verifica:
   pronunciando la parola la probabilità sale. ← prova che la catena funziona.
5. **Soglia + debounce + trigger.** Sopra soglia per N inferenze consecutive →
   chiama l'avvio chat (stesso punto del click encoder). Tuning anti-falsi-positivi.
6. **Modello "Alexo" CUSTOM.** Allenarlo col Colab di `OHF-Voice/micro-wake-word`
   (campioni sintetici via TTS), esportare INT8, sostituire l'array del passo 4.
7. **Rifinitura**: gestione stato "sordo durante la chat", rientro in ascolto,
   eventuale doppia conferma, consumo.

## Stato implementazione (aggiornato giu 2026)

- **Passo 2 FATTO ✅**: TFLite Micro gira sull'S3. Lib = **Chirale_TensorFlowLite**
  (in `platformio.ini`). Self-test `hello_world` (`TFL_SELFTEST` + `src/tfltest.cpp`
  + `src/tfl_hello_model.h`): inferenza 38–132 µs, output corretti. `esp-tflite-micro`
  scartato (gira male in PlatformIO).
- **Modello scelto**: microWakeWord **v2 "alexa"** (vicino ad "Alexo"), da
  `esphome/micro-wake-word-models/models/v2/alexa.tflite`. Già incorporato in
  **`src/wake_model.h`** (`g_wake_model`, 55856 byte, INT8). Manifest v2:
  `probability_cutoff=0.9`, `sliding_window_size=5`, `feature_step_size=10ms`,
  `tensor_arena_size=22348` (~22 KB, in PSRAM). Detection: media mobile della
  probabilità su 5 step > 0.9 (+ refrattario anti-ripetizione).

### Frontend feature — specifica ESATTA (deve combaciare col training)
Preprocessore micro_speech / **TFLM microfrontend** (`tensorflow/lite/experimental/
microfrontend/lib/`), config da `preprocessor_settings.h` di ESPHome:
- sample rate **16000**, finestra **30 ms** (480 camp.), passo **10 ms** (160 camp.)
- **40** canali mel, banda **125–7500 Hz**
- noise reduction: smoothing_bits=10, even=0.025, odd=0.06, min_signal_remaining=0.05
- PCAN: enable=true, strength=0.95, offset=80.0, gain_bits=21
- log scale: enable=true, scale_shift=6
- output: 40 feature INT8/UINT8 per slice (un'inferenza ogni step da 10 ms).

### Vendorizzazione microfrontend (passo 3, IN CORSO)
Chirale NON include il microfrontend → va vendorizzato in `lib/microfrontend/`.
File core da `tensorflow/lite/experimental/microfrontend/lib/` (no _io/_test/_main/
memmap/BUILD): frontend, frontend_util, filterbank(+util), noise_reduction(+util),
pcan_gain_control(+util), log_scale(+util), log_lut, window(+util), fft, fft_util,
kiss_fft_int16, kiss_fft_common.h, bits.h. **Dipendenza kissfft**: kiss_fft_int16
include, dentro `namespace kissfft_fixed16` con `FIXED_POINT=16`, i sorgenti kissfft
`kiss_fft.h/.c`, `tools/kiss_fftr.h/.c` (+ `_kiss_fft_guts.h`) dal repo
mborgerding/kissfft alla versione di `tensorflow/lite/micro/tools/make/kissfft_download.sh`
(+ `third_party/kissfft/kissfft.patch`). Struttura lib PlatformIO:
`lib/microfrontend/src/tensorflow/...` (per gli include assoluti TFLM) e i sorgenti
kissfft raggiungibili come `kiss_fft.h`/`tools/kiss_fftr.h` dall'include path.

## Integrazione audio (nota importante)

Oggi a riposo il `loop()` fa `micPeekLevel()` (un `i2s_read` per il ring reattivo).
Col wake servirà leggere **tutto** lo stream in continuo: si unificherà la lettura
I2S a riposo in modo che lo **stesso** chunk alimenti sia il livello del ring sia
il buffer del wake (niente doppio `i2s_read`, altrimenti il modello perde metà
audio). Vedi seam in `wakeword.cpp` / `main.cpp` (`WAKE_ENABLE`).

## Cosa serve da te (in parallelo)

- Per il passo 6: far girare il **Colab di microWakeWord** per allenare "Alexo"
  (genera campioni sintetici, ~decine di minuti). Output: un `.tflite` INT8 + i
  parametri del frontend. Te lo guido quando arriviamo lì.
- Per i passi 2–5 non serve nulla da te se non flashare e leggere il Telnet.

## Fonti
- microWakeWord: https://microwakeword.com/ (train: https://microwakeword.com/train)
- Repo training: https://github.com/OHF-Voice/micro-wake-word
- Modelli pronti: https://github.com/esphome/micro-wake-word-models
- esp-tflite-micro: https://github.com/espressif/esp-tflite-micro
- ESPHome micro_wake_word: https://esphome.io/components/micro_wake_word/
- Guida pratica S3+TFLM: https://dev.to/zediot/esp32-s3-tensorflow-lite-micro-a-practical-guide-to-local-wake-word-edge-ai-inference-5540
