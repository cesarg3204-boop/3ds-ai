#pragma once
#include <citro2d.h>
#include <stdbool.h>

typedef struct {
	u32 bg, card, card2, text, sub, faint, accent, bar, line, shade;
} Theme;

extern Theme T;
extern const u32 CH_COLORS[8];
extern const char *CH_COLOR_NAMES[8];
extern const char *ICON_NAMES[10];

void gfx_init(void);
void gfx_exit(void);
void gfx_set_theme(bool dark);
void gfx_frame_start(void);  // call after C3D_FrameBegin

// ---- text (system font) ----
float text_width(const char *s, float size);
float text_line_h(float size);
void text_draw(const char *s, float x, float y, float size, u32 color);
// Draws s on one line, cut with "…" so it fits maxw.
void text_draw_fit(const char *s, float x, float y, float size, u32 color, float maxw);
void text_draw_center(const char *s, float cx, float y, float size, u32 color);
// Word wraps s to maxw. Returns line count; *out is a malloc'd array of malloc'd lines.
int text_wrap(const char *s, float size, float maxw, char ***out);
void lines_free(char **lines, int n);
// Draws up to maxlines wrapped lines; returns lines drawn.
int text_draw_wrapped(const char *s, float x, float y, float size, u32 color, float maxw, int maxlines);

// ---- shapes ----
void rect(float x, float y, float w, float h, u32 c);
void rrect(float x, float y, float w, float h, float r, u32 c);
void vgrad(float x, float y, float w, float h, u32 top, u32 bottom);
void tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 c);
void line(float x0, float y0, float x1, float y1, float t, u32 c);
void arc(float cx, float cy, float r, float a0, float a1, float t, u32 c);
void fan(float cx, float cy, float rx, float ry, float a0, float a1, u32 c);
void icon_draw(int icon, float cx, float cy, float size, u32 c);
void glyph_plus(float cx, float cy, float s, u32 c);
void glyph_gear(float cx, float cy, float s, u32 c, u32 bg);
void glyph_back(float cx, float cy, float s, u32 c);
void glyph_refresh(float cx, float cy, float s, u32 c, u32 bg);
void spinner(float cx, float cy, float r, u32 c);

// ---- pictures (textures from decoded images) ----
typedef struct {
	C3D_Tex tex;
	Tex3DS_SubTexture sub;
	C2D_Image img;
	u32 key;
	bool used;
	u64 last;
} Picture;

Picture *pic_find(u32 key);
// Takes ownership of nothing; copies pixels into a new texture, evicting the oldest if full.
Picture *pic_add(u32 key, const u32 *pixels, int tw, int th, int iw, int ih);
void pic_draw(Picture *p, float x, float y, float w, float h);
void pic_clear(void);

static inline u32 rgba(u8 r, u8 g, u8 b, u8 a) { return C2D_Color32(r, g, b, a); }
