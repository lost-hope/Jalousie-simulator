#include <Adafruit_ILI9341.h>
#include <Adafruit_GFX.h>
#include <SPI.h>
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
const int BLINDS_SPEED_PXPS = 20;            // Jalousie-Geschwindigkeit in Pixel/Sekunde
const int SLAT_SPEED_SPS = 1;               // Lamellen-Geschwindigkeit in Schritten/Sekunde
const int BLINDS_STEP_PER_FRAME = max(1, (int)(BLINDS_SPEED_PXPS * FRAME_INTERVAL_MS / 1000));
const int SLAT_STEP_PER_FRAME = max(1, (int)(SLAT_SPEED_SPS * FRAME_INTERVAL_MS / 1000));
unsigned long last_frame_time = 0;

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

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== Jalousie Simulator ===");
  
  pinMode(PIN_AB, INPUT_PULLUP);
  pinMode(PIN_AUF, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  ledcAttach(PIN_BUZZER, BUZZER_FREQUENCY, BUZZER_RESOLUTION);

  // TFT initialisieren - Hardware-SPI mit custom Pins über die GPIO-Matrix
  Serial.println("Initializing display...");
  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin(80000000);  // Max SPI-Speed für ESP32 (über GPIO-Matrix geroutet)
  tft.setRotation(2);

  // Hintergrundbild laden
  loadBackgroundImage();
}

void loop() {
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

  // Nicht-blockierende Taktung: Animationsgeschwindigkeit wird über
  // BLINDS_STEP_PER_FRAME / SLAT_STEP_PER_FRAME geregelt, nicht über delay().
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
      openSlats(SLAT_STEP_PER_FRAME);
    } else if (blinds_height > 0) {
      raiseBlinds(BLINDS_STEP_PER_FRAME);
    }
  }else if (!digitalRead(PIN_AB)) {
    if (slat_open > 0) {
      closeSlats(SLAT_STEP_PER_FRAME);
    } else if (blinds_height < blinds_max_height) {
      lowerBlinds(BLINDS_STEP_PER_FRAME);
    }
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
