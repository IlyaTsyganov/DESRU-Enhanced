# DESRU-Enhanced

[English](README.md) · [Français](README.fr.md) · [Deutsch](README.de.md) · [Русский](README.ru.md)

Ein inoffizielles Windows-Hilfsprogramm für DOOM Eternal, basierend auf [bowsrs DESRU 1.6.17](https://github.com/bowsr/DESRU), gepflegt von IlyaTsyganov. Git-Verlauf und ursprüngliche Urheberangaben bleiben erhalten.

## Installation

1. Unter [Releases](https://github.com/IlyaTsyganov/DESRU-Enhanced/releases/latest) die Datei **DESRU-Enhanced-v1.1.0-standalone-win-x64.zip** aus Assets herunterladen. „Source code“ enthält nur den Quellcode.
2. Unter Windows 10/11 x64 andere DESRU-Instanzen schließen und das gesamte Archiv mit allen Unterordnern in einen beschreibbaren Ordner entpacken. Eine vorhandene Installation und Einstellungen vor dem Ersetzen sichern.
3. **DESRU.exe** starten, die Administratorabfrage bestätigen und bei Bedarf den Spielordner auswählen.
4. DESRU geöffnet lassen und DOOM Eternal normal starten. Die Startreihenfolge ist beliebig; ein separater Spielstarter ist nicht erforderlich.

Das eigenständige Paket enthält die .NET-Desktoplaufzeit 6.0.36, Abhängigkeiten, Schriftarten, Freescroll-Makro, meath00k und das native Grafikmodul. Weder eine bestehende DESRU-Installation noch eine separate .NET-Installation ist nötig. Spieldateien oder Level werden nicht mitgeliefert. Automatische Updates des ursprünglichen DESRU sind standardmäßig deaktiviert, damit dieser Fork erhalten bleibt. Neue Fork-Versionen manuell installieren.

## Änderungen gegenüber dem ursprünglichen DESRU

- Geschwindigkeitsanzeigen im nativen HUD, auch im exklusiven Vollbild: aktuelle horizontale Geschwindigkeit und Geschwindigkeit bei der letzten Landung einschließlich des kurzen Landeimpulses. Getrennte Positionen/Farben, gemeinsame Größe, optionale Landeanzeige.
- Einstellbare Deckkraft beider identifizierter Ebenen der Färbung bei niedriger Gesundheit, ohne Gesundheit oder Rüstung zu verändern.
- Cheats und meath00k während des Spiels ein-/ausschaltbar. Beim Ausschalten werden überwachte cvars auf ihre Werte vor der Sitzung zurückgesetzt und verwaltete Spieler-Cheats deaktiviert. Keine veraltete Neustartmeldung, sobald beides ausgeschaltet ist.
- Anzeige geänderter geschützter cvars sowie aktiver noclip-, notarget- und god-Modi. Beide Startreihenfolgen funktionieren; Ausblenden des Treibernamens berücksichtigt AMD-Zeichenketten.
- Firewall-Regel und F-Anzeige lassen sich anhand des tatsächlichen Regelzustands wieder korrekt einschalten.
- Lighting Fix: gespeicherte Checkbox in Options, Status im Info Panel und X-Anzeige. Während des Spiels umschaltbar, auch bei direktem Spielstart.
- Collision Viewer über die Konsole: Trigger in Cyan, spielerblockierende Level-Kollisionen in Grün, ein-/ausblendbare unsichtbare Wände in Orange. Keine Checkbox und kein V-Indikator; Befehle werden mit Cheats freigeschaltet.
- Ganze Objekte erscheinen, sobald ein Teil ihrer Grenzen den Radius erreicht. Alte feste Geometriegrenze entfernt. Eine Hintergrundberechnung entfernt verdeckte innere Flächen und erhält sichtbare Schnittkonturen, auch ohne Füllung. Bekannte Formen ohne Spielerkollision werden gefiltert.
- Kompaktere Anordnung von Options/Trainer; überarbeitete Hilfe für On-Screen Display und Info Panel; neue Seite Other mit allen DESRU-Befehlen, Mausrad und dunkler Scrollleiste.

Ursprüngliche Funktionen wie Makro, Hotkeys, Skalierung und Trainer bleiben verfügbar. Vor Speedruns die Kategorieregeln prüfen; dieser Fork bedeutet keine Zulassung für Bestenlisten.

## Konsolenbefehle

Jeder Befehl zeigt ohne Argumente seinen aktuellen Wert und die Hilfe an.

| Collision Viewer | Funktion |
| --- | --- |
| `desru_volumes_show 0/1` | Alle Volumen aus-/einblenden; zu Beginn jeder Spielsitzung 0. |
| `desru_volumes_fill 0/1` | Nur Konturen / mit Füllung; Standard 1. |
| `desru_triggers_show 0/1` | Trigger aus-/einblenden. |
| `desru_collision_show 0/1` | Level-Kollisionen aus-/einblenden. |
| `desru_invisible_walls_show 0/1` | Ein-/ausblendbare unsichtbare Wände anzeigen/verbergen. |
| `desru_volumes_render_distance <Meter>` | Darstellungsradius; Standard 40, etwa 73.5; endlich und nicht negativ. |

Cheats aktivieren, danach `desru_volumes_show 1` eingeben. Cheats ausschalten verbirgt den Viewer und sperrt seine Befehle. Füllung, Kategorien und Entfernung werden gespeichert. Schnittkonturen erscheinen nach Abschluss der Hintergrundoptimierung.

| Geschwindigkeitsanzeige / Färbung | Funktion |
| --- | --- |
| `desru_speedometer_show 0/1` | Hauptanzeige aus-/einblenden. |
| `desru_speedometer_showlanding 0/1` | Landeanzeige schalten; Hauptanzeige muss aktiv sein. |
| `desru_speedometer_x_pos <Pixel> [current\|landing]` | Abstand vom linken Rand. |
| `desru_speedometer_y_pos <Pixel> [current\|landing]` | Abstand vom oberen Rand. |
| `desru_speedometer_size <0.1–10>` | Textgröße beider Anzeigen. |
| `desru_speedometer_color <R> <G> <B> [current\|landing]` | RGB-Farbe, je Kanal 0–255. |
| `desru_lowhptintopacity <0–100>` | Deckkraft bei niedriger Gesundheit; Standard 100. |

Diese Befehle benötigen keine Cheats. Geschwindigkeit in Metern pro Sekunde mit zwei Nachkommastellen; `--` bedeutet noch keine aufgezeichnete Landung. Positionen verwenden physische Bildschirmpixel ab oben links. Ohne current/landing werden beide Positionen/Farben geändert. Beispiele: `desru_speedometer_x_pos 100 landing`, `desru_speedometer_color 255 255 255 current`. Einstellungen werden während der Ausführung gespeichert. Neue Standardwerte: beide Anzeigen weiß und aktiv, current (0, 480), landing (100, 480), Größe 3.5; vorhandene Einstellungen haben Vorrang.

## Kompatibilität und Build

Keine künstliche Versionssperre für den Viewer; die Engine-Strukturen müssen trotzdem erfolgreich ermittelt werden. HUD/Färbung wurden mit 1.1 und Steam 6.66 Rev 3.2 geprüft, Viewer/Lighting mit 1.0 Release. Nicht jede Funktion auf jedem Build/Treiber wurde getestet. Leichtes Flimmern einzelner Flächen kann bleiben; größere Entfernungen benötigen mehr Ressourcen.

Windows-Build mit Visual Studio MSBuild, .NET-Desktop-SDK und C++-Werkzeugen (CMake, Ninja, Windows SDK). Vulkan-Header liegen bei. Die Firewall-COM-Bibliothek erfordert Visual Studio MSBuild. `/p:SkipBundleBuildEvents=true` umgeht die alten Ereignisse, die den Ausgabeordner ersetzen. Befehle und Ressourcen: [Paketbau](docs/STANDALONE.md), [englisches README](README.md). Weitere Dokumentation: [Geschwindigkeit](docs/SPEEDOMETERS.md), [Grafikfunktionen](docs/VISUAL-FEATURES.md), [Cheats](docs/LIVE-CHEATS.md), [Firewall](docs/FIREWALL.md), [ursprüngliches README](docs/UPSTREAM-README.md).

## Urheber und Lizenz

Original-DESRU: **bowsr**; Fork: **IlyaTsyganov**. Integration aus [billymooreqs Projekt](https://github.com/billymooreq/DoomEternal-v1.1--RTXfix-DrlaCollisionViewer). Der Viewer basiert auf IlyaTsyganovs Arbeit mit Erlaubnis des Autors, nicht auf einer pauschalen MIT-Lizenz. meath00k: brongo und Mitwirkende. Siehe [CREDITS](CREDITS.md). DESRU bleibt unter GPL-3.0; Drittkomponenten behalten ihre jeweiligen Bedingungen. Inoffiziell, ohne Unterstützung durch id Software/Bethesda.
