#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum { JOB_TEXT, JOB_IMAGE } JobKind;

typedef struct Job {
	// request
	JobKind kind;
	int tag;      // what the result is for (see main.c)
	int arg;      // e.g. channel index
	u32 key;      // e.g. image cache key
	char *url;
	int w, h;     // image box size
	char *post;   // POST body (NULL = GET)
	char header[200]; // one extra request header, e.g. "X-Spike-Key: ..."
	long timeout; // seconds (0 = default)
	long status;  // HTTP status

	// result
	bool ok;
	char err[200];
	char *body;   // JOB_TEXT: NUL-terminated response
	size_t len;
	u32 *pixels;  // JOB_IMAGE: Morton-tiled RGBA8, tw*th
	int tw, th;   // texture size (power of two)
	int iw, ih;   // image size inside the texture

	struct Job *next;
} Job;

bool net_init(void);
void net_exit(void);

Job *job_new(JobKind kind, int tag, const char *url);
void job_free(Job *j);

void net_submit(Job *j, bool urgent);
Job *net_poll(void);          // a finished job, or NULL
void net_drop_low(void);      // forget queued (not started) low-priority jobs
bool net_busy(void);
void net_set_verify(bool on);

// Decodes a JPG/PNG into j->pixels, cover-cropped to j->w x j->h (0 = natural size).
bool image_decode(Job *j, const unsigned char *data, size_t len);

// URL-encodes s into a new malloc'd string.
char *net_escape(const char *s);
