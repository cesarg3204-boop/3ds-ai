// Spike AI: a chat assistant for the Nintendo 3DS.
// Top screen: the conversation. Bottom screen: buttons and menus.
// The AI runs on a Cloudflare Worker relay (see relay/), the 3DS just sends text.

#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "gfx.h"
#include "net.h"
#include "media.h"
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"
#include <dirent.h>
#include <strings.h>
#include <ctype.h>

#define BASE "sdmc:/3ds/SpikeAI"
#define MAX_CHATS 300
#define MAX_PROJECTS 40
#define HISTORY 16
#define VOICE_PH "(voice message)"

// ---------------------------------------------------------------- personas

#define NPERSONA 6
static const char *P_ID[NPERSONA] = { "nofluff", "chill", "analyst", "searcher", "gamer", "custom" };
static const char *P_NAME[NPERSONA] = { "No Fluff", "Chill", "Analyst", "Searcher", "Gamer", "Custom" };
static const char *P_DESC[NPERSONA] = {
	"Straight answers. No intro, no filler.",
	"Relaxed and friendly, like texting a buddy.",
	"Detailed breakdowns, pros and cons, a clear pick.",
	"Always looks things up online and cites sources.",
	"Your hype gaming buddy: tips, builds, recommendations.",
	"Spike follows the instructions you write yourself.",
};

// ---------------------------------------------------------------- small helpers

typedef struct { char *d; size_t n, cap; } Str;

static void s_add(Str *s, const char *t)
{
	size_t k = strlen(t);
	if (s->n + k + 1 > s->cap) {
		size_t c = s->cap ? s->cap : 1024;
		while (c < s->n + k + 1) c *= 2;
		s->d = realloc(s->d, c);
		s->cap = c;
	}
	memcpy(s->d + s->n, t, k + 1);
	s->n += k;
}

static char *esc(const char *t)
{
	char *o = malloc(strlen(t) * 2 + 1), *p = o;
	for (; *t; t++) {
		if (*t == '\\') { *p++ = '\\'; *p++ = '\\'; }
		else if (*t == '\n') { *p++ = '\\'; *p++ = 'n'; }
		else if (*t == '\t') { *p++ = '\\'; *p++ = 't'; }
		else if (*t != '\r') *p++ = *t;
	}
	*p = 0;
	return o;
}

// Unescapes in place.
static char *unesc(char *t)
{
	char *r = t, *w = t;
	while (*r) {
		if (*r == '\\' && r[1]) {
			r++;
			*w++ = *r == 'n' ? '\n' : *r == 't' ? '\t' : *r;
			r++;
		} else *w++ = *r++;
	}
	*w = 0;
	return t;
}

static void copy(char *dst, size_t n, const char *src)
{
	snprintf(dst, n, "%s", src ? src : "");
}

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *d = malloc(n + 1);
	n = fread(d, 1, n, f);
	d[n] = 0;
	fclose(f);
	return d;
}

// Splits text into lines in place; calls fn for each non-empty line.
static void each_line(char *d, void (*fn)(char *ln, void *ud), void *ud)
{
	char *p = d;
	while (p && *p) {
		char *nl = strchr(p, '\n');
		if (nl) *nl = 0;
		size_t k = strlen(p);
		if (k && p[k - 1] == '\r') p[k - 1] = 0;
		if (*p) fn(p, ud);
		p = nl ? nl + 1 : NULL;
	}
}

// Splits at the first tab; returns the part after it (or "").
static char *cut(char *s)
{
	char *t = strchr(s, '\t');
	if (!t) return s + strlen(s);
	*t = 0;
	return t + 1;
}

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

// ---------------------------------------------------------------- data

typedef struct {
	char relay[200];
	char key[100];
	int persona;
	char custom[1200];
	bool web;
	bool verify;
	bool mcp;
	int last_chat;
} Config;

typedef struct {
	int id, project;
	long long updated;
	char title[64];
} ChatInfo;

typedef struct {
	int id, persona; // persona -1 = use the default
	char name[48];
	char instr[1600];
} Project;

static Config cfg;
static ChatInfo chats[MAX_CHATS];
static int nchats;
static Project projects[MAX_PROJECTS];
static int nprojects;

// ---- messages / conversations

typedef struct {
	char role;   // 'U' or 'A'
	char *text;
	char *src;   // "title\n" per source (assistant only), may be NULL
	char att;    // attachment on a user message: 'P' picture, 'F' text file, 0 none
	char *att_path;
	bool thumb_fail;
	// layout cache
	char **ln;
	int nln;
	char **sln;
	int nsln;
	float bw;
	float h;
} Msg;

typedef struct {
	Msg *m;
	int n, cap;
	int id;       // 0 = not saved yet
	int project;  // 0 = none
	char title[64];
} Conv;

static Conv cur, preview;

static void msg_layout_free(Msg *m)
{
	if (m->ln) lines_free(m->ln, m->nln);
	if (m->sln) lines_free(m->sln, m->nsln);
	m->ln = m->sln = NULL;
	m->nln = m->nsln = 0;
	m->h = -1;
}

static void conv_clear(Conv *c)
{
	for (int i = 0; i < c->n; i++) {
		msg_layout_free(&c->m[i]);
		free(c->m[i].text);
		free(c->m[i].src);
		free(c->m[i].att_path);
	}
	free(c->m);
	memset(c, 0, sizeof *c);
}

static Msg *conv_add(Conv *c, char role, const char *text)
{
	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 16;
		c->m = realloc(c->m, c->cap * sizeof(Msg));
	}
	Msg *m = &c->m[c->n++];
	memset(m, 0, sizeof *m);
	m->role = role;
	m->text = strdup(text);
	m->h = -1;
	return m;
}

static void src_add(Msg *m, const char *title)
{
	size_t a = m->src ? strlen(m->src) : 0, b = strlen(title);
	m->src = realloc(m->src, a + b + 2);
	memcpy(m->src + a, title, b);
	m->src[a + b] = '\n';
	m->src[a + b + 1] = 0;
}

// ---------------------------------------------------------------- storage

static void ensure_dirs(void)
{
	mkdir("sdmc:/3ds", 0777);
	mkdir(BASE, 0777);
	mkdir(BASE "/chats", 0777);
	mkdir(BASE "/att", 0777);
	// first run with skills: add the starter skills (imported from Claude)
	if (mkdir(BASE "/skills", 0777) == 0) {
		static const char *def[][2] = {
			{ "truth", 
	"Honest answers: says what's true, labels guesses, doesn't just agree.\n"
	"Tell the user what is true and what is needed, never just what is pleasant.\n"
	"- Verdict first: lead with your honest assessment, then the reason. Praise only when it is specific and earned.\n"
	"- Label confidence on important claims: certain, likely or guess. Never present a guess as fact.\n"
	"- If the user pushes back, change your answer only for a new fact or a new argument, and say what changed. Insisting or annoyance is not evidence.\n"
	"- Never invent data or quietly fill in a missing value. Never swap in an easier task without saying so.\n"
	"- If something needed is missing, answer in this shape:\n"
	"  Can do now: ...\n"
	"  Can't do yet, because: ...\n"
	"  Need from you: ...\n"
	"- Don't say something works or is done unless it was checked; otherwise say it's unverified.\n"
	"- Be direct about ideas, never rude to the person. Don't invent disagreement either.\n" },
			{ "research",
	"Deeper research: splits the question into parts and compares sources.\n"
	"Research the user's question carefully before answering.\n"
	"- If a key detail is ambiguous (which country, which product, which year) and it changes the answer, ask one short question first.\n"
	"- Split the question into 2 to 4 angles (for example: facts, current status, options, outlook) and cover each one.\n"
	"- Use the web results. Prefer official and primary sources, and recent ones. When sources disagree, say so and say which is more reliable.\n"
	"- Give numbers, dates and names only when a source has them, and cite like [1].\n"
	"- Shape for the small screen: a 2 to 3 sentence summary first, then a short part per angle with dashes, then \"Not sure about:\" for any gaps.\n"
	"- Keep it under about 350 words unless the user asks for more.\n" },
			{ "manual",
	"Manual didáctico: explica un tema como un pequeño manual de curso.\n"
	"Escribe la respuesta como un manual didáctico corto, en el idioma del usuario.\n"
	"- Empieza con un título y una frase de apertura que diga para qué sirve.\n"
	"- Divide el tema en 3 a 6 secciones numeradas (1., 2., ...). Cada sección abre con una idea guía.\n"
	"- Usa recuadros de texto cuando ayuden: \"Punto clave:\", \"Error frecuente:\", \"Aviso:\" y \"Ejemplo:\".\n"
	"- Usa guiones para listas y pasos numerados para procesos.\n"
	"- Para esquemas usa texto simple, por ejemplo: Paso A -> Paso B -> Paso C.\n"
	"- Termina con \"Resumen\" y una lista de repaso con [ ] para marcar.\n"
	"- Verifica los datos; si no estás seguro de algo, dilo.\n"
	"- Sin tablas ni markdown: la pantalla es pequeña.\n" },
		};
		for (size_t i = 0; i < sizeof def / sizeof def[0]; i++) {
			char p[96];
			snprintf(p, sizeof p, BASE "/skills/%s.txt", def[i][0]);
			file_write(p, def[i][1], strlen(def[i][1]));
		}
	}
}

// ---------------------------------------------------------------- skills
// A skill is a .txt file in sdmc:/3ds/SpikeAI/skills: the first line says what
// it does, the rest is instructions. Start a message with /name to use it.

#define MAX_SKILLS 40
typedef struct { char name[32]; char desc[96]; } Skill;
static Skill skills[MAX_SKILLS];
static int nskills;

static void load_skills(void)
{
	nskills = 0;
	DIR *d = opendir(BASE "/skills");
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) && nskills < MAX_SKILLS) {
		size_t L = strlen(e->d_name);
		if (L < 5 || L > 35 || strcasecmp(e->d_name + L - 4, ".txt")) continue;
		Skill *k = &skills[nskills++];
		snprintf(k->name, sizeof k->name, "%.*s", (int)(L - 4), e->d_name);
		for (char *q = k->name; *q; q++) { *q = tolower((unsigned char)*q); if (*q == ' ') *q = '-'; }
		char p[300];
		snprintf(p, sizeof p, BASE "/skills/%s", e->d_name);
		k->desc[0] = 0;
		FILE *f = fopen(p, "r");
		if (f) {
			if (fgets(k->desc, sizeof k->desc, f)) { char *nl = strpbrk(k->desc, "\r\n"); if (nl) *nl = 0; }
			fclose(f);
		}
	}
	closedir(d);
	// sort by name
	for (int i = 1; i < nskills; i++)
		for (int j = i; j > 0 && strcmp(skills[j - 1].name, skills[j].name) > 0; j--) {
			Skill t = skills[j]; skills[j] = skills[j - 1]; skills[j - 1] = t;
		}
}

// The skill named at the start of a message ("/truth is this good?"), or NULL.
// name gets the skill's name either way.
static char *skill_for(const char *text, char *name, size_t n)
{
	name[0] = 0;
	if (!text || text[0] != '/') return NULL;
	size_t k = 0;
	for (const char *q = text + 1; *q && !isspace((unsigned char)*q) && k + 1 < n; q++) name[k++] = tolower((unsigned char)*q);
	name[k] = 0;
	if (!k) return NULL;
	DIR *d = opendir(BASE "/skills");
	if (!d) return NULL;
	struct dirent *e;
	char file[300] = "";
	while ((e = readdir(d))) {
		size_t L = strlen(e->d_name);
		if (L != k + 4 || strcasecmp(e->d_name + k, ".txt")) continue;
		char low[64];
		snprintf(low, sizeof low, "%.*s", (int)k, e->d_name);
		for (char *q = low; *q; q++) { *q = tolower((unsigned char)*q); if (*q == ' ') *q = '-'; }
		if (!strcmp(low, name)) { snprintf(file, sizeof file, BASE "/skills/%s", e->d_name); break; }
	}
	closedir(d);
	if (!file[0]) return NULL;
	size_t len = 0;
	unsigned char *d2 = file_read(file, 8000, &len);
	return (char *)d2;
}

static void cfg_line(char *ln, void *ud)
{
	(void)ud;
	char *eq = strchr(ln, '=');
	if (!eq) return;
	*eq = 0;
	char *v = unesc(eq + 1);
	if (!strcmp(ln, "relay")) copy(cfg.relay, sizeof cfg.relay, v);
	else if (!strcmp(ln, "key")) copy(cfg.key, sizeof cfg.key, v);
	else if (!strcmp(ln, "persona")) cfg.persona = atoi(v);
	else if (!strcmp(ln, "custom")) copy(cfg.custom, sizeof cfg.custom, v);
	else if (!strcmp(ln, "web")) cfg.web = atoi(v) != 0;
	else if (!strcmp(ln, "mcp")) cfg.mcp = atoi(v) != 0;
	else if (!strcmp(ln, "verify")) cfg.verify = atoi(v) != 0;
	else if (!strcmp(ln, "last")) cfg.last_chat = atoi(v);
}

static void cfg_load(void)
{
	memset(&cfg, 0, sizeof cfg);
	cfg.persona = 1;
	cfg.verify = true;
	cfg.web = true;
	char *d = read_file(BASE "/config.txt");
	if (d) { each_line(d, cfg_line, NULL); free(d); }
	if (cfg.persona < 0 || cfg.persona >= NPERSONA) cfg.persona = 1;
}

static void trim(char *t)
{
	char *a = t;
	while (*a == ' ' || *a == '\n' || *a == '\t') a++;
	memmove(t, a, strlen(a) + 1);
	size_t k = strlen(t);
	while (k && (t[k - 1] == ' ' || t[k - 1] == '\n' || t[k - 1] == '\t')) t[--k] = 0;
}

static void cfg_save(void)
{
	trim(cfg.key);
	trim(cfg.relay);
	FILE *f = fopen(BASE "/config.txt", "wb");
	if (!f) return;
	char *a = esc(cfg.relay), *b = esc(cfg.key), *c = esc(cfg.custom);
	fprintf(f, "relay=%s\nkey=%s\npersona=%d\ncustom=%s\nweb=%d\nverify=%d\nlast=%d\nmcp=%d\n", a, b, cfg.persona, c, cfg.web, cfg.verify, cfg.last_chat, cfg.mcp);
	free(a); free(b); free(c);
	fclose(f);
}

static void idx_line(char *ln, void *ud)
{
	(void)ud;
	if (nchats >= MAX_CHATS) return;
	char *a = cut(ln), *b = cut(a), *c = cut(b);
	ChatInfo *ci = &chats[nchats];
	ci->id = atoi(ln);
	ci->project = atoi(a);
	ci->updated = atoll(b);
	copy(ci->title, sizeof ci->title, unesc(c));
	if (ci->id > 0) nchats++;
}

static void proj_line(char *ln, void *ud)
{
	(void)ud;
	if (nprojects >= MAX_PROJECTS) return;
	char *a = cut(ln), *b = cut(a), *c = cut(b);
	Project *p = &projects[nprojects];
	p->id = atoi(ln);
	p->persona = atoi(a);
	if (p->persona < -1 || p->persona >= NPERSONA) p->persona = -1;
	copy(p->name, sizeof p->name, unesc(b));
	copy(p->instr, sizeof p->instr, unesc(c));
	if (p->id > 0) nprojects++;
}

static int cmp_chat(const void *a, const void *b)
{
	long long x = ((const ChatInfo *)a)->updated, y = ((const ChatInfo *)b)->updated;
	return x < y ? 1 : x > y ? -1 : 0;
}

static void index_load(void)
{
	nchats = nprojects = 0;
	char *d = read_file(BASE "/chats.txt");
	if (d) { each_line(d, idx_line, NULL); free(d); }
	d = read_file(BASE "/projects.txt");
	if (d) { each_line(d, proj_line, NULL); free(d); }
	qsort(chats, nchats, sizeof *chats, cmp_chat);
}

static void index_save(void)
{
	qsort(chats, nchats, sizeof *chats, cmp_chat);
	FILE *f = fopen(BASE "/chats.txt", "wb");
	if (f) {
		for (int i = 0; i < nchats; i++) {
			char *t = esc(chats[i].title);
			fprintf(f, "%d\t%d\t%lld\t%s\n", chats[i].id, chats[i].project, chats[i].updated, t);
			free(t);
		}
		fclose(f);
	}
	f = fopen(BASE "/projects.txt", "wb");
	if (f) {
		for (int i = 0; i < nprojects; i++) {
			char *a = esc(projects[i].name), *b = esc(projects[i].instr);
			fprintf(f, "%d\t%d\t%s\t%s\n", projects[i].id, projects[i].persona, a, b);
			free(a); free(b);
		}
		fclose(f);
	}
}

static ChatInfo *chat_find(int id)
{
	for (int i = 0; i < nchats; i++) if (chats[i].id == id) return &chats[i];
	return NULL;
}

static Project *proj_find(int id)
{
	for (int i = 0; i < nprojects; i++) if (projects[i].id == id) return &projects[i];
	return NULL;
}

static void chat_path(char *out, size_t n, int id) { snprintf(out, n, BASE "/chats/%d.txt", id); }

static void conv_line(char *ln, void *ud)
{
	Conv *c = ud;
	char *v = cut(ln);
	if (!strcmp(ln, "U") || !strcmp(ln, "A")) conv_add(c, ln[0], unesc(v));
	else if (!strcmp(ln, "F") && c->n && c->m[c->n - 1].role == 'U') {
		char *path = cut(v);
		c->m[c->n - 1].att = v[0];
		c->m[c->n - 1].att_path = strdup(unesc(path));
	}
	else if (!strcmp(ln, "S") && c->n && c->m[c->n - 1].role == 'A') {
		cut(v); // drop the url
		src_add(&c->m[c->n - 1], unesc(v));
	}
}

static void conv_load(Conv *c, int id)
{
	conv_clear(c);
	ChatInfo *ci = chat_find(id);
	if (!ci) return;
	c->id = id;
	c->project = ci->project;
	copy(c->title, sizeof c->title, ci->title);
	char path[96];
	chat_path(path, sizeof path, id);
	char *d = read_file(path);
	if (d) { each_line(d, conv_line, c); free(d); }
}

static void conv_save(Conv *c)
{
	if (!c->id) return;
	char path[96];
	chat_path(path, sizeof path, c->id);
	FILE *f = fopen(path, "wb");
	if (!f) return;
	for (int i = 0; i < c->n; i++) {
		char *t = esc(c->m[i].text);
		fprintf(f, "%c\t%s\n", c->m[i].role, t);
		free(t);
		if (c->m[i].att && c->m[i].att_path) {
			char *e = esc(c->m[i].att_path);
			fprintf(f, "F\t%c\t%s\n", c->m[i].att, e);
			free(e);
		}
		if (c->m[i].src) {
			char *s = strdup(c->m[i].src), *p = s;
			while (*p) {
				char *nl = strchr(p, '\n');
				if (nl) *nl = 0;
				char *e = esc(p);
				if (*p) fprintf(f, "S\t%s\t\n", e);
				free(e);
				if (!nl) break;
				p = nl + 1;
			}
			free(s);
		}
	}
	fclose(f);
}

static int next_chat_id(void)
{
	int m = 0;
	for (int i = 0; i < nchats; i++) if (chats[i].id > m) m = chats[i].id;
	return m + 1;
}

static void make_title(const char *text, char *out, size_t n)
{
	char t[64];
	int k = 0;
	const char *p = text;
	while (*p == ' ' || *p == '\n') p++;
	for (; *p && k < 44; p++) t[k++] = (*p == '\n' || *p == '\t') ? ' ' : *p;
	t[k] = 0;
	if (*p) {
		// don't cut a UTF-8 character in half, prefer a word boundary
		while (k && ((unsigned char)t[k] & 0xC0) == 0x80) k--;
		t[k] = 0;
		char *sp = strrchr(t, ' ');
		if (sp && sp - t > 20) *sp = 0;
		strcat(t, "...");
	}
	copy(out, n, t);
}

// Saves cur (creating its index entry on first save).
static void cur_commit(void)
{
	if (!cur.n) return;
	if (!cur.id) {
		if (nchats >= MAX_CHATS) { // forget the oldest
			char path[96];
			chat_path(path, sizeof path, chats[nchats - 1].id);
			remove(path);
			nchats--;
		}
		cur.id = next_chat_id();
		const char *first = cur.m[0].text;
		if (!first[0] && cur.m[0].att == 'P') first = "Photo";
		else if (!first[0] && cur.m[0].att_path) { const char *b = strrchr(cur.m[0].att_path, '/'); first = b ? b + 1 : cur.m[0].att_path; }
		make_title(first, cur.title, sizeof cur.title);
		ChatInfo *ci = &chats[nchats++];
		ci->id = cur.id;
		ci->project = cur.project;
		copy(ci->title, sizeof ci->title, cur.title);
	}
	ChatInfo *ci = chat_find(cur.id);
	if (ci) ci->updated = (long long)time(NULL);
	conv_save(&cur);
	index_save();
	if (cfg.last_chat != cur.id) { cfg.last_chat = cur.id; cfg_save(); }
}

static void chat_delete(int id)
{
	char path[96];
	chat_path(path, sizeof path, id);
	remove(path);
	for (int i = 0; i < nchats; i++)
		if (chats[i].id == id) { memmove(&chats[i], &chats[i + 1], (nchats - i - 1) * sizeof *chats); nchats--; break; }
	if (cur.id == id) { int p = cur.project; conv_clear(&cur); cur.project = proj_find(p) ? p : 0; }
	if (preview.id == id) conv_clear(&preview);
	index_save();
}

static int proj_chat_count(int pid)
{
	int n = 0;
	for (int i = 0; i < nchats; i++) if (chats[i].project == pid) n++;
	return n;
}

// The personality used for a chat in project pid.
static int persona_for(int pid)
{
	Project *p = proj_find(pid);
	return p && p->persona >= 0 ? p->persona : cfg.persona;
}

// ---------------------------------------------------------------- keyboard

static char kbuf[4100];

static bool kb(const char *hint, const char *initial, int maxlen, bool multiline, bool need_text, const char *ok)
{
	SwkbdState k;
	swkbdInit(&k, SWKBD_TYPE_NORMAL, 2, maxlen);
	swkbdSetHintText(&k, hint);
	swkbdSetInitialText(&k, initial ? initial : "");
	swkbdSetButton(&k, SWKBD_BUTTON_LEFT, "Cancel", false);
	swkbdSetButton(&k, SWKBD_BUTTON_RIGHT, ok ? ok : "OK", true);
	swkbdSetFeatures(&k, SWKBD_DARKEN_TOP_SCREEN | (multiline ? SWKBD_MULTILINE : 0));
	swkbdSetValidation(&k, need_text ? SWKBD_NOTEMPTY_NOTBLANK : SWKBD_ANYTHING, 0, 0);
	kbuf[0] = 0;
	SwkbdButton b = swkbdInputText(&k, kbuf, sizeof kbuf);
	return b == SWKBD_BUTTON_RIGHT;
}

// ---------------------------------------------------------------- app state

typedef enum { V_CHAT, V_CHATS, V_PROJECTS, V_PROJECT, V_PROJEDIT, V_SETTINGS, V_FILES, V_CAMERA, V_TALK, V_MCP, V_SKILLS } View;

static View view = V_CHAT;
static int sel[12], scrl[12];        // list selection / first visible row per view
static int open_project;           // project shown in V_PROJECT / V_PROJEDIT
static float chat_scroll;          // pixels up from the bottom
static bool thinking;
static int thinking_chat;          // chat id the pending answer belongs to (0 = cur unsaved)
static char chat_err[240];
static char toast[200];
static u64 toast_until;
static int armed = -1;             // row waiting for a second Y to delete
static View armed_view;
static char ping_status[200];
static float t_anim;
// attachment waiting for the next message
static char pend_att;
static char pend_path[300];
// last recording, kept until Spike understood it (for "try again")
static char *voice_b64;
static char voice_err[160];
// file browser
#define MAX_FILES 400
typedef struct { char name[128]; bool dir; } FileEnt;
static FileEnt files[MAX_FILES];
static int nfiles;
static char cwd[300] = "sdmc:/";
static bool cam_inner;

static void say(const char *msg)
{
	copy(toast, sizeof toast, msg);
	toast_until = osGetTime() + 2600;
}

// ---------------------------------------------------------------- relay

static void relay_url(char *out, size_t n, const char *path)
{
	const char *r = cfg.relay;
	while (*r == ' ') r++;
	char base[220];
	if (strncmp(r, "http://", 7) && strncmp(r, "https://", 8)) snprintf(base, sizeof base, "https://%s", r);
	else copy(base, sizeof base, r);
	size_t k = strlen(base);
	while (k && (base[k - 1] == '/' || base[k - 1] == ' ')) base[--k] = 0;
	snprintf(out, n, "%s%s", base, path);
}

enum { TAG_CHAT = 1, TAG_PING, TAG_MCP };

// MCP servers the relay is connected to (from /v1/mcp)
typedef struct { char name[40]; char count[12]; char detail[1200]; } McpSrv;
static McpSrv mcps[10];
static int nmcps;
static char mcp_status[160];

static void add_kv(Str *s, const char *k, const char *v)
{
	char *e = esc(v);
	s_add(s, k);
	s_add(s, "\t");
	s_add(s, e);
	s_add(s, "\n");
	free(e);
}

static void send_chat(void)
{
	if (!cfg.relay[0]) {
		copy(chat_err, sizeof chat_err, "Spike needs a relay first. Open Settings and add your relay address.");
		return;
	}
	Str s = {0};
	int p = persona_for(cur.project);
	add_kv(&s, "P", P_ID[p]);
	if (p == 5) add_kv(&s, "C", cfg.custom);
	Project *pr = proj_find(cur.project);
	if (pr && pr->instr[0]) add_kv(&s, "I", pr->instr);
	if (cfg.web) add_kv(&s, "W", "1");
	if (cur.n && cur.m[cur.n - 1].role == 'U') {
		char nm[32];
		char *sk = skill_for(cur.m[cur.n - 1].text, nm, sizeof nm);
		if (sk) {
			char *e = esc(sk);
			s_add(&s, "Z\t"); s_add(&s, nm); s_add(&s, "\t"); s_add(&s, e); s_add(&s, "\n");
			free(e); free(sk);
		}
	}
	int start = cur.n > HISTORY ? cur.n - HISTORY : 0;
	int last_pic = -1;
	for (int i = start; i < cur.n; i++) if (cur.m[i].att == 'P') last_pic = i;
	for (int i = start; i < cur.n; i++) {
		Msg *m = &cur.m[i];
		bool voice = m->role == 'U' && i == cur.n - 1 && voice_b64 && !strcmp(m->text, VOICE_PH);
		add_kv(&s, m->role == 'U' ? "U" : "A", voice ? "" : m->text);
		if (voice) { s_add(&s, "V\t"); s_add(&s, voice_b64); s_add(&s, "\n"); }
		if (m->att == 'F' && m->att_path) {
			size_t n = 0;
			unsigned char *d = file_read(m->att_path, 200000, &n);
			if (d) {
				if (n > 16000) d[16000] = 0;
				const char *b = strrchr(m->att_path, '/');
				char *e = esc((char *)d), *nm = esc(b ? b + 1 : m->att_path);
				s_add(&s, "X\t"); s_add(&s, nm); s_add(&s, "\t"); s_add(&s, e); s_add(&s, "\n");
				free(e); free(nm); free(d);
			}
		}
		if (m->att == 'P' && i == last_pic && m->att_path) {
			const char *mime = NULL;
			size_t n = 0;
			char err[120];
			unsigned char *d = picture_for_upload(m->att_path, &mime, &n, err, sizeof err);
			if (!d) {
				if (i == cur.n - 1) { copy(chat_err, sizeof chat_err, err); free(s.d); return; }
				continue;
			}
			char *b = b64_encode(d, n);
			free(d);
			if (b) { s_add(&s, "G\t"); s_add(&s, mime); s_add(&s, "\t"); s_add(&s, b); s_add(&s, "\n"); free(b); }
		}
	}

	char url[260];
	relay_url(url, sizeof url, "/v1/chat");
	Job *j = job_new(JOB_TEXT, TAG_CHAT, url);
	j->post = s.d;
	j->timeout = 150;
	j->arg = cur.id;
	net_submit(j, true);
	thinking = true;
	thinking_chat = cur.id;
	chat_err[0] = 0;
	chat_scroll = 0;
}

static void send_ping(void)
{
	if (!cfg.relay[0]) { copy(ping_status, sizeof ping_status, "Add the relay address first."); return; }
	char url[260];
	relay_url(url, sizeof url, "/v1/ping");
	Job *j = job_new(JOB_TEXT, TAG_PING, url);
	j->timeout = 20;
	net_submit(j, true);
	copy(ping_status, sizeof ping_status, "Testing...");
}

static void got_chat(Job *j)
{
	thinking = false;
	int id = j->arg;
	// a chat that was saved while waiting (the first message) has a new id
	if (id == 0 && thinking_chat == 0) id = cur.id;
	char err[240] = "";
	char *answer = NULL, *heard = NULL;
	Str srcs = {0};
	if (!j->ok) snprintf(err, sizeof err, "Couldn't reach the relay: %s", j->err);
	else if (!strncmp(j->body, "ERR\t", 4)) {
		char *e = j->body + 4;
		char *nl = strchr(e, '\n');
		if (nl) *nl = 0;
		snprintf(err, sizeof err, "%s", unesc(e));
	} else if (!strncmp(j->body, "OK", 2)) {
		char *p = j->body;
		while (p && *p) {
			char *nl = strchr(p, '\n');
			if (nl) *nl = 0;
			if (!strncmp(p, "A\t", 2)) answer = unesc(p + 2);
			else if (!strncmp(p, "T\t", 2)) heard = unesc(p + 2);
			else if (!strncmp(p, "S\t", 2)) {
				char *t = p + 2;
				cut(t);
				s_add(&srcs, unesc(t));
				s_add(&srcs, "\n");
			}
			p = nl ? nl + 1 : NULL;
		}
		if (!answer || !*answer) copy(err, sizeof err, "Spike came back empty. Try again.");
	} else copy(err, sizeof err, "The relay answered something odd. Check its address in Settings.");

	bool here = id == cur.id;
	if (err[0]) {
		if (here) copy(chat_err, sizeof chat_err, err);
		else say("A chat in the background couldn't get an answer.");
		free(srcs.d);
		return;
	}
	Conv tmp = {0}, *c = here ? &cur : &tmp;
	if (!here) conv_load(&tmp, id);
	if (!here && !tmp.id) { free(srcs.d); return; } // chat was deleted
	if (heard && *heard) {
		// show what Spike heard instead of "(voice message)"
		for (int i = c->n - 1; i >= 0; i--) {
			if (c->m[i].role != 'U') continue;
			if (!strcmp(c->m[i].text, VOICE_PH)) {
				free(c->m[i].text);
				c->m[i].text = strdup(heard);
				msg_layout_free(&c->m[i]);
				ChatInfo *ci = chat_find(id);
				if (ci && !strcmp(ci->title, VOICE_PH)) {
					make_title(heard, ci->title, sizeof ci->title);
					if (here) copy(cur.title, sizeof cur.title, ci->title);
				}
			}
			break;
		}
		free(voice_b64);
		voice_b64 = NULL;
	}
	Msg *m = conv_add(c, 'A', answer);
	if (srcs.d) { m->src = srcs.d; }
	if (here) { cur_commit(); chat_scroll = 0; }
	else {
		ChatInfo *ci = chat_find(id);
		if (ci) ci->updated = (long long)time(NULL);
		conv_save(&tmp);
		index_save();
		conv_clear(&tmp);
		say("Spike answered in another chat.");
	}
	if (preview.id == id) preview.id = -1; // reload
}

static void send_mcp_status(void)
{
	if (!cfg.relay[0]) { copy(mcp_status, sizeof mcp_status, "Add the relay address in Settings first."); return; }
	char url[260];
	relay_url(url, sizeof url, "/v1/mcp");
	Job *j = job_new(JOB_TEXT, TAG_MCP, url);
	j->timeout = 60;
	net_submit(j, true);
	copy(mcp_status, sizeof mcp_status, "Checking your MCP servers...");
}

static void got_mcp(Job *j)
{
	nmcps = 0;
	if (!j->ok) { snprintf(mcp_status, sizeof mcp_status, "Can't reach the relay: %s", j->err); return; }
	if (strncmp(j->body, "OK", 2)) {
		char *e = j->body;
		if (!strncmp(e, "ERR\t", 4)) e += 4;
		char *nl = strchr(e, '\n');
		if (nl) *nl = 0;
		snprintf(mcp_status, sizeof mcp_status, "%s", strncmp(j->body, "ERR", 3) ? "Update your relay code to use MCP." : unesc(e));
		return;
	}
	char *p = j->body;
	while (p && *p && nmcps < 10) {
		char *nl = strchr(p, '\n');
		if (nl) *nl = 0;
		if (!strncmp(p, "S\t", 2)) {
			char *name = p + 2, *cnt = cut(name), *det = cut(cnt);
			McpSrv *m = &mcps[nmcps++];
			copy(m->name, sizeof m->name, unesc(name));
			copy(m->count, sizeof m->count, cnt);
			copy(m->detail, sizeof m->detail, unesc(det));
		}
		p = nl ? nl + 1 : NULL;
	}
	if (!nmcps) copy(mcp_status, sizeof mcp_status, "No MCP servers on your relay yet. Add FIRECRAWL_KEY or ZAPIER_TOKEN in Cloudflare.");
	else snprintf(mcp_status, sizeof mcp_status, "%d server%s connected.", nmcps, nmcps == 1 ? "" : "s");
}

static void got_ping(Job *j)
{
	if (!j->ok) { snprintf(ping_status, sizeof ping_status, "Can't reach it: %s", j->err); return; }
	if (strncmp(j->body, "SPIKE OK", 8)) { copy(ping_status, sizeof ping_status, "That address answered, but it isn't a Spike relay."); return; }
	bool ai = strstr(j->body, "\tai") != NULL;
	if (!ai) copy(ping_status, sizeof ping_status, "Relay found, but no AI is set up on it yet.");
	else if (strstr(j->body, "gemini")) copy(ping_status, sizeof ping_status, "All good! Spike is using Gemini with Google Search.");
	else copy(ping_status, sizeof ping_status, "All good! Spike is using Cloudflare AI (add GEMINI_KEY for a smarter Spike).");
}

// ---------------------------------------------------------------- drawing: Spike

static void sprout(float cx, float cy, float r, bool blink)
{
	u32 leaf = rgba(96, 206, 128, 255), leaf2 = rgba(60, 160, 96, 255);
	u32 body = rgba(70, 148, 255, 255), body2 = rgba(40, 104, 214, 255);
	// stem and the spiky leaf
	line(cx, cy - r * 0.8f, cx + r * 0.12f, cy - r * 1.32f, r * 0.13f, leaf2);
	float bx = cx + r * 0.12f, by = cy - r * 1.32f;
	tri(bx, by, cx + r * 0.38f, cy - r * 1.86f, cx + r * 0.9f, cy - r * 1.72f, leaf);
	tri(bx, by, cx + r * 0.9f, cy - r * 1.72f, cx + r * 0.62f, cy - r * 1.3f, leaf2);
	tri(cx + r * 0.62f, cy - r * 1.62f, cx + r * 0.9f, cy - r * 1.72f, cx + r * 1.08f, cy - r * 1.98f, leaf);
	// body
	fan(cx, cy + r * 0.06f, r * 1.06f, r * 0.94f, 0, 6.2832f, body2);
	fan(cx, cy, r * 1.02f, r * 0.9f, 0, 6.2832f, body);
	fan(cx - r * 0.38f, cy - r * 0.45f, r * 0.28f, r * 0.16f, 0, 6.2832f, rgba(255, 255, 255, 60));
	// eyes
	for (int s = -1; s <= 1; s += 2) {
		float ex = cx + s * r * 0.36f, ey = cy - r * 0.04f;
		if (blink) {
			rect(ex - r * 0.18f, ey, r * 0.36f, r * 0.08f, rgba(20, 26, 46, 255));
			continue;
		}
		fan(ex, ey, r * 0.23f, r * 0.29f, 0, 6.2832f, rgba(255, 255, 255, 255));
		fan(ex + r * 0.04f, ey + r * 0.06f, r * 0.14f, r * 0.18f, 0, 6.2832f, rgba(20, 26, 46, 255));
		fan(ex + r * 0.0f, ey - r * 0.02f, r * 0.05f, r * 0.05f, 0, 6.2832f, rgba(255, 255, 255, 255));
	}
	// cheeks + smile
	fan(cx - r * 0.66f, cy + r * 0.3f, r * 0.13f, r * 0.08f, 0, 6.2832f, rgba(255, 140, 170, 120));
	fan(cx + r * 0.66f, cy + r * 0.3f, r * 0.13f, r * 0.08f, 0, 6.2832f, rgba(255, 140, 170, 120));
	arc(cx, cy + r * 0.2f, r * 0.15f, 0.5f, 2.64f, r * 0.07f, rgba(20, 26, 46, 255));
}

static bool blinking(void)
{
	u64 t = osGetTime() % 4200;
	return t < 130;
}

// ---------------------------------------------------------------- drawing: conversation (top screen)

#define TOP_W 400
#define BAR_H 24
#define FS 0.52f
#define FS_SRC 0.42f
#define USER_MAXW 270
#define SPIKE_X 34
#define SPIKE_MAXW (TOP_W - SPIKE_X - 10)

#define THUMB_W 160
#define THUMB_H 120

static const char *att_label(Msg *m)
{
	static char b[160];
	const char *n = m->att_path ? strrchr(m->att_path, '/') : NULL;
	snprintf(b, sizeof b, "[File] %s", n ? n + 1 : "file");
	return b;
}

static u32 path_key(const char *s)
{
	u32 h = 2166136261u;
	for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
	return h | 1;
}

// A small picture of a file, cover-cropped to w x h (cached).
static Picture *thumb(const char *path, int w, int h, bool *failed)
{
	u32 key = path_key(path) ^ (u32)(w * 131 + h);
	Picture *p = pic_find(key);
	if (p || *failed) return p;
	size_t n = 0;
	unsigned char *d = file_read(path, 12 * 1024 * 1024, &n);
	if (!d) { *failed = true; return NULL; }
	Job j = {0};
	j.w = w; j.h = h;
	// image_decode can't shrink huge photos without running out of memory
	int iw = 0, ih = 0, c;
	if (stbi_info_from_memory(d, n, &iw, &ih, &c) && (long long)iw * ih <= 4400000 && image_decode(&j, d, n))
		p = pic_add(key, j.pixels, j.tw, j.th, j.iw, j.ih);
	else *failed = true;
	free(j.pixels);
	free(d);
	return p;
}

static void msg_layout(Msg *m)
{
	if (m->h >= 0) return;
	float lh = text_line_h(FS);
	if (m->role == 'U') {
		m->nln = m->text[0] ? text_wrap(m->text, FS, USER_MAXW - 20, &m->ln) : 0;
		float w = 0;
		for (int i = 0; i < m->nln; i++) { float x = text_width(m->ln[i], FS); if (x > w) w = x; }
		m->h = m->nln * lh + 12;
		if (m->att == 'P') { m->h += THUMB_H + (m->nln ? 6 : 0); if (w < THUMB_W) w = THUMB_W; }
		if (m->att == 'F') { m->h += lh; float fw = text_width(att_label(m), FS); if (fw > w) w = fw; if (w > USER_MAXW - 20) w = USER_MAXW - 20; }
		m->bw = w + 20;
	} else {
		m->nln = text_wrap(m->text, FS, SPIKE_MAXW, &m->ln);
		m->h = m->nln * lh + 4;
		if (m->src) {
			// number the sources
			Str s = {0};
			int k = 1;
			char *d = strdup(m->src), *p = d;
			while (p && *p) {
				char *nl = strchr(p, '\n');
				if (nl) *nl = 0;
				char b[40];
				snprintf(b, sizeof b, "[%d] ", k++);
				s_add(&s, b);
				s_add(&s, p);
				s_add(&s, "\n");
				p = nl ? nl + 1 : NULL;
			}
			free(d);
			// one line per source, cut to fit
			m->nsln = 0;
			int cap = k;
			m->sln = malloc(cap * sizeof(char *));
			p = s.d;
			while (p && *p) {
				char *nl = strchr(p, '\n');
				if (nl) *nl = 0;
				m->sln[m->nsln++] = strdup(p);
				p = nl ? nl + 1 : NULL;
			}
			free(s.d);
			m->h += 6 + m->nsln * text_line_h(FS_SRC);
		}
	}
}

static float conv_height(Conv *c)
{
	float h = 0;
	for (int i = 0; i < c->n; i++) { msg_layout(&c->m[i]); h += c->m[i].h + 12; }
	return h;
}

static void draw_msg(Msg *m, float y)
{
	float lh = text_line_h(FS);
	if (m->role == 'U') {
		float x = TOP_W - 10 - m->bw;
		rrect(x, y, m->bw, m->h, 10, T.card2);
		float ty = y;
		if (m->att == 'P') {
			Picture *pic = m->att_path ? thumb(m->att_path, THUMB_W, THUMB_H, &m->thumb_fail) : NULL;
			float px = x + m->bw - 10 - THUMB_W;
			if (pic) pic_draw(pic, px, y + 6, THUMB_W, THUMB_H);
			else { rrect(px, y + 6, THUMB_W, THUMB_H, 6, T.card); text_draw_center("Photo", px + THUMB_W / 2, y + 6 + THUMB_H / 2 - 8, 0.5f, T.sub); }
			ty += THUMB_H + 6;
		}
		if (m->att == 'F') {
			text_draw_fit(att_label(m), x + 10, y + 6, FS, T.accent, m->bw - 20);
			ty += lh;
		}
		for (int i = 0; i < m->nln; i++) {
			float ly = ty + 6 + i * lh;
			if (ly > 240 || ly + lh < BAR_H) continue;
			text_draw(m->ln[i], x + 10, ly, FS, T.text);
		}
	} else {
		sprout(16, y + 11, 7, false);
		for (int i = 0; i < m->nln; i++) {
			float ly = y + i * lh;
			if (ly > 240 || ly + lh < BAR_H) continue;
			text_draw(m->ln[i], SPIKE_X, ly, FS, T.text);
		}
		float sy = y + m->nln * lh + 6;
		for (int i = 0; i < m->nsln; i++) {
			float ly = sy + i * text_line_h(FS_SRC);
			if (ly > 240 || ly < BAR_H - 10) continue;
			text_draw_fit(m->sln[i], SPIKE_X, ly, FS_SRC, T.accent, SPIKE_MAXW);
		}
	}
}

static void draw_thinking(float y)
{
	float bob = sinf(t_anim * 5) * 1.5f;
	sprout(16, y + 11 + bob, 7, false);
	for (int i = 0; i < 3; i++) {
		float a = 0.5f + 0.5f * sinf(t_anim * 6 - i * 0.8f);
		fan(SPIKE_X + 6 + i * 13, y + 10, 4, 4, 0, 6.2832f, rgba(170, 170, 176, (u8)(80 + 175 * a)));
	}
}

static void draw_top_bar(const char *title, const char *right)
{
	rect(0, 0, TOP_W, BAR_H, T.bg);
	rect(0, BAR_H - 1, TOP_W, 1, T.line);
	sprout(14, 15, 5.5f, blinking());
	float rw = right ? text_width(right, 0.42f) : 0;
	text_draw_fit(title, 28, 4, 0.5f, T.text, TOP_W - 40 - rw);
	if (right) text_draw(right, TOP_W - 8 - rw, 6, 0.42f, T.sub);
}

static void draw_welcome(float cy, const char *line1, const char *line2)
{
	float bob = sinf(t_anim * 2) * 2;
	sprout(200, cy + bob, 26, blinking());
	text_draw_center(line1, 200, cy + 44, 0.62f, T.text);
	if (line2) text_draw_center(line2, 200, cy + 68, 0.45f, T.sub);
}

// Draws a whole conversation bottom-aligned; scroll = px up from the bottom.
static void draw_conv(Conv *c, float *scroll, bool with_thinking, const char *err)
{
	float h = conv_height(c);
	float extra = 0;
	if (with_thinking) extra += 30;
	char **eln = NULL;
	int neln = 0;
	if (err && *err) {
		neln = text_wrap(err, 0.45f, SPIKE_MAXW, &eln);
		extra += neln * text_line_h(0.45f) + 26;
	}
	float view_h = 240 - BAR_H - 8;
	float total = h + extra;
	float maxs = total > view_h ? total - view_h : 0;
	*scroll = clampf(*scroll, 0, maxs);
	float y = 240 - 6 - total + *scroll;
	if (total < view_h) y = BAR_H + 8;
	for (int i = 0; i < c->n; i++) {
		Msg *m = &c->m[i];
		if (y + m->h >= BAR_H && y <= 240) draw_msg(m, y);
		y += m->h + 12;
	}
	if (with_thinking) { draw_thinking(y); y += 30; }
	if (neln) {
		rrect(SPIKE_X - 6, y - 2, SPIKE_MAXW + 4, neln * text_line_h(0.45f) + 8, 6, rgba(90, 30, 34, 255));
		for (int i = 0; i < neln; i++) text_draw(eln[i], SPIKE_X, y + 2 + i * text_line_h(0.45f), 0.45f, rgba(255, 190, 190, 255));
		text_draw("Press A to try again", SPIKE_X, y + 10 + neln * text_line_h(0.45f), 0.42f, T.sub);
		lines_free(eln, neln);
	}
	// scroll bar
	if (maxs > 0) {
		float vh = 240 - BAR_H;
		float bh = clampf(vh * view_h / total, 14, vh);
		float by = BAR_H + (vh - bh) * (1 - *scroll / maxs);
		rrect(TOP_W - 4, by, 3, bh, 1.5f, T.faint);
	}
}

// ---------------------------------------------------------------- drawing: bottom screen

#define BOT_W 320

typedef enum {
	A_NONE, A_TYPE, A_RETRY, A_CHATS, A_NEW, A_WEB, A_STYLE, A_SETTINGS, A_SOON,
	A_BACK, A_TAB_CHATS, A_TAB_PROJECTS, A_OPEN, A_RENAME, A_DELETE, A_EDITPROJ,
	A_TALK, A_PHOTO, A_ATTACH, A_UNATTACH, A_SNAP, A_FLIP, A_VOICE_SEND, A_CANCEL, A_UP,
	A_ROW = 100,
} Action;

typedef struct { float x, y, w, h; int act; } Hit;
static Hit hits[48];
static int nhits;
static int chat_focus = A_TYPE;   // chat button picked with the D-Pad

static void hit(float x, float y, float w, float h, int act)
{
	if (nhits < 48) hits[nhits++] = (Hit){ x, y, w, h, act };
}

static void button(float x, float y, float w, float h, const char *label, const char *hint, int act, int style)
{
	// style 0 normal, 1 primary, 2 disabled
	u32 bg = style == 1 ? T.accent : T.card;
	u32 fg = style == 2 ? T.faint : style == 1 ? rgba(255, 255, 255, 255) : T.text;
	if (view == V_CHAT && act == chat_focus && act != A_NONE) rrect(x - 3, y - 3, w + 6, h + 6, 12, rgba(255, 255, 255, 235));
	rrect(x, y, w, h, 9, bg);
	float fs = h >= 40 ? 0.55f : 0.47f;
	float ty = y + (hint && h >= 40 ? h / 2 - 15 : h / 2 - text_line_h(fs) / 2);
	text_draw_center(label, x + w / 2, ty, fs, fg);
	if (hint) {
		u32 hc = style == 1 ? rgba(255, 255, 255, 190) : T.faint;
		if (h >= 40) text_draw_center(hint, x + w / 2, ty + 20, 0.4f, hc);
		else text_draw(hint, x + w - 6 - text_width(hint, 0.36f), y + 2, 0.36f, hc);
	}
	hit(x, y, w, h, act);
}

static void draw_bottom_header(const char *title, const char *sub)
{
	rect(0, 0, BOT_W, 30, T.bar);
	rect(0, 30, BOT_W, 1, T.line);
	text_draw_fit(title, 10, 4, 0.55f, T.text, sub ? 170 : 300);
	if (sub) text_draw(sub, BOT_W - 10 - text_width(sub, 0.42f), 9, 0.42f, T.sub);
}

static void draw_footer(const char *hint)
{
	rect(0, 214, BOT_W, 26, T.bar);
	rect(0, 214, BOT_W, 1, T.line);
	text_draw_fit(hint, 10, 220, 0.42f, T.sub, 300);
}

static void draw_toast(void)
{
	if (!toast[0] || osGetTime() > toast_until) return;
	float w = text_width(toast, 0.45f) + 24;
	if (w > 300) w = 300;
	rrect(BOT_W / 2 - w / 2, 176, w, 28, 14, T.card2);
	text_draw_fit(toast, BOT_W / 2 - w / 2 + 12, 182, 0.45f, T.text, w - 24);
}

static void draw_chat_bottom(void)
{
	Project *p = proj_find(cur.project);
	char sub[64] = "";
	if (p) snprintf(sub, sizeof sub, "%s", p->name);
	rect(0, 0, BOT_W, 30, T.bar);
	rect(0, 30, BOT_W, 1, T.line);
	text_draw_fit(cur.n ? cur.title : "New chat", 10, p ? 1 : 5, p ? 0.5f : 0.55f, T.text, 214);
	if (p) text_draw_fit(sub, 10, 17, 0.36f, T.sub, 214);
	button(232, 3, 82, 24, "Settings", NULL, A_SETTINGS, 0);

	if (thinking) button(12, 42, 296, 58, "Spike is thinking...", NULL, A_NONE, 2);
	else if (chat_err[0]) {
		button(12, 42, 144, 58, "Try again", "A", A_RETRY, 1);
		button(164, 42, 144, 58, "Type", "touch", A_TYPE, 0);
	} else if (pend_att) button(12, 42, 296, 58, pend_att == 'P' ? "Ask about the photo" : "Ask about the file", "A", A_TYPE, 1);
	else button(12, 42, 296, 58, "Type a message", "A", A_TYPE, 1);

	if (pend_att) {
		const char *b = strrchr(pend_path, '/');
		char lbl[96];
		snprintf(lbl, sizeof lbl, "%s %s", pend_att == 'P' ? "Photo:" : "File:", b ? b + 1 : pend_path);
		rrect(12, 110, 210, 44, 9, T.card);
		text_draw_fit(lbl, 22, 123, 0.47f, T.accent, 190);
		button(228, 110, 80, 44, "Remove", "B", A_UNATTACH, 0);
	} else {
		int st = thinking ? 2 : 0;
		button(12, 110, 94, 44, "Talk", "R", A_TALK, st);
		button(113, 110, 94, 44, "Photo", NULL, A_PHOTO, st);
		button(214, 110, 94, 44, "Attach", NULL, A_ATTACH, st);
	}

	char web[24], style[32];
	snprintf(web, sizeof web, "Web %s", cfg.web ? "on" : "off");
	snprintf(style, sizeof style, "%s", P_NAME[persona_for(cur.project)]);
	button(12, 164, 70, 42, "Chats", "B", A_CHATS, 0);
	button(87, 164, 70, 42, "New", "X", A_NEW, 0);
	button(162, 164, 70, 42, web, "Y", A_WEB, cfg.web ? 1 : 0);
	button(237, 164, 71, 42, style, "L", A_STYLE, 0);
	draw_footer("D-Pad move  A press  Circle Pad scroll  START exit");
	draw_toast();
}

// ---- lists

typedef enum { R_NEWCHAT, R_CHAT, R_NEWPROJ, R_PROJ, R_SET, R_PSET, R_UP, R_DIR, R_FILE, R_MCPSET, R_MCPSRV, R_SKILL } RowKind;
typedef struct { RowKind kind; int ref; char title[80]; char sub[96]; } Row;
static Row rows[MAX_CHATS + MAX_FILES + 8];

enum { S_RELAY, S_PERSONA, S_CUSTOM, S_WEB, S_MCP, S_SKILLS, S_VERIFY, S_TEST, S_COUNT };
enum { PS_NAME, PS_PERSONA, PS_INSTR, PS_DELETE, PS_COUNT };

static void date_str(long long t, char *out, size_t n)
{
	if (!t) { copy(out, n, ""); return; }
	time_t now = time(NULL), tt = (time_t)t;
	struct tm a = *gmtime(&now), b = *gmtime(&tt);
	static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) snprintf(out, n, "Today %d:%02d", b.tm_hour, b.tm_min);
	else if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday + 1) snprintf(out, n, "Yesterday");
	else snprintf(out, n, "%s %d", mon[b.tm_mon], b.tm_mday);
}

static void yesno(bool v, char *out, size_t n) { copy(out, n, v ? "On" : "Off"); }

static int build_rows(View v)
{
	int n = 0;
	if (v == V_CHATS || v == V_PROJECT) {
		Row *r = &rows[n++];
		r->kind = R_NEWCHAT;
		r->ref = v == V_PROJECT ? open_project : 0;
		copy(r->title, sizeof r->title, "+ New chat");
		r->sub[0] = 0;
		for (int i = 0; i < nchats; i++) {
			if (v == V_PROJECT && chats[i].project != open_project) continue;
			r = &rows[n++];
			r->kind = R_CHAT;
			r->ref = chats[i].id;
			copy(r->title, sizeof r->title, chats[i].title);
			char d[32];
			date_str(chats[i].updated, d, sizeof d);
			Project *p = v == V_CHATS ? proj_find(chats[i].project) : NULL;
			if (p) snprintf(r->sub, sizeof r->sub, "%s  -  %s", d, p->name);
			else copy(r->sub, sizeof r->sub, d);
		}
	} else if (v == V_PROJECTS) {
		Row *r = &rows[n++];
		r->kind = R_NEWPROJ;
		copy(r->title, sizeof r->title, "+ New project");
		r->sub[0] = 0;
		for (int i = 0; i < nprojects; i++) {
			r = &rows[n++];
			r->kind = R_PROJ;
			r->ref = projects[i].id;
			copy(r->title, sizeof r->title, projects[i].name);
			int c = proj_chat_count(projects[i].id);
			snprintf(r->sub, sizeof r->sub, "%d chat%s  -  %s", c, c == 1 ? "" : "s",
				projects[i].persona >= 0 ? P_NAME[projects[i].persona] : "Default personality");
		}
	} else if (v == V_SETTINGS) {
		for (int i = 0; i < S_COUNT; i++) {
			Row *r = &rows[n++];
			r->kind = R_SET;
			r->ref = i;
			r->sub[0] = 0;
			switch (i) {
			case S_RELAY: copy(r->title, sizeof r->title, "Relay address"); copy(r->sub, sizeof r->sub, cfg.relay[0] ? cfg.relay : "Not set"); break;
			case S_PERSONA: copy(r->title, sizeof r->title, "Personality"); copy(r->sub, sizeof r->sub, P_NAME[cfg.persona]); break;
			case S_CUSTOM: copy(r->title, sizeof r->title, "Custom instructions"); copy(r->sub, sizeof r->sub, cfg.custom[0] ? cfg.custom : "Empty"); break;
			case S_WEB: copy(r->title, sizeof r->title, "Look things up online"); yesno(cfg.web, r->sub, sizeof r->sub); break;
			case S_VERIFY: copy(r->title, sizeof r->title, "Certificate check"); yesno(cfg.verify, r->sub, sizeof r->sub); break;
			case S_MCP: copy(r->title, sizeof r->title, "MCP tools  (@)"); copy(r->sub, sizeof r->sub, "Type @zapier, @firecrawl... in a message"); break;
			case S_SKILLS: copy(r->title, sizeof r->title, "Skills  (/)"); copy(r->sub, sizeof r->sub, "Start a message with /name"); break;
			case S_TEST: copy(r->title, sizeof r->title, "Test connection"); copy(r->sub, sizeof r->sub, ping_status[0] ? ping_status : "Checks the relay and password"); break;
			}
			for (char *q = r->sub; *q; q++) if (*q == '\n') *q = ' ';
		}
	} else if (v == V_MCP) {
		Row *r = &rows[n++];
		r->kind = R_MCPSET; r->ref = 1;
		copy(r->title, sizeof r->title, "Check servers");
		copy(r->sub, sizeof r->sub, mcp_status[0] ? mcp_status : "See what's connected to your relay");
		for (int i = 0; i < nmcps; i++) {
			r = &rows[n++];
			r->kind = R_MCPSRV; r->ref = i;
			copy(r->title, sizeof r->title, mcps[i].name);
			if (!strcmp(mcps[i].count, "ERR")) snprintf(r->sub, sizeof r->sub, "Not working: %s", mcps[i].detail);
			else snprintf(r->sub, sizeof r->sub, "%s tools  -  A: write @%s", mcps[i].count, mcps[i].name);
		}
	} else if (v == V_SKILLS) {
		for (int i = 0; i < nskills; i++) {
			Row *r = &rows[n++];
			r->kind = R_SKILL; r->ref = i;
			snprintf(r->title, sizeof r->title, "/%s", skills[i].name);
			copy(r->sub, sizeof r->sub, skills[i].desc);
		}
	} else if (v == V_FILES) {
		if (strcmp(cwd, "sdmc:/")) {
			Row *r = &rows[n++];
			r->kind = R_UP; r->ref = -1;
			copy(r->title, sizeof r->title, ".. (back up)");
			r->sub[0] = 0;
		}
		for (int i = 0; i < nfiles; i++) {
			Row *r = &rows[n++];
			r->kind = files[i].dir ? R_DIR : R_FILE;
			r->ref = i;
			snprintf(r->title, sizeof r->title, "%s%s", files[i].name, files[i].dir ? "/" : "");
			r->sub[0] = 0;
		}
	} else if (v == V_PROJEDIT) {
		Project *p = proj_find(open_project);
		for (int i = 0; p && i < PS_COUNT; i++) {
			Row *r = &rows[n++];
			r->kind = R_PSET;
			r->ref = i;
			r->sub[0] = 0;
			switch (i) {
			case PS_NAME: copy(r->title, sizeof r->title, "Name"); copy(r->sub, sizeof r->sub, p->name); break;
			case PS_PERSONA: copy(r->title, sizeof r->title, "Personality"); copy(r->sub, sizeof r->sub, p->persona >= 0 ? P_NAME[p->persona] : "Default (from Settings)"); break;
			case PS_INSTR: copy(r->title, sizeof r->title, "Instructions"); copy(r->sub, sizeof r->sub, p->instr[0] ? p->instr : "What Spike should know for this project"); break;
			case PS_DELETE: copy(r->title, sizeof r->title, "Delete project"); copy(r->sub, sizeof r->sub, "Also deletes its chats"); break;
			}
			for (char *q = r->sub; *q; q++) if (*q == '\n') *q = ' ';
		}
	}
	return n;
}

#define LIST_Y 36
#define ROW_H 35
#define LIST_ROWS 5

static void fix_scroll(View v, int n)
{
	if (sel[v] >= n) sel[v] = n - 1;
	if (sel[v] < 0) sel[v] = 0;
	if (sel[v] < scrl[v]) scrl[v] = sel[v];
	if (sel[v] >= scrl[v] + LIST_ROWS) scrl[v] = sel[v] - LIST_ROWS + 1;
	if (scrl[v] > n - LIST_ROWS) scrl[v] = n - LIST_ROWS;
	if (scrl[v] < 0) scrl[v] = 0;
}

static void draw_list(View v, int n)
{
	fix_scroll(v, n);
	for (int k = 0; k < LIST_ROWS && scrl[v] + k < n; k++) {
		int i = scrl[v] + k;
		Row *r = &rows[i];
		float y = LIST_Y + k * ROW_H;
		bool on = i == sel[v];
		if (on) rrect(6, y, BOT_W - 12, ROW_H - 3, 8, T.card2);
		bool arm = armed == i && armed_view == v;
		u32 tc = arm ? rgba(255, 120, 120, 255) : (r->kind == R_NEWCHAT || r->kind == R_NEWPROJ) ? T.accent : T.text;
		if (r->kind == R_PSET && r->ref == PS_DELETE) tc = rgba(255, 120, 120, 255);
		if (r->sub[0]) {
			text_draw_fit(arm ? "Press Y again to delete" : r->title, 16, y + 2, 0.5f, tc, BOT_W - 40);
			text_draw_fit(r->sub, 16, y + 17, 0.4f, T.sub, BOT_W - 40);
		} else text_draw_fit(r->title, 16, y + 8, 0.5f, tc, BOT_W - 40);
		if (r->kind == R_PROJ || (r->kind == R_SET && (r->ref == S_PERSONA)) || (r->kind == R_PSET && r->ref == PS_PERSONA))
			text_draw(r->kind == R_PROJ ? ">" : "<>", BOT_W - 26, y + 9, 0.45f, T.faint);
		hit(6, y, BOT_W - 12, ROW_H - 3, A_ROW + i);
	}
	if (n > LIST_ROWS) {
		float vh = LIST_ROWS * ROW_H;
		float bh = vh * LIST_ROWS / n;
		float by = LIST_Y + (vh - bh) * scrl[v] / (n - LIST_ROWS);
		rrect(BOT_W - 4, by, 3, bh, 1.5f, T.faint);
	}
}

static void draw_tabs(bool chats_tab)
{
	rect(0, 0, BOT_W, 30, T.bar);
	rect(0, 30, BOT_W, 1, T.line);
	const char *names[2] = { "Chats", "Projects" };
	for (int i = 0; i < 2; i++) {
		bool on = (i == 0) == chats_tab;
		float cx = 80 + i * 160;
		text_draw_center(names[i], cx, 5, 0.55f, on ? T.text : T.faint);
		if (on) rrect(cx - 34, 26, 68, 3, 1.5f, T.accent);
		hit(i * 160, 0, 160, 30, i == 0 ? A_TAB_CHATS : A_TAB_PROJECTS);
	}
	text_draw("L", 8, 8, 0.4f, T.faint);
	text_draw("R", BOT_W - 14, 8, 0.4f, T.faint);
}

static void draw_bottom(void)
{
	nhits = 0;
	int n;
	switch (view) {
	case V_CHAT: draw_chat_bottom(); return;
	case V_CHATS:
		n = build_rows(V_CHATS);
		draw_tabs(true);
		draw_list(V_CHATS, n);
		draw_footer(rows[sel[V_CHATS]].kind == R_CHAT ? "A open   X rename   Y delete   B back" : "A start a new chat   B back");
		break;
	case V_PROJECTS:
		n = build_rows(V_PROJECTS);
		draw_tabs(false);
		draw_list(V_PROJECTS, n);
		draw_footer(rows[sel[V_PROJECTS]].kind == R_PROJ ? "A open   X rename   Y delete   B back" : "A create a project   B back");
		break;
	case V_PROJECT: {
		Project *p = proj_find(open_project);
		n = build_rows(V_PROJECT);
		draw_bottom_header(p ? p->name : "Project", "SELECT edit");
		hit(200, 0, 120, 30, A_EDITPROJ);
		draw_list(V_PROJECT, n);
		draw_footer(rows[sel[V_PROJECT]].kind == R_CHAT ? "A open   X rename   Y delete   B back" : "A new chat here   SELECT edit project   B back");
		break;
	}
	case V_PROJEDIT: {
		Project *p = proj_find(open_project);
		n = build_rows(V_PROJEDIT);
		char t[80];
		snprintf(t, sizeof t, "Edit %s", p ? p->name : "project");
		draw_bottom_header(t, NULL);
		draw_list(V_PROJEDIT, n);
		draw_footer("A change   Left/Right personality   B back");
		break;
	}
	case V_FILES: {
		n = build_rows(V_FILES);
		char t[120];
		snprintf(t, sizeof t, "%s", strlen(cwd) > 6 ? cwd + 5 : "SD card");
		draw_bottom_header(t, NULL);
		if (n) draw_list(V_FILES, n);
		else text_draw_center("No pictures or .txt files here", BOT_W / 2, 110, 0.47f, T.sub);
		draw_footer("A open / pick   B back   (JPG, PNG, TXT)");
		break;
	}
	case V_MCP:
		n = build_rows(V_MCP);
		draw_bottom_header("MCP tools", NULL);
		draw_list(V_MCP, n);
		draw_footer("A use it / check   B back");
		break;
	case V_SKILLS:
		n = build_rows(V_SKILLS);
		draw_bottom_header("Skills", NULL);
		if (n) draw_list(V_SKILLS, n);
		else text_draw_center("No skills yet", BOT_W / 2, 110, 0.47f, T.sub);
		draw_footer("A use it   B back");
		break;
	case V_CAMERA:
		draw_bottom_header("Photo", NULL);
		button(12, 42, 296, 80, "Snap", "A", A_SNAP, 1);
		button(12, 132, 144, 70, cam_inner ? "Back camera" : "Selfie camera", "X", A_FLIP, 0);
		button(164, 132, 144, 70, "Cancel", "B", A_CANCEL, 0);
		draw_footer("The top screen shows what the camera sees");
		break;
	case V_TALK: {
		draw_bottom_header("Talk", NULL);
		char t[64];
		snprintf(t, sizeof t, "%.0f / %d s", mic_seconds(), MIC_MAX_SECONDS);
		text_draw_center(t, BOT_W / 2, 44, 0.5f, T.sub);
		button(12, 70, 296, 80, "Send", "A or R", A_VOICE_SEND, 1);
		button(12, 158, 296, 46, "Cancel", "B", A_CANCEL, 0);
		draw_footer("Talk in English or Spanish, then press Send");
		break;
	}
	case V_SETTINGS:
		n = build_rows(V_SETTINGS);
		draw_bottom_header("Settings", "Spike AI 1.0");
		draw_list(V_SETTINGS, n);
		draw_footer("A change   Left/Right personality   B back");
		break;
	}
	draw_toast();
}

// ---------------------------------------------------------------- drawing: top screen per view

static bool is_pic(const char *n);
static void file_path(int i, char *out, size_t n);

static void draw_text_panel(const char *title, const char *body, const char *note)
{
	sprout(40, 66, 20, blinking());
	text_draw_fit(title, 76, 44, 0.65f, T.text, 310);
	if (note) text_draw_fit(note, 76, 70, 0.45f, T.sub, 310);
	if (body && *body) {
		rrect(12, 108, 376, 124, 10, T.card);
		text_draw_wrapped(body, 24, 116, 0.47f, T.text, 352, 7);
	}
}

static void draw_top(void)
{
	char right[64];
	switch (view) {
	case V_CHAT: {
		Project *p = proj_find(cur.project);
		snprintf(right, sizeof right, "%s%s%s", p ? p->name : "", p ? "  |  " : "", P_NAME[persona_for(cur.project)]);
		if (!cur.n && !thinking) draw_welcome(100, "Hi! I'm Spike.", "A type, R talk, Left photo, Right attach. English y español.");
		else draw_conv(&cur, &chat_scroll, thinking && thinking_chat == cur.id, chat_err);
		draw_top_bar(cur.n ? cur.title : "Spike", right);
		break;
	}
	case V_CHATS:
	case V_PROJECT: {
		Row *r = &rows[sel[view]];
		if (r->kind == R_CHAT) {
			if (preview.id != r->ref) { conv_load(&preview, r->ref); }
			float s = 0;
			draw_conv(&preview, &s, false, NULL);
			draw_top_bar(preview.title, "Preview");
		} else if (view == V_PROJECT) {
			Project *p = proj_find(open_project);
			if (p) {
				char note[96];
				snprintf(note, sizeof note, "Personality: %s", p->persona >= 0 ? P_NAME[p->persona] : "Default");
				draw_text_panel(p->name, p->instr[0] ? p->instr : "No instructions yet. Press SELECT to tell Spike what this project is about.", note);
			}
			draw_top_bar("Project", NULL);
		} else {
			draw_welcome(96, "Your chats", "Old chats you don't need? Press Y twice to delete.");
			draw_top_bar("Spike", NULL);
		}
		break;
	}
	case V_PROJECTS: {
		Row *r = &rows[sel[view]];
		Project *p = r->kind == R_PROJ ? proj_find(r->ref) : NULL;
		if (p) {
			char note[96];
			snprintf(note, sizeof note, "Personality: %s", p->persona >= 0 ? P_NAME[p->persona] : "Default");
			draw_text_panel(p->name, p->instr[0] ? p->instr : "No instructions yet.", note);
		} else draw_text_panel("Projects", "A project is a folder for chats about one thing, like a game you're playing or your gym plan. Give it instructions and a personality, and every chat inside it follows them.", NULL);
		draw_top_bar("Projects", NULL);
		break;
	}
	case V_SETTINGS: {
		Row *r = &rows[sel[view]];
		const char *body = "";
		char tmp[1400];
		switch (r->ref) {
		case S_RELAY: body = "The address of your Spike relay on Cloudflare, like spike-ai.yourname.workers.dev"; break;
		case S_PERSONA:
			snprintf(tmp, sizeof tmp, "%s: %s\n\nUsed in every chat, except projects that have their own personality. Press Left/Right to change it.", P_NAME[cfg.persona], P_DESC[cfg.persona]);
			body = tmp;
			break;
		case S_CUSTOM:
			snprintf(tmp, sizeof tmp, "Used when the personality is Custom.\n\n%s", cfg.custom[0] ? cfg.custom : "Example: Answer like a pirate and keep it under 3 sentences.");
			body = tmp;
			break;
		case S_WEB: body = "When on, Spike decides when to search the web and reads the actual sites before answering (products, nutrition, games, news, prices). Answers take a few seconds longer. The Searcher personality always searches."; break;
		case S_VERIFY: body = "Checks the relay's security certificate. Leave it on. Turn it off only if the 3DS clock is wrong and you get certificate errors."; break;
		case S_MCP: body = "MCP lets Spike use other services' tools: Firecrawl for reading websites, Zapier for your apps (WhatsApp, Gmail, Calendar...). Tag one in a message, like @zapier, to use it. Press A to see what's connected."; break;
		case S_SKILLS: body = "Skills change how Spike answers. Start a message with /name, like /truth or /research. Press A to see your skills.\n\nYour own: add .txt files to sdmc:/3ds/SpikeAI/skills"; break;
		case S_TEST: body = ping_status[0] ? ping_status : "Press A to check that Spike can reach your relay."; break;
		}
		draw_text_panel("Settings", body, NULL);
		draw_top_bar("Settings", NULL);
		break;
	}
	case V_FILES: {
		Row *r = build_rows(V_FILES) ? &rows[sel[V_FILES]] : NULL;
		if (r && r->kind == R_FILE) {
			char path[300];
			file_path(r->ref, path, sizeof path);
			if (is_pic(path)) {
				static char last[300];
				static bool failed;
				if (strcmp(last, path)) { copy(last, sizeof last, path); failed = false; }
				Picture *pic = thumb(path, 320, 200, &failed);
				if (pic) pic_draw(pic, 40, 32, 320, 200);
				else text_draw_center(failed ? "Can't preview this one (too big?)" : "Loading...", 200, 120, 0.5f, T.sub);
			} else {
				static char last[300], prev[1600];
				if (strcmp(last, path)) {
					copy(last, sizeof last, path);
					size_t n = 0;
					unsigned char *d = file_read(path, 300000, &n);
					copy(prev, sizeof prev, d ? (char *)d : "(can't read this file)");
					free(d);
				}
				draw_text_panel(files[r->ref].name, prev, "Press A to attach");
			}
		} else draw_text_panel("Attach a file", "Pick a JPG or PNG picture, or a .txt file, from your SD card. Spike can look at pictures and read text files. Your 3DS photos are in the DCIM folder.", NULL);
		draw_top_bar("SD card", NULL);
		break;
	}
	case V_MCP: {
		int n = build_rows(V_MCP);
		Row *r = n ? &rows[sel[V_MCP]] : NULL;
		if (r && r->kind == R_MCPSRV) {
			McpSrv *m = &mcps[r->ref];
			char body[1300];
			snprintf(body, sizeof body, "%s", strcmp(m->count, "ERR") ? m->detail : m->detail);
			draw_text_panel(m->name, body, strcmp(m->count, "ERR") ? "Tools Spike can use:" : "This server isn't working:");
		} else draw_text_panel("MCP tools", "Tag a server in your message to use its tools:\n  @zapier send me a WhatsApp saying hi\n  @firecrawl what's on nintendo.com today?\n  @all uses every server.\nWithout an @, Spike doesn't touch your tools. Spike asks before sending or changing anything.\n\nAdd servers in Cloudflare (worker Settings, Variables): FIRECRAWL_KEY, ZAPIER_TOKEN, MCP_SERVERS.", "How to use");
		draw_top_bar("MCP", NULL);
		break;
	}
	case V_SKILLS: {
		int n = build_rows(V_SKILLS);
		if (n) {
			Skill *k = &skills[rows[sel[V_SKILLS]].ref];
			char p[300], title[40];
			snprintf(p, sizeof p, BASE "/skills/%s.txt", k->name);
			size_t len = 0;
			char *t = (char *)file_read(p, 8000, &len);
			char body[1300];
			snprintf(body, sizeof body, "%s", t ? t : k->desc);
			free(t);
			snprintf(title, sizeof title, "/%s", k->name);
			draw_text_panel(title, body, "Press A to write a message with it");
		} else draw_text_panel("Skills", "Skills are instructions Spike follows when your message starts with /name, like /truth or /research.\n\nAdd your own: put a .txt file in sdmc:/3ds/SpikeAI/skills. The first line says what it does, the rest tells Spike how to answer.", NULL);
		draw_top_bar("Skills", NULL);
		break;
	}
	case V_CAMERA:
		cam_draw(0, 0);
		rrect(150, 214, 100, 20, 10, rgba(0, 0, 0, 140));
		text_draw_center("A = snap", 200, 216, 0.45f, rgba(255, 255, 255, 255));
		break;
	case V_TALK: {
		float lv = mic_level();
		float pulse = 30 + lv * 40 + sinf(t_anim * 4) * 3;
		fan(200, 104, pulse, pulse, 0, 6.2832f, rgba(74, 144, 255, 60));
		fan(200, 104, pulse * 0.75f, pulse * 0.75f, 0, 6.2832f, rgba(74, 144, 255, 80));
		sprout(200, 108, 24, blinking());
		text_draw_center("I'm listening...", 200, 176, 0.62f, T.text);
		for (int i = 0; i < 24; i++) {
			float h = 4 + lv * 24 * (0.5f + 0.5f * sinf(t_anim * 9 + i * 0.9f));
			rrect(110 + i * 8, 216 - h / 2, 4, h, 2, T.accent);
		}
		draw_top_bar("Talk to Spike", NULL);
		break;
	}
	case V_PROJEDIT: {
		Project *p = proj_find(open_project);
		if (p) {
			char note[96];
			snprintf(note, sizeof note, "Personality: %s", p->persona >= 0 ? P_NAME[p->persona] : "Default");
			draw_text_panel(p->name, p->instr[0] ? p->instr : "Tell Spike what this project is about. Example: I'm playing Zelda TotK, no spoilers past the Water Temple.", note);
		}
		draw_top_bar("Edit project", NULL);
		break;
	}
	}
}

// ---------------------------------------------------------------- actions

static void go_chat(void) { view = V_CHAT; armed = -1; }

static bool is_pic(const char *n)
{
	const char *e = strrchr(n, '.');
	return e && (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg") || !strcasecmp(e, ".png"));
}

static bool is_txt(const char *n)
{
	const char *e = strrchr(n, '.');
	return e && (!strcasecmp(e, ".txt") || !strcasecmp(e, ".md") || !strcasecmp(e, ".csv"));
}

static int cmp_file(const void *a, const void *b)
{
	const FileEnt *x = a, *y = b;
	if (x->dir != y->dir) return x->dir ? -1 : 1;
	return strcasecmp(x->name, y->name);
}

static void load_dir(void)
{
	nfiles = 0;
	DIR *d = opendir(cwd);
	if (!d) { copy(cwd, sizeof cwd, "sdmc:/"); d = opendir(cwd); }
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) && nfiles < MAX_FILES) {
		if (e->d_name[0] == '.') continue;
		bool dir = e->d_type == DT_DIR;
		if (!dir && !is_pic(e->d_name) && !is_txt(e->d_name)) continue;
		copy(files[nfiles].name, sizeof files[nfiles].name, e->d_name);
		files[nfiles].dir = dir;
		nfiles++;
	}
	closedir(d);
	qsort(files, nfiles, sizeof *files, cmp_file);
	sel[V_FILES] = scrl[V_FILES] = 0;
}

static void file_path(int i, char *out, size_t n)
{
	size_t k = strlen(cwd);
	snprintf(out, n, "%s%s%s", cwd, k && cwd[k - 1] == '/' ? "" : "/", files[i].name);
}

static void dir_up(void)
{
	size_t k = strlen(cwd);
	if (k && cwd[k - 1] == '/') cwd[--k] = 0;
	char *s = strrchr(cwd, '/');
	if (s && s - cwd >= 5) s[1] = 0;
	else copy(cwd, sizeof cwd, "sdmc:/");
	if (strncmp(cwd, "sdmc:/", 6)) copy(cwd, sizeof cwd, "sdmc:/");
	load_dir();
}

static void open_files(void)
{
	if (thinking) return;
	load_dir();
	view = V_FILES;
}

static void open_camera(void)
{
	if (thinking) return;
	if (!cam_start(cam_inner)) { cam_stop(); say("The camera didn't start."); return; }
	view = V_CAMERA;
}

static void close_camera(void)
{
	cam_stop();
	go_chat();
}

static void type_message(const char *pre);

static void attach(char kind, const char *path)
{
	pend_att = kind;
	copy(pend_path, sizeof pend_path, path);
	go_chat();
	type_message(NULL);
}

static void snap(void)
{
	char path[128];
	snprintf(path, sizeof path, BASE "/att/%lld.png", (long long)osGetTime());
	bool ok = cam_save_png(path);
	cam_stop();
	if (!ok) { say("Couldn't save the photo."); go_chat(); return; }
	attach('P', path);
}

static void start_talk(void)
{
	if (thinking) return;
	if (!mic_start()) { say("The microphone didn't start."); return; }
	voice_err[0] = 0;
	view = V_TALK;
}

static void cancel_talk(void)
{
	mic_cancel();
	go_chat();
}

static void send_voice(void)
{
	size_t n = 0;
	unsigned char *wav = mic_stop(&n);
	go_chat();
	if (!wav) { say("That was too short. Hold on a bit longer."); return; }
	free(voice_b64);
	voice_b64 = b64_encode(wav, n);
	free(wav);
	if (!voice_b64) { say("Out of memory."); return; }
	conv_add(&cur, 'U', VOICE_PH);
	cur_commit();
	send_chat();
}

static void new_chat(int project)
{
	conv_clear(&cur);
	cur.project = project;
	chat_err[0] = 0;
	chat_scroll = 0;
	go_chat();
}

static void open_chat(int id)
{
	if (cur.id != id) {
		conv_load(&cur, id);
		chat_err[0] = 0;
		// an unanswered last message (e.g. the app was closed): offer a retry
		if (cur.n && cur.m[cur.n - 1].role == 'U' && !(thinking && thinking_chat == id))
			copy(chat_err, sizeof chat_err, "This message never got an answer.");
		cfg.last_chat = id;
		cfg_save();
	}
	chat_scroll = 0;
	go_chat();
}

static void type_message(const char *pre)
{
	if (thinking) return;
	if (pend_att) {
		if (!kb(pend_att == 'P' ? "Ask about this photo (or just Send)" : "Ask about this file (or just Send)", pre, 1000, true, false, "Send")) return;
	} else if (!kb("Message Spike  (@tool  /skill)", pre, 1000, true, true, "Send")) return;
	if (kbuf[0] == '/') {
		char nm[32];
		char *sk = skill_for(kbuf, nm, sizeof nm);
		if (sk) free(sk);
		else if (nm[0]) { char t[80]; snprintf(t, sizeof t, "No skill called /%s (see Settings > Skills)", nm); say(t); }
	}
	// first message of a new chat: create it now so the answer has a home
	Msg *m = conv_add(&cur, 'U', kbuf);
	if (pend_att) {
		m->att = pend_att;
		m->att_path = strdup(pend_path);
		if (!kbuf[0] && pend_att == 'F') { free(m->text); m->text = strdup("Read this file and tell me what it's about."); }
		pend_att = 0;
	}
	cur_commit();
	send_chat();
}

static void retry(void)
{
	if (thinking || !cur.n) return;
	if (cur.m[cur.n - 1].role != 'U') return;
	send_chat();
}

static void rename_row(Row *r)
{
	if (r->kind == R_CHAT) {
		ChatInfo *ci = chat_find(r->ref);
		if (!ci || !kb("Chat name", ci->title, 60, false, true, "Save")) return;
		copy(ci->title, sizeof ci->title, kbuf);
		if (cur.id == ci->id) copy(cur.title, sizeof cur.title, kbuf);
		if (preview.id == ci->id) copy(preview.title, sizeof preview.title, kbuf);
		index_save();
		say("Renamed");
	} else if (r->kind == R_PROJ) {
		Project *p = proj_find(r->ref);
		if (!p || !kb("Project name", p->name, 40, false, true, "Save")) return;
		copy(p->name, sizeof p->name, kbuf);
		index_save();
		say("Renamed");
	}
}

static void project_delete(int id)
{
	for (int i = nchats - 1; i >= 0; i--) if (chats[i].project == id) chat_delete(chats[i].id);
	for (int i = 0; i < nprojects; i++)
		if (projects[i].id == id) { memmove(&projects[i], &projects[i + 1], (nprojects - i - 1) * sizeof *projects); nprojects--; break; }
	if (cur.project == id) { conv_clear(&cur); }
	index_save();
}

static void delete_row(View v, int i)
{
	Row *r = &rows[i];
	if (r->kind != R_CHAT && r->kind != R_PROJ && !(r->kind == R_PSET && r->ref == PS_DELETE)) return;
	if (armed != i || armed_view != v) { armed = i; armed_view = v; return; }
	armed = -1;
	if (r->kind == R_CHAT) { chat_delete(r->ref); say("Chat deleted"); }
	else if (r->kind == R_PROJ) { project_delete(r->ref); say("Project deleted"); }
	else { project_delete(open_project); view = V_PROJECTS; say("Project deleted"); }
}

static void new_project(void)
{
	if (nprojects >= MAX_PROJECTS) { say("That's a lot of projects. Delete one first."); return; }
	if (!kb("Project name", "", 40, false, true, "Create")) return;
	int m = 0;
	for (int i = 0; i < nprojects; i++) if (projects[i].id > m) m = projects[i].id;
	Project *p = &projects[nprojects++];
	memset(p, 0, sizeof *p);
	p->id = m + 1;
	p->persona = -1;
	copy(p->name, sizeof p->name, kbuf);
	index_save();
	open_project = p->id;
	view = V_PROJEDIT;
	sel[V_PROJEDIT] = PS_INSTR;
	say("Project created. Add instructions?");
}

static void cycle_persona(int *p, int dir, bool allow_default)
{
	int lo = allow_default ? -1 : 0;
	*p += dir;
	if (*p >= NPERSONA) *p = lo;
	if (*p < lo) *p = NPERSONA - 1;
}

static void setting_change(int s, int dir)
{
	switch (s) {
	case S_RELAY:
		if (dir) return;
		if (kb("Relay address (spike-ai.you.workers.dev)", cfg.relay, 190, false, false, "Save")) { copy(cfg.relay, sizeof cfg.relay, kbuf); ping_status[0] = 0; }
		break;
	case S_PERSONA: cycle_persona(&cfg.persona, dir ? dir : 1, false); break;
	case S_CUSTOM:
		if (dir) return;
		if (kb("How should Spike talk and answer?", cfg.custom, 1000, true, false, "Save")) {
			copy(cfg.custom, sizeof cfg.custom, kbuf);
			if (cfg.custom[0]) { cfg.persona = 5; say("Personality set to Custom"); }
		}
		break;
	case S_WEB: cfg.web = !cfg.web; break;
	case S_VERIFY: cfg.verify = !cfg.verify; net_set_verify(cfg.verify); break;
	case S_TEST: if (!dir) send_ping(); return;
	case S_MCP:
		if (dir) return;
		view = V_MCP; sel[V_MCP] = 0;
		if (!nmcps) send_mcp_status();
		return;
	case S_SKILLS:
		if (dir) return;
		load_skills();
		view = V_SKILLS; sel[V_SKILLS] = 0;
		return;
	}
	cfg_save();
}

static void projset_change(int s, int dir)
{
	Project *p = proj_find(open_project);
	if (!p) return;
	switch (s) {
	case PS_NAME:
		if (dir) return;
		if (kb("Project name", p->name, 40, false, true, "Save")) copy(p->name, sizeof p->name, kbuf);
		break;
	case PS_PERSONA: cycle_persona(&p->persona, dir ? dir : 1, true); break;
	case PS_INSTR:
		if (dir) return;
		if (kb("What should Spike know for this project?", p->instr, 1500, true, false, "Save")) copy(p->instr, sizeof p->instr, kbuf);
		break;
	case PS_DELETE: return;
	}
	index_save();
}

static void activate_row(View v, int i)
{
	Row *r = &rows[i];
	switch (r->kind) {
	case R_NEWCHAT: new_chat(r->ref); break;
	case R_CHAT: open_chat(r->ref); break;
	case R_NEWPROJ: new_project(); break;
	case R_PROJ: open_project = r->ref; view = V_PROJECT; sel[V_PROJECT] = 0; scrl[V_PROJECT] = 0; break;
	case R_SET: setting_change(r->ref, 0); break;
	case R_PSET:
		if (r->ref == PS_DELETE) delete_row(v, i);
		else projset_change(r->ref, 0);
		break;
	case R_UP: dir_up(); break;
	case R_DIR: {
		char p[300];
		file_path(r->ref, p, sizeof p);
		snprintf(cwd, sizeof cwd, "%s/", p);
		load_dir();
		break;
	}
	case R_MCPSET: send_mcp_status(); break;
	case R_MCPSRV: {
		if (!strcmp(mcps[r->ref].count, "ERR")) { say("That server isn't working right now"); break; }
		char pre[48];
		snprintf(pre, sizeof pre, "@%s ", mcps[r->ref].name);
		go_chat();
		type_message(pre);
		break;
	}
	case R_SKILL: {
		char pre[48];
		snprintf(pre, sizeof pre, "/%s ", skills[r->ref].name);
		go_chat();
		type_message(pre);
		break;
	}
	case R_FILE: {
		char p[300];
		file_path(r->ref, p, sizeof p);
		attach(is_pic(p) ? 'P' : 'F', p);
		break;
	}
	}
}

// Moves the chat's D-Pad highlight to the nearest button in that direction.
static void move_focus(int dx, int dy)
{
	int cur_i = -1;
	for (int i = 0; i < nhits; i++) if (hits[i].act == chat_focus) cur_i = i;
	if (cur_i < 0) { chat_focus = A_TYPE; return; }
	float cx = hits[cur_i].x + hits[cur_i].w / 2, cy = hits[cur_i].y + hits[cur_i].h / 2;
	int best = -1;
	float bs = 1e9f;
	for (int i = 0; i < nhits; i++) {
		if (i == cur_i || hits[i].act == A_NONE || hits[i].act == chat_focus) continue;
		float x = hits[i].x + hits[i].w / 2, y = hits[i].y + hits[i].h / 2;
		float along = dx ? (x - cx) * dx : (y - cy) * dy;
		float side = dx ? fabsf(y - cy) : fabsf(x - cx);
		if (along < 8) continue;
		float sc = along + side * 2;
		if (sc < bs) { bs = sc; best = i; }
	}
	if (best >= 0) chat_focus = hits[best].act;
}

static void back(void)
{
	armed = -1;
	switch (view) {
	case V_CHAT: view = V_CHATS; break;
	case V_PROJECT: view = V_PROJECTS; break;
	case V_PROJEDIT: view = V_PROJECT; break;
	case V_FILES: if (strcmp(cwd, "sdmc:/")) dir_up(); else go_chat(); break;
	case V_CAMERA: close_camera(); break;
	case V_TALK: cancel_talk(); break;
	case V_MCP: case V_SKILLS: view = V_SETTINGS; break;
	default: go_chat(); break;
	}
}

static void style_cycle(int dir)
{
	Project *p = proj_find(cur.project);
	if (p && p->persona >= 0) { cycle_persona(&p->persona, dir, false); index_save(); }
	else { cycle_persona(&cfg.persona, dir, false); cfg_save(); }
	char m[80];
	snprintf(m, sizeof m, "Personality: %s", P_NAME[persona_for(cur.project)]);
	say(m);
}

static void do_action(int a)
{
	if (a >= A_ROW) {
		int i = a - A_ROW;
		if (view == V_CHAT) return;
		// one tap opens/changes the row, like a phone
		if (sel[view] != i && armed_view == view) armed = -1;
		sel[view] = i;
		activate_row(view, i);
		return;
	}
	switch (a) {
	case A_TYPE: type_message(NULL); break;
	case A_RETRY: retry(); break;
	case A_CHATS: view = V_CHATS; break;
	case A_NEW: new_chat(cur.project); say("New chat"); break;
	case A_WEB: cfg.web = !cfg.web; cfg_save(); say(cfg.web ? "Web lookups on" : "Web lookups off"); break;
	case A_STYLE: style_cycle(1); break;
	case A_SETTINGS: view = V_SETTINGS; break;
	case A_SOON: say("Coming in the next update!"); break;
	case A_TALK: start_talk(); break;
	case A_PHOTO: open_camera(); break;
	case A_ATTACH: open_files(); break;
	case A_UNATTACH: pend_att = 0; say("Removed"); break;
	case A_SNAP: snap(); break;
	case A_FLIP: cam_inner = !cam_inner; if (!cam_start(cam_inner)) close_camera(); break;
	case A_VOICE_SEND: send_voice(); break;
	case A_CANCEL: back(); break;
	case A_UP: back(); break;
	case A_TAB_CHATS: view = V_CHATS; armed = -1; break;
	case A_TAB_PROJECTS: view = V_PROJECTS; armed = -1; break;
	case A_EDITPROJ: view = V_PROJEDIT; sel[V_PROJEDIT] = 0; break;
	}
}

static void handle_keys(u32 down, u32 held)
{
	if (view == V_CAMERA) {
		if (down & KEY_A) snap();
		else if (down & KEY_X) do_action(A_FLIP);
		else if (down & KEY_B) close_camera();
		return;
	}
	if (view == V_TALK) {
		if (down & (KEY_A | KEY_R)) send_voice();
		else if (down & KEY_B) cancel_talk();
		return;
	}
	if (view == V_CHAT) {
		if (down & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT))
			move_focus(down & KEY_DLEFT ? -1 : down & KEY_DRIGHT ? 1 : 0, down & KEY_DUP ? -1 : down & KEY_DDOWN ? 1 : 0);
		if (down & KEY_A) {
			bool shown = false;
			for (int i = 0; i < nhits; i++) if (hits[i].act == chat_focus) shown = true;
			if (chat_err[0] && !thinking && (chat_focus == A_TYPE || !shown)) retry();
			else if (shown && chat_focus != A_TYPE) do_action(chat_focus);
			else type_message(NULL);
		}
		if (down & KEY_R) start_talk();
		if (down & KEY_B) { if (pend_att) do_action(A_UNATTACH); else view = V_CHATS; }
		if (down & KEY_X) do_action(A_NEW);
		if (down & KEY_Y) do_action(A_WEB);
		if (down & KEY_L) style_cycle(1);
		if (down & KEY_SELECT) view = V_SETTINGS;
		float sp = 0;
		circlePosition cp;
		hidCircleRead(&cp);
		if (abs(cp.dy) > 20) sp += cp.dy / 16.0f;
		chat_scroll += sp;
		return;
	}
	int n = build_rows(view);
	if (down & (KEY_DUP | KEY_DDOWN)) armed = -1;
	if (down & KEY_DUP) sel[view] = sel[view] > 0 ? sel[view] - 1 : n - 1;
	if (down & KEY_DDOWN) sel[view] = sel[view] < n - 1 ? sel[view] + 1 : 0;
	static int rep;
	if (held & (KEY_CPAD_UP | KEY_CPAD_DOWN)) {
		if (rep++ % 8 == 0) {
			if (held & KEY_CPAD_UP && sel[view] > 0) sel[view]--;
			if (held & KEY_CPAD_DOWN && sel[view] < n - 1) sel[view]++;
		}
	} else rep = 0;
	fix_scroll(view, n);
	int i = sel[view];
	if (down & KEY_A) activate_row(view, i);
	else if (down & KEY_X && view != V_FILES && view != V_MCP && view != V_SKILLS) rename_row(&rows[i]);
	else if (down & KEY_Y && view != V_FILES && view != V_MCP && view != V_SKILLS) delete_row(view, i);
	else if (down & KEY_B) back();
	else if (down & (KEY_L | KEY_R) && (view == V_CHATS || view == V_PROJECTS)) { view = view == V_CHATS ? V_PROJECTS : V_CHATS; armed = -1; }
	else if (down & (KEY_DLEFT | KEY_DRIGHT)) {
		int dir = down & KEY_DLEFT ? -1 : 1;
		if (view == V_SETTINGS && (rows[i].ref == S_PERSONA || rows[i].ref == S_WEB || rows[i].ref == S_VERIFY)) setting_change(rows[i].ref, dir);
		if (view == V_PROJEDIT && rows[i].ref == PS_PERSONA) projset_change(PS_PERSONA, dir);
	} else if (down & KEY_SELECT) {
		if (view == V_PROJECT) do_action(A_EDITPROJ);
		else if (view == V_SETTINGS) go_chat();
		else view = V_SETTINGS;
	}
}

// ---------------------------------------------------------------- main

static void boot_step(const char *what)
{
	printf("- %s\n", what);
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
	FILE *f = fopen(BASE "/boot.txt", !strcmp(what, "SD card folders") ? "w" : "a");
	if (f) { fprintf(f, "%s\n", what); fclose(f); }
}

int main(void)
{
	gfxInitDefault();
	// Start-up steps are printed on the top screen (and saved to boot.txt), so
	// if Spike ever freezes while starting, the last line says where.
	consoleInit(GFX_TOP, NULL);
	printf("Spike AI is starting...\n\n");
	boot_step("SD card folders");
	ensure_dirs();
	boot_step("romfs");
	romfsInit();
	boot_step("settings");
	cfg_load();
	boot_step("chat list");
	index_load();
	boot_step("network");
	net_init();
	net_set_verify(cfg.verify);
	boot_step("graphics");
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(8192);
	C2D_Prepare();
	C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget *bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	boot_step("font");
	gfx_init();
	gfx_set_theme(true);
	boot_step("last chat");
	if (cfg.last_chat && chat_find(cfg.last_chat)) open_chat(cfg.last_chat);
	if (!cfg.relay[0]) { view = V_SETTINGS; sel[V_SETTINGS] = S_RELAY; }

	boot_step("ready");
	bool touching = false;
	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld();
		if (down & KEY_START) break;

		if (view == V_CAMERA) cam_frame();
		if (view == V_TALK) {
			mic_poll();
			if (mic_seconds() >= MIC_MAX_SECONDS - 0.1f) send_voice();
		}

		Job *j;
		while ((j = net_poll())) {
			if (j->tag == TAG_CHAT) got_chat(j);
			else if (j->tag == TAG_PING) got_ping(j);
			else if (j->tag == TAG_MCP) got_mcp(j);
			job_free(j);
		}

		if (down & KEY_TOUCH) {
			touchPosition tp;
			hidTouchRead(&tp);
			touching = true;
			for (int i = nhits - 1; i >= 0; i--) {
				Hit *h = &hits[i];
				if (tp.px >= h->x && tp.px < h->x + h->w && tp.py >= h->y && tp.py < h->y + h->h) { do_action(h->act); break; }
			}
		} else if (!(held & KEY_TOUCH)) touching = false;
		if (!(down & KEY_TOUCH)) handle_keys(down, held);
		(void)touching;

		t_anim += 1.0f / 60;
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		gfx_frame_start();
		// bottom first: it builds the rows the top screen previews
		C2D_TargetClear(bot, T.bg);
		C2D_SceneBegin(bot);
		draw_bottom();
		C2D_TargetClear(top, T.bg);
		C2D_SceneBegin(top);
		draw_top();
		C3D_FrameEnd(0);
	}

	cam_stop();
	mic_cancel();
	net_exit();
	conv_clear(&cur);
	conv_clear(&preview);
	gfx_exit();
	C2D_Fini();
	C3D_Fini();
	romfsExit();
	gfxExit();
	return 0;
}
