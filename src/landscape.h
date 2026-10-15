#pragma once

#define LANDSCAPE_WIDTH 240
#define LANDSCAPE_HEIGHT 320
#define LANDSCAPE_BYTES_PER_PIXEL 2

typedef struct {
  unsigned int width;
  unsigned int height;
  unsigned int bytes_per_pixel;
  char *comment;
  unsigned char pixel_data[LANDSCAPE_WIDTH * LANDSCAPE_HEIGHT * LANDSCAPE_BYTES_PER_PIXEL + 1];
} gimp_image_t;

#ifdef __cplusplus
extern "C" {
#endif
extern const gimp_image_t gimp_image;
#ifdef __cplusplus
}
#endif
