#!/usr/bin/env python3
"""
Generiert ein Hintergrundbild (Landschaft) und speichert es als 24-bit BMP
fuer den ESP32 mit TFT_eSPI Display (320x240)
"""

from PIL import Image, ImageDraw
import struct

# Display-Groesse
WIDTH = 240
HEIGHT = 320

# Farben in RGB
SKY_TOP = (40, 120, 255)
SKY_BOTTOM = (140, 210, 255)
YELLOW = (255, 255, 0)
WHITE = (255, 255, 255)
GREEN = (0, 128, 0)
DARK_GREEN = (34, 100, 0)

def rgb_to_rgb565(r, g, b):
    """Konvertiert RGB zu RGB565 (16-bit Format)"""
    r5 = (r >> 3) & 0x1F
    g6 = (g >> 2) & 0x3F
    b5 = (b >> 3) & 0x1F
    return (r5 << 11) | (g6 << 5) | b5

def create_landscape_image():
    """Erstellt ein Landschaftsbild"""
    # Neues Bild erstellen
    img = Image.new('RGB', (WIDTH, HEIGHT), 'white')
    pixels = img.load()
    
    # Himmel mit Farbverlauf zeichnen
    print("Zeichne Himmelsschicht...")
    for y in range(HEIGHT):
        # Interpolation zwischen SK_TOP und SKY_BOTTOM
        ratio = y / (HEIGHT - 1)
        r = int(SKY_TOP[0] + (SKY_BOTTOM[0] - SKY_TOP[0]) * ratio)
        g = int(SKY_TOP[1] + (SKY_BOTTOM[1] - SKY_TOP[1]) * ratio)
        b = int(SKY_TOP[2] + (SKY_BOTTOM[2] - SKY_TOP[2]) * ratio)
        
        for x in range(WIDTH):
            pixels[x, y] = (r, g, b)
    
    # Mit PIL zeichnen für komplexere Formen
    draw = ImageDraw.Draw(img)
    
    # Sonne
    print("Zeichne Sonne...")
    draw.ellipse([50-28, 50-28, 50+28, 50+28], fill=YELLOW)
    
    # Wolken (aus Kreisen zusammengesetzt)
    print("Zeichne Wolken...")
    def draw_cloud(x, y, scale=1):
        r1 = 12 * scale
        r2 = 16 * scale
        r3 = 10 * scale
        draw.ellipse([x-r1, y-r1, x+r1, y+r1], fill=WHITE)
        draw.ellipse([x+16*scale-r2, y-6*scale-r2, x+16*scale+r2, y-6*scale+r2], fill=WHITE)
        draw.ellipse([x+32*scale-r3, y-r3, x+32*scale+r3, y+r3], fill=WHITE)
    
    draw_cloud(120, 40, 1)
    draw_cloud(220, 70, 2)
    draw_cloud(40, 90, 1)
    
    # Hügel (Kreise)
    print("Zeichne Hügel...")
    draw.ellipse([40-120, HEIGHT+40-120, 40+120, HEIGHT+40+120], fill=GREEN)
    draw.ellipse([160-140, HEIGHT+60-140, 160+140, HEIGHT+60+140], fill=DARK_GREEN)
    draw.ellipse([280-120, HEIGHT+50-120, 280+120, HEIGHT+50+120], fill=GREEN)
    
    # Vordergrund (Hügel-Basis)
    print("Zeichne Vordergrund...")
    draw.rectangle([0, HEIGHT-40, WIDTH, HEIGHT], fill=DARK_GREEN)
    
    return img

def save_as_bmp(img, filename):
    """Speichert Bild als 24-bit BMP (unkomprimiert)."""
    print(f"Speichere als {filename}...")
    img_rgb = img.convert('RGB')
    img_rgb.save(filename, format='BMP')
    print(f"✓ Datei gespeichert: {filename}")


if __name__ == '__main__':
    print(f"Generiere Landschaftsbild ({WIDTH}x{HEIGHT})...")
    img = create_landscape_image()
    
    # Als PNG fuer Vorschau speichern
    img.save('landscape.png')
    print("✓ Vorschau gespeichert: landscape.png")

    # Als BMP speichern (24-bit, unkomprimiert)
    save_as_bmp(img, 'landscape.bmp')
