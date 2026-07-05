# ALEXO — Guida al cablaggio hardware

Riepilogo completo di **tutti** i collegamenti pin-a-pin tra i componenti, con
condensatori, resistenze, varianti di nomenclatura serigrafata sui moduli e
buone norme. Niente schemi ASCII: solo elenchi "da pin a pin".

Fonte dei pin: [`include/config.h`](include/config.h) — **unica fonte di verità**.
Se cambi un cablaggio, modifica i `#define` lì, non altrove.

---

## Premesse generali (valgono per tutto)

- **L'ESP32-S3 lavora a 3.3V sui pin.** Mai mettere 5V su un GPIO. I moduli si
  alimentano dal pin `5V` (chiamato anche `VIN` / `VBUS`) o dal `3V3` della
  board a seconda del componente — specificato sotto per ognuno.
- **GND comune obbligatorio.** Tutti i GND di tutti i moduli (mic, VS1053,
  display, ring, encoder, bottone) devono finire sullo stesso GND della ESP32.
  Senza massa comune i segnali "ballano" e niente funziona. È la regola d'oro:
  se un modulo si comporta in modo strano, il primo sospetto è sempre una massa
  mancante o ballerina.
- **Pin da NON toccare** su questa board (ESP32-S3 N16R8): GPIO 33–37 (PSRAM
  octal), 19/20 (USB), 0/3/45/46 (strapping). Sono già evitati nella config.

---

## Microfono — DUE opzioni (se ne collega UNA sola)

La scelta si fa con `MIC_USE_I2S` in [`config.h`](include/config.h):
`1` = mic I2S digitale ICS-43434 (**in uso ora**), `0` = MAX4466 analogico
(backend alternativo). L'API software è identica → cambi solo la riga, ricompili,
ricabli.

### Opzione A — MAX4466 analogico (alternativo, `MIC_USE_I2S 0`)

| Pin sul modulo MAX4466 | Va collegato a | Note |
|---|---|---|
| `VCC` (`VDD` / `V` / `+`) | **3V3** della ESP32 | **Non** 5V: a 5V il livello DC di riposo esce dal range dell'ADC |
| `GND` (`G` / `-`) | GND comune | — |
| `OUT` (`AUD` / `A0` / `S`) | **GPIO4** (ADC1_CH3) | uscita analogica |

**Condensatore (importante, già previsto nel progetto):** **470 µF elettrolitico
tra VCC e GND del MAX4466**, il più vicino possibile al modulo. Disaccoppia
l'alimentazione: senza, i NeoPixel che si accendono "sporcano" la tensione e il
mic capta i loro disturbi (l'idle reattivo non funziona). Polarità: gamba lunga
(+) su VCC, gamba corta / lato con striscia chiara e segno `–` su GND.

Il guadagno si regola col **trimmer sul retro** del MAX4466 (cacciavite piccolo),
nessuna resistenza esterna.

### Opzione B — mic I2S digitale ICS-43434 / INMP441 (IN USO ORA, `MIC_USE_I2S 1`)

Microfono digitale: niente ADC, niente condensatori di disaccoppiamento (sparisce
del tutto l'accoppiamento LED→mic). Alimentare a **3V3**.

| Pin sul modulo I2S | Va collegato a | Nomi possibili sul modulo |
|---|---|---|
| Alimentazione | **3V3** | `VDD`, `VCC`, `3V3`, `+` |
| Massa | GND comune | `GND`, `G`, `-` |
| Bit clock | **GPIO5** | `SCK`, `BCLK`, `CLK`, `SCLK` |
| Word select | **GPIO6** | `WS`, `LRCL`, `LRCLK`, `FS` |
| Dati dal microfono | **GPIO7** | `SD`, `DOUT`, `DATA`, `DO` |
| Selezione canale | **GND** | `L/R`, `SEL`, `LRSEL` → a **GND = canale LEFT** (quello che il codice si aspetta) |

> Sul modulo INMP441 può esserci anche `VDD`/`GND` doppi: collega comunque tutti
> i GND. Il pin `L/R` a GND seleziona LEFT; a 3V3 selezionerebbe RIGHT (da evitare).

**Buone norme I2S:**
- Niente resistenze/condensatori esterni necessari.
- Dopo lo swap va **ri-tarata** la soglia dell'idle reattivo
  (`MIC_LVL_NOISE` / `MIC_LVL_GAIN` nel ramo I2S di [`src/mic.cpp`](src/mic.cpp))
  e l'`I2S_SHIFT` in config (alza = più volume, conversione 32→16 bit).
- I tre fili dati (SCK/WS/SD) tienili cortini e lontani dai fili audio/SPI.

---

## VS1053 (decoder MP3 → altoparlante, bus SPI)

È il modulo con più nomi diversi sulla serigrafia. Per ogni funzione elenco
**tutte** le diciture possibili (LC Technology, Adafruit, Sparkfun, cloni):

| Funzione / pin nel codice | GPIO ESP32 | Nomi possibili sul modulo |
|---|---|---|
| **Alimentazione** | `5V` | `5V`, `VCC`, `V+`, `VIN` — i moduli LC Technology vogliono **5V** (regolatore a bordo) |
| **Massa** | GND | `GND`, `G`, `-` |
| **Clock SPI (SCK)** | `GPIO12` | `SCK`, `SCLK`, `CLK`, `C` |
| **Dati verso il VS1053 (MOSI)** | `GPIO11` | `MOSI`, `SI`, `DI`, `SDI`, `DIN` |
| **Dati dal VS1053 (MISO)** | `GPIO13` | `MISO`, `SO`, `DO`, `SDO`, `DOUT` |
| **Chip Select comandi (XCS)** | `GPIO10` | `XCS`, `CS`, `SCS`, `xCS` |
| **Chip Select dati (XDCS)** | `GPIO21` | `XDCS`, `DCS`, `BSYNC`, `SDCS`, `X-DCS` |
| **Data Request (DREQ)** | `GPIO18` | `DREQ`, `DREQ/INT`, `DQ`, `D-REQ` — **uscita** del VS1053 (input per la ESP32): "sono pronto a ricevere altri dati". Spostato da GPIO47 a **GPIO18** |
| **Reset (XRST)** | `GPIO8` | `XRST`, `RST`, `RESET`, `XRESET`, `RES` — spostato da GPIO38 (era il "BUILTIN LED" della board: sporcava XRST durante il float di boot → reset del VS1003 inaffidabile a freddo) |

Altri pin presenti sul connettore VS1053 e cosa farne:
- **`AGND` / `LOUT` / `ROUT` / `GBUF`**: uscita audio analogica / cuffie.
  `LOUT`/`ROUT` = canale sinistro/destro, `GBUF` = massa virtuale cuffie.
  Vanno all'altoparlante (via ampli o jack), **non** alla ESP32.
- **Slot microSD** (`CARD_CS` o un secondo gruppo `CS/SCK/MISO/MOSI`): molti
  moduli LC Technology ce l'hanno sullo stesso bus SPI. Se non lo usi, lascialo
  scollegato (CS della SD non collegato = SD disabilitata).
- **`MIDI` / `RX` / `GPIO0..3` del VS1053**: pin di modalità/MIDI, non servono.

**Buone norme VS1053:**
- È sullo **stesso bus SPI (FSPI)** della board (SCK/MOSI/MISO condivisi); il
  display sta su un **bus separato (HSPI)** apposta per non litigare. Non
  spostare il VS1053 sul bus del display.
- Se "si pianta" sui suoni brevi (bip) è un comportamento noto del chip già
  gestito nel codice (niente `stopSong`, si manda una coda di silenzio). Non è
  un problema di cablaggio.

> ⚠️ Conflitto da ricordare: `GPIO13` è MISO del VS1053 **e** sarebbe `I2S_SHIFT`
> nel ramo I2S — ma sono mutuamente esclusivi (il `13` lì è solo un parametro
> numerico di shift, non un pin). Nessun conflitto reale.

---

## Amplificatore PAM8302A (ampli Class-D mono → altoparlante)

Amplifica l'uscita di linea del VS1053 per pilotare un altoparlante a volume
pieno. L'**ingresso** del PAM si prende dall'uscita audio del VS1053, **non** da
un GPIO.

| Funzione / pin nel codice | Va collegato a | Nomi possibili sul modulo |
|---|---|---|
| **Alimentazione** | `5V` | `VDD`, `VCC`, `V+`, `5V` (regge 2.0–5.5V) |
| **Massa** | GND comune | `GND`, `G`, `-` |
| **Ingresso audio +** | **`LOUT`** (o `ROUT`) del VS1053 | `IN+`, `A+`, `IN`, `L` |
| **Ingresso audio −** | **`AGND`** del VS1053 | `IN-`, `A-`, `GND audio` |
| **Shutdown** | **`GPIO39`** | `SD`, `/SD`, `SHDN`, `EN` |
| **Uscita +/−** | altoparlante | `+`/`-`, `OUT+`/`OUT-` |

**Pin SD (shutdown), ATTIVO BASSO — gestito dal firmware:**
- `HIGH` = ampli acceso; `LOW` = ampli **muto** (consumo ~0, **niente fruscio**
  Class-D di fondo, niente pop). Sulla maggior parte delle breakout, se lasci SD
  **scollegato** l'ampli resta acceso (pull-up a bordo).
- Il firmware lo tiene acceso **solo durante l'interazione** (bip + voce) e muto
  a riposo: vedi `AMP_SD_PIN` in [`config.h`](include/config.h) e `ampEnable()`
  in [`src/main.cpp`](src/main.cpp). Metti `AMP_SD_PIN -1` per disabilitare il
  controllo (ampli sempre acceso).
- **Per provarlo "fisso acceso"**: porta SD alla **stessa tensione di VDD**
  (se alimenti il PAM a 5V → SD a 5V). Mai SD sopra la VDD dell'ampli.

> Nota pin: `GPIO39` fa parte del gruppo JTAG (MTCK), già rinunciato dal progetto
> (40/41/42 = MTDO/MTDI/MTMS sono il display). Nessun conflitto.

---

## Encoder rotativo KY-040 (scroll della chat sul display)

| Funzione / pin nel codice | GPIO ESP32 | Nomi possibili sul modulo |
|---|---|---|
| **Alimentazione** | **3V3** | `+`, `VCC`, `V`, `5V` (sul KY-040 c'è scritto `+`; **alimentalo a 3V3**, non 5V) |
| **Massa** | GND | `GND`, `G`, `-` |
| **Canale A (CLK)** | `GPIO15` | `CLK`, `A`, `OUT_A`, `S1`, `ENC_A` |
| **Canale B (DT)** | `GPIO16` | `DT`, `B`, `OUT_B`, `S2`, `ENC_B` |
| **Pulsante (premere la manopola)** | `GPIO17` | `SW`, `KEY`, `BTN`, `PUSH`, `S` |

> ⚠️ **Cablaggio fisico vs firmware (situazione attuale, funzionante):** sopra c'è
> il cablaggio FISICO reale (CLK→GPIO15, DT→GPIO16). In [`config.h`](include/config.h)
> però i `#define` sono **invertiti apposta** — `ENC_A_PIN 16`, `ENC_B_PIN 15` —
> per correggere via software il verso di rotazione (A/B erano risultati scambiati
> in saldatura). **Non "raddrizzare" i define per farli combaciare con questa
> tabella**: girando i due valori si inverte il senso dello scroll. Così com'è,
> gira e scrolla nel verso giusto.

Funzione: girare = scorri la chat su/giù; premere il pulsante = torna "live".

**Resistenze / condensatori sull'encoder:**
- Il **KY-040** ha già a bordo due resistenze di pull-up (10kΩ) su CLK e DT. Con
  un encoder "nudo" servirebbero pull-up esterne — ma in pratica si usano le
  **pull-up interne della ESP32** via software (`INPUT_PULLUP`): non saldi nulla.
- Gli encoder meccanici "rimbalzano". Opzionale: **100 nF ceramico tra CLK e GND
  e un altro tra DT e GND** pulisce molto la lettura. I ceramici **non** hanno
  polarità, vanno in qualunque verso.

---

## Bottone push-to-talk — RIMOSSO (l'encoder è l'unico comando)

Non c'è un bottone dedicato: il contenitore ha il **solo encoder**. Il pulsante
dell'encoder fa tutto. **GPIO14**, ex push-to-talk, è ora riusato per il
**backlight del display** (vedi sezione TFT ST7735).

Gesti dell'encoder (vedi [`src/gobbo.cpp`](src/gobbo.cpp)):

| Gesto | Effetto |
|---|---|
| **Click** (a riposo) | avvia la chat (inizia a registrare) |
| **Click** (mentre registra) | ferma e invia *(toggle)* |
| **Premuto + giro** | volume su/giù |
| **Giro libero** | scroll della chat |

> Predisposizione per il futuro vocale: il "click di avvio" sarà rimpiazzato dal
> **wake-word** e il "click di stop" dal **rilevamento del silenzio**, senza
> toccare il resto della pipeline (start/stop sono già eventi separati).

---

## Ring NeoPixel WS2812 (12 LED)

| Pin sul ring | Va collegato a | Note |
|---|---|---|
| `5V` (`VCC` / `+5V` / `PWR` / `+`) | **5V** | i WS2812 vogliono 5V per colori pieni |
| `GND` (`-` / `G`) | GND comune | — |
| `DIN` (`DI` / `IN` / `Data In`, segui la freccia ➜ stampata) | **GPIO48** | il dato entra dal lato `DIN`, non `DOUT` |

**Condensatori e resistenze (raccomandati, in parte già nel progetto):**
- **1000 µF elettrolitico tra 5V e GND del ring**, vicino al ring. Già previsto:
  assorbe i picchi di corrente quando i LED cambiano colore insieme (altrimenti
  disturbano il mic). Polarità: + su 5V, – (striscia chiara) su GND.
- **Resistenza ~330 Ω in serie sul filo DIN** (tra GPIO48 e l'ingresso dati del
  ring). Protegge il primo LED e fa da rimedio anti-EMI. Senza polarità; va *in
  linea* sul filo dati (taglia il filo e metti la resistenza in mezzo).

**Nota EMI (già documentata):** tieni il filo `DIN` del ring **lontano** dai fili
SCLK/MOSI del display, altrimenti l'accoppiamento elettromagnetico fa lampeggiare
il ring durante lo scroll. La resistenza serie da 330 Ω sul DIN aiuta anche qui.

---

## Display TFT ST7735 1.8" (bus SPI dedicato HSPI)

| Funzione / pin nel codice | GPIO ESP32 | Nomi possibili sul modulo |
|---|---|---|
| Clock | `GPIO2` | `SCK`, `SCL`, `CLK` |
| Dati | `GPIO1` | `SDA`, `MOSI`, `DIN`, `SI` |
| Chip Select | `GPIO42` | `CS`, `LCD_CS` |
| Data/Command | `GPIO41` | `DC`, `A0`, `RS`, `D/C` |
| Reset | `GPIO40` | `RES`, `RST`, `RESET` |
| Alimentazione | `3V3` | `VCC`, `VDD` |
| Massa | GND | `GND` |
| Retroilluminazione | `GPIO14` | `LED`, `BL`, `BLK` |

**Retroilluminazione su GPIO (novità):** il pin `LED`/`BL` **non** va più a 3V3
fisso ma a **GPIO14** (era libero, ex push-to-talk). Così il firmware può
**spegnere il display a riposo** (dopo `DISPLAY_SLEEP_MS`, default 2 min) e
riaccenderlo al primo intervento. Il pin assorbe **~2 mA** (misurati): si pilota
**direttamente dal GPIO**, *niente transistor né resistenza esterna* (il catodo
del LED è già a GND dentro il modulo, e la resistenza di limitazione è a bordo).
HIGH = acceso, LOW = spento. Se volessi tenerlo sempre acceso: `DISPLAY_SLEEP_MS 0`
in [`config.h`](include/config.h).

Nessun passivo extra oltre a quelli a bordo del modulo. Se vedi "garbage" a
schermo è questione di clock SPI (scendi da 24 MHz a 16/20 MHz in config), non di
cablaggio.

---

## Riepilogo componenti passivi da avere

| Componente | Quantità | Dove | Polarità? |
|---|---|---|---|
| Elettrolitico **470 µF** | 1 | VCC↔GND del MAX4466 (solo mic analogico) | **Sì** (+ su VCC) |
| Elettrolitico **1000 µF** | 1 | 5V↔GND del ring | **Sì** (+ su 5V) |
| Resistenza **330 Ω** | 1 | in serie sul filo DIN del ring | No |
| Ceramico **100 nF** | 0–2 (opz.) | CLK↔GND e DT↔GND dell'encoder | No |

> Con il **mic I2S** il 470 µF del mic **non serve più** (mic digitale).

**Polarità di un elettrolitico, in due parole:** gamba **lunga = +**; sul corpo
c'è una **striscia chiara col segno `–`** dal lato della gamba corta. Il `+` va
verso l'alimentazione (VCC o 5V), il `–` verso GND. Resistenze e ceramici si
montano in qualunque verso.

---

## Tabella riassuntiva GPIO usati

> 📌 Pinout di riferimento in [`IMMAGINI/`](IMMAGINI/): `esp32-S3-DevKitC-1.png`
> (vista dall'alto, lato componenti) e `esp32-S3-DevKitC-1_LATO-SALDATURE.png`
> (vista da **sotto**, lato saldature — lati invertiti ma testo leggibile, utile
> mentre saldi guardando il retro della board).


| GPIO | Componente | Segnale |
|---|---|---|
| 1 | TFT ST7735 | MOSI/SDA |
| 2 | TFT ST7735 | SCLK/SCL |
| 4 | MAX4466 | OUT (ADC1) — *solo mic analogico* |
| 5 | mic I2S | BCLK — *solo mic I2S* |
| 6 | mic I2S | WS — *solo mic I2S* |
| 7 | mic I2S | SD (dati) — *solo mic I2S* |
| 10 | VS1053 | XCS |
| 11 | VS1053 | MOSI |
| 12 | VS1053 | SCK |
| 13 | VS1053 | MISO |
| 14 | TFT ST7735 | LED/BL (backlight, ex push-to-talk) |
| 15 | Encoder | A / CLK |
| 16 | Encoder | B / DT |
| 17 | Encoder | SW (pulsante = avvia/ferma chat) |
| 21 | VS1053 | XDCS |
| 8 | VS1053 | XRST (spostato da GPIO38 = builtin LED) |
| 39 | PAM8302A | SD (shutdown ampli) |
| 40 | TFT ST7735 | RST |
| 41 | TFT ST7735 | DC/A0/RS (Data/Command) |
| 42 | TFT ST7735 | CS |
| 18 | VS1053 | DREQ (spostato da GPIO47) |
| 48 | Ring WS2812 | DIN |
