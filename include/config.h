#pragma once
// ============================================================================
//  ALEXO - Configurazione hardware (ESP32-S3 N16R8 DevKitC-1)
//  Modifica qui i pin se hai cablato diversamente. Tutto il resto del codice
//  legge da questo file, quindi non serve toccare altro.
// ============================================================================
//
//  NOTE IMPORTANTI sui pin dell'ESP32-S3 N16R8:
//   - La PSRAM octal usa internamente i GPIO 33..37: NON usarli.
//   - GPIO 19/20 sono USB: lasciarli liberi.
//   - L'ADC1 (per il microfono analogico) e' sui GPIO 1..10.
//   - GPIO 0 / 45 / 46 / 3 sono strapping pin: evitarli per le periferiche.
//
// ----------------------------------------------------------------------------

// --- Display TFT ST7735 1.8" 128x160 (SPI) ----------------------------------
//  Display TEMPORANEO (in attesa del 5"). Sta su un bus SPI DEDICATO (HSPI),
//  separato da quello del VS1053 (FSPI): cosi' lo scroll della chat sul core 0
//  non litiga col feed audio sul core 1 (niente singhiozzi).
//  Tutti GPIO liberi (non strapping, non USB, non ADC2, non PSRAM).
//  Cablaggio modulo ST7735: VCC->3V3  GND->GND  LED/BL->3V3  e i pin sotto.
#define TFT_SCLK_PIN     2      // SCK / SCL
#define TFT_MOSI_PIN     1      // SDA / MOSI (DIN)
#define TFT_CS_PIN       42     // CS
#define TFT_DC_PIN       41     // DC / A0 / RS
#define TFT_RST_PIN      40     // RES / RST
#define TFT_WIDTH        128
#define TFT_HEIGHT       160
//  Variante del "tab" del modulo: se i colori sono invertiti o c'e' un bordo,
//  prova INITR_GREENTAB o INITR_REDTAB. Il classico 1.8" rosso e' BLACKTAB.
#define TFT_INITR        INITR_BLACKTAB
//  Clock SPI del TFT. Il lampeggio del ring durante lo
//  scroll era EMI dei fronti SPI accoppiati sul filo DIN del WS2812: a 2MHz
//  spariva ma lo scroll andava "a onde" (blit a schermo intero ~164ms = raster
//  lento e visibile). FIX HW: DIN allontanato dai fili SCLK/MOSI (+ event. 330ohm
//  serie sul DIN) -> si puo' tenere il clock alto. 24MHz = blit ~14ms, scroll
//  liscio. Se il TUO modulo mostra garbage a 24MHz, scendi a 16/20 MHz.
#define TFT_SPI_HZ       24000000

//  Retroilluminazione (LED/BL) pilotata da GPIO, NON piu' fissa a 3V3: cosi'
//  possiamo spegnerla a riposo per risparmiare. Il pin assorbe pochissimo
//  (~2 mA misurati col multimetro) -> si pilota DIRETTAMENTE dal GPIO, senza
//  transistor ne' resistenza esterna. HIGH = acceso, LOW = spento.
//  CABLAGGIO: il pin LED/BL del modulo va a GPIO14 (prima andava a 3V3). GPIO14
//  era libero (ex push-to-talk). Il catodo del LED e' gia' a GND dentro il modulo.
#define TFT_BL_PIN       14
//  Dopo questi ms SENZA interventi (nessun click/giro encoder/nuovo messaggio,
//  e Alexo a riposo) il display si spegne; si riaccende al primo intervento.
//  Metti 0 per tenerlo sempre acceso.
#define DISPLAY_SLEEP_MS 120000   // 2 minuti
//  Splash animato all'accensione (HUD futuristico su TFT + ring "carica"). 0 = off.
#define SPLASH_BOOT      1

// --- Ring NeoPixel 12 LED WS2812 --------------------------------------------
#define LED_RING_PIN     48     // data in del ring
#define LED_RING_COUNT   12
#define LED_BRIGHTNESS   40     // 0-255, tienilo basso per non scaldare/assorbire troppo
//  Idle reattivo al suono: 1 = ring "balla" col microfono a riposo (psichedelico),
//  0 = ring SPENTO a riposo (luci solo negli stati attivi). Riacceso
//  dopo il fix del rumore mic (passa-alto in mic.cpp): ora il fondo a riposo e'
//  basso e il ring non lampeggia piu' a vuoto.
#define IDLE_REACTIVE    1

// --- Bottone push-to-talk (RIMOSSO) -----------------------------------------
//  Storico: c'era un bottone dedicato su GPIO14. Ora l'UNICO comando e' il
//  pulsante dell'encoder (vedi sotto): click = avvia/ferma la chat, premuto+giro
//  = volume, giro = scroll. GPIO14 e' ora usato dal backlight del display
//  (TFT_BL_PIN sopra). Define rimossi (non piu' usati da nessuna parte).

// --- MICROFONO --------------------------------------------------------------
//  Selezione del microfono. L'API software e' identica per entrambi, quindi
//  per cambiare basta questa riga (e ricablare): NON si tocca altro codice.
//    0 = MAX4466 analogico (attuale, su ADC1)
//    1 = mic I2S digitale (ICS-43434 / INMP441) -> metti 1, ricompila, flasha
#define MIC_USE_I2S      1

#define MIC_SAMPLE_RATE  16000  // Hz, quello che vuole Whisper (per entrambi)

//  MAX4466 (analogico, usato se MIC_USE_I2S = 0)
#define MIC_ADC_PIN      4      // GPIO4 = ADC1_CH3 (uscita analogica del mic)

//  ICS-43434 (I2S, usato se MIC_USE_I2S = 1). Alimentare a 3V3.
//    VDD->3V3  GND->GND  SCK->I2S_SCK  WS->I2S_WS  SD->I2S_SD  L/R->GND(=LEFT)
#define I2S_SCK_PIN      5      // BCLK / SCK
#define I2S_WS_PIN       6      // WS / LRCL
#define I2S_SD_PIN       7      // SD / DOUT (dati dal microfono)
#define I2S_SHIFT        15     // conversione 32->16 bit (alza = piu' volume). 15 scelto
                                //  in diagnostica Step 0: voce ~10k di picco, niente
                                //  clipping (13 saturava). La registrazione applica
                                //  anche un passa-alto ~120Hz (vedi mic.cpp) che toglie
                                //  il DC e il rumble a bassa freq (grosso del rumore).
//  Diagnostica mic (Step 0 wake-word): metti 1, flasha via OTA, apri il monitor.
//  Il firmware si ferma in modalita' diagnostica e stampa il rumore di fondo a 24
//  bit + cosa darebbe ogni shift (13..16) a riposo e mentre parli: serve a
//  scegliere I2S_SHIFT con un numero invece che a tentativi. La chat NON parte, ma
//  l'OTA RESTA ATTIVO: per uscire rimetti 0 e riflasha via OTA. Default 0.
#define MIC_DIAG         0

// --- Wake word locale "Alexo" (microWakeWord, vedi WAKEWORD.md) -------------
//  In sviluppo. 0 = disattivo (avvio chat SOLO col click encoder, attuale).
//  1 = abilita il rilevamento wake (richiede TFLite Micro + modello, work in
//  progress in wakeword.cpp). Lo stub attuale non fa nulla: tenere 0.
#define WAKE_ENABLE      1
//  Parametri detection (dal manifest v2 "alexa"): cutoff probabilita' 0..255
//  (0.9*255≈230) e dimensione della finestra mobile su cui si fa la media.
#define WAKE_PROB_CUTOFF 246
#define WAKE_WINDOW      5
//  Guadagno digitale del solo percorso wake (il PCM a shift 15 e' troppo basso:
//  serviva gridare). Moltiplica i campioni prima del frontend, con clamp. Alza
//  se devi ancora alzare la voce, abbassa se compaiono falsi positivi.
#define WAKE_GAIN        3

// --- Registrazione voce (dopo l'attivazione: wake word o click) -------------
//  Stop automatico al silenzio: dopo aver sentito parlare, se restano REC_SILENCE_MS
//  di silenzio continuo la registrazione si chiude da sola (niente attesa del tetto).
#define REC_MAX_MS        20000   // tetto massimo registrazione (serve MIC_MAX_SECONDS>=20)
#define REC_SILENCE_MS     1500   // stop dopo questo silenzio continuo (0 = disattiva)
//  Stop-al-silenzio con SOGLIA AUTO-ADATTIVA (vedi mic.cpp). Non e' piu' un
//  livello fisso: la "voce" e' relativa al rumore di fondo stimato in continuo
//  (ventilatore/vento). soglia = noiseFloor * MARGIN + FLOOR (in unita' RMS, la
//  stessa scala di AC_HP16 in MIC_DIAG). Cosi' si adatta da solo se cambia il
//  rumore. MARGIN = quanto sopra il fondo conta come voce; FLOOR = margine minimo
//  assoluto (col mic tranquillo il fondo e' ~100, la voce ~500).
#define REC_SILENCE_MARGIN  1.6f  // moltiplicatore sul fondo (alza se il rumore fa da "voce")
#define REC_SILENCE_FLOOR    150  // margine minimo assoluto in RMS (alza se taglia tardi)
#define REC_MIN_MS          800   // grazia iniziale: non fermarti prima (lascia iniziare a parlare)
//  Self-test TFLite Micro (passo 2 di WAKEWORD.md): 1 = al boot gira il modello
//  di prova "hello_world" (sin) + frontend, stampa su Telnet. OTA resta attivo.
#define TFL_SELFTEST     0
//  Test catena wake senza mic: 1 = gira frontend->modello->prob su
//  audio sintetico (silenzio/seno) e stampa la probabilita', SENZA mic. Per
//  validare la catena e i falsi-positivi prima del test dal vivo. OTA resta attivo.
#define WAKE_TEST        0

// --- VS1053 (uscita audio, bus SPI) -----------------------------------------
//  Bus SPI condiviso (FSPI)
#define SPI_SCK_PIN      12
#define SPI_MOSI_PIN     11
#define SPI_MISO_PIN     13
//  Pin di controllo del VS1053
#define VS1053_XCS_PIN   10     // Chip Select (comandi)
#define VS1053_XDCS_PIN  21     // Data Chip Select (dati)
#define VS1053_DREQ_PIN  18     // Data Request (input) - spostato da GPIO47 a GPIO18
#define VS1053_XRST_PIN  8      // Reset (-1 se non collegato) - spostato da GPIO38
                                //  (il 38 e' il "BUILTIN LED" della board: il suo
                                //  circuito a bordo sporcava XRST durante il float
                                //  di boot -> reset del VS1003 inaffidabile a freddo)

// --- Amplificatore PAM8302A (shutdown via GPIO) -----------------------------
//  Il pin SD (shutdown, /SD) del PAM8302A: e' la funzione SHUTDOWN a essere
//  attiva-bassa, quindi per ACCENDERE l'ampli si porta il pin ALTO.
//      GPIO39 HIGH -> shutdown OFF -> ampli ACCESO
//      GPIO39 LOW  -> shutdown ON  -> ampli MUTO (consumo ~0, no fruscio, no pop)
//  Lo teniamo acceso solo durante l'interazione (bip + voce) e muto a riposo.
//  Cablaggio: SD del PAM8302A -> GPIO39. L'INGRESSO audio del PAM va preso dal
//  LOUT/ROUT (+ AGND) del VS1053, NON da un GPIO. Alimenta il PAM dai 5V.
//  GPIO39 e' libero (gruppo JTAG MTCK, gia' rinunciato: 40/41/42 sono il TFT).
//  Metti -1 per disabilitare il controllo (ampli sempre acceso / SD scollegato).
#define AMP_SD_PIN       39

// --- Encoder rotativo (scroll della chat sul gobbo) -------------------------
//  KY-040 o simile: CLK->A, DT->B, SW->pulsante. Alimentare a 3V3.
//  Pin scelti liberi e sicuri (non strapping, non USB, non ADC2, non PSRAM).
//  Giro = scorri la chat su/giu'; pressione del pulsante = torna "live".
#define ENC_A_PIN        16     // CLK (A) - GPIO16/15 invertiti: A/B scambiati in saldatura
#define ENC_B_PIN        15     // DT  (B)
#define ENC_SW_PIN       17     // SW  (pulsante, opzionale)

// --- Volume audio (VS1053, scala 0..100; 100 = massimo) ---------------------
//  Si regola PREMENDO il pulsante dell'encoder e girando: orario = su, antiorario
//  = giu' (vedi gobbo.cpp). Il valore e' salvato in NVS, quindi sopravvive ai
//  riavvii. Se i versi sono invertiti, cambia segno a VOLUME_STEP.
#define VOLUME_DEFAULT   90     // volume al primo avvio (poi vince quello salvato)
#define VOLUME_MIN        0     // 0 = MUTO vero (VS1053 silenzio + ampli spento)
#define VOLUME_STEP       5     // quanto cambia per ogni scatto dell'encoder
//  Il VS1053 ha scala LOGARITMICA (dB): la sua 0..~60 e' praticamente muta, solo
//  ~60..100 e' udibile. Per usare TUTTA la corsa dello slider/encoder rimappiamo
//  il volume utente 1..100 nella zona udibile VOLUME_VS_MIN..100 (vedi
//  volumeVsValue in volume.cpp). Alza VOLUME_VS_MIN se il minimo e' ancora muto.
#define VOLUME_VS_MIN    63     // valore VS1053 corrispondente al volume utente = 1

// --- Pannello impostazioni via web (settings.cpp + webui.cpp) ---------------
//  I parametri "tarabili" qui sotto sono i VALORI DI FABBRICA. All'avvio il
//  modulo settings li carica dall'NVS (se l'utente li ha cambiati dal pannello
//  web http://alexo.local/) altrimenti usa questi. "Ripristina default" nel
//  pannello riscrive questi valori. NB: i default di mic/silenzio/LED/wake sono
//  gia' le macro qui sopra (REC_SILENCE_*, MIC_LVL_*_DEF, WAKE_*, IDLE_REACTIVE).
//  Livello LED reattivi (default, poi modificabili dal pannello). Prima erano
//  #define fissi dentro mic.cpp; ora vivono qui come default.
#define MIC_LVL_MARGIN_DEF  3.0f   // quanto sopra il fondo prima di accendere i LED
#define MIC_LVL_FLOOR_DEF   40.0f  // margine assoluto minimo LED (anti-jitter)
//  Envelope dei LED reattivi: velocita' di salita (reattivita') e di discesa
//  (permanenza). 0..1: piu' basso = piu' lento. ATTACK basso = niente flash sugli
//  impulsi isolati; RELEASE basso = i LED "trascinano" (sfumano piano).
#define MIC_LVL_ATTACK_DEF  0.12f  // reattivita' (salita): quanto in fretta si accendono
#define MIC_LVL_RELEASE_DEF 0.25f  // permanenza (discesa): quanto in fretta si spengono
//  Voce ElevenLabs di default (Voice ID). Prima era in tts.cpp.
#define ELEVEN_VOICE_DEF    "fTHp5NEBwS4InadKS0Ci"
//  Voce ALTERNATIVA + parola-trigger: se la frase inizia con VOICE_TRIGGER_DEF,
//  Alexo risponde con ELEVEN_VOICE_ALT_DEF. Prima erano in main.cpp. Trigger vuoto
//  = disattiva la voce alternativa. Confronto in minuscolo.
#define ELEVEN_VOICE_ALT_DEF "CiwzbDpaN3pQXjTgx3ML"
#define VOICE_TRIGGER_DEF    "bene"
//  Termini dell'easter-egg (separati da virgola): se la domanda ne contiene uno,
//  Alexo antepone una frase fissa (vedi runInteraction in main.cpp). Vuoto = off.
#define EGG_TERMS_DEF        "sorprendimi"
//  Frasi-fantasma di Whisper (separate da virgola): se la trascrizione e' ESATTAMENTE
//  una di queste (tipiche allucinazioni sul silenzio), viene scartata in silenzio.
//  Vuoto = filtro disattivato. Confronto minuscolo, senza punteggiatura ai bordi.
#define HALLUC_TERMS_DEF \
    "grazie,grazie a tutti,grazie mille,grazie a tutti e arrivederci," \
    "grazie per la visione,grazie per l'attenzione,grazie e arrivederci," \
    "grazie per aver guardato,grazie per aver guardato il video," \
    "sottotitoli e revisione a cura di qtss,sottotitoli creati dalla comunità amara.org," \
    "ciao,ciao a tutti,buona giornata,arrivederci,prego"
//  Cervello: modello Claude di default e "personalita'" (system prompt). Prima
//  erano in llm.cpp. claude-haiku-4-5 = veloce/economico; claude-opus-4-8 = piu'
//  intelligente ma piu' lento/costoso.
#define LLM_MODEL_DEF       "claude-haiku-4-5"
#define SYSTEM_PROMPT_DEF \
    "Sei Alexo, un assistente vocale domestico in italiano. " \
    "Rispondi in modo breve, naturale e colloquiale, come parlando ad alta voce. " \
    "Massimo 2-3 frasi. Niente elenchi puntati, niente markdown, niente emoji. " \
    "Hai accesso a una ricerca web: usala quando serve un'informazione aggiornata " \
    "(meteo, notizie, orari, eventi recenti, prezzi). L'utente e' in Italia. " \
    "Riassumi i risultati in modo parlato e conciso. Se non sai, dillo con semplicita'."
//  Stazioni musica (web-radio MP3) editabili dal pannello. Una per riga, formato
//  "chiave | nome | url": la CHIAVE e' cio' che si cerca nella frase ("metti
//  <chiave>"), il NOME appare sul display, l'URL e' lo stream MP3. L'ordine conta:
//  le chiavi piu' specifiche prima (es. "alternativ" prima di "rock"). Solo stream
//  MP3 (il VS1053 non decodifica AAC): NO url .aac/.m3u8/HLS. URL http:// e https://
//  entrambi ok (music.cpp usa WiFiClientSecure per gli https). Righe doppie =
//  sinonimi per la stessa radio. Stazioni VERIFICATE funzionanti (181.fm, SomaFM, Kiss Kiss).
#define MUSIC_STATIONS_DEF \
    "alternativo | rock alternativo | http://listen.181fm.com/181-buzz_128k.mp3\n" \
    "metal | metal | http://listen.181fm.com/181-hardrock_128k.mp3\n" \
    "ottanta | anni 80 | http://listen.181fm.com/181-awesome80s_128k.mp3\n" \
    "anni 80 | anni 80 | http://listen.181fm.com/181-awesome80s_128k.mp3\n" \
    "novanta | anni 90 | http://listen.181fm.com/181-star90s_128k.mp3\n" \
    "anni 90 | anni 90 | http://listen.181fm.com/181-star90s_128k.mp3\n" \
    "settanta | anni 70 | http://listen.181fm.com/181-70s_128k.mp3\n" \
    "anni 70 | anni 70 | http://listen.181fm.com/181-70s_128k.mp3\n" \
    "country | country | http://listen.181fm.com/181-realcountry_128k.mp3\n" \
    "jazz | jazz | http://listen.181fm.com/181-classicaljazz_128k.mp3\n" \
    "blues | blues | http://listen.181fm.com/181-blues_128k.mp3\n" \
    "reggae | reggae | http://listen.181fm.com/181-reggae_128k.mp3\n" \
    "salsa | salsa | http://listen.181fm.com/181-salsa_128k.mp3\n" \
    "classica | classica | http://listen.181fm.com/181-classical_128k.mp3\n" \
    "dance | dance | http://listen.181fm.com/181-energy98_128k.mp3\n" \
    "lounge | lounge | http://listen.181fm.com/181-chilled_128k.mp3\n" \
    "hip hop | hip hop | http://listen.181fm.com/181-thebox_128k.mp3\n" \
    "indie | indie | http://ice1.somafm.com/indiepop-128-mp3\n" \
    "ambient | ambient | http://ice1.somafm.com/dronezone-128-mp3\n" \
    "kiss kiss | Radio Kiss Kiss | http://ice07.fluidstream.net/KissKiss.mp3\n" \
    "rock | rock | http://listen.181fm.com/181-eagle_128k.mp3\n" \
    "pop | pop | http://listen.181fm.com/181-power_128k.mp3"

// --- WiFi -------------------------------------------------------------------
//  Le credenziali e le API key stanno in include/secrets.h (NON versionato).
//  Copia secrets.example.h in secrets.h e compila i valori.
