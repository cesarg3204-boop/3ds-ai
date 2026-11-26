#include "gfx.h"
#include <3ds.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

Theme T;

const u32 CH_COLORS[8] = {
	0xff3a3ae5, // red     (ABGR)
	0xff2f8af2, // orange
	0xff2ec4f2, // yellow
	0xff5cb84c, // green
	0xffa8a826, // teal
	0xffe0862e, // blue
	0xffc8549b, // purple
	0xff9a5ce8, // pink
};
const char *CH_COLOR_NAMES[8] = { "Red", "Orange", "Yellow", "Green", "Teal", "Blue", "Purple", "Pink" };
const char *ICON_NAMES[10] = { "Book", "Controller", "Snack bar", "Bowl", "Brick", "Star", "Globe", "Heart", "Music", "Tag" };

static C2D_TextBuf s_buf;
static CFNT_s *s_font;
static float s_cw[128];  // cached widths (scale 1) for ASCII

#define MAX_PICS 40
static Picture s_pics[MAX_PICS];
static C3D_Tex s_dead[MAX_PICS * 2];
static int s_ndead;
static size_t s_picbytes;
#define PIC_BUDGET (8 * 1024 * 1024)

void gfx_set_theme(bool dark)
{
	if (dark) {
		T.bg = rgba(17, 17, 17, 255);
		T.card = rgba(38, 38, 38, 255);
		T.card2 = rgba(54, 54, 56, 255);
		T.text = rgba(245, 245, 245, 255);
		T.sub = rgba(168, 168, 172, 255);
		T.faint = rgba(110, 110, 116, 255);
		T.accent = rgba(74, 144, 255, 255);
		T.bar = rgba(26, 26, 27, 255);
		T.line = rgba(48, 48, 50, 255);
		T.shade = rgba(0, 0, 0, 120);
	} else {
		T.bg = rgba(242, 243, 246, 255);
		T.card = rgba(255, 255, 255, 255);
		T.card2 = rgba(228, 230, 235, 255);
		T.text = rgba(22, 24, 29, 255);
		T.sub = rgba(92, 97, 108, 255);
		T.faint = rgba(150, 155, 166, 255);
		T.accent = rgba(230, 64, 48, 255);
		T.bar = rgba(255, 255, 255, 255);
		T.line = rgba(218, 220, 226, 255);
		T.shade = rgba(0, 0, 0, 50);
	}
}

static float char_w(u32 cp)
{
	if (cp < 128 && s_cw[cp] >= 0) return s_cw[cp];
	int idx = fontGlyphIndexFromCodePoint(s_font, cp);
	charWidthInfo_s *wi = fontGetCharWidthInfo(s_font, idx);
	float w = wi ? wi->charWidth : 12;
	if (cp < 128) s_cw[cp] = w;
	return w;
}

Result gfx_init(void)
{
	s_buf = C2D_TextBufNew(12000);
	// The system font is shared with the HOME Menu. Right after launch it can
	// briefly be unavailable, so retry for up to ~3 seconds before giving up.
	Result rc = fontEnsureMapped();
	for (int i = 0; i < 30 && R_FAILED(rc); i++) {
		svcSleepThread(100000000LL);
		rc = fontEnsureMapped();
	}
	s_font = fontGetSystemFont();
	for (int i = 0; i < 128; i++) s_cw[i] = -1;
	gfx_set_theme(true);
	if (R_SUCCEEDED(rc) && !s_font) rc = -1;
	if (R_SUCCEEDED(rc) && !s_buf) rc = -2;
	return rc;
}

void gfx_exit(void)
{
	pic_clear();
	for (int i = 0; i < s_ndead; i++) C3D_TexDelete(&s_dead[i]);
	s_ndead = 0;
	C2D_TextBufDelete(s_buf);
}

void gfx_frame_start(void)
{
	C2D_TextBufClear(s_buf);
	// The previous frame has finished drawing (FrameBegin syncs), so evicted
	// textures can be freed now.
	for (int i = 0; i < s_ndead; i++) C3D_TexDelete(&s_dead[i]);
	s_ndead = 0;
}

// ---------- text ----------

static int next_cp(const char *s, u32 *cp)
{
	ssize_t n = decode_utf8(cp, (const uint8_t *)s);
	if (n <= 0) { *cp = '?'; return 1; }
	return (int)n;
}

float text_width(const char *s, float size)
{
	float w = 0;
	while (*s) {
		u32 cp;
		s += next_cp(s, &cp);
		w += char_w(cp);
	}
	return w * size;
}

float text_line_h(float size)
{
	return fontGetInfo(s_font)->lineFeed * size;
}

void text_draw(const char *s, float x, float y, float size, u32 color)
{
	if (!s || !*s) return;
	C2D_Text t;
	const char *end = C2D_TextParse(&t, s_buf, s);
	(void)end;
	C2D_TextOptimize(&t);
	C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, size, size, color);
}

void text_draw_center(const char *s, float cx, float y, float size, u32 color)
{
	text_draw(s, cx - text_width(s, size) / 2, y, size, color);
}

void text_draw_fit(const char *s, float x, float y, float size, u32 color, float maxw)
{
	if (!s) return;
	if (text_width(s, size) <= maxw) { text_draw(s, x, y, size, color); return; }
	float ell = text_width("…", size);
	char tmp[512];
	float w = 0;
	size_t i = 0;
	const char *p = s;
	while (*p && i < sizeof tmp - 8) {
		u32 cp;
		int n = next_cp(p, &cp);
		float cw = char_w(cp) * size;
		if (w + cw + ell > maxw) break;
		memcpy(tmp + i, p, n);
		i += n; p += n; w += cw;
	}
	while (i > 0 && tmp[i - 1] == ' ') i--;
	memcpy(tmp + i, "…", 4); // includes NUL
	text_draw(tmp, x, y, size, color);
}

int text_wrap(const char *s, float size, float maxw, char ***out)
{
	int cap = 8, n = 0;
	char **lines = malloc(cap * sizeof(char *));
	const char *p = s ? s : "";
	while (*p) {
		const char *start = p, *q = p, *brk = NULL, *end, *next;
		float w = 0;
		for (;;) {
			if (!*q || *q == '\n') { end = q; next = *q ? q + 1 : q; break; }
			u32 cp;
			int len = next_cp(q, &cp);
			float cw = char_w(cp) * size;
			if (cp == ' ') brk = q;
			if (w + cw > maxw && q > start) {
				if (brk && brk > start) { end = brk; next = brk + 1; }
				else { end = q; next = q; }
				break;
			}
			w += cw;
			q += len;
		}
		if (n == cap) { cap *= 2; lines = realloc(lines, cap * sizeof(char *)); }
		size_t len = end - start;
		char *l = malloc(len + 1);
		memcpy(l, start, len);
		l[len] = 0;
		lines[n++] = l;
		p = next;
		while (*p == ' ') p++;
	}
	*out = lines;
	return n;
}

void lines_free(char **lines, int n)
{
	for (int i = 0; i < n; i++) free(lines[i]);
	free(lines);
}

int text_draw_wrapped(const char *s, float x, float y, float size, u32 color, float maxw, int maxlines)
{
	char **lines;
	int n = text_wrap(s, size, maxw, &lines);
	float lh = text_line_h(size);
	int drawn = 0;
	for (int i = 0; i < n && drawn < maxlines; i++, drawn++) {
		if (drawn == maxlines - 1 && i < n - 1) {
			// last visible line: join the rest and cut with an ellipsis
			char tmp[600];
			snprintf(tmp, sizeof tmp, "%s %s", lines[i], lines[i + 1]);
			text_draw_fit(tmp, x, y + drawn * lh, size, color, maxw);
		} else text_draw(lines[i], x, y + drawn * lh, size, color);
	}
	lines_free(lines, n);
	return drawn;
}

// ---------- shapes ----------

void rect(float x, float y, float w, float h, u32 c)
{
	C2D_DrawRectSolid(x, y, 0.5f, w, h, c);
}

void rrect(float x, float y, float w, float h, float r, u32 c)
{
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	if (r < 1) { rect(x, y, w, h, c); return; }
	rect(x + r, y, w - 2 * r, h, c);
	rect(x, y + r, r, h - 2 * r, c);
	rect(x + w - r, y + r, r, h - 2 * r, c);
	C2D_DrawCircleSolid(x + r, y + r, 0.5f, r, c);
	C2D_DrawCircleSolid(x + w - r, y + r, 0.5f, r, c);
	C2D_DrawCircleSolid(x + r, y + h - r, 0.5f, r, c);
	C2D_DrawCircleSolid(x + w - r, y + h - r, 0.5f, r, c);
}

void vgrad(float x, float y, float w, float h, u32 top, u32 bottom)
{
	C2D_DrawRectangle(x, y, 0.5f, w, h, top, top, bottom, bottom);
}

void tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 c)
{
	C2D_DrawTriangle(x0, y0, c, x1, y1, c, x2, y2, c, 0.5f);
}

void line(float x0, float y0, float x1, float y1, float t, u32 c)
{
	C2D_DrawLine(x0, y0, c, x1, y1, c, t, 0.5f);
}

void arc(float cx, float cy, float r, float a0, float a1, float t, u32 c)
{
	int seg = 14;
	float px = cx + cosf(a0) * r, py = cy + sinf(a0) * r;
	for (int i = 1; i <= seg; i++) {
		float a = a0 + (a1 - a0) * i / seg;
		float nx = cx + cosf(a) * r, ny = cy + sinf(a) * r;
		line(px, py, nx, ny, t, c);
		px = nx; py = ny;
	}
}

void fan(float cx, float cy, float rx, float ry, float a0, float a1, u32 c)
{
	int seg = 16;
	float px = cx + cosf(a0) * rx, py = cy + sinf(a0) * ry;
	for (int i = 1; i <= seg; i++) {
		float a = a0 + (a1 - a0) * i / seg;
		float nx = cx + cosf(a) * rx, ny = cy + sinf(a) * ry;
		tri(cx, cy, px, py, nx, ny, c);
		px = nx; py = ny;
	}
}

void icon_draw(int icon, float cx, float cy, float s, u32 c)
{
	const u32 d = rgba(0, 0, 0, 80);
	float h = s / 2;
	switch (icon) {
	case 0: // book
		rrect(cx - h, cy - h * 0.7f, h * 0.94f, h * 1.4f, s * 0.06f, c);
		rrect(cx + h * 0.06f, cy - h * 0.7f, h * 0.94f, h * 1.4f, s * 0.06f, c);
		for (int i = 0; i < 3; i++) {
			float yy = cy - h * 0.35f + i * h * 0.3f;
			rect(cx - h * 0.8f, yy, h * 0.6f, s * 0.05f, d);
			rect(cx + h * 0.25f, yy, h * 0.6f, s * 0.05f, d);
		}
		break;
	case 1: // controller
		C2D_DrawCircleSolid(cx - h * 0.62f, cy + h * 0.1f, 0.5f, h * 0.38f, c);
		C2D_DrawCircleSolid(cx + h * 0.62f, cy + h * 0.1f, 0.5f, h * 0.38f, c);
		rrect(cx - h * 0.75f, cy - h * 0.4f, h * 1.5f, h * 0.8f, h * 0.25f, c);
		rect(cx - h * 0.72f, cy - h * 0.06f, h * 0.36f, h * 0.12f, d);
		rect(cx - h * 0.6f, cy - h * 0.18f, h * 0.12f, h * 0.36f, d);
		C2D_DrawCircleSolid(cx + h * 0.42f, cy + h * 0.02f, 0.5f, h * 0.09f, d);
		C2D_DrawCircleSolid(cx + h * 0.62f, cy - h * 0.15f, 0.5f, h * 0.09f, d);
		break;
	case 2: // snack bar
		rrect(cx - h * 0.7f, cy - h * 0.32f, h * 1.4f, h * 0.64f, s * 0.05f, c);
		tri(cx - h * 0.7f, cy - h * 0.32f, cx - h, cy - h * 0.42f, cx - h * 0.7f, cy, c);
		tri(cx - h * 0.7f, cy + h * 0.32f, cx - h, cy + h * 0.42f, cx - h * 0.7f, cy, c);
		tri(cx + h * 0.7f, cy - h * 0.32f, cx + h, cy - h * 0.42f, cx + h * 0.7f, cy, c);
		tri(cx + h * 0.7f, cy + h * 0.32f, cx + h, cy + h * 0.42f, cx + h * 0.7f, cy, c);
		rect(cx - h * 0.3f, cy - h * 0.32f, h * 0.6f, h * 0.64f, d);
		break;
	case 3: // bowl
		rect(cx - h, cy - h * 0.12f, s, s * 0.08f, c);
		fan(cx, cy - h * 0.05f, h * 0.9f, h * 0.75f, 0, (float)M_PI, c);
		arc(cx - h * 0.3f, cy - h * 0.55f, h * 0.18f, (float)M_PI * 0.5f, (float)M_PI * 1.5f, s * 0.05f, c);
		arc(cx + h * 0.2f, cy - h * 0.6f, h * 0.18f, (float)M_PI * 0.5f, (float)M_PI * 1.5f, s * 0.05f, c);
		break;
	case 4: // brick
		rrect(cx - h, cy - h * 0.35f, s, h * 1.0f, s * 0.05f, c);
		rrect(cx - h * 0.72f, cy - h * 0.62f, h * 0.5f, h * 0.32f, s * 0.04f, c);
		rrect(cx + h * 0.22f, cy - h * 0.62f, h * 0.5f, h * 0.32f, s * 0.04f, c);
		rect(cx - h, cy - h * 0.35f, s, s * 0.04f, d);
		break;
	case 5: { // star
		float pts[10][2];
		for (int i = 0; i < 10; i++) {
			float r = (i & 1) ? h * 0.42f : h;
			float a = -(float)M_PI / 2 + i * (float)M_PI / 5;
			pts[i][0] = cx + cosf(a) * r;
			pts[i][1] = cy + sinf(a) * r * 0.98f + h * 0.08f;
		}
		for (int i = 0; i < 10; i++) {
			int j = (i + 1) % 10;
			tri(cx, cy + h * 0.08f, pts[i][0], pts[i][1], pts[j][0], pts[j][1], c);
		}
		break;
	}
	case 6: // globe
		C2D_DrawCircleSolid(cx, cy, 0.5f, h * 0.9f, c);
		arc(cx, cy, h * 0.9f, 0, (float)M_PI * 2, s * 0.03f, d);
		line(cx - h * 0.9f, cy, cx + h * 0.9f, cy, s * 0.05f, d);
		line(cx - h * 0.78f, cy - h * 0.45f, cx + h * 0.78f, cy - h * 0.45f, s * 0.04f, d);
		line(cx - h * 0.78f, cy + h * 0.45f, cx + h * 0.78f, cy + h * 0.45f, s * 0.04f, d);
		for (int i = 0; i < 16; i++) {
			float a0 = (float)M_PI * 2 * i / 16, a1 = (float)M_PI * 2 * (i + 1) / 16;
			line(cx + cosf(a0) * h * 0.4f, cy + sinf(a0) * h * 0.9f, cx + cosf(a1) * h * 0.4f, cy + sinf(a1) * h * 0.9f, s * 0.05f, d);
		}
		break;
	case 7: // heart
		C2D_DrawCircleSolid(cx - h * 0.42f, cy - h * 0.25f, 0.5f, h * 0.46f, c);
		C2D_DrawCircleSolid(cx + h * 0.42f, cy - h * 0.25f, 0.5f, h * 0.46f, c);
		tri(cx - h * 0.86f, cy - h * 0.08f, cx + h * 0.86f, cy - h * 0.08f, cx, cy + h * 0.85f, c);
		break;
	case 8: // music
		C2D_DrawEllipseSolid(cx - h * 0.75f, cy + h * 0.38f, 0.5f, h * 0.6f, h * 0.45f, c);
		rect(cx - h * 0.25f, cy - h * 0.8f, s * 0.08f, h * 1.42f, c);
		tri(cx - h * 0.25f, cy - h * 0.8f, cx + h * 0.55f, cy - h * 0.4f, cx - h * 0.25f, cy - h * 0.3f, c);
		break;
	case 9: // price tag
		tri(cx - h * 0.95f, cy, cx - h * 0.4f, cy - h * 0.6f, cx - h * 0.4f, cy + h * 0.6f, c);
		rect(cx - h * 0.4f, cy - h * 0.6f, h * 1.3f, h * 1.2f, c);
		C2D_DrawCircleSolid(cx - h * 0.4f, cy, 0.5f, h * 0.16f, d);
		rect(cx - h * 0.1f, cy - h * 0.12f, h * 0.8f, h * 0.24f, d);
		break;
	case 10: // search
		arc(cx - h * 0.18f, cy - h * 0.18f, h * 0.52f, 0, (float)M_PI * 2, s * 0.13f, c);
		line(cx + h * 0.2f, cy + h * 0.2f, cx + h * 0.8f, cy + h * 0.8f, s * 0.16f, c);
		break;
	case 11: // clock
		C2D_DrawCircleSolid(cx, cy, 0.5f, h * 0.9f, c);
		line(cx, cy, cx, cy - h * 0.6f, s * 0.08f, d);
		line(cx, cy, cx + h * 0.45f, cy + h * 0.1f, s * 0.08f, d);
		break;
	case 12: // person
		C2D_DrawCircleSolid(cx, cy - h * 0.4f, 0.5f, h * 0.4f, c);
		fan(cx, cy + h * 0.85f, h * 0.8f, h * 0.85f, (float)M_PI, (float)M_PI * 2, c);
		break;
	case 13: { // sparkle
		float r = h, k = h * 0.22f;
		tri(cx, cy - r, cx - k, cy, cx + k, cy, c);
		tri(cx, cy + r, cx - k, cy, cx + k, cy, c);
		tri(cx - r, cy, cx, cy - k, cx, cy + k, c);
		tri(cx + r, cy, cx, cy - k, cx, cy + k, c);
		float sx = cx + h * 0.62f, sy = cy - h * 0.62f, r2 = h * 0.35f, k2 = h * 0.09f;
		tri(sx, sy - r2, sx - k2, sy, sx + k2, sy, c);
		tri(sx, sy + r2, sx - k2, sy, sx + k2, sy, c);
		tri(sx - r2, sy, sx, sy - k2, sx, sy + k2, c);
		tri(sx + r2, sy, sx, sy - k2, sx, sy + k2, c);
		break;
	}
	case 14: // plane
		rrect(cx - h * 0.95f, cy - h * 0.12f, s * 0.95f, h * 0.24f, h * 0.12f, c);
		tri(cx - h * 0.1f, cy - h * 0.1f, cx + h * 0.25f, cy - h * 0.1f, cx - h * 0.35f, cy - h * 0.9f, c);
		tri(cx - h * 0.1f, cy + h * 0.1f, cx + h * 0.25f, cy + h * 0.1f, cx - h * 0.35f, cy + h * 0.9f, c);
		tri(cx - h * 0.95f, cy, cx - h * 0.65f, cy, cx - h * 0.95f, cy - h * 0.45f, c);
		break;
	case 15: // ball
		C2D_DrawCircleSolid(cx, cy, 0.5f, h * 0.9f, c);
		arc(cx - h * 1.1f, cy, h * 0.75f, -(float)M_PI * 0.35f, (float)M_PI * 0.35f, s * 0.05f, d);
		arc(cx + h * 1.1f, cy, h * 0.75f, (float)M_PI * 0.65f, (float)M_PI * 1.35f, s * 0.05f, d);
		line(cx, cy - h * 0.9f, cx, cy + h * 0.9f, s * 0.05f, d);
		break;
	case 16: // checkered flag
		rect(cx - h * 0.8f, cy - h * 0.85f, s * 0.07f, s * 0.85f, c);
		for (int r = 0; r < 3; r++)
			for (int q = 0; q < 4; q++)
				rect(cx - h * 0.66f + q * h * 0.38f, cy - h * 0.85f + r * h * 0.34f, h * 0.38f, h * 0.34f, (r + q) & 1 ? d : c);
		break;
	case 17: // chip
		rrect(cx - h * 0.55f, cy - h * 0.55f, h * 1.1f, h * 1.1f, s * 0.06f, c);
		for (int i = 0; i < 3; i++) {
			float o = -h * 0.35f + i * h * 0.35f;
			rect(cx - h * 0.85f, cy + o - s * 0.03f, h * 0.3f, s * 0.06f, c);
			rect(cx + h * 0.55f, cy + o - s * 0.03f, h * 0.3f, s * 0.06f, c);
			rect(cx + o - s * 0.03f, cy - h * 0.85f, s * 0.06f, h * 0.3f, c);
			rect(cx + o - s * 0.03f, cy + h * 0.55f, s * 0.06f, h * 0.3f, c);
		}
		rect(cx - h * 0.25f, cy - h * 0.25f, h * 0.5f, h * 0.5f, d);
		break;
	case 18: // fork and knife
		rect(cx - h * 0.5f, cy - h * 0.1f, s * 0.08f, h * 1.0f, c);
		for (int i = 0; i < 3; i++) rect(cx - h * 0.78f + i * h * 0.26f, cy - h * 0.85f, s * 0.05f, h * 0.6f, c);
		rrect(cx - h * 0.8f, cy - h * 0.3f, h * 0.62f, h * 0.25f, s * 0.04f, c);
		C2D_DrawEllipseSolid(cx + h * 0.2f, cy - h * 0.9f, 0.5f, h * 0.45f, h * 1.0f, c);
		rect(cx + h * 0.36f, cy - h * 0.1f, s * 0.08f, h * 1.0f, c);
		break;
	}
}

void glyph_plus(float cx, float cy, float s, u32 c)
{
	float t = s * 0.16f;
	rect(cx - s / 2, cy - t / 2, s, t, c);
	rect(cx - t / 2, cy - s / 2, t, s, c);
}

void glyph_gear(float cx, float cy, float s, u32 c, u32 bg)
{
	float r = s * 0.34f;
	for (int i = 0; i < 8; i++) {
		float a = (float)M_PI * 2 * i / 8;
		line(cx, cy, cx + cosf(a) * s * 0.5f, cy + sinf(a) * s * 0.5f, s * 0.2f, c);
	}
	C2D_DrawCircleSolid(cx, cy, 0.5f, r, c);
	C2D_DrawCircleSolid(cx, cy, 0.5f, r * 0.45f, bg);
}

void glyph_back(float cx, float cy, float s, u32 c)
{
	float t = s * 0.16f;
	line(cx + s * 0.2f, cy - s * 0.4f, cx - s * 0.25f, cy, t, c);
	line(cx - s * 0.25f, cy, cx + s * 0.2f, cy + s * 0.4f, t, c);
	C2D_DrawCircleSolid(cx - s * 0.25f, cy, 0.5f, t / 2, c);
}

void glyph_refresh(float cx, float cy, float s, u32 c, u32 bg)
{
	(void)bg;
	float r = s * 0.38f;
	arc(cx, cy, r, -(float)M_PI * 0.35f, (float)M_PI * 1.45f, s * 0.13f, c);
	float ax = cx + cosf(-(float)M_PI * 0.35f) * r, ay = cy + sinf(-(float)M_PI * 0.35f) * r;
	tri(ax - s * 0.2f, ay - s * 0.08f, ax + s * 0.14f, ay - s * 0.14f, ax + s * 0.02f, ay + s * 0.22f, c);
}

void spinner(float cx, float cy, float r, u32 c)
{
	float t = (osGetTime() % 1000) / 1000.0f;
	for (int i = 0; i < 8; i++) {
		float a = (float)M_PI * 2 * i / 8;
		float k = fmodf(1.0f + i / 8.0f - t, 1.0f);
		u32 cc = (c & 0x00ffffff) | ((u32)(40 + 215 * k) << 24);
		C2D_DrawCircleSolid(cx + cosf(a) * r, cy + sinf(a) * r, 0.5f, r * 0.22f, cc);
	}
}

// ---------- pictures ----------

Picture *pic_find(u32 key)
{
	for (int i = 0; i < MAX_PICS; i++)
		if (s_pics[i].used && s_pics[i].key == key) {
			s_pics[i].last = osGetTime();
			return &s_pics[i];
		}
	return NULL;
}

static void pic_release(Picture *p)
{
	if (!p->used) return;
	s_picbytes -= (size_t)p->tex.width * p->tex.height * 4;
	if (s_ndead < (int)(sizeof s_dead / sizeof s_dead[0])) s_dead[s_ndead++] = p->tex;
	else C3D_TexDelete(&p->tex);
	p->used = false;
}

Picture *pic_add(u32 key, const u32 *pixels, int tw, int th, int iw, int ih)
{
	Picture *p = pic_find(key);
	if (p) return p;
	size_t bytes = (size_t)tw * th * 4;
	while (s_picbytes + bytes > PIC_BUDGET) {
		Picture *old = NULL;
		for (int i = 0; i < MAX_PICS; i++)
			if (s_pics[i].used && (!old || s_pics[i].last < old->last)) old = &s_pics[i];
		if (!old) break;
		pic_release(old);
	}
	Picture *slot = NULL;
	for (int i = 0; i < MAX_PICS; i++)
		if (!s_pics[i].used) { slot = &s_pics[i]; break; }
	if (!slot) {
		slot = &s_pics[0];
		for (int i = 1; i < MAX_PICS; i++)
			if (s_pics[i].last < slot->last) slot = &s_pics[i];
		pic_release(slot);
	}
	if (!C3D_TexInit(&slot->tex, tw, th, GPU_RGBA8)) return NULL;
	memcpy(slot->tex.data, pixels, (size_t)tw * th * 4);
	GSPGPU_FlushDataCache(slot->tex.data, (u32)tw * th * 4);
	C3D_TexSetFilter(&slot->tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&slot->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	slot->sub.width = iw;
	slot->sub.height = ih;
	slot->sub.left = 0.0f;
	slot->sub.top = 1.0f;
	slot->sub.right = (float)iw / tw;
	slot->sub.bottom = 1.0f - (float)ih / th;
	slot->img.tex = &slot->tex;
	slot->img.subtex = &slot->sub;
	slot->key = key;
	slot->used = true;
	s_picbytes += bytes;
	slot->last = osGetTime();
	return slot;
}

void pic_draw(Picture *p, float x, float y, float w, float h)
{
	if (!p) return;
	C2D_DrawImageAt(p->img, x, y, 0.5f, NULL, w / p->sub.width, h / p->sub.height);
}

void pic_clear(void)
{
	for (int i = 0; i < MAX_PICS; i++) pic_release(&s_pics[i]);
}
