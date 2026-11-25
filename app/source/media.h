#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

// ---- helpers ----
char *b64_encode(const unsigned char *d, size_t n);           // malloc'd, NUL-terminated
unsigned char *file_read(const char *path, size_t max, size_t *len); // NULL if missing or > max
bool file_write(const char *path, const void *d, size_t n);

// Encodes 8-bit RGB pixels as a PNG (malloc'd).
unsigned char *png_encode(const unsigned char *rgb, int w, int h, size_t *len);

// Prepares a picture file for upload: small JPG/PNG files go as they are, big
// ones are decoded and shrunk to fit 800x800. Returns malloc'd bytes + mime.
unsigned char *picture_for_upload(const char *path, const char **mime, size_t *len, char *err, size_t errn);

// ---- camera (top screen size, 400x240) ----
bool cam_start(bool inner);
bool cam_frame(void);        // grabs a new frame into the preview; false if none
void cam_draw(float x, float y);
bool cam_save_png(const char *path);
void cam_stop(void);
bool cam_on(void);

// ---- microphone ----
#define MIC_RATE 16360
#define MIC_MAX_SECONDS 20
bool mic_start(void);
void mic_poll(void);
float mic_seconds(void);
float mic_level(void);       // 0..1, recent loudness
// Stops and returns the recording as a WAV file (malloc'd), or NULL.
unsigned char *mic_stop(size_t *len);
void mic_cancel(void);
bool mic_on(void);
