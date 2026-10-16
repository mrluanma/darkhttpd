/* upnp.c - minimal DLNA/UPnP AV MediaServer for darkhttpd.
 *
 * See upnp.h for an overview.  This file is intentionally dependency-free and
 * portable across POSIX systems (Linux, *BSD, macOS).
 *
 * Copyright (c) 2025 - released under the same ISC terms as darkhttpd.
 */

#include "upnp.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#ifndef MAXNAMLEN
# ifdef NAME_MAX
#  define MAXNAMLEN NAME_MAX
# else
#  define MAXNAMLEN 255
# endif
#endif

#define SSDP_ADDR        "239.255.255.250"
#define SSDP_PORT        1900
#define SSDP_MAX_AGE     1800
/* Re-announce well inside max-age so a lost packet or two on Wi-Fi does not
 * let control points expire us. */
#define SSDP_NOTIFY_EVERY 300
/* Periodically rebuild the SSDP socket: this is the part a restart repairs
 * when discovery mysteriously stops (lost multicast membership, stale
 * interface binding, etc.). */
#define SSDP_REOPEN_AFTER 900
/* Retry interval when the initial socket could not be opened. */
#define SSDP_RETRY        30

#define UPNP_SERVER      "UPnP/1.0 darkhttpd/1.17"
#define DLNA_PROTO_SUFFIX \
    "DLNA.ORG_OP=01;DLNA.ORG_CI=0;" \
    "DLNA.ORG_FLAGS=01700000000000000000000000000000"

#define DEVICE_TYPE      "urn:schemas-upnp-org:device:MediaServer:1"
#define CD_SERVICE       "urn:schemas-upnp-org:service:ContentDirectory:1"
#define CM_SERVICE       "urn:schemas-upnp-org:service:ConnectionManager:1"

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static int active = 0;              /* non-zero once upnp_init() succeeded   */
static int ssdp_sock = -1;          /* SSDP multicast UDP socket             */
static time_t ssdp_born = 0;        /* when ssdp_sock was last (re)opened    */
static unsigned short http_port = 8080;
static struct in_addr server_addr;  /* address advertised to clients         */
static char server_addr_str[64];    /* dotted-quad of server_addr            */
static int ip_fixed = 0;            /* user pinned the advertised address    */
static char uuid[64];               /* stable device UUID                    */
static char friendly[128];          /* friendly name                         */
static char *root = NULL;           /* wwwroot, trailing slash stripped      */
static unsigned int bootid = 1;
static time_t next_alive = 0;       /* when the next announcement is due     */
static int alive_burst = 0;         /* extra startup announcements remaining */

/* ------------------------------------------------------------------ */
/* Growable string buffer                                              */
/* ------------------------------------------------------------------ */

struct buf {
    char *s;
    size_t len, cap;
};

static void oom(void) {
    fprintf(stderr, "upnp: out of memory\n");
    exit(1);
}

static void binit(struct buf *b) {
    b->cap = 256;
    b->len = 0;
    b->s = malloc(b->cap);
    if (b->s == NULL) oom();
    b->s[0] = '\0';
}

static void bgrow(struct buf *b, size_t need) {
    if (b->len + need + 1 > b->cap) {
        size_t cap = b->cap;
        while (b->len + need + 1 > cap) cap *= 2;
        b->s = realloc(b->s, cap);
        if (b->s == NULL) oom();
        b->cap = cap;
    }
}

static void braw(struct buf *b, const char *s, size_t n) {
    if (n == 0) return;
    bgrow(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void bstr(struct buf *b, const char *s) {
    braw(b, s, strlen(s));
}

static void bprintf(struct buf *b, const char *fmt, ...) {
    va_list ap, ap2;
    int n;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    bgrow(b, (size_t)n);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

/* Escape < > & ' " for use in XML text or attributes. */
static void besc(struct buf *b, const char *s) {
    for (; *s != '\0'; s++) {
        switch (*s) {
        case '<': bstr(b, "&lt;"); break;
        case '>': bstr(b, "&gt;"); break;
        case '&': bstr(b, "&amp;"); break;
        case '"': bstr(b, "&quot;"); break;
        case '\'': bstr(b, "&apos;"); break;
        default: braw(b, s, 1); break;
        }
    }
}

static char *btake(struct buf *b, size_t *len) {
    char *s;
    if (len != NULL) *len = b->len;
    s = b->s;
    b->s = NULL;
    b->len = b->cap = 0;
    return s;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static const char *ci_strstr(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0) return hay;
    for (; *hay != '\0'; hay++)
        if (strncasecmp(hay, needle, n) == 0) return hay;
    return NULL;
}

/* Decode &amp; &lt; &gt; &quot; &apos; in place. */
static void xml_unescape(char *s) {
    char *r = s, *w = s;
    while (*r != '\0') {
        if (r[0] == '&') {
            if (strncmp(r, "&amp;", 5) == 0) { *w++ = '&'; r += 5; continue; }
            if (strncmp(r, "&lt;", 4) == 0)  { *w++ = '<'; r += 4; continue; }
            if (strncmp(r, "&gt;", 4) == 0)  { *w++ = '>'; r += 4; continue; }
            if (strncmp(r, "&quot;", 6) == 0){ *w++ = '"'; r += 6; continue; }
            if (strncmp(r, "&apos;", 6) == 0){ *w++ = '\''; r += 6; continue; }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/* Return the text content of the first <tag>...</tag> element (an optional
 * namespace prefix is tolerated).  Returns 1 on success. */
static int xml_value(const char *req, const char *tag, char *out, size_t outsz) {
    size_t taglen = strlen(tag);
    const char *p = req;

    while ((p = strchr(p, '<')) != NULL) {
        const char *name = p + 1;
        const char *colon, *gt, *val, *end;
        size_t n;

        p++;
        if (*name == '/') continue;             /* closing tag */
        gt = strchr(name, '>');
        if (gt == NULL) break;
        colon = strchr(name, ':');
        if (colon != NULL && colon < gt) name = colon + 1;
        if ((size_t)(gt - name) < taglen ||
            strncmp(name, tag, taglen) != 0 ||
            (name[taglen] != '>' && !isspace((unsigned char)name[taglen])))
            continue;

        val = gt + 1;
        end = strchr(val, '<');
        if (end == NULL) return 0;
        n = (size_t)(end - val);
        if (n >= outsz) n = outsz - 1;
        memcpy(out, val, n);
        out[n] = '\0';
        xml_unescape(out);
        return 1;
    }
    return 0;
}

/* Return the value of an HTTP header as an allocated string, or NULL. */
static char *header_value(const char *req, const char *name) {
    size_t nlen = strlen(name);
    char *pat, *out;
    const char *p, *end, *v;
    size_t n;

    pat = malloc(nlen + 3);
    if (pat == NULL) oom();
    pat[0] = '\n';
    memcpy(pat + 1, name, nlen);
    pat[nlen + 1] = ':';
    pat[nlen + 2] = '\0';
    p = ci_strstr(req, pat);
    free(pat);
    if (p == NULL) return NULL;

    v = p + nlen + 2;
    end = v;
    while (*end != '\0' && *end != '\r' && *end != '\n') end++;
    while (v < end && (*v == ' ' || *v == '\t')) v++;
    while (end > v && (end[-1] == ' ' || end[-1] == '\t')) end--;
    n = (size_t)(end - v);

    out = malloc(n + 1);
    if (out == NULL) oom();
    memcpy(out, v, n);
    out[n] = '\0';
    return out;
}

static int needs_encoding(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9'))
        return 0;
    switch (c) {
    case '-': case '.': case '_': case '~': case '/':
        return 0;
    }
    return 1;
}

/* Append an RFC3986 percent-encoded copy of src (keeping '/'). */
static void url_enc(struct buf *b, const char *src) {
    static const char hex[] = "0123456789ABCDEF";
    for (; *src != '\0'; src++) {
        unsigned char c = (unsigned char)*src;
        if (needs_encoding(c)) {
            char t[3];
            t[0] = '%';
            t[1] = hex[(c >> 4) & 0xF];
            t[2] = hex[c & 0xF];
            braw(b, t, 3);
        } else {
            braw(b, (const char *)&c, 1);
        }
    }
}

/* Percent-decode a URL path.  Caller frees. */
static char *path_decode(const char *in) {
    size_t n = strlen(in), j = 0, i;
    char *out = malloc(n + 1);
    if (out == NULL) oom();
    for (i = 0; i < n; i++) {
        if (in[i] == '%' && i + 2 < n &&
            isxdigit((unsigned char)in[i + 1]) &&
            isxdigit((unsigned char)in[i + 2])) {
            char h[3];
            h[0] = in[i + 1];
            h[1] = in[i + 2];
            h[2] = '\0';
            out[j++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = '\0';
    return out;
}

/* Reject paths that could escape the content root. */
static int path_safe(const char *p) {
    const char *s;
    if (p[0] != '/') return 0;
    for (s = p; *s != '\0'; s++)
        if (s[0] == '.' && s[1] == '.' &&
            (s[2] == '/' || s[2] == '\0'))
            return 0;
    return 1;
}

static const char *path_basename(const char *p) {
    const char *slash = strrchr(p, '/');
    return slash != NULL ? slash + 1 : p;
}

/* Parent object id for a child id: one path component is removed, with the
 * content root represented by "0". */
static void parent_of(const char *id, char *out, size_t outsz) {
    const char *slash;
    size_t len;
    if (id[0] != '/') { snprintf(out, outsz, "0"); return; }
    slash = strrchr(id, '/');
    if (slash == NULL || slash == id) { snprintf(out, outsz, "0"); return; }
    len = (size_t)(slash - id);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, id, len);
    out[len] = '\0';
}

static void rfc1123(time_t t, char *out, size_t outsz) {
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, outsz, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

/* ------------------------------------------------------------------ */
/* Media type classification                                           */
/* ------------------------------------------------------------------ */

struct ext_mime {
    const char *ext, *mime;
};

static const struct ext_mime media_types[] = {
    { "mp4",  "video/mp4" },
    { "m4v",  "video/mp4" },
    { "mkv",  "video/x-matroska" },
    { "webm", "video/webm" },
    { "avi",  "video/x-msvideo" },
    { "mov",  "video/quicktime" },
    { "qt",   "video/quicktime" },
    { "mpg",  "video/mpeg" },
    { "mpeg", "video/mpeg" },
    { "mpe",  "video/mpeg" },
    { "ts",   "video/mp2t" },
    { "m2ts", "video/mp2t" },
    { "wmv",  "video/x-ms-wmv" },
    { "flv",  "video/x-flv" },
    { "3gp",  "video/3gpp" },
    { "ogv",  "video/ogg" },
    { "mp3",  "audio/mpeg" },
    { "m4a",  "audio/mp4" },
    { "aac",  "audio/aac" },
    { "flac", "audio/flac" },
    { "ogg",  "audio/ogg" },
    { "oga",  "audio/ogg" },
    { "opus", "audio/ogg" },
    { "wav",  "audio/wav" },
    { "wma",  "audio/x-ms-wma" },
    { "jpg",  "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "png",  "image/png" },
    { "gif",  "image/gif" },
    { "bmp",  "image/bmp" },
    { "webp", "image/webp" },
    { NULL, NULL }
};

static const char *mime_for(const char *name) {
    const char *dot = strrchr(name, '.');
    size_t i;
    if (dot == NULL) return NULL;
    dot++;
    for (i = 0; media_types[i].ext != NULL; i++)
        if (strcasecmp(dot, media_types[i].ext) == 0)
            return media_types[i].mime;
    return NULL;
}

static const char *class_for(const char *mime) {
    if (strncmp(mime, "video/", 6) == 0) return "object.item.videoItem";
    if (strncmp(mime, "audio/", 6) == 0) return "object.item.audioItem.musicTrack";
    if (strncmp(mime, "image/", 6) == 0) return "object.item.imageItem.photo";
    return "object.item";
}

/* ------------------------------------------------------------------ */
/* Address / identity                                                  */
/* ------------------------------------------------------------------ */

static unsigned long long fnv1a(const char *s, unsigned long long h) {
    while (*s != '\0') {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

static int detect_addr(const char *bind_addr, const char *advertise,
        struct in_addr *out) {
    const char *src = advertise != NULL ? advertise : bind_addr;
    int s;
    struct sockaddr_in dst;

    if (src != NULL && strcmp(src, "0.0.0.0") != 0) {
        if (inet_aton(src, out)) return 0;
    }

    /* Ask the kernel which local address would be used to reach the outside
     * world.  No packets are actually sent. */
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s != -1) {
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_port = htons(53);
        if (inet_aton("8.8.8.8", &dst.sin_addr) &&
            connect(s, (struct sockaddr *)&dst, sizeof(dst)) == 0) {
            struct sockaddr_in local;
            socklen_t len = sizeof(local);
            if (getsockname(s, (struct sockaddr *)&local, &len) == 0) {
                *out = local.sin_addr;
                close(s);
                return 0;
            }
        }
        close(s);
    }

    out->s_addr = htonl(INADDR_LOOPBACK);
    return 0;
}

static void make_uuid(const char *seed) {
    unsigned long long a = fnv1a(seed, 1469598103934665603ULL);
    unsigned long long b = fnv1a(seed, 1099511628211ULL);
    snprintf(uuid, sizeof(uuid), "%08lx-%04lx-%04lx-%04lx-%012llx",
        (unsigned long)((a >> 32) & 0xffffffffUL),
        (unsigned long)((a >> 16) & 0xffffUL),
        (unsigned long)((a & 0x0fffUL) | 0x4000UL),
        (unsigned long)(((b >> 48) & 0x3fffUL) | 0x8000UL),
        (unsigned long long)(b & 0xffffffffffffULL));
}

/* ------------------------------------------------------------------ */
/* SSDP                                                                */
/* ------------------------------------------------------------------ */

static const char *location_url(void) {
    static char buf[96];
    snprintf(buf, sizeof(buf), "http://%s:%u/rootDesc.xml",
        server_addr_str, (unsigned)http_port);
    return buf;
}

static void ssdp_send_to(const struct buf *b, const struct sockaddr_in *to) {
    if (sendto(ssdp_sock, b->s, b->len, 0,
               (const struct sockaddr *)to, sizeof(*to)) < 0) {
        /* best effort; discovery is inherently unreliable */
    }
}

static void ssdp_send_search_response(const struct sockaddr_in *to,
        const char *st, const char *usn) {
    struct buf b;
    char date[64];

    rfc1123(time(NULL), date, sizeof(date));
    binit(&b);
    bstr(&b, "HTTP/1.1 200 OK\r\n");
    bprintf(&b, "CACHE-CONTROL: max-age=%d\r\n", SSDP_MAX_AGE);
    bprintf(&b, "DATE: %s\r\n", date);
    bstr(&b, "EXT:\r\n");
    bprintf(&b, "LOCATION: %s\r\n", location_url());
    bprintf(&b, "SERVER: %s\r\n", UPNP_SERVER);
    bprintf(&b, "ST: %s\r\n", st);
    bprintf(&b, "USN: %s\r\n", usn);
    bprintf(&b, "BOOTID.UPNP.ORG: %u\r\n", bootid);
    bstr(&b, "CONFIGID.UPNP.ORG: 1\r\n");
    bstr(&b, "\r\n");
    ssdp_send_to(&b, to);
    free(b.s);
}

static void ssdp_send_notify(const char *nt, const char *usn, int byebye) {
    struct sockaddr_in to;
    struct buf b;

    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(SSDP_PORT);
    inet_aton(SSDP_ADDR, &to.sin_addr);

    binit(&b);
    bstr(&b, "NOTIFY * HTTP/1.1\r\n");
    bstr(&b, "HOST: 239.255.255.250:1900\r\n");
    if (!byebye) {
        bprintf(&b, "CACHE-CONTROL: max-age=%d\r\n", SSDP_MAX_AGE);
        bprintf(&b, "LOCATION: %s\r\n", location_url());
    }
    bprintf(&b, "NT: %s\r\n", nt);
    bprintf(&b, "NTS: %s\r\n", byebye ? "ssdp:byebye" : "ssdp:alive");
    bprintf(&b, "SERVER: %s\r\n", UPNP_SERVER);
    bprintf(&b, "USN: %s\r\n", usn);
    bprintf(&b, "BOOTID.UPNP.ORG: %u\r\n", bootid);
    bstr(&b, "CONFIGID.UPNP.ORG: 1\r\n");
    bstr(&b, "\r\n");
    ssdp_send_to(&b, &to);
    free(b.s);
}

static void announce_one(const char *nt, int byebye) {
    char usn[256];
    if (strncmp(nt, "uuid:", 5) == 0)
        snprintf(usn, sizeof(usn), "%s", nt);
    else
        snprintf(usn, sizeof(usn), "uuid:%s::%s", uuid, nt);
    ssdp_send_notify(nt, usn, byebye);
}

static void ssdp_announce(int byebye) {
    char u[80];
    snprintf(u, sizeof(u), "uuid:%s", uuid);
    announce_one(u, byebye);
    announce_one("upnp:rootdevice", byebye);
    announce_one(DEVICE_TYPE, byebye);
    announce_one(CD_SERVICE, byebye);
    announce_one(CM_SERVICE, byebye);
}

static void respond_st(const struct sockaddr_in *from, const char *st) {
    char usn[256];
    if (strncmp(st, "uuid:", 5) == 0)
        snprintf(usn, sizeof(usn), "%s", st);
    else
        snprintf(usn, sizeof(usn), "uuid:%s::%s", uuid, st);
    ssdp_send_search_response(from, st, usn);
}

static void handle_msearch(const char *req, const struct sockaddr_in *from) {
    char *man, *st;
    char u[80];

    man = header_value(req, "MAN");
    if (man == NULL || ci_strstr(man, "ssdp:discover") == NULL) {
        free(man);
        return;
    }
    free(man);

    st = header_value(req, "ST");
    if (st == NULL) return;

    if (strcasecmp(st, "ssdp:all") == 0) {
        respond_st(from, "upnp:rootdevice");
        snprintf(u, sizeof(u), "uuid:%s", uuid);
        respond_st(from, u);
        respond_st(from, DEVICE_TYPE);
        respond_st(from, CD_SERVICE);
        respond_st(from, CM_SERVICE);
    } else if (strcasecmp(st, "upnp:rootdevice") == 0) {
        respond_st(from, "upnp:rootdevice");
    } else if (strcasecmp(st, DEVICE_TYPE) == 0) {
        respond_st(from, DEVICE_TYPE);
    } else if (strcasecmp(st, CD_SERVICE) == 0) {
        respond_st(from, CD_SERVICE);
    } else if (strcasecmp(st, CM_SERVICE) == 0) {
        respond_st(from, CM_SERVICE);
    } else {
        snprintf(u, sizeof(u), "uuid:%s", uuid);
        if (strcasecmp(st, u) == 0)
            respond_st(from, u);
    }

    free(st);
}

int upnp_ssdp_fd(void) {
    return ssdp_sock;
}

void upnp_handle_ssdp(void) {
    for (;;) {
        char buf[2048];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n;

        n = recvfrom(ssdp_sock, buf, sizeof(buf) - 1, 0,
            (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            return;
        }
        buf[n] = '\0';
        if (strncasecmp(buf, "M-SEARCH", 8) == 0)
            handle_msearch(buf, &from);
    }
}

/* Defined with the socket helpers below. */
static void ssdp_join(int fd);
static int ssdp_open(void);
static void ssdp_reopen(void);

/* Re-detect the address we advertise, unless the user pinned it with
 * --dlna-ip/--addr.  Without this, a DHCP lease change leaves every LOCATION
 * and media URL pointing at the old address until the server is restarted. */
static void refresh_addr(void) {
    struct in_addr a;
    char buf[64];

    if (ip_fixed) return;

    detect_addr(NULL, NULL, &a);
    if (a.s_addr == htonl(INADDR_LOOPBACK)) return; /* offline; keep old */
    if (a.s_addr == server_addr.s_addr) return;

    server_addr = a;
    if (inet_ntop(AF_INET, &server_addr, buf, sizeof(buf)) != NULL)
        snprintf(server_addr_str, sizeof(server_addr_str), "%s", buf);
    next_alive = 0; /* re-announce the new location immediately */
}

int upnp_next_timeout(void) {
    time_t now;
    if (!active) return -1;
    /* If the socket is not up yet, wake up periodically to retry opening it. */
    if (ssdp_sock == -1) return SSDP_RETRY;
    now = time(NULL);
    if (next_alive <= now) return 0;
    return (int)(next_alive - now);
}

void upnp_periodic(void) {
    time_t now;
    if (!active) return;

    now = time(NULL);

    /* If the SSDP socket never opened (port busy at startup, interface not
     * ready), keep trying rather than staying invisible until a restart. */
    if (ssdp_sock == -1) {
        if (now - ssdp_born < SSDP_RETRY) return;
        ssdp_reopen();
        if (ssdp_sock == -1) return;
        next_alive = 0;
    }

    /* Heal a changed IP or a dropped multicast membership so the device does
     * not silently vanish from control points. */
    refresh_addr();

    now = time(NULL);

    /* Rebuild the socket every so often.  Whatever accumulated state makes
     * discovery stop (lost membership, stale interface binding), this is the
     * part a restart clears -- so clear it in place instead. */
    if (now - ssdp_born >= SSDP_REOPEN_AFTER) {
        ssdp_reopen();
        if (ssdp_sock != -1)
            next_alive = 0;
    }

    now = time(NULL);
    if (now < next_alive) return;

    ssdp_join(ssdp_sock);
    ssdp_announce(0);
    if (alive_burst > 0) {
        alive_burst--;
        next_alive = now + 1;
    } else {
        next_alive = now + SSDP_NOTIFY_EVERY;
    }
}

/* Point multicast egress at the advertised interface and (re)join the SSDP
 * group on it.  Called again before each announcement so a membership the
 * kernel dropped (interface flap, IGMP snooping timeout) gets restored
 * without restarting the server. */
static void ssdp_join(int fd) {
    struct ip_mreq mreq;
    struct in_addr iface;

    iface = server_addr;
    if (iface.s_addr == htonl(INADDR_LOOPBACK))
        iface.s_addr = htonl(INADDR_ANY);

    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));

    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(SSDP_ADDR);
    mreq.imr_interface.s_addr = iface.s_addr;
    /* Not fatal if this fails: we can still answer unicast M-SEARCH and send
     * NOTIFYs. */
    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
}

static int ssdp_open(void) {
    struct sockaddr_in a;
    int on = 1, ttl = 4, flags, s;

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == -1) return -1;

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(SSDP_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) == -1) {
        /* Some systems prefer binding the multicast address directly. */
        a.sin_addr.s_addr = inet_addr(SSDP_ADDR);
        if (bind(s, (struct sockaddr *)&a, sizeof(a)) == -1) {
            close(s);
            return -1;
        }
    }

    ssdp_join(s);

    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    flags = fcntl(s, F_GETFL, 0);
    if (flags != -1)
        fcntl(s, F_SETFL, flags | O_NONBLOCK);

    ssdp_born = time(NULL);
    return s;
}

/* Replace the SSDP socket with a fresh one, re-binding the port and rejoining
 * the group.  The new socket is opened before the old one is closed so there
 * is no window without a listener.  On failure the old socket (if any) is
 * kept. */
static void ssdp_reopen(void) {
    int ns = ssdp_open();

    if (ns == -1) {
        ssdp_born = time(NULL); /* remember the attempt; do not spin */
        return;
    }
    if (ssdp_sock != -1) close(ssdp_sock);
    ssdp_sock = ns;
}

/* ------------------------------------------------------------------ */
/* Static XML documents                                                */
/* ------------------------------------------------------------------ */

static const char scpd_content_directory[] =
"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
"<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
"<specVersion><major>1</major><minor>0</minor></specVersion>"
"<actionList>"
"<action><name>GetSearchCapabilities</name><argumentList>"
"<argument><name>SearchCaps</name><direction>out</direction>"
"<relatedStateVariable>SearchCapabilities</relatedStateVariable>"
"</argument></argumentList></action>"
"<action><name>GetSortCapabilities</name><argumentList>"
"<argument><name>SortCaps</name><direction>out</direction>"
"<relatedStateVariable>SortCapabilities</relatedStateVariable>"
"</argument></argumentList></action>"
"<action><name>GetSystemUpdateID</name><argumentList>"
"<argument><name>Id</name><direction>out</direction>"
"<relatedStateVariable>SystemUpdateID</relatedStateVariable>"
"</argument></argumentList></action>"
"<action><name>Browse</name><argumentList>"
"<argument><name>ObjectID</name><direction>in</direction>"
"<relatedStateVariable>A_ARG_TYPE_ObjectID</relatedStateVariable></argument>"
"<argument><name>BrowseFlag</name><direction>in</direction>"
"<relatedStateVariable>A_ARG_TYPE_BrowseFlag</relatedStateVariable></argument>"
"<argument><name>Filter</name><direction>in</direction>"
"<relatedStateVariable>A_ARG_TYPE_Filter</relatedStateVariable></argument>"
"<argument><name>StartingIndex</name><direction>in</direction>"
"<relatedStateVariable>A_ARG_TYPE_Index</relatedStateVariable></argument>"
"<argument><name>RequestedCount</name><direction>in</direction>"
"<relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>"
"<argument><name>SortCriteria</name><direction>in</direction>"
"<relatedStateVariable>A_ARG_TYPE_SortCriteria</relatedStateVariable></argument>"
"<argument><name>Result</name><direction>out</direction>"
"<relatedStateVariable>A_ARG_TYPE_Result</relatedStateVariable></argument>"
"<argument><name>NumberReturned</name><direction>out</direction>"
"<relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>"
"<argument><name>TotalMatches</name><direction>out</direction>"
"<relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>"
"<argument><name>UpdateID</name><direction>out</direction>"
"<relatedStateVariable>A_ARG_TYPE_UpdateID</relatedStateVariable></argument>"
"</argumentList></action>"
"</actionList>"
"<serviceStateTable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ObjectID</name>"
"<dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Result</name>"
"<dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_BrowseFlag</name>"
"<dataType>string</dataType><allowedValueList>"
"<allowedValue>BrowseMetadata</allowedValue>"
"<allowedValue>BrowseDirectChildren</allowedValue>"
"</allowedValueList></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Filter</name>"
"<dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_SortCriteria</name>"
"<dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Index</name>"
"<dataType>ui4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Count</name>"
"<dataType>ui4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_UpdateID</name>"
"<dataType>ui4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>SearchCapabilities</name>"
"<dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>SortCapabilities</name>"
"<dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"yes\"><name>SystemUpdateID</name>"
"<dataType>ui4</dataType></stateVariable>"
"<stateVariable sendEvents=\"yes\"><name>ContainerUpdateIDs</name>"
"<dataType>string</dataType></stateVariable>"
"</serviceStateTable>"
"</scpd>";

static const char scpd_connection_manager[] =
"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
"<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
"<specVersion><major>1</major><minor>0</minor></specVersion>"
"<actionList>"
"<action><name>GetProtocolInfo</name><argumentList>"
"<argument><name>Source</name><direction>out</direction>"
"<relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument>"
"<argument><name>Sink</name><direction>out</direction>"
"<relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>GetCurrentConnectionIDs</name><argumentList>"
"<argument><name>ConnectionIDs</name><direction>out</direction>"
"<relatedStateVariable>CurrentConnectionIDs</relatedStateVariable></argument>"
"</argumentList></action>"
"<action><name>GetCurrentConnectionInfo</name><argumentList>"
"<argument><name>ConnectionID</name><direction>in</direction>"
"<relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>"
"<argument><name>RcsID</name><direction>out</direction>"
"<relatedStateVariable>A_ARG_TYPE_RcsID</relatedStateVariable></argument>"
"<argument><name>AVTransportID</name><direction>out</direction>"
"<relatedStateVariable>A_ARG_TYPE_AVTransportID</relatedStateVariable></argument>"
"<argument><name>ProtocolInfo</name><direction>out</direction>"
"<relatedStateVariable>A_ARG_TYPE_ProtocolInfo</relatedStateVariable></argument>"
"<argument><name>PeerConnectionManager</name><direction>out</direction>"
"<relatedStateVariable>A_ARG_TYPE_ConnectionManager</relatedStateVariable></argument>"
"<argument><name>PeerConnectionID</name><direction>out</direction>"
"<relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>"
"<argument><name>Direction</name><direction>out</direction>"
"<relatedStateVariable>A_ARG_TYPE_Direction</relatedStateVariable></argument>"
"<argument><name>Status</name><direction>out</direction>"
"<relatedStateVariable>A_ARG_TYPE_ConnectionStatus</relatedStateVariable></argument>"
"</argumentList></action>"
"</actionList>"
"<serviceStateTable>"
"<stateVariable sendEvents=\"yes\"><name>SourceProtocolInfo</name>"
"<dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"yes\"><name>SinkProtocolInfo</name>"
"<dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"yes\"><name>CurrentConnectionIDs</name>"
"<dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ConnectionStatus</name>"
"<dataType>string</dataType><allowedValueList>"
"<allowedValue>OK</allowedValue><allowedValue>ContentFormatMismatch</allowedValue>"
"<allowedValue>InsufficientBandwidth</allowedValue>"
"<allowedValue>UnreliableChannel</allowedValue>"
"<allowedValue>Unknown</allowedValue></allowedValueList></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ConnectionManager</name>"
"<dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Direction</name>"
"<dataType>string</dataType><allowedValueList>"
"<allowedValue>Input</allowedValue><allowedValue>Output</allowedValue>"
"</allowedValueList></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ProtocolInfo</name>"
"<dataType>string</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ConnectionID</name>"
"<dataType>i4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_AVTransportID</name>"
"<dataType>i4</dataType></stateVariable>"
"<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_RcsID</name>"
"<dataType>i4</dataType></stateVariable>"
"</serviceStateTable>"
"</scpd>";

static char *build_device_desc(size_t *len) {
    struct buf b;
    binit(&b);
    bstr(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    bstr(&b, "<root xmlns=\"urn:schemas-upnp-org:device-1-0\""
              " xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">");
    bstr(&b, "<specVersion><major>1</major><minor>0</minor></specVersion>");
    bstr(&b, "<device>");
    bstr(&b, "<deviceType>" DEVICE_TYPE "</deviceType>");
    bstr(&b, "<friendlyName>");
    besc(&b, friendly);
    bstr(&b, "</friendlyName>");
    bstr(&b, "<manufacturer>darkhttpd</manufacturer>");
    bstr(&b, "<manufacturerURL>https://unix4lyfe.org/darkhttpd/</manufacturerURL>");
    bstr(&b, "<modelDescription>darkhttpd DLNA media server</modelDescription>");
    bstr(&b, "<modelName>darkhttpd</modelName>");
    bstr(&b, "<modelNumber>1.17</modelNumber>");
    bstr(&b, "<UDN>uuid:");
    bstr(&b, uuid);
    bstr(&b, "</UDN>");
    bstr(&b, "<dlna:X_DLNADOC>DMS-1.50</dlna:X_DLNADOC>");
    bstr(&b, "<serviceList>");
    bstr(&b, "<service>");
    bstr(&b, "<serviceType>" CD_SERVICE "</serviceType>");
    bstr(&b, "<serviceId>urn:upnp-org:serviceId:ContentDirectory</serviceId>");
    bstr(&b, "<SCPDURL>/ContentDirectory/scpd.xml</SCPDURL>");
    bstr(&b, "<controlURL>/ContentDirectory/control</controlURL>");
    bstr(&b, "<eventSubURL>/ContentDirectory/event</eventSubURL>");
    bstr(&b, "</service>");
    bstr(&b, "<service>");
    bstr(&b, "<serviceType>" CM_SERVICE "</serviceType>");
    bstr(&b, "<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>");
    bstr(&b, "<SCPDURL>/ConnectionManager/scpd.xml</SCPDURL>");
    bstr(&b, "<controlURL>/ConnectionManager/control</controlURL>");
    bstr(&b, "<eventSubURL>/ConnectionManager/event</eventSubURL>");
    bstr(&b, "</service>");
    bstr(&b, "</serviceList>");
    bstr(&b, "</device></root>");
    return btake(&b, len);
}

/* ------------------------------------------------------------------ */
/* DIDL-Lite content tree                                              */
/* ------------------------------------------------------------------ */

static void didl_begin(struct buf *d) {
    bstr(d, "<DIDL-Lite"
             " xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\""
             " xmlns:dc=\"http://purl.org/dc/elements/1.1/\""
             " xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\""
             " xmlns:dlna=\"urn:schemas-dlna-org:metadata-1-0/\">");
}

/* Count the children we would present for a directory: subdirectories plus
 * files with a known media extension.  Picky TV firmwares use childCount to
 * decide whether a container is worth showing. */
static int count_children(const char *fsdir) {
    DIR *d = opendir(fsdir);
    struct dirent *de;
    int n = 0;

    if (d == NULL) return 0;
    while ((de = readdir(d)) != NULL) {
        size_t nlen = strlen(de->d_name);
        char *fs;
        struct stat st;
        int is_dir;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (de->d_name[0] == '.') continue;
        if (nlen > MAXNAMLEN) continue;

        fs = malloc(strlen(fsdir) + nlen + 2);
        if (fs == NULL) oom();
        snprintf(fs, strlen(fsdir) + nlen + 2, "%s/%s", fsdir, de->d_name);
        if (stat(fs, &st) == -1) { free(fs); continue; }
        is_dir = S_ISDIR(st.st_mode);
        free(fs);

        if (is_dir || mime_for(de->d_name) != NULL) n++;
    }
    closedir(d);
    return n;
}

static void emit_container(struct buf *d, const char *id, const char *parent,
        const char *title, int child_count) {
    bstr(d, "<container id=\"");
    besc(d, id);
    bstr(d, "\" parentID=\"");
    besc(d, parent);
    bprintf(d, "\" childCount=\"%d\" restricted=\"1\" searchable=\"0\">",
        child_count);
    bstr(d, "<dc:title>");
    besc(d, title);
    bstr(d, "</dc:title>");
    bstr(d, "<upnp:class>object.container.storageFolder</upnp:class>");
    bstr(d, "<upnp:storageUsed>-1</upnp:storageUsed>");
    bstr(d, "</container>");
}

static void emit_item(struct buf *d, const char *id, const char *parent,
        const char *title, const char *mime, off_t size) {
    bstr(d, "<item id=\"");
    besc(d, id);
    bstr(d, "\" parentID=\"");
    besc(d, parent);
    bstr(d, "\" restricted=\"1\">");
    bstr(d, "<dc:title>");
    besc(d, title);
    bstr(d, "</dc:title>");
    bstr(d, "<upnp:class>");
    bstr(d, class_for(mime));
    bstr(d, "</upnp:class>");
    bstr(d, "<res ");
    if (size > 0)
        bprintf(d, "size=\"%lld\" ", (long long)size);
    bstr(d, "protocolInfo=\"http-get:*:");
    besc(d, mime);
    bstr(d, ":" DLNA_PROTO_SUFFIX "\">");
    bstr(d, "http://");
    bstr(d, server_addr_str);
    bprintf(d, ":%u", (unsigned)http_port);
    url_enc(d, id);
    bstr(d, "</res>");
    bstr(d, "</item>");
}

struct dirent_ent {
    char *name;
    int is_dir;
};

static int ent_cmp(const void *a, const void *b) {
    const struct dirent_ent *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir; /* dirs first */
    return strcmp(x->name, y->name);
}

/* Build the id string for a child of container `parent_id`. Caller frees. */
static char *child_id(const char *parent_id, const char *name) {
    size_t plen = strlen(parent_id), nlen = strlen(name);
    char *id;
    if (strcmp(parent_id, "0") == 0) {
        id = malloc(nlen + 2);
        if (id == NULL) oom();
        id[0] = '/';
        memcpy(id + 1, name, nlen + 1);
    } else {
        id = malloc(plen + nlen + 2);
        if (id == NULL) oom();
        memcpy(id, parent_id, plen);
        id[plen] = '/';
        memcpy(id + plen + 1, name, nlen + 1);
    }
    return id;
}

/* Join the content root with an absolute object path. Caller frees. */
static char *fs_path(const char *id) {
    size_t rlen = strlen(root), ilen = strlen(id);
    char *p = malloc(rlen + ilen + 1);
    if (p == NULL) oom();
    memcpy(p, root, rlen);
    memcpy(p + rlen, id, ilen + 1);
    return p;
}

/* Browse the children of a directory object. */
static void browse_children(const char *container_id, const char *fsdir,
        int start, int count, struct buf *didl, int *nret, int *total) {
    DIR *d = opendir(fsdir);
    struct dirent *de;
    struct dirent_ent *ents = NULL;
    size_t n = 0, cap = 0, i;
    int emitted = 0;

    *nret = 0;
    *total = 0;
    if (d == NULL) return;

    while ((de = readdir(d)) != NULL) {
        size_t nlen = strlen(de->d_name);
        char *fs;
        struct stat st;
        int is_dir;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (de->d_name[0] == '.') continue;
        if (nlen > MAXNAMLEN) continue;

        fs = malloc(strlen(fsdir) + nlen + 2);
        if (fs == NULL) oom();
        snprintf(fs, strlen(fsdir) + nlen + 2, "%s/%s", fsdir, de->d_name);
        if (stat(fs, &st) == -1) { free(fs); continue; }
        is_dir = S_ISDIR(st.st_mode);
        if (!is_dir && mime_for(de->d_name) == NULL) { free(fs); continue; }
        free(fs);

        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            ents = realloc(ents, cap * sizeof(*ents));
            if (ents == NULL) oom();
        }
        ents[n].name = malloc(nlen + 1);
        if (ents[n].name == NULL) oom();
        strcpy(ents[n].name, de->d_name);
        ents[n].is_dir = is_dir;
        n++;
    }
    closedir(d);

    qsort(ents, n, sizeof(*ents), ent_cmp);
    *total = (int)n;

    for (i = 0; i < n; i++) {
        char *id;
        char parent[512];
        char *fs;
        struct stat st;

        if ((int)i < start) continue;
        if (count > 0 && emitted >= count) break;

        id = child_id(container_id, ents[i].name);
        fs = fs_path(id);
        if (stat(fs, &st) == -1) { free(id); free(fs); continue; }

        if (strcmp(container_id, "0") == 0)
            snprintf(parent, sizeof(parent), "0");
        else
            snprintf(parent, sizeof(parent), "%s", container_id);

        if (ents[i].is_dir) {
            emit_container(didl, id, parent, ents[i].name, count_children(fs));
        } else {
            const char *mime = mime_for(ents[i].name);
            emit_item(didl, id, parent, ents[i].name, mime, st.st_size);
        }
        emitted++;
        free(id);
        free(fs);
    }

    for (i = 0; i < n; i++) free(ents[i].name);
    free(ents);
    *nret = emitted;
}

/* Browse a single object (BrowseMetadata). */
static void browse_object(const char *object_id, struct buf *didl,
        int *nret, int *total) {
    char parent[512];

    *nret = 0;
    *total = 0;

    if (strcmp(object_id, "0") == 0) {
        emit_container(didl, "0", "-1", friendly, count_children(root));
        *nret = 1;
        *total = 1;
        return;
    }

    {
        char *fs = fs_path(object_id);
        struct stat st;
        if (stat(fs, &st) == 0) {
            parent_of(object_id, parent, sizeof(parent));
            if (S_ISDIR(st.st_mode)) {
                emit_container(didl, object_id, parent,
                    path_basename(object_id), count_children(fs));
            } else {
                const char *mime = mime_for(object_id);
                if (mime != NULL)
                    emit_item(didl, object_id, parent,
                        path_basename(object_id), mime, st.st_size);
            }
            *nret = 1;
            *total = 1;
        }
        free(fs);
    }
}

/* Produce a DIDL-Lite document for a Browse request. Caller frees. */
static char *didl_browse(const char *object_id, const char *browse_flag,
        int start, int count, int *nret, int *total) {
    struct buf didl;
    char idbuf[1024];
    const char *id = object_id;

    /* Normalise the requested object id. */
    if (id == NULL || id[0] == '\0') id = "0";
    if (strcmp(id, "0") == 0 || strcmp(id, "/") == 0) {
        id = "0";
    } else {
        char *decoded = path_decode(id);
        if (!path_safe(decoded)) {
            free(decoded);
            *nret = 0;
            *total = 0;
            binit(&didl);
            didl_begin(&didl);
            bstr(&didl, "</DIDL-Lite>");
            return btake(&didl, NULL);
        }
        snprintf(idbuf, sizeof(idbuf), "%s", decoded);
        free(decoded);
        id = idbuf;
    }

    binit(&didl);
    didl_begin(&didl);

    if (browse_flag != NULL && strcmp(browse_flag, "BrowseMetadata") == 0) {
        browse_object(id, &didl, nret, total);
    } else {
        const char *fsdir;
        char *fs = NULL;
        if (strcmp(id, "0") == 0) {
            fsdir = root;
        } else {
            fs = fs_path(id);
            fsdir = fs;
        }
        browse_children(id, fsdir, start, count, &didl, nret, total);
        free(fs);
    }

    bstr(&didl, "</DIDL-Lite>");
    return btake(&didl, NULL);
}

/* ------------------------------------------------------------------ */
/* SOAP                                                                */
/* ------------------------------------------------------------------ */

static void soap_begin(struct buf *b) {
    bstr(b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    bstr(b, "<s:Envelope "
            "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
            "<s:Body>");
}

static void soap_end(struct buf *b) {
    bstr(b, "</s:Body></s:Envelope>");
}

static void soap_action_begin(struct buf *b, const char *service,
        const char *action) {
    soap_begin(b);
    bprintf(b, "<u:%sResponse "
            "xmlns:u=\"urn:schemas-upnp-org:service:%s:1\">",
            action, service);
}

static void soap_action_end(struct buf *b, const char *action) {
    bprintf(b, "</u:%sResponse>", action);
    soap_end(b);
}

static void soap_fault(struct buf *b, int code, const char *text) {
    soap_begin(b);
    bstr(b, "<s:Fault><faultcode>s:Client</faultcode>"
            "<faultstring>UPnPError</faultstring><detail>"
            "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">");
    bprintf(b, "<errorCode>%d</errorCode>", code);
    bstr(b, "<errorDescription>");
    besc(b, text);
    bstr(b, "</errorDescription></UPnPError></detail></s:Fault>");
    soap_end(b);
}

static void soap_protocol_info(struct buf *b) {
    size_t i;
    for (i = 0; media_types[i].ext != NULL; i++) {
        size_t j;
        int seen = 0;
        for (j = 0; j < i; j++) {
            if (strcmp(media_types[j].mime, media_types[i].mime) == 0) {
                seen = 1;
                break;
            }
        }
        if (seen) continue;
        bprintf(b, "http-get:*:%s:%s\n",
            media_types[i].mime, DLNA_PROTO_SUFFIX);
    }
}

/* Handle a ContentDirectory or ConnectionManager control request. */
static int soap_control(const char *service, const char *request,
        char **response, size_t *response_length) {
    char *sa, action[64];
    struct buf b;

    action[0] = '\0';
    sa = header_value(request, "SOAPAction");
    if (sa != NULL) {
        char *hash = strchr(sa, '#');
        if (hash != NULL) {
            const char *p = hash + 1;
            size_t n = 0;
            while (p[n] != '\0' && p[n] != '"' && p[n] != '\r' &&
                   p[n] != '\n' && n < sizeof(action) - 1) {
                action[n] = p[n];
                n++;
            }
            action[n] = '\0';
        }
        free(sa);
    }

    binit(&b);

    if (strcmp(service, "ContentDirectory") == 0) {
        if (strcmp(action, "Browse") == 0) {
            char object_id[1024] = "0";
            char browse_flag[64] = "BrowseDirectChildren";
            char tmp[32];
            int start = 0, count = 0, nret = 0, total = 0;
            char *didl;

            xml_value(request, "ObjectID", object_id, sizeof(object_id));
            xml_value(request, "BrowseFlag", browse_flag, sizeof(browse_flag));
            if (xml_value(request, "StartingIndex", tmp, sizeof(tmp)))
                start = atoi(tmp);
            if (xml_value(request, "RequestedCount", tmp, sizeof(tmp)))
                count = atoi(tmp);
            if (start < 0) start = 0;
            if (count < 0) count = 0;

            didl = didl_browse(object_id, browse_flag, start, count,
                &nret, &total);

            soap_action_begin(&b, service, "Browse");
            bstr(&b, "<Result>");
            besc(&b, didl);
            bstr(&b, "</Result>");
            bprintf(&b, "<NumberReturned>%d</NumberReturned>", nret);
            bprintf(&b, "<TotalMatches>%d</TotalMatches>", total);
            bstr(&b, "<UpdateID>0</UpdateID>");
            soap_action_end(&b, "Browse");
            free(didl);
        } else if (strcmp(action, "GetSearchCapabilities") == 0) {
            soap_action_begin(&b, service, action);
            bstr(&b, "<SearchCaps></SearchCaps>");
            soap_action_end(&b, action);
        } else if (strcmp(action, "GetSortCapabilities") == 0) {
            soap_action_begin(&b, service, action);
            bstr(&b, "<SortCaps></SortCaps>");
            soap_action_end(&b, action);
        } else if (strcmp(action, "GetSystemUpdateID") == 0) {
            soap_action_begin(&b, service, action);
            bstr(&b, "<Id>0</Id>");
            soap_action_end(&b, action);
        } else {
            soap_fault(&b, 401, "Invalid Action");
            *response = btake(&b, response_length);
            return 500;
        }
    } else { /* ConnectionManager */
        if (strcmp(action, "GetProtocolInfo") == 0) {
            soap_action_begin(&b, service, action);
            bstr(&b, "<Source>");
            soap_protocol_info(&b);
            bstr(&b, "</Source><Sink></Sink>");
            soap_action_end(&b, action);
        } else if (strcmp(action, "GetCurrentConnectionIDs") == 0) {
            soap_action_begin(&b, service, action);
            bstr(&b, "<ConnectionIDs>0</ConnectionIDs>");
            soap_action_end(&b, action);
        } else if (strcmp(action, "GetCurrentConnectionInfo") == 0) {
            soap_action_begin(&b, service, action);
            bstr(&b, "<RcsID>0</RcsID><AVTransportID>0</AVTransportID>"
                    "<ProtocolInfo></ProtocolInfo>"
                    "<PeerConnectionManager></PeerConnectionManager>"
                    "<PeerConnectionID>-1</PeerConnectionID>"
                    "<Direction>Output</Direction><Status>OK</Status>");
            soap_action_end(&b, action);
        } else {
            soap_fault(&b, 401, "Invalid Action");
            *response = btake(&b, response_length);
            return 500;
        }
    }

    *response = btake(&b, response_length);
    return 200;
}

/* ------------------------------------------------------------------ */
/* HTTP response construction                                          */
/* ------------------------------------------------------------------ */

static int make_response(char **response, size_t *response_length, int code,
        const char *reason, const char *ctype, const char *body,
        size_t body_length) {
    struct buf b;
    size_t n;

    binit(&b);
    bprintf(&b, "HTTP/1.1 %d %s\r\n", code, reason);
    if (ctype != NULL)
        bprintf(&b, "Content-Type: %s\r\n", ctype);
    bprintf(&b, "Content-Length: %llu\r\n", (unsigned long long)body_length);
    bprintf(&b, "Server: %s\r\n", UPNP_SERVER);
    bstr(&b, "Connection: close\r\n");
    bstr(&b, "EXT:\r\n");
    bstr(&b, "\r\n");
    if (body != NULL && body_length > 0)
        braw(&b, body, body_length);

    *response = btake(&b, &n);
    *response_length = n;
    return code;
}

static int method_not_allowed(char **response, size_t *response_length) {
    return make_response(response, response_length, 405, "Method Not Allowed",
        "text/plain; charset=utf-8", "Method not allowed.\n", 20);
}

/* SUBSCRIBE/UNSUBSCRIBE on an event URL.  We accept subscriptions but never
 * deliver events; this is enough for most control points. */
static int event_request(const char *method, char **response,
        size_t *response_length) {
    struct buf b;
    size_t n;

    if (strcasecmp(method, "SUBSCRIBE") != 0 &&
        strcasecmp(method, "UNSUBSCRIBE") != 0)
        return method_not_allowed(response, response_length);

    binit(&b);
    bstr(&b, "HTTP/1.1 200 OK\r\n");
    bprintf(&b, "SID: uuid:%s\r\n", uuid);
    bstr(&b, "TIMEOUT: Second-1800\r\n");
    bstr(&b, "Server: " UPNP_SERVER "\r\n");
    bstr(&b, "Content-Length: 0\r\n");
    bstr(&b, "Connection: close\r\n");
    bstr(&b, "\r\n");
    *response = btake(&b, &n);
    *response_length = n;
    return 200;
}

int upnp_http_request(const char *method, const char *url,
        const char *request, size_t request_length,
        char **response, size_t *response_length) {
    char path[512];
    const char *q, *p;
    size_t n;
    int is_get, is_post;

    (void)request_length;

    if (!active) return 0;

    p = url;
    q = strchr(url, '?');
    n = q != NULL ? (size_t)(q - url) : strlen(url);

    /* Tolerate absolute-form request targets (http://host/path). */
    if (n > 7 && strncasecmp(p, "http://", 7) == 0) {
        const char *slash = memchr(p + 7, '/', n - 7);
        if (slash == NULL) { p = "/"; n = 1; }
        else { n -= (size_t)(slash - p); p = slash; }
    } else if (n > 8 && strncasecmp(p, "https://", 8) == 0) {
        const char *slash = memchr(p + 8, '/', n - 8);
        if (slash == NULL) { p = "/"; n = 1; }
        else { n -= (size_t)(slash - p); p = slash; }
    }

    if (n >= sizeof(path)) return 0;
    memcpy(path, p, n);
    path[n] = '\0';

    is_get = strcasecmp(method, "GET") == 0 ||
             strcasecmp(method, "HEAD") == 0;
    is_post = strcasecmp(method, "POST") == 0;

    if (strcmp(path, "/rootDesc.xml") == 0) {
        char *body;
        size_t blen;
        int code;
        if (!is_get) return method_not_allowed(response, response_length);
        body = build_device_desc(&blen);
        code = make_response(response, response_length, 200, "OK",
            "text/xml; charset=\"utf-8\"", body, blen);
        free(body);
        return code;
    }

    if (strcmp(path, "/ContentDirectory/scpd.xml") == 0) {
        if (!is_get) return method_not_allowed(response, response_length);
        return make_response(response, response_length, 200, "OK",
            "text/xml; charset=\"utf-8\"", scpd_content_directory,
            strlen(scpd_content_directory));
    }

    if (strcmp(path, "/ConnectionManager/scpd.xml") == 0) {
        if (!is_get) return method_not_allowed(response, response_length);
        return make_response(response, response_length, 200, "OK",
            "text/xml; charset=\"utf-8\"", scpd_connection_manager,
            strlen(scpd_connection_manager));
    }

    if (strcmp(path, "/ContentDirectory/control") == 0 ||
        strcmp(path, "/ConnectionManager/control") == 0) {
        char *body = NULL;
        size_t blen = 0;
        int code;
        const char *service;
        if (!is_post) return method_not_allowed(response, response_length);
        service = strncmp(path, "/ContentDirectory", 17) == 0
            ? "ContentDirectory" : "ConnectionManager";
        code = soap_control(service, request, &body, &blen);
        code = make_response(response, response_length, code,
            code == 200 ? "OK" : "Internal Server Error",
            "text/xml; charset=\"utf-8\"", body, blen);
        free(body);
        return code;
    }

    if (strcmp(path, "/ContentDirectory/event") == 0 ||
        strcmp(path, "/ConnectionManager/event") == 0) {
        return event_request(method, response, response_length);
    }

    return 0;
}

const char *upnp_media_headers(const char *mimetype) {
    static char buf[192];
    const char *mode;

    if (mimetype == NULL) return "";
    if (strncmp(mimetype, "video/", 6) == 0)
        mode = "Streaming";
    else if (strncmp(mimetype, "audio/", 6) == 0)
        mode = "Streaming";
    else if (strncmp(mimetype, "image/", 6) == 0)
        mode = "Interactive";
    else
        return "";

    snprintf(buf, sizeof(buf),
        "transferMode.dlna.org: %s\r\n"
        "contentFeatures.dlna.org: " DLNA_PROTO_SUFFIX "\r\n",
        mode);
    return buf;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

int upnp_init(const char *friendly_name, const char *wwwroot,
        unsigned short port, const char *bind_addr, const char *advertise_ip) {
    char seed[512];
    char host[128];

    if (root != NULL) return 0; /* already initialized */

    if (friendly_name != NULL && friendly_name[0] != '\0') {
        snprintf(friendly, sizeof(friendly), "%s", friendly_name);
    } else if (gethostname(host, sizeof(host)) == 0) {
        host[sizeof(host) - 1] = '\0';
        snprintf(friendly, sizeof(friendly), "darkhttpd (%s)", host);
    } else {
        snprintf(friendly, sizeof(friendly), "darkhttpd");
    }

    root = strdup(wwwroot != NULL ? wwwroot : ".");
    if (root == NULL) oom();
    /* Strip a trailing slash (but keep a lone "/"). */
    {
        size_t rlen = strlen(root);
        while (rlen > 1 && root[rlen - 1] == '/') root[--rlen] = '\0';
    }

    http_port = port;
    bootid = (unsigned int)time(NULL);

    /* If the operator chose the address, advertise exactly that and never
     * re-detect. */
    ip_fixed =
        (advertise_ip != NULL && strcmp(advertise_ip, "0.0.0.0") != 0) ||
        (bind_addr != NULL && strcmp(bind_addr, "0.0.0.0") != 0);

    detect_addr(bind_addr, advertise_ip, &server_addr);
    if (inet_ntop(AF_INET, &server_addr, server_addr_str,
                  sizeof(server_addr_str)) == NULL)
        snprintf(server_addr_str, sizeof(server_addr_str), "127.0.0.1");

    snprintf(seed, sizeof(seed), "%s|%s|%u", friendly, root,
        (unsigned)http_port);
    make_uuid(seed);

    ssdp_sock = ssdp_open();
    if (ssdp_sock == -1)
        printf("  SSDP:          unavailable (no discovery)\n");

    active = 1;
    next_alive = 0;      /* announce immediately */
    alive_burst = 2;     /* and twice more shortly after */

    printf("DLNA/UPnP media server enabled:\n");
    printf("  friendly name: %s\n", friendly);
    printf("  uuid:          uuid:%s\n", uuid);
    printf("  description:   %s\n", location_url());

    return 0;
}

void upnp_shutdown(void) {
    if (!active) return;
    if (ssdp_sock != -1) {
        ssdp_announce(1); /* ssdp:byebye */
        close(ssdp_sock);
        ssdp_sock = -1;
    }
    active = 0;
    free(root);
    root = NULL;
}

/* vim:set ts=4 sw=4 sts=4 expandtab tw=78: */
