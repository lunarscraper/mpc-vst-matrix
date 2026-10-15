# mpc-vst-matrix

**Matrix** ist ein Drum-Step-Sequencer als VST2-Insert-Effekt für die Akai Force (MPC OS). Das Vorbild
ist das BLM 16×16 des MIDIbox SEQ. Der Sequencer läuft im Plugin auf der Uhr der Force, das 16×16-Gitter
zum Bearbeiten öffnest du auf dem iPad oder Handy im WLAN. Die Noten gehen über einen eigenen MIDI-Port
an eine Drum-Spur.

**Status:** Version 0.1.0, offline getestet (x86, ASan/UBSan, Uhr bis auf den Puls geprüft). Auf der
Force noch nicht getestet.

## Was es kann

- **16 Spuren** (Kick, Snare, Clap, Hats, Toms …), jede mit Name, Note, MIDI-Kanal, Mute und Solo.
  Diese Einstellungen bilden das **Kit** und gelten für alle Patterns.
- **16 Patterns.** Jede Spur hat pro Pattern eine eigene **Länge von 1–64 Steps**. Damit sind
  Polymetrien möglich, zum Beispiel Hats über 12 Steps gegen eine Kick über 16.
- **Pro Step:** Velocity (0 = aus), **Chance** (1–100 %) und **Ratchet** (1–4 Schläge im Step).
- **Patternwechsel** springen nie mitten im Takt: Das neue Pattern beginnt am nächsten Taktanfang.
  Im Stopp wird sofort umgeschaltet.
- **Uhr:** Matrix folgt der Songposition der Force mit 96 Pulsen pro Viertel und bleibt so exakt im
  Raster. Loop und Locate werden sauber übernommen, beim Stopp verstummen alle Noten. Swing reicht
  von 50–75 %, Gate von 5–95 % eines Sechzehntels.
- **32 Presets** mit Kit und allen 16 Patterns. Vier Werks-Presets sind dabei: leeres Kit 36–51,
  House (3 Patterns), Breakbeat und Techno 64 mit Polymetrie.

## Einrichtung auf der Force

1. Installieren wie die anderen Plugins: Release-ZIP aus GitHub Actions, `matrix.so` nach `/sdcard/vst/`,
   den Skin nach `/sdcard/Synths/` und die Zeile aus `pluginlist-entry.xml` eintragen.
2. **Matrix** als Insert-Effekt auf eine beliebige Spur legen. Die Drum-Spur selbst bietet sich an.
3. *Preferences → MIDI*: Beim neuen Eingang **Matrix MIDI Out** „Track" einschalten.
4. Auf der Drum-Spur als MIDI-Eingang **Matrix MIDI Out** wählen und die Spur scharf schalten
   (Monitor/Arm), damit sie die Noten spielt.
5. Die Noten der 16 Spuren sind ab Werk **36–51**, also Pad 1–16 eines Drum-Programms. Passt das nicht,
   gibt es zwei Wege: Auf dem iPad „General MIDI" wählen, oder eine Spur auf **Lernen** stellen und ein
   Pad anschlagen. Matrix übernimmt dann Note und Kanal.
6. Play drücken.

Diagnose: `/tmp/matrix_vst.log` schreibt alle 5 s eine Zeile mit Uhr, Noten und MIDI-Eingang.

## Auf dem iPad / Handy

Das iPad und die Force müssen im selben WLAN sein. Die Adresse steht in Matrix im Tab **SETUP** unter
„OPEN ON PHONE: HTTP://". Öffnest du sie, erscheint zuerst der **Hub** mit einer Kachel für jedes
Plugin, das gerade läuft: CC Map, Matrix und weitere. Eine Kachel antippen öffnet das Plugin, das
Haus-Symbol führt zurück zum Hub. Am besten den Hub als Lesezeichen oder „Zum Home-Bildschirm" sichern.

Mit dem iPad quer siehst du alle 16 Spuren × 16 Steps auf einmal. Auf dem Handy geht es auch,
quer ist dort aber besser.

- **Oben:** Die Patterns 1–16. Antippen spielt das Pattern (ab dem nächsten Takt) und zeigt es an.
  Mit „Nur bearbeiten" (unten) bearbeitest du ein Pattern, ohne es zu starten. Das spielende Pattern
  ist grün umrandet, ein wartendes blinkt orange, ein Punkt markiert belegte Patterns.
- **Seiten 1–4:** Steps 1–16, 17–32, 33–48 und 49–64. **Folgen** blättert mit dem Lauflicht mit.
- **Noten:** Antippen schaltet einen Step an oder aus, Wischen malt mehrere. Die Helligkeit zeigt die
  Velocity, ein Balken unten im Step die Chance, „2x"–„4x" den Ratchet. Hinter der Länge einer Spur
  stehen gedimmt die Steps, die sich dort wiederholen. Tippen bearbeitet den wiederholten Step.
- **Velocity / Chance / Ratchet:** Die Ansicht wie im Track-View des BLM. Das Gitter zeigt die gewählte
  Spur als Balken, je höher, desto mehr. Tippe links auf einen Spurnamen, um eine Spur zu wählen, und
  wische über die Balken.
- **M / S** neben dem Namen schalten Mute und Solo.
- **Name antippen** (Modus Noten) spielt die Note einmal an und öffnet die Spur. Dort stellst du Name,
  Note (±1, ±12), Kanal und Länge ein (Schnellwahl 8–64 oder für alle Spuren). Außerdem gibt es
  **Lernen**, **Schieben** (Rotation um einen Step), **Wiederholen bis 64** (füllt einen 16er-Beat für
  Variationen auf 64 Steps auf) und **Spur leeren**.
- **Unten:** Pattern benennen, kopieren und leeren, Swing, Gate und die Velocity neuer Steps,
  Notenbelegung (36–51 oder General MIDI), alle Mutes oder Solos aus, Presets laden und speichern
  (mit Namen).

Was du am iPad änderst, siehst du sofort auch auf der Force, und umgekehrt. Mehrere iPads oder
Handys können gleichzeitig verbunden sein.

## Auf der Force (Skin)

| Tab | Regler |
|---|---|
| MAIN | PATTERN, SWING, GATE, TAP VELOCITY; STATUS (Takt, Pattern, nächstes Pattern), die ersten 16 Steps der gewählten Spur als Text |
| TRACK | TRACK (Auswahl), NOTE, CHANNEL, LENGTH (im spielenden Pattern), MUTE, SOLO, LEARN NOTE, CLEAR TRACK |
| SETUP | CONTROL CH, PHONE VIEW, Adresse für das iPad |
| PRESET | PRESET, LOAD, SAVE (auch über die Preset-Liste der Force) |

Die Q-Links folgen der Reihenfolge der Regler.

## Steuern per MIDI (Control Ch, Voreinstellung 14)

Matrix hört auf alle angeschlossenen USB/DIN-Controller und auf MIDI, das an seine Spur geht, und
zwar nur auf dem **Control Ch**:

| Nachricht | Wirkung |
|---|---|
| CC 1–16 | Mute Spur 1–16 (Wert ≥ 64 = stumm) |
| CC 17–32 | Solo Spur 1–16 |
| CC 33 | Pattern (Wert 0–15) |
| CC 34 / 35 | Swing / Gate |
| CC 36 / 37 | nächstes / vorheriges Pattern (Wert ≥ 64) |
| Program Change 0–15 | Pattern 1–16 |

Mit **CC Map** kannst du die Launch Control dafür einrichten: Pads als TOGGLE auf CC 1–8 (Mute), Ziel
Kanal 14, LINK A auf CONTROL IN. CC Map spricht dann direkt mit dem „Control In" von Matrix.

## Mehrere Matrix-Instanzen

Jede Instanz hat ihren eigenen Sequencer, MIDI-Port und Port im Hub (8000–8009). Im Hub erscheinen
sie als „Matrix 2", „Matrix 3" usw.

## Dateien

| Datei | Inhalt |
|---|---|
| `vst/matrix_core.h` | Sequencer ohne VST und ALSA: Kit, Patterns, Steps, Wiedergabe, Zustand als Text, Werks-Patterns |
| `vst/matrix_vst.cpp` | VST2-Hülle (handgeschrieben), PpqClock (aus mpc-vst-drumgen), ALSA-Port und Control In, Presets, Projekt-Chunk |
| `vst/forcehub.h` | gemeinsamer Hub der Force-Plugins (die gleiche Datei wie in mpc-vst-ccmap) |
| `vst/matrix_web.h`, `vst/page/index.html` | das Gitter auf dem iPad; `gen_page.py` bettet die Seite als `matrix_page.h` ein |
| `vst/module.json`, `vst/vst.json`, `vst/layout.conf` | Parameter und Skin für `gen_vst.py` |
| `vst/captions.py`, `vst/fonts/` | Beschriftungen in Titillium Web (SIL OFL) |
| `vst/host_test.cpp`, `vst/test.sh` | Offline-Test: Host-Uhr pulsgenau, MIDI über Test-Hooks, Gitter über echtes HTTP |

Build: Actions → „VST release (draft)" → Run workflow (Werkzeuge: sd88me/mpc-vst-plugins, wie bei CC Map).

## Ideen für später

- Live-Aufnahme von den Pads der Force in das Gitter
- Flam, Nudge (Micro-Timing) und Länge pro Step
- Pattern-Ketten (Song-Modus)
- Mute- und Solo-LEDs auf der Launch Control

## Hinweis

Entwickelt mit Unterstützung von Claude (Anthropic). Angelehnt an das BLM 16×16 des MIDIbox SEQ
(Thorsten Klose, uCApps). „Akai" und „Force" gehören ihren Inhabern, es besteht keine Verbindung.
