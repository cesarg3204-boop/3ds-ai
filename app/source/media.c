#include "media.h"
#include <citro2d.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <math.h>

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

// ---------------------------------------------------------------- helpers

char *b64_encode(const unsigned char *d, size_t n)
{
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char *o = malloc((n + 2) / 3 * 4 + 1), *p = o;
	if (!o) return NULL;
	size_t i = 0;
	for (; i + 2 < n; i += 3) {
		u32 v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
		*p++ = t[v >> 18]; *p++ = t[(v >> 12) & 63]; *p++ = t[(v >> 6) & 63]; *p++ = t[v & 63];
	}
	if (i < n) {
		u32 v = d[i] << 16;
		if (i + 1 < n) v |= d[i + 1] << 8;
		*p++ = t[v >> 18];
		*p++ = t[(v >> 12) & 63];
		*p++ = i + 1 < n ? t[(v >> 6) & 63] : '=';
		*p++ = '=';
	}
	*p = 0;
	return o;
}

unsigned char *file_read(const char *path, size_t max, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0 || (size_t)n > max) { fclose(f); return NULL; }
	unsigned char *d = malloc(n + 1);
	if (!d) { fclose(f); return NULL; }
	n = fread(d, 1, n, f);
	d[n] = 0;
	fclose(f);
	if (len) *len = n;
	return d;
}

bool file_write(const char *path, const void *d, size_t n)
{
	FILE *f = fopen(path, "wb");
	if (!f) return false;
	bool ok = fwrite(d, 1, n, f) == n;
	fclose(f);
	return ok;
}

static void put32(unsigned char *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

unsigned char *png_encode(const unsigned char *rgb, int w, int h, size_t *len)
{
	size_t raw_n = (size_t)(w * 3 + 1) * h;
	unsigned char *raw = malloc(raw_n);
	if (!raw) return NULL;
	for (int y = 0; y < h; y++) {
		unsigned char *row = raw + (size_t)y * (w * 3 + 1);
		row[0] = 0;
		memcpy(row + 1, rgb + (size_t)y * w * 3, w * 3);
	}
	uLongf zn = compressBound(raw_n);
	unsigned char *out = malloc(8 + 25 + 12 + zn + 12);
	if (!out) { free(raw); return NULL; }
	static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	memcpy(out, sig, 8);
	unsigned char *p = out + 8;
	// IHDR
	put32(p, 13); memcpy(p + 4, "IHDR", 4);
	put32(p + 8, w); put32(p + 12, h);
	p[16] = 8; p[17] = 2; p[18] = 0; p[19] = 0; p[20] = 0;
	put32(p + 21, crc32(0, p + 4, 17));
	p += 25;
	// IDAT
	if (compress2(p + 8, &zn, raw, raw_n, 6) != Z_OK) { free(raw); free(out); return NULL; }
	free(raw);
	put32(p, zn); memcpy(p + 4, "IDAT", 4);
	put32(p + 8 + zn, crc32(0, p + 4, zn + 4));
	p += 12 + zn;
	// IEND
	put32(p, 0); memcpy(p + 4, "IEND", 4); put32(p + 8, crc32(0, p + 4, 4));
	p += 12;
	*len = p - out;
	return out;
}

static bool ends_with(const char *s, const char *e)
{
	size_t a = strlen(s), b = strlen(e);
	return a >= b && strcasecmp(s + a - b, e) == 0;
}

unsigned char *picture_for_upload(const char *path, const char **mime, size_t *len, char *err, size_t errn)
{
	size_t n = 0;
	unsigned char *d = file_read(path, 12 * 1024 * 1024, &n);
	if (!d) { snprintf(err, errn, "Couldn't read that picture (max 12 MB)."); return NULL; }
	bool png = n > 8 && d[0] == 137 && d[1] == 'P';
	bool jpg = n > 3 && d[0] == 0xFF && d[1] == 0xD8;
	if (!png && !jpg) { free(d); snprintf(err, errn, "That isn't a JPG or PNG picture."); return NULL; }
	if (n <= 1200 * 1024) { *mime = png ? "image/png" : "image/jpeg"; *len = n; return d; }

	int w, h, c;
	if (!stbi_info_from_memory(d, n, &w, &h, &c) || (long long)w * h > 4400000) {
		free(d);
		snprintf(err, errn, "That picture is too big for the 3DS to shrink. Try a smaller one.");
		return NULL;
	}
	unsigned char *px = stbi_load_from_memory(d, n, &w, &h, &c, 3);
	free(d);
	if (!px) { snprintf(err, errn, "Couldn't open that picture."); return NULL; }
	float s = 800.0f / (w > h ? w : h);
	if (s > 1) s = 1;
	int ow = (int)(w * s), oh = (int)(h * s);
	if (ow < 1) ow = 1;
	if (oh < 1) oh = 1;
	unsigned char *small = malloc((size_t)ow * oh * 3);
	if (!small) { stbi_image_free(px); snprintf(err, errn, "Out of memory."); return NULL; }
	for (int y = 0; y < oh; y++) {
		int sy = (int)(y / s);
		if (sy >= h) sy = h - 1;
		for (int x = 0; x < ow; x++) {
			int sx = (int)(x / s);
			if (sx >= w) sx = w - 1;
			memcpy(small + ((size_t)y * ow + x) * 3, px + ((size_t)sy * w + sx) * 3, 3);
		}
	}
	stbi_image_free(px);
	unsigned char *out = png_encode(small, ow, oh, len);
	free(small);
	if (!out) { snprintf(err, errn, "Couldn't shrink that picture."); return NULL; }
	*mime = "image/png";
	return out;
}

// ---------------------------------------------------------------- camera

#define CW 400
#define CH 240

static bool s_cam;
static u16 *s_frame;
static u32 s_xfer;
static C3D_Tex s_tex;
static Tex3DS_SubTexture s_sub;
static bool s_tex_ok;
static bool s_inner;

static inline u32 morton(int x, int y, int tw)
{
	return ((((y >> 3) * (tw >> 3) + (x >> 3)) << 6) +
		((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3)));
}

bool cam_on(void) { return s_cam; }

bool cam_start(bool inner)
{
	if (s_cam) cam_stop();
	if (R_FAILED(camInit())) return false;
	u32 sel = inner ? SELECT_IN1 : SELECT_OUT1;
	s_inner = inner;
	CAMU_SetSize(sel, SIZE_CTR_TOP_LCD, CONTEXT_A);
	CAMU_SetOutputFormat(sel, OUTPUT_RGB_565, CONTEXT_A);
	CAMU_SetFrameRate(sel, FRAME_RATE_30);
	CAMU_SetNoiseFilter(sel, true);
	CAMU_SetAutoExposure(sel, true);
	CAMU_SetAutoWhiteBalance(sel, true);
	CAMU_SetTrimming(PORT_CAM1, false);
	CAMU_GetMaxBytes(&s_xfer, CW, CH);
	CAMU_SetTransferBytes(PORT_CAM1, s_xfer, CW, CH);
	CAMU_Activate(sel);
	CAMU_ClearBuffer(PORT_CAM1);
	CAMU_StartCapture(PORT_CAM1);
	if (!s_frame) s_frame = linearAlloc(CW * CH * 2);
	if (!s_tex_ok) {
		s_tex_ok = C3D_TexInit(&s_tex, 512, 256, GPU_RGBA8);
		C3D_TexSetFilter(&s_tex, GPU_LINEAR, GPU_LINEAR);
		s_sub = (Tex3DS_SubTexture){ CW, CH, 0.0f, 1.0f, (float)CW / 512, 1.0f - (float)CH / 256 };
		if (s_tex_ok) memset(s_tex.data, 0, 512 * 256 * 4);
	}
	s_cam = true;
	return s_frame && s_tex_ok;
}

bool cam_frame(void)
{
	if (!s_cam) return false;
	Handle ev = 0;
	if (R_FAILED(CAMU_SetReceiving(&ev, s_frame, PORT_CAM1, CW * CH * 2, (s16)s_xfer))) return false;
	Result r = svcWaitSynchronization(ev, 200000000ULL);
	svcCloseHandle(ev);
	if (R_FAILED(r)) return false;
	u32 *t = s_tex.data;
	for (int y = 0; y < CH; y++)
		for (int x = 0; x < CW; x++) {
			u16 c = s_frame[y * CW + (s_inner ? CW - 1 - x : x)];
			u32 r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
			t[morton(x, y, 512)] = ((r5 * 255 / 31) << 24) | ((g6 * 255 / 63) << 16) | ((b5 * 255 / 31) << 8) | 0xff;
		}
	GSPGPU_FlushDataCache(s_tex.data, 512 * 256 * 4);
	return true;
}

void cam_draw(float x, float y)
{
	if (!s_tex_ok) return;
	C2D_Image img = { &s_tex, &s_sub };
	C2D_DrawImageAt(img, x, y, 0.5f, NULL, 1, 1);
}

bool cam_save_png(const char *path)
{
	if (!s_frame) return false;
	unsigned char *rgb = malloc(CW * CH * 3);
	if (!rgb) return false;
	for (int y = 0; y < CH; y++)
		for (int x = 0; x < CW; x++) {
			u16 c = s_frame[y * CW + (s_inner ? CW - 1 - x : x)];
			unsigned char *p = rgb + (y * CW + x) * 3;
			p[0] = ((c >> 11) & 31) * 255 / 31;
			p[1] = ((c >> 5) & 63) * 255 / 63;
			p[2] = (c & 31) * 255 / 31;
		}
	size_t n;
	unsigned char *png = png_encode(rgb, CW, CH, &n);
	free(rgb);
	if (!png) return false;
	bool ok = file_write(path, png, n);
	free(png);
	return ok;
}

void cam_stop(void)
{
	if (!s_cam) return;
	CAMU_StopCapture(PORT_CAM1);
	CAMU_Activate(SELECT_NONE);
	camExit();
	s_cam = false;
}

// ---------------------------------------------------------------- microphone

#define MICBUF_SIZE 0x30000
static u8 *s_micbuf;
static u32 s_micdata, s_micpos;
static bool s_mic;
static unsigned char *s_audio;   // 44-byte WAV header + PCM
static size_t s_alen, s_acap;

bool mic_on(void) { return s_mic; }

bool mic_start(void)
{
	if (s_mic) return true;
	s_acap = 44 + MIC_RATE * 2 * MIC_MAX_SECONDS;
	s_audio = malloc(s_acap);
	s_micbuf = memalign(0x1000, MICBUF_SIZE);
	if (!s_audio || !s_micbuf) { free(s_audio); free(s_micbuf); s_audio = NULL; s_micbuf = NULL; return false; }
	if (R_FAILED(micInit(s_micbuf, MICBUF_SIZE))) { free(s_audio); free(s_micbuf); s_audio = NULL; s_micbuf = NULL; return false; }
	s_micdata = micGetSampleDataSize();
	s_micpos = 0;
	s_alen = 44;
	if (R_FAILED(MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_16360, 0, s_micdata, true))) {
		micExit();
		free(s_audio); free(s_micbuf); s_audio = NULL; s_micbuf = NULL;
		return false;
	}
	s_mic = true;
	return true;
}

void mic_poll(void)
{
	if (!s_mic) return;
	u32 last = micGetLastSampleOffset();
	while (s_micpos != last && s_alen < s_acap) {
		s_audio[s_alen++] = s_micbuf[s_micpos];
		s_micpos = (s_micpos + 1) % s_micdata;
	}
}

float mic_seconds(void) { return s_mic ? (float)(s_alen - 44) / (MIC_RATE * 2) : 0; }

float mic_level(void)
{
	if (!s_mic || s_alen < 44 + 1600) return 0;
	const s16 *smp = (const s16 *)(s_audio + ((s_alen - 1600) & ~1u));
	float sum = 0;
	for (int i = 0; i < 800; i++) sum += (float)smp[i] * smp[i];
	float rms = sqrtf(sum / 800) / 6000.0f;
	return rms > 1 ? 1 : rms;
}

static void mic_end(void)
{
	MICU_StopSampling();
	micExit();
	free(s_micbuf);
	s_micbuf = NULL;
	s_mic = false;
}

void mic_cancel(void)
{
	if (!s_mic) return;
	mic_end();
	free(s_audio);
	s_audio = NULL;
}

static void put32le(unsigned char *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

unsigned char *mic_stop(size_t *len)
{
	if (!s_mic) return NULL;
	mic_poll();
	mic_end();
	unsigned char *w = s_audio;
	s_audio = NULL;
	u32 data = s_alen - 44;
	if (data < MIC_RATE) { free(w); return NULL; } // under half a second
	memcpy(w, "RIFF", 4); put32le(w + 4, 36 + data);
	memcpy(w + 8, "WAVEfmt ", 8); put32le(w + 16, 16);
	w[20] = 1; w[21] = 0; w[22] = 1; w[23] = 0;           // PCM, mono
	put32le(w + 24, MIC_RATE); put32le(w + 28, MIC_RATE * 2);
	w[32] = 2; w[33] = 0; w[34] = 16; w[35] = 0;          // block align, bits
	memcpy(w + 36, "data", 4); put32le(w + 40, data);
	*len = s_alen;
	return w;
}
