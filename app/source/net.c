#include "net.h"
#include <curl/curl.h>
#include <mbedtls/x509_crt.h>
#include <time.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000
#define MAX_BODY       (3 * 1024 * 1024)

static u32 *s_socbuf;
static Thread s_thread;
static volatile bool s_quit;
static LightLock s_lock;
static LightEvent s_event;
static Job *s_urgent, *s_low, *s_done;
static volatile bool s_working;
static CURL *s_curl;
static char *s_ca;
static size_t s_calen;
static volatile bool s_verify = true;

void net_set_verify(bool on) { s_verify = on; }

static void push_back(Job **q, Job *j)
{
	j->next = NULL;
	while (*q) q = &(*q)->next;
	*q = j;
}

static Job *pop_front(Job **q)
{
	Job *j = *q;
	if (j) *q = j->next;
	return j;
}

Job *job_new(JobKind kind, int tag, const char *url)
{
	Job *j = calloc(1, sizeof *j);
	if (!j) return NULL;
	j->kind = kind;
	j->tag = tag;
	j->url = strdup(url ? url : "");
	return j;
}

void job_free(Job *j)
{
	if (!j) return;
	free(j->url);
	free(j->post);
	free(j->body);
	free(j->pixels);
	free(j);
}

char *net_escape(const char *s)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t n = strlen(s);
	char *o = malloc(n * 3 + 1), *p = o;
	if (!o) return NULL;
	for (; *s; s++) {
		unsigned char c = *s;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') *p++ = c;
		else { *p++ = '%'; *p++ = hex[c >> 4]; *p++ = hex[c & 15]; }
	}
	*p = 0;
	return o;
}

// ---------- HTTP ----------

typedef struct { char *data; size_t len, cap; bool overflow; } Buf;

static size_t on_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
	Buf *b = ud;
	size_t n = size * nmemb;
	if (b->len + n + 1 > MAX_BODY) { b->overflow = true; return 0; }
	if (b->len + n + 1 > b->cap) {
		size_t nc = b->cap ? b->cap : 16384;
		while (nc < b->len + n + 1) nc *= 2;
		char *nd = realloc(b->data, nc);
		if (!nd) return 0;
		b->data = nd;
		b->cap = nc;
	}
	memcpy(b->data + b->len, ptr, n);
	b->len += n;
	b->data[b->len] = 0;
	return n;
}

static int on_progress(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un)
{
	(void)ud; (void)dt; (void)dn; (void)ut; (void)un;
	return s_quit ? 1 : 0;
}

static bool http_get(Job *j, Buf *b)
{
	curl_easy_reset(s_curl);
	curl_easy_setopt(s_curl, CURLOPT_URL, j->url);
	curl_easy_setopt(s_curl, CURLOPT_USERAGENT, "SpikeAI/1.0 (3DS)");
	curl_easy_setopt(s_curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(s_curl, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(s_curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(s_curl, CURLOPT_TIMEOUT, j->timeout > 0 ? j->timeout : 45L);
	struct curl_slist *hdrs = NULL;
	if (j->header[0]) hdrs = curl_slist_append(hdrs, j->header);
	if (j->post) {
		hdrs = curl_slist_append(hdrs, "Content-Type: text/plain; charset=utf-8");
		curl_easy_setopt(s_curl, CURLOPT_POSTFIELDS, j->post);
		curl_easy_setopt(s_curl, CURLOPT_POSTFIELDSIZE, (long)strlen(j->post));
	}
	if (hdrs) curl_easy_setopt(s_curl, CURLOPT_HTTPHEADER, hdrs);
	curl_easy_setopt(s_curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(s_curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(s_curl, CURLOPT_BUFFERSIZE, 65536L);
	curl_easy_setopt(s_curl, CURLOPT_WRITEFUNCTION, on_write);
	curl_easy_setopt(s_curl, CURLOPT_WRITEDATA, b);
	curl_easy_setopt(s_curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(s_curl, CURLOPT_XFERINFOFUNCTION, on_progress);
	curl_easy_setopt(s_curl, CURLOPT_SSL_VERIFYPEER, s_verify ? 1L : 0L);
	curl_easy_setopt(s_curl, CURLOPT_SSL_VERIFYHOST, s_verify ? 2L : 0L);
	if (s_verify && s_ca) {
		struct curl_blob blob = { .data = s_ca, .len = s_calen, .flags = CURL_BLOB_NOCOPY };
		curl_easy_setopt(s_curl, CURLOPT_CAINFO_BLOB, &blob);
	}

	CURLcode rc = curl_easy_perform(s_curl);
	long code = 0;
	curl_easy_getinfo(s_curl, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(hdrs);
	j->status = code;
	if (rc != CURLE_OK) {
		if (b->overflow) snprintf(j->err, sizeof j->err, "Response too big");
		else if (rc == CURLE_PEER_FAILED_VERIFICATION) {
			long vr = 0;
			char info[96] = "";
			curl_easy_getinfo(s_curl, CURLINFO_SSL_VERIFYRESULT, &vr);
			mbedtls_x509_crt_verify_info(info, sizeof info, "", (uint32_t)vr);
			for (char *p = info; *p; p++) if (*p == '\n') *p = ' ';
			time_t now = time(NULL);
			struct tm *tm = gmtime(&now);
			char day[16];
			strftime(day, sizeof day, "%Y-%m-%d", tm);
			snprintf(j->err, sizeof j->err, "Certificate problem: %s(3DS date %s, %s)", info[0] ? info : "unknown ",
				day, s_ca ? "CA list loaded" : "no CA list");
		} else snprintf(j->err, sizeof j->err, "%s", curl_easy_strerror(rc));
		return false;
	}
	if ((code < 200 || code >= 300) && !(b->data && strncmp(b->data, "ERR\t", 4) == 0)) {
		snprintf(j->err, sizeof j->err, "Server answered %ld", code);
		return false;
	}
	return true;
}

// ---------- images ----------

static int pow2(int v)
{
	int p = 8;
	while (p < v) p <<= 1;
	return p;
}

static inline u32 morton(int x, int y, int tw)
{
	return ((((y >> 3) * (tw >> 3) + (x >> 3)) << 6) +
		((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3)));
}

// Scales (cover-crop, nearest) the decoded image into a w*h box and tiles it.
bool image_decode(Job *j, const unsigned char *data, size_t len)
{
	int sw, sh, comp;
	unsigned char *px = stbi_load_from_memory(data, (int)len, &sw, &sh, &comp, 4);
	if (!px) { snprintf(j->err, sizeof j->err, "Bad image"); return false; }
	int w = j->w > 0 ? j->w : sw, h = j->h > 0 ? j->h : sh;
	if (w > 1024) w = 1024;
	if (h > 1024) h = 1024;
	int tw = pow2(w), th = pow2(h);
	u32 *out = calloc(tw * th, 4);
	if (!out) { stbi_image_free(px); snprintf(j->err, sizeof j->err, "Out of memory"); return false; }

	// cover: scale so the image fills the box, crop the overflow (centered)
	float sx = (float)sw / w, sy = (float)sh / h;
	float s = sx < sy ? sx : sy;
	float ox = (sw - w * s) * 0.5f, oy = (sh - h * s) * 0.5f;
	for (int y = 0; y < h; y++) {
		int yy = (int)(oy + (y + 0.5f) * s);
		if (yy >= sh) yy = sh - 1;
		for (int x = 0; x < w; x++) {
			int xx = (int)(ox + (x + 0.5f) * s);
			if (xx >= sw) xx = sw - 1;
			const unsigned char *p = px + (yy * sw + xx) * 4;
			out[morton(x, y, tw)] = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | 0xff;
		}
	}
	stbi_image_free(px);
	j->pixels = out;
	j->tw = tw; j->th = th;
	j->iw = w; j->ih = h;
	return true;
}

// ---------- worker ----------

static void run_job(Job *j)
{
	Buf b = {0};
	if (!s_curl) { snprintf(j->err, sizeof j->err, "Network not ready"); return; }
	if (!http_get(j, &b)) { free(b.data); return; }
	if (j->kind == JOB_TEXT) {
		if (!b.data) { b.data = strdup(""); b.len = 0; }
		j->body = b.data;
		j->len = b.len;
		j->ok = true;
	} else {
		j->ok = b.data && image_decode(j, (unsigned char *)b.data, b.len);
		free(b.data);
	}
}

static void worker(void *arg)
{
	(void)arg;
	while (!s_quit) {
		LightLock_Lock(&s_lock);
		Job *j = pop_front(&s_urgent);
		if (!j) j = pop_front(&s_low);
		s_working = j != NULL;
		LightLock_Unlock(&s_lock);
		if (!j) {
			LightEvent_Wait(&s_event);
			continue;
		}
		run_job(j);
		LightLock_Lock(&s_lock);
		push_back(&s_done, j);
		s_working = false;
		LightLock_Unlock(&s_lock);
	}
}

static void load_ca(void)
{
	FILE *f = fopen("romfs:/cacert.pem", "rb");
	if (!f) return;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	s_ca = malloc(n + 1);
	curl_easy_setopt(s_curl, CURLOPT_SSL_VERIFYPEER, s_verify ? 1L : 0L);
	curl_easy_setopt(s_curl, CURLOPT_SSL_VERIFYHOST, s_verify ? 2L : 0L);
	if (s_verify && s_ca) {
		s_calen = fread(s_ca, 1, n, f);
		s_ca[s_calen] = 0;
		s_calen++; // PEM blobs must include the terminating NUL for mbedTLS
	}
	fclose(f);
}

bool net_init(void (*step)(const char *what, long rc))
{
	LightLock_Init(&s_lock);
	LightEvent_Init(&s_event, RESET_ONESHOT);

	s_socbuf = memalign(SOC_ALIGN, SOC_BUFFERSIZE);
	Result rc = s_socbuf ? socInit(s_socbuf, SOC_BUFFERSIZE) : -1;
	if (step) step("network: wifi sockets", rc);
	if (!s_socbuf || R_FAILED(rc)) {
		free(s_socbuf);
		s_socbuf = NULL;
	}
	long crc = curl_global_init(CURL_GLOBAL_DEFAULT);
	s_curl = curl_easy_init();
	if (step) step("network: curl", s_curl ? crc : -1);
	load_ca();
	if (step) step("network: certificates", s_ca ? 0 : -1);

	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	s_quit = false;
	s_thread = threadCreate(worker, NULL, 128 * 1024, prio + 1, -2, false);
	if (step) step("network: worker thread", s_thread ? 0 : -1);
	return s_thread != NULL && s_socbuf != NULL;
}

void net_exit(void)
{
	s_quit = true;
	if (s_thread) {
		LightEvent_Signal(&s_event);
		threadJoin(s_thread, U64_MAX);
		threadFree(s_thread);
		s_thread = NULL;
	}
	Job *j;
	while ((j = pop_front(&s_urgent))) job_free(j);
	while ((j = pop_front(&s_low))) job_free(j);
	while ((j = pop_front(&s_done))) job_free(j);
	if (s_curl) curl_easy_cleanup(s_curl);
	s_curl = NULL;
	curl_global_cleanup();
	free(s_ca);
	s_ca = NULL;
	if (s_socbuf) {
		socExit();
		free(s_socbuf);
		s_socbuf = NULL;
	}
}

void net_submit(Job *j, bool urgent)
{
	if (!j) return;
	LightLock_Lock(&s_lock);
	push_back(urgent ? &s_urgent : &s_low, j);
	LightLock_Unlock(&s_lock);
	LightEvent_Signal(&s_event);
}

Job *net_poll(void)
{
	LightLock_Lock(&s_lock);
	Job *j = pop_front(&s_done);
	LightLock_Unlock(&s_lock);
	return j;
}

void net_drop_low(void)
{
	LightLock_Lock(&s_lock);
	Job *j;
	while ((j = pop_front(&s_low))) job_free(j);
	LightLock_Unlock(&s_lock);
}

bool net_busy(void)
{
	LightLock_Lock(&s_lock);
	bool b = s_working || s_urgent || s_low;
	LightLock_Unlock(&s_lock);
	return b;
}
