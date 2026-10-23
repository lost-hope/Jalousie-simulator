#include <Adafruit_ILI9341.h>
#include <Adafruit_GFX.h>
#include <SPI.h>
#include <Preferences.h>
#include "landscape.h"
#include "Arduino.h"

// TFT Display Pins für ESP32
#define TFT_MOSI  27  // SDA
#define TFT_MISO  14  // MISO (optional)
#define TFT_SCLK  13  // SCK
#define TFT_CS   26   // CS
#define TFT_DC   25   // DC/RS
#define TFT_RST  33    // RST

// Hardware-SPI-Konstruktor (cs,dc,mosi,sclk,rst,miso würde Software-Bitbang-SPI
// erzwingen, siehe setup()). ESP32s GPIO-Matrix erlaubt Hardware-SPI auch auf
// diesen frei gewählten Pins, daher kein Rewiring nötig.
Adafruit_ILI9341 tft = Adafruit_ILI9341(&SPI, TFT_DC, TFT_CS, TFT_RST);

#define PIN_AB 32
#define PIN_AUF 4
#define PIN_BUZZER 21

const int BUZZER_FREQUENCY = 1000;
const int BUZZER_RESOLUTION = 8;
const int SLAT_HEIGHT = 20;  // Höhe einer Lamelle
const int TRANSPARENT_HEIGHT = 3;  // Breite des durchsichtigen Streifens geschlossen
const int MIN_GRAY_HEIGHT = 3;  // Minimale Höhe der grauen Balken
const int MAX_SLAT_OPEN = SLAT_HEIGHT - TRANSPARENT_HEIGHT - MIN_GRAY_HEIGHT - 1;  // Max Öffnungswinkel
const int DISPLAY_WIDTH = 240;
const int DISPLAY_HEIGHT = 320;
int blinds_height = 0;
int blinds_height_prev = 0;
int slat_open = 0;  // Öffnungswinkel (0-14)
int slat_open_prev = 0;
const int blinds_max_height = 320;
unsigned long last_debug_time = 0;
const int DEBUG_INTERVAL = 200;  // Debug-Ausgabe alle 200ms

// Animations-Geschwindigkeit: zeitbasiert statt über delay() im loop() gesteuert,
// damit die Geschwindigkeit nicht von der (vernachlässigbaren) Zeichendauer abhängt.
const unsigned long FRAME_INTERVAL_MS = 20;   // Taktung ~50 FPS

// Fahrzeiten (Sekunden) - über NVS konfigurierbar, siehe loadConfig()/handleConfigCommand().
// Defaults entsprechen den früheren Compile-Time-Konstanten (20px/s bzw. 1 Schritt/s).
const float DEFAULT_TRAVEL_SECONDS = blinds_max_height / 20.0f;
const float DEFAULT_SLAT_SECONDS = MAX_SLAT_OPEN / 1.0f;
const float TRAVEL_SECONDS_MIN = 1.0f;
const float TRAVEL_SECONDS_MAX = 120.0f;
const float SLAT_SECONDS_MIN = 0.5f;
const float SLAT_SECONDS_MAX = 60.0f;

// Display-Rotation - nur 0 oder 2 zulässig (180°-Flip für Überkopf-Montage),
// da Hintergrundbild und Jalousie-Höhenlogik fest auf das 240x320-Hochformat
// ausgelegt sind. 1/3 (Querformat) würden beides unbrauchbar machen.
const int DEFAULT_ROTATION = 2;
int display_rotation = DEFAULT_ROTATION;

float travel_seconds = DEFAULT_TRAVEL_SECONDS;
float slat_seconds = DEFAULT_SLAT_SECONDS;

// Sub-Pixel-Akkumulatoren: pro Frame wird ein ggf. gebrochener Betrag (< 1px/Schritt)
// aufaddiert, ein ganzzahliger Schritt wird erst ausgeführt, wenn genug angesammelt
// wurde. So bleiben auch sehr langsame Fahrzeiten (hohe Sekundenwerte) exakt abbildbar,
// statt auf eine Mindestgeschwindigkeit von 1px/Frame aufgerundet zu werden.
float blinds_px_per_frame = 0;
float slat_steps_per_frame = 0;
float blinds_accum = 0;
float slat_accum = 0;
unsigned long last_frame_time = 0;

Preferences prefs;

// Globaler Zeilenpuffer für drawBitmapRegion (nur 8 Zeilen statt 32 = 3.8KB statt 15KB)
uint16_t g_line_buffer[240 * 8];

void loadBackgroundImage();
void drawScreen();
void drawBitmapRegion(int16_t x, int16_t y, int16_t w, int16_t h, int16_t src_x, int16_t src_y);
void drawSlatRows(int y_start, int y_end, int transparent_height);
void drawSlatDelta(int blinds_height, int old_transparent, int new_transparent);
void lowerBlinds(int step = 2);
void raiseBlinds(int step = 2);
void openSlats(int step = 1);
void closeSlats(int step = 1);
void beepBuzzer(int frequency, int durationMs);
void alarmtone();
void loadConfig();
void applySpeedConfig();
void handleConfigCommand(String line);
int takeSteps(float &accumulator, float incrementPerFrame);

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== Jalousie Simulator ===");

  loadConfig();

  pinMode(PIN_AB, INPUT_PULLUP);
  pinMode(PIN_AUF, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  ledcAttach(PIN_BUZZER, BUZZER_FREQUENCY, BUZZER_RESOLUTION);

  // TFT initialisieren - Hardware-SPI mit custom Pins über die GPIO-Matrix
  Serial.println("Initializing display...");
  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin(80000000);  // Max SPI-Speed für ESP32 (über GPIO-Matrix geroutet)
  tft.setRotation(display_rotation);

  // Hintergrundbild laden
  loadBackgroundImage();
}

void loop() {
  // Konfigurations-Befehle über Serial (GET / SET TRAVEL=.. / SET SLAT=..),
  // z.B. von der Einstellungen-Seite des Web-Installers.
  while (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    handleConfigCommand(line);
  }

  // Debug-Ausgabe drosseln
  unsigned long current_time = millis();
  if (current_time - last_debug_time >= DEBUG_INTERVAL) {
    Serial.print("Blinds height: ");
    Serial.print(blinds_height);
    Serial.print(" | Slat open: ");
    Serial.print(slat_open);
    Serial.print(" | Buttons: Hoch[");
    Serial.print(digitalRead(PIN_AUF) ? "True" : "False");
    Serial.print("]| Runter[");
    Serial.print(digitalRead(PIN_AB) ? "True" : "False");
    Serial.println("]");
    last_debug_time = current_time;
  }

  // Nicht-blockierende Taktung: Animationsgeschwindigkeit wird über die
  // Sub-Pixel-Akkumulatoren (takeSteps()) geregelt, nicht über delay().
  if (current_time - last_frame_time < FRAME_INTERVAL_MS) {
    return;
  }
  last_frame_time = current_time;

  if (!digitalRead(PIN_AUF) && !digitalRead(PIN_AB)) {
    // Beide Tasten gedrückt - Fehlerfenster anzeigen
    Serial.println("Fehler: Beide Tasten gleichzeitig gedrückt!");
    tft.fillScreen(ILI9341_RED);
    tft.setCursor(20, 150);
    tft.setTextColor(ILI9341_WHITE);
    tft.setTextSize(3);
    tft.println("Kurzschluss!");
    //buzzer ansteuern
    alarmtone();
    //reset values to default
    blinds_height = 0;
    slat_open = 0;

    loadBackgroundImage();  // Hintergrund wiederherstellen
  }else if (!digitalRead(PIN_AUF)) {
    if (slat_open < MAX_SLAT_OPEN) {
      int steps = takeSteps(slat_accum, slat_steps_per_frame);
      if (steps > 0) openSlats(steps);
    } else if (blinds_height > 0) {
      int steps = takeSteps(blinds_accum, blinds_px_per_frame);
      if (steps > 0) raiseBlinds(steps);
    }
  }else if (!digitalRead(PIN_AB)) {
    if (slat_open > 0) {
      int steps = takeSteps(slat_accum, slat_steps_per_frame);
      if (steps > 0) closeSlats(steps);
    } else if (blinds_height < blinds_max_height) {
      int steps = takeSteps(blinds_accum, blinds_px_per_frame);
      if (steps > 0) lowerBlinds(steps);
    }
  } else {
    // Keine Taste gedrückt - Akkumulatoren zurücksetzen, damit der nächste
    // Tastendruck nicht von angesammeltem Rest aus einer anderen Bewegung springt.
    blinds_accum = 0;
    slat_accum = 0;
  }
}

void alarmtone()
{
  beepBuzzer(1000, 1000);
  delay(1000);
  beepBuzzer(1000, 1000);
  delay(1000);
  beepBuzzer(1000, 1000);
}

// Berechnet blinds_px_per_frame / slat_steps_per_frame (Fließkomma, kann < 1 sein)
// aus den konfigurierten Fahrzeiten (Sekunden) neu - muss nach jeder Änderung von
// travel_seconds/slat_seconds aufgerufen werden.
void applySpeedConfig() {
  float blinds_pxps = blinds_max_height / travel_seconds;
  float slat_sps = MAX_SLAT_OPEN / slat_seconds;
  blinds_px_per_frame = blinds_pxps * FRAME_INTERVAL_MS / 1000.0f;
  slat_steps_per_frame = slat_sps * FRAME_INTERVAL_MS / 1000.0f;
}

// Addiert 'incrementPerFrame' (kann < 1 sein) zum Akkumulator und gibt die Anzahl
// ganzer Schritte zurück, die diesen Frame ausgeführt werden sollen (0, wenn noch
// nicht genug angesammelt wurde). Der ausgeschüttete Anteil wird abgezogen, der Rest
// bleibt für die folgenden Frames erhalten.
int takeSteps(float &accumulator, float incrementPerFrame) {
  accumulator += incrementPerFrame;
  int steps = (int)accumulator;
  if (steps < 1) return 0;
  accumulator -= steps;
  return steps;
}

// Lädt die Fahrzeiten aus dem NVS (persistiert über SET-Befehle, siehe
// handleConfigCommand()). Fehlen Werte oder liegen sie außerhalb des gültigen
// Bereichs, werden die Default-Fahrzeiten verwendet.
void loadConfig() {
  prefs.begin("jalousie", true);
  travel_seconds = prefs.getFloat("travel", DEFAULT_TRAVEL_SECONDS);
  slat_seconds = prefs.getFloat("slat", DEFAULT_SLAT_SECONDS);
  display_rotation = prefs.getInt("rotation", DEFAULT_ROTATION);
  prefs.end();

  if (travel_seconds < TRAVEL_SECONDS_MIN || travel_seconds > TRAVEL_SECONDS_MAX) {
    travel_seconds = DEFAULT_TRAVEL_SECONDS;
  }
  if (slat_seconds < SLAT_SECONDS_MIN || slat_seconds > SLAT_SECONDS_MAX) {
    slat_seconds = DEFAULT_SLAT_SECONDS;
  }
  if (display_rotation != 0 && display_rotation != 2) {
    display_rotation = DEFAULT_ROTATION;
  }
  applySpeedConfig();
}

// Prüft, ob 'value' eine reine positive Dezimalzahl ist (Ziffern, höchstens ein Punkt).
bool isValidNumber(const String &value) {
  if (value.length() == 0) return false;
  bool seenDot = false;
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '.' && !seenDot) {
      seenDot = true;
    } else if (!isDigit(c)) {
      return false;
    }
  }
  return true;
}

// Serial-Konfigurationsprotokoll für die Einstellungen-Seite des Web-Installers:
//   GET               -> CFG:TRAVEL=<s>,SLAT=<s>,ROTATION=<0|2>
//   SET TRAVEL=<s>    -> CFG:OK TRAVEL=<s>    oder  CFG:ERR TRAVEL
//   SET SLAT=<s>      -> CFG:OK SLAT=<s>      oder  CFG:ERR SLAT
//   SET ROTATION=<r>  -> CFG:OK ROTATION=<r>  oder  CFG:ERR ROTATION
// Ungültige Eingaben (keine Zahl, außerhalb des gültigen Bereichs, bei ROTATION
// alles außer 0/2) werden abgelehnt und der bisherige Wert bleibt unverändert.
void handleConfigCommand(String line) {
  line.trim();
  if (line.length() == 0) return;

  if (line == "GET") {
    Serial.print("CFG:TRAVEL=");
    Serial.print(travel_seconds, 2);
    Serial.print(",SLAT=");
    Serial.print(slat_seconds, 2);
    Serial.print(",ROTATION=");
    Serial.println(display_rotation);
    return;
  }

  if (line.startsWith("SET ROTATION=")) {
    String valueStr = line.substring(line.indexOf('=') + 1);
    valueStr.trim();

    if ((valueStr != "0" && valueStr != "2")) {
      Serial.println("CFG:ERR ROTATION");
      return;
    }

    int newRotation = valueStr.toInt();
    if (newRotation != display_rotation) {
      display_rotation = newRotation;
      tft.setRotation(display_rotation);
      // Rotation ändert die Pixel-Adressierung - Anzeige muss komplett neu
      // aufgebaut werden, der bisherige Inhalt passt nicht mehr.
      blinds_height = 0;
      blinds_height_prev = 0;
      slat_open = 0;
      slat_open_prev = 0;
      loadBackgroundImage();

      prefs.begin("jalousie", false);
      prefs.putInt("rotation", display_rotation);
      prefs.end();
    }

    Serial.print("CFG:OK ROTATION=");
    Serial.println(display_rotation);
    return;
  }

  bool isTravel = line.startsWith("SET TRAVEL=");
  bool isSlat = line.startsWith("SET SLAT=");
  if (!isTravel && !isSlat) {
    Serial.println("CFG:ERR UNKNOWN_COMMAND");
    return;
  }

  String valueStr = line.substring(line.indexOf('=') + 1);
  valueStr.trim();

  float minV = isTravel ? TRAVEL_SECONDS_MIN : SLAT_SECONDS_MIN;
  float maxV = isTravel ? TRAVEL_SECONDS_MAX : SLAT_SECONDS_MAX;
  float value = isValidNumber(valueStr) ? valueStr.toFloat() : -1.0f;

  if (value < minV || value > maxV) {
    Serial.print("CFG:ERR ");
    Serial.println(isTravel ? "TRAVEL" : "SLAT");
    return;
  }

  if (isTravel) {
    travel_seconds = value;
  } else {
    slat_seconds = value;
  }
  applySpeedConfig();

  prefs.begin("jalousie", false);
  prefs.putFloat(isTravel ? "travel" : "slat", value);
  prefs.end();

  Serial.print("CFG:OK ");
  Serial.print(isTravel ? "TRAVEL=" : "SLAT=");
  Serial.println(value, 2);
}
void drawScreen() {
  int transparent_height = TRANSPARENT_HEIGHT + slat_open;

  // Lamellen öffnen/schließen - nur das Delta-Band pro Lamelle neu zeichnen
  // (nicht die komplette Höhe, sonst sichtbares Tearing/"Wischen" bei langen Jalousien)
  if (slat_open != slat_open_prev) {
    int transparent_height_prev = TRANSPARENT_HEIGHT + slat_open_prev;
    drawSlatDelta(blinds_height, transparent_height_prev, transparent_height);
  }
  // Jalousie runter/hochfahren
  else if (blinds_height != blinds_height_prev) {
    if (blinds_height > blinds_height_prev) {
      // Jalousie fährt runter - nur neue Zeilen zeichnen
      drawSlatRows(blinds_height_prev, blinds_height, transparent_height);
    } else {
      // Jalousie fährt hoch - Hintergrundbild wiederherstellen
      int block_height = blinds_height_prev - blinds_height;
      if (block_height > 0) {
        drawBitmapRegion(0, blinds_height, DISPLAY_WIDTH, block_height, 0, blinds_height);
      }
    }
  }
  blinds_height_prev = blinds_height;
  slat_open_prev = slat_open;
}

// Zeichnet die Lamellen-Zeilen im Bereich [y_start, y_end) als zusammenhängende
// Blöcke statt zeilenweise - jeder durchsichtige/graue Lauf innerhalb einer
// Lamelle wird in einem einzigen SPI-Transfer gezeichnet statt in SLAT_HEIGHT
// einzelnen Aufrufen (deutlich weniger Transaktions-Overhead).
void drawSlatRows(int y_start, int y_end, int transparent_height) {
  int y = y_start;
  while (y < y_end) {
    int line_in_slat = y % SLAT_HEIGHT;
    if (line_in_slat < transparent_height) {
      int run = min(transparent_height - line_in_slat, y_end - y);
      drawBitmapRegion(0, y, DISPLAY_WIDTH, run, 0, y);
      y += run;
    } else {
      int run = min(SLAT_HEIGHT - line_in_slat, y_end - y);
      tft.fillRect(0, y, DISPLAY_WIDTH, run, ILI9341_DARKGREY);
      y += run;
    }
  }
}

// Zeichnet beim Ändern des Lamellenwinkels nur das Delta-Band pro Lamelle neu:
// nur die Zeilen, die zwischen altem und neuem Öffnungswinkel tatsächlich von
// grau auf Hintergrund wechseln (oder umgekehrt) - nicht die komplette Höhe.
void drawSlatDelta(int blinds_height, int old_transparent, int new_transparent) {
  int lo = min(old_transparent, new_transparent);
  int hi = max(old_transparent, new_transparent);
  bool opening = new_transparent > old_transparent;

  for (int period_start = 0; period_start < blinds_height; period_start += SLAT_HEIGHT) {
    int row_lo = period_start + lo;
    if (row_lo >= blinds_height) break;
    int row_hi = min(period_start + hi, blinds_height);
    int run = row_hi - row_lo;
    if (run <= 0) continue;

    if (opening) {
      drawBitmapRegion(0, row_lo, DISPLAY_WIDTH, run, 0, row_lo);
    } else {
      tft.fillRect(0, row_lo, DISPLAY_WIDTH, run, ILI9341_DARKGREY);
    }
  }
}

void loadBackgroundImage() {
  Serial.println("\n=== Loading Background ===");
  Serial.print("Image size: ");
  Serial.print(gimp_image.width);
  Serial.print(" x ");
  Serial.println(gimp_image.height);

  unsigned long t0 = micros();
  tft.drawRGBBitmap(0, 0, (const uint16_t *)gimp_image.pixel_data,
                    gimp_image.width, gimp_image.height);
  unsigned long elapsed_us = micros() - t0;
  Serial.print("✓ Background loaded in ");
  Serial.print(elapsed_us / 1000.0);
  Serial.println(" ms");
}

// Zeichnet einen Teilbereich aus der Bitmap - optimiert mit globalem Zeilenpuffer
// (x, y) = Zielposition auf dem Display
// (w, h) = Breite und Höhe des Ausschnitts
// (src_x, src_y) = Startposition in der Quell-Bitmap
void drawBitmapRegion(int16_t x, int16_t y, int16_t w, int16_t h, int16_t src_x, int16_t src_y) {
  const uint16_t* pixel_data = (const uint16_t *)gimp_image.pixel_data;
  
  // Begrenzungen prüfen
  if (src_x + w > gimp_image.width) w = gimp_image.width - src_x;
  if (src_y + h > gimp_image.height) h = gimp_image.height - src_y;
  if (h <= 0 || w <= 0) return;
  
  // Mehrere Zeilen gleichzeitig zeichnen (max 8 Zeilen als Block)
  const int LINE_BUFFER = 8;
  
  for (int16_t row = 0; row < h; row += LINE_BUFFER) {
    int16_t lines_to_draw = min((int16_t)LINE_BUFFER, (int16_t)(h - row));
    int buffer_idx = 0;
    
    // Puffer mit Zeilen füllen
    for (int16_t line = 0; line < lines_to_draw; line++) {
      int src_offset = (src_y + row + line) * gimp_image.width + src_x;
      for (int16_t col = 0; col < w; col++) {
        g_line_buffer[buffer_idx++] = pixel_data[src_offset + col];
      }
    }
    
    // Gesamten Puffer auf einmal zeichnen
    tft.drawRGBBitmap(x, y + row, g_line_buffer, w, lines_to_draw);
  }
}

// Jalousie nach unten fahren (um step Pixel)
void lowerBlinds(int step) {
  int new_height = min(blinds_height + step, blinds_max_height);
  if (new_height != blinds_height) {
    blinds_height = new_height;
    if (slat_open != 0) slat_open = 0;  // Lamellen geschlossen
    drawScreen();
  }
}

// Jalousie nach oben fahren (um step Pixel)
void raiseBlinds(int step) {
  int new_height = max(blinds_height - step, 0);
  if (new_height != blinds_height) {
    blinds_height = new_height;
    if (slat_open != MAX_SLAT_OPEN) slat_open = MAX_SLAT_OPEN;
    drawScreen();
  }
}

// Lamellen öffnen (um step Schritte)
void openSlats(int step) {
  int new_open = min(slat_open + step, MAX_SLAT_OPEN);
  if (new_open != slat_open) {
    slat_open = new_open;
    drawScreen();
  }
}

// Lamellen schließen (um step Schritte)
void closeSlats(int step) {
  int new_open = max(slat_open - step, 0);
  if (new_open != slat_open) {
    slat_open = new_open;
    drawScreen();
  }
}

void beepBuzzer(int frequency, int durationMs) {
  ledcWriteTone(PIN_BUZZER, frequency);
  delay(durationMs);
  ledcWriteTone(PIN_BUZZER, 0);
}
