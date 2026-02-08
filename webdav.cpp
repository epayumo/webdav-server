#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <stdint.h>
#include <direct.h>
#include <getopt.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define mkdir(path, mode) _mkdir(path)
#define lstat stat
#else
#include <unistd.h>
#endif
#include <sys/types.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>
#ifndef _WIN32
#include <strings.h>
#include <pwd.h>
#include <grp.h>
#endif
#include <assert.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <sys/resource.h>
#include <poll.h>  // Para poll()
#endif
/* ==================== CONFIGURAÇÕES ==================== */
#define BACKLOG 128
#define RECV_BUF 8192
#define SEND_BUF 8192
#define SMALL_BUF 512
#ifdef _WIN32
#undef MAX_PATH
#endif
#define MAX_PATH 4096
#define MAX_ENCODED_PATH (MAX_PATH * 3)
#define MAX_BODY 1099511627776LL
#define MAX_MULTIPART_BODY (100LL * 1024 * 1024)  // 100 MB hard limit for multipart POST
#define MAX_CONNECTIONS 100
#define DEFAULT_TIMEOUT 30
#define LOCK_TIMEOUT_DEFAULT 600
#define RATE_LIMIT_WINDOW 60
#define RATE_LIMIT_MAX_REQ 1000
/* ==================== TIPOS E ESTRUTURAS ==================== */
typedef enum {
HTTP_200_OK = 200, HTTP_201_CREATED = 201, HTTP_204_NO_CONTENT = 204,
HTTP_206_PARTIAL_CONTENT = 206, HTTP_207_MULTI_STATUS = 207,
HTTP_303_SEE_OTHER = 303, HTTP_304_NOT_MODIFIED = 304,
HTTP_400_BAD_REQUEST = 400, HTTP_401_UNAUTHORIZED = 401,
HTTP_403_FORBIDDEN = 403, HTTP_404_NOT_FOUND = 404,
HTTP_405_METHOD_NOT_ALLOWED = 405, HTTP_409_CONFLICT = 409,
HTTP_412_PRECONDITION_FAILED = 412, HTTP_413_PAYLOAD_TOO_LARGE = 413,
HTTP_416_RANGE_NOT_SATISFIABLE = 416, HTTP_423_LOCKED = 423,
HTTP_429_TOO_MANY_REQUESTS = 429, HTTP_500_INTERNAL_SERVER_ERROR = 500,
HTTP_501_NOT_IMPLEMENTED = 501, HTTP_503_SERVICE_UNAVAILABLE = 503,
HTTP_507_INSUFFICIENT_STORAGE = 507
} http_status_t;
typedef enum { LOCK_EXCLUSIVE, LOCK_SHARED } lock_type_t;
typedef enum { DEPTH_ZERO = 0, DEPTH_ONE = 1, DEPTH_INFINITY = -1 } depth_t;
typedef struct {
char token[64]; lock_type_t type; char owner[256]; char path[MAX_PATH];
time_t created; time_t expires; int depth;
} lock_entry_t;
typedef struct {
char keys[128][SMALL_BUF]; char vals[128][SMALL_BUF]; int count;
} headers_t;
typedef struct {
char *data; size_t length; size_t capacity;
} buffer_t;
typedef struct {
char ip[INET_ADDRSTRLEN + 1]; time_t window_start; int request_count;
pthread_mutex_t mutex;
} rate_limit_entry_t;
/* ==================== VARIÁVEIS GLOBAIS ==================== */
static char *ROOT_DIR = NULL;
static char *AUTH_USER = NULL;
static char *AUTH_PASS = NULL;
static int VERBOSE = 0;
static int PORT = 8080;
static int TIMEOUT = DEFAULT_TIMEOUT;
static int MAX_REQ = RATE_LIMIT_MAX_REQ;
static volatile sig_atomic_t RUNNING = 1;
#ifdef _WIN32
static pthread_mutex_t lock_mutex;
#else
static pthread_mutex_t lock_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
static lock_entry_t locks[100]; static int lock_count = 0;
#ifdef _WIN32
static pthread_mutex_t rate_mutex;
#else
static pthread_mutex_t rate_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
static rate_limit_entry_t rate_table[1000]; static int rate_count = 0;
#ifdef __APPLE__
typedef struct {
pthread_mutex_t mutex;
pthread_cond_t cond;
int count;
} conn_sem_t;

static int conn_sem_init(conn_sem_t *s, unsigned int value) {
if (pthread_mutex_init(&s->mutex, NULL) != 0) return -1;
if (pthread_cond_init(&s->cond, NULL) != 0) {
pthread_mutex_destroy(&s->mutex);
return -1;
}
s->count = (int)value;
return 0;
}
static int conn_sem_wait(conn_sem_t *s) {
pthread_mutex_lock(&s->mutex);
while (s->count == 0) pthread_cond_wait(&s->cond, &s->mutex);
s->count--;
pthread_mutex_unlock(&s->mutex);
return 0;
}
static int conn_sem_trywait(conn_sem_t *s) {
int rc = -1;
pthread_mutex_lock(&s->mutex);
if (s->count > 0) {
s->count--;
rc = 0;
} else {
errno = EAGAIN;
}
pthread_mutex_unlock(&s->mutex);
return rc;
}
static int conn_sem_post(conn_sem_t *s) {
pthread_mutex_lock(&s->mutex);
s->count++;
pthread_cond_signal(&s->cond);
pthread_mutex_unlock(&s->mutex);
return 0;
}
static int conn_sem_destroy(conn_sem_t *s) {
pthread_cond_destroy(&s->cond);
pthread_mutex_destroy(&s->mutex);
return 0;
}
#else
typedef sem_t conn_sem_t;
static int conn_sem_init(conn_sem_t *s, unsigned int value) { return sem_init(s, 0, value); }
static int conn_sem_wait(conn_sem_t *s) { return sem_wait(s); }
static int conn_sem_trywait(conn_sem_t *s) { return sem_trywait(s); }
static int conn_sem_post(conn_sem_t *s) { return sem_post(s); }
static int conn_sem_destroy(conn_sem_t *s) { return sem_destroy(s); }
#endif
static conn_sem_t connection_sem;
static int self_pipe[2] = {-1, -1};  // Self-pipe trick para shutdown gracioso
/* ==================== DESIGN BY CONTRACT ==================== */
#define REQUIRE(condition, msg) do { if (!(condition)) { log_error("PRE-CONDITION FAILED: %s at %s:%d", msg, __FILE__, __LINE__); return; } } while(0)
#define REQUIRE_RET(condition, msg, ret) do { if (!(condition)) { log_error("PRE-CONDITION FAILED: %s at %s:%d", msg, __FILE__, __LINE__); return ret; } } while(0)
#define ENSURE(condition, msg) do { if (!(condition)) { log_error("POST-CONDITION FAILED: %s at %s:%d", msg, __FILE__, __LINE__); } } while(0)
/* ==================== LOGGING ==================== */
static void log_msg(const char *level, const char *fmt, ...) {
if (!VERBOSE && strcmp(level, "ERROR") != 0) return;
time_t now = time(NULL); struct tm *tm_info = localtime(&now); char timebuf[32];
strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_info);
fprintf(stderr, "[%s] [%s] ", timebuf, level);
va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
fprintf(stderr, "\n"); fflush(stderr);
}
#define log_debug(fmt, ...) log_msg("DEBUG", fmt, ##__VA_ARGS__)
#define log_info(fmt, ...) log_msg("INFO", fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...) log_msg("WARN", fmt, ##__VA_ARGS__)
#define log_error(fmt, ...) log_msg("ERROR", fmt, ##__VA_ARGS__)
/* ==================== SELF-PIPE TRICK ==================== */
static void setup_self_pipe(void) {
#if defined(_WIN32) || defined(__COSMOPOLITAN__)
self_pipe[0] = -1; self_pipe[1] = -1;
return;
#else
if (pipe(self_pipe) != 0) {
perror("pipe (self-pipe)");
exit(1);
}
// Tornar ambos os fds não-bloqueantes
fcntl(self_pipe[0], F_SETFL, O_NONBLOCK);
fcntl(self_pipe[1], F_SETFL, O_NONBLOCK);
#endif
}
static void trigger_shutdown(void) {
#if !defined(_WIN32) && !defined(__COSMOPOLITAN__)
if (self_pipe[1] != -1) {
char byte = 'Q';
if (write(self_pipe[1], &byte, 1) < 0) {}  // Acorda o poll()
}
#endif
}
static void sigint_handler(int sig) {
(void)sig;
RUNNING = 0;
trigger_shutdown();  // Acorda o poll() bloqueado
}
static void sigterm_handler(int sig) {
(void)sig;
RUNNING = 0;
trigger_shutdown();
}
#ifndef _WIN32
static void sigpipe_handler(int sig) { (void)sig; }
#endif

static void close_socket_fd(int fd) {
#ifdef _WIN32
closesocket((SOCKET)fd);
#else
close(fd);
#endif
}

#ifdef _WIN32
static int windows_socket_init(void) {
WSADATA wsa_data;
if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
log_error("WSAStartup failed");
return -1;
}
return 0;
}
#endif
/* ==================== UTILITÁRIOS ==================== */

static int parse_http_date_gmt(const char *s, time_t *out) {
REQUIRE_RET(s != NULL, "date string cannot be NULL", 0);
REQUIRE_RET(out != NULL, "out cannot be NULL", 0);
#ifdef _WIN32
int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
char mon[4] = {0};
if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d GMT", &day, mon, &year, &hh, &mm, &ss) != 6) return 0;
const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
const char *m = strstr(months, mon);
if (!m) return 0;
int month = (int)((m - months) / 3);
struct tm tm_if;
memset(&tm_if, 0, sizeof(tm_if));
tm_if.tm_mday = day;
tm_if.tm_mon = month;
tm_if.tm_year = year - 1900;
tm_if.tm_hour = hh;
tm_if.tm_min = mm;
tm_if.tm_sec = ss;
*out = _mkgmtime(&tm_if);
return (*out != (time_t)-1);
#else
struct tm tm_if;
memset(&tm_if, 0, sizeof(tm_if));
if (!strptime(s, "%a, %d %b %Y %H:%M:%S GMT", &tm_if)) return 0;
*out = timegm(&tm_if);
return 1;
#endif
}

static void trim_inplace(char *s) {
if (!s || !*s) return;
char *start = s; while (*start && isspace((unsigned char)*start)) start++;
if (start != s) memmove(s, start, strlen(start) + 1);
size_t len = strlen(s); while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}
static void url_decode(char *dst, const char *src) {
REQUIRE(dst != NULL, "dst cannot be NULL"); REQUIRE(src != NULL, "src cannot be NULL");
char *d = dst; const char *s = src; char a, b;
while (*s) {
if ((*s == '%') && ((a = s[1]) && (b = s[2])) && (isxdigit((unsigned char)a) && isxdigit((unsigned char)b))) {
char hex[3] = {a, b, '\0'}; *d++ = (char)strtol(hex, NULL, 16); s += 3;
} else if (*s == '+') { *d++ = ' '; s++; }
else { *d++ = *s++; }
}
*d = '\0'; ENSURE(dst != NULL, "url_decode produced valid output");
}
static void url_encode(char *dst, size_t dstlen, const char *src) {
REQUIRE(dst != NULL, "dst cannot be NULL"); REQUIRE(src != NULL, "src cannot be NULL"); REQUIRE(dstlen > 0, "dstlen must be > 0");
size_t i = 0;
for (; *src && i < dstlen - 1; src++) {
if (isalnum((unsigned char)*src) || strchr("-_.~/", (unsigned char)*src)) dst[i++] = *src;
else {
if (i + 3 >= dstlen) break;
snprintf(dst + i, dstlen - i, "%%%02X", (unsigned char)*src); i += 3;
}
}
dst[i] = '\0'; ENSURE(i < dstlen, "url_encode did not overflow");
}
static void xml_escape(char *dst, size_t dstlen, const char *src) {
REQUIRE(dst != NULL, "dst cannot be NULL"); REQUIRE(src != NULL, "src cannot be NULL"); REQUIRE(dstlen > 0, "dstlen must be > 0");
size_t i = 0;
for (; *src && i < dstlen - 1; src++) {
switch (*src) {
case '&': if (i + 5 >= dstlen) break; memcpy(dst + i, "&amp;", 5); i += 5; break;
case '<': if (i + 4 >= dstlen) break; memcpy(dst + i, "&lt;", 4); i += 4; break;
case '>': if (i + 4 >= dstlen) break; memcpy(dst + i, "&gt;", 4); i += 4; break;
case '"': if (i + 6 >= dstlen) break; memcpy(dst + i, "&quot;", 6); i += 6; break;
case '\'': if (i + 6 >= dstlen) break; memcpy(dst + i, "&apos;", 6); i += 6; break;
default: dst[i++] = *src; break;
}
}
dst[i] = '\0'; ENSURE(i < dstlen, "xml_escape did not overflow");
}
static char* http_time(time_t t) {
static __thread char buf[64]; struct tm tm_info; gmtime_r(&t, &tm_info);
strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_info); return buf;
}
static char* iso8601_time(time_t t) {
static __thread char buf[64]; struct tm tm_info; gmtime_r(&t, &tm_info);
strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_info); return buf;
}
/* ==================== BASE64 ==================== */
static int base64_decode(const char *src, char *dst, size_t dstlen) {
REQUIRE_RET(src != NULL, "src cannot be NULL", -1); REQUIRE_RET(dst != NULL, "dst cannot be NULL", -1);
static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int map[256]; for (int i = 0; i < 256; i++) map[i] = -1;
for (int i = 0; i < 64; i++) map[(unsigned char)tbl[i]] = i;
map['='] = 0;
unsigned char quartet[4]; int qidx = 0; size_t outidx = 0;
for (const unsigned char *p = (const unsigned char *)src; *p && outidx < dstlen - 1; ++p) {
unsigned char c = *p;
if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
if (map[c] == -1) return -1;
quartet[qidx++] = c;
if (qidx == 4) {
int vals[4]; for (int i = 0; i < 4; i++) vals[i] = (quartet[i] == '=') ? -1 : map[quartet[i]];
unsigned char b0 = (unsigned char)((vals[0] << 2) | ((vals[1] & 0x30) >> 4));
unsigned char b1 = 0, b2 = 0;
if (vals[1] != -1) b1 = (unsigned char)(((vals[1] & 0x0F) << 4) | ((vals[2] & 0x3C) >> 2));
if (vals[2] != -1) b2 = (unsigned char)(((vals[2] & 0x03) << 6) | (vals[3] & 0x3F));
dst[outidx++] = b0;
if (quartet[2] != '=' && outidx < dstlen - 1) dst[outidx++] = b1;
if (quartet[3] != '=' && outidx < dstlen - 1) dst[outidx++] = b2;
qidx = 0;
}
}
if (qidx != 0) {
if (qidx == 1) return -1;
unsigned char pad[4] = {'=', '=', '=', '='};
for (int i = 0; i < qidx; i++) pad[i] = quartet[i];
int vals[4]; for (int i = 0; i < 4; i++) vals[i] = (pad[i] == '=') ? -1 : map[pad[i]];
unsigned char b0 = (unsigned char)((vals[0] << 2) | ((vals[1] & 0x30) >> 4));
dst[outidx++] = b0;
}
dst[outidx] = '\0'; return 0;
}
/* ==================== AUTENTICAÇÃO ==================== */
static int check_auth(const char *auth_header) {
if (!AUTH_USER && !AUTH_PASS) return 1;           // auth desabilitada
if (!AUTH_USER || !AUTH_PASS) { log_warn("Authentication misconfigured: user/password missing"); return 0; }
if (!auth_header) { log_debug("No Authorization header"); return 0; }
log_debug("Received Authorization: %.50s", auth_header);
if (strncasecmp(auth_header, "Basic ", 6) != 0) { log_debug("Not Basic auth"); return 0; }
const char *encoded = auth_header + 6; char decoded[1024];
if (base64_decode(encoded, decoded, sizeof(decoded)) != 0) { log_debug("Base64 decode failed"); return 0; }
char *colon = strchr(decoded, ':'); if (!colon) { log_debug("No colon in decoded"); return 0; }
*colon = '\0'; char *user = decoded; char *pass = colon + 1;
log_debug("Auth attempt - User: %s", user);
if (strcmp(user, AUTH_USER) == 0 && strcmp(pass, AUTH_PASS) == 0) { log_debug("Auth success"); return 1; }
else { log_debug("Auth fail"); return 0; }
}
/* ==================== PATH VALIDATION ==================== */
static int validate_path(const char *path) {
REQUIRE_RET(path != NULL, "path cannot be NULL", -1);
if (strstr(path, "..") != NULL) { log_warn("Path validation failed: contains '..'"); return -1; }
if (strstr(path, "//") != NULL) { log_warn("Path validation failed: contains '//'"); return -1; }
if (strstr(path, "/./") != NULL) { log_warn("Path validation failed: contains '/./'"); return -1; }
return 0;
}
static int build_fs_path(const char *root, const char *reqpath, char *out, size_t outlen) {
REQUIRE_RET(root != NULL, "root cannot be NULL", -1);
REQUIRE_RET(reqpath != NULL, "reqpath cannot be NULL", -1);
REQUIRE_RET(out != NULL, "out cannot be NULL", -1);
REQUIRE_RET(outlen > 0, "outlen must be > 0", -1);
char decoded[MAX_PATH]; url_decode(decoded, reqpath); char *q = strchr(decoded, '?');
if (q) *q = '\0';
char tmp[MAX_PATH + 1];
if (decoded[0] != '/') {
if (strlen(decoded) >= sizeof(tmp) - 2) { log_warn("Path too long: %s", decoded); return -1; }
snprintf(tmp, sizeof(tmp), "/%s", decoded);
} else {
if (strlen(decoded) >= sizeof(tmp)) { log_warn("Path too long: %s", decoded); return -1; }
strncpy(tmp, decoded, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = '\0';
}
if (validate_path(tmp) != 0) { log_warn("Path forbidden: %s", tmp); return -1; }
char full[MAX_PATH + 1];
if (snprintf(full, sizeof(full), "%s%s", root, tmp) >= (int)sizeof(full)) {
log_warn("Path too long: %s%s", root, tmp); return -1;
}
char real[MAX_PATH + 1];
if (realpath(full, real) == NULL) {
if (errno != ENOENT) { log_error("realpath failed: %s (%s)", full, strerror(errno)); return -1; }
char parent[MAX_PATH + 1]; strncpy(parent, full, sizeof(parent) - 1); parent[sizeof(parent) - 1] = '\0';
char *p = parent + strlen(parent); while (p > parent && *p != '/') p--;
if (p == parent) return -1;
*p = '\0';
char realparent[MAX_PATH + 1];
if (realpath(parent, realparent) == NULL) return -1;
if (snprintf(real, sizeof(real), "%s%s", realparent, full + strlen(parent)) >= (int)sizeof(real)) {
log_warn("Path too long after realpath"); return -1;
}
}
char realroot[MAX_PATH + 1]; if (!realpath(root, realroot)) return -1;
size_t rlen = strlen(realroot);
if (strncmp(real, realroot, rlen) != 0 || (real[rlen] != '\0' && real[rlen] != '/')) {
log_warn("Path outside root: %s", real); return -1;
}
if (strlen(real) >= outlen) { log_warn("Output buffer too small for path"); return -1; }
strcpy(out, real); return 0;
}
/* ==================== LOCKS ==================== */
static int generate_lock_token(char *token, size_t len) {
REQUIRE_RET(token != NULL, "token cannot be NULL", -1);
struct timeval tv; gettimeofday(&tv, NULL);
unsigned long long timestamp = (unsigned long long)tv.tv_sec * 1000000 + tv.tv_usec;
unsigned long pid = (unsigned long)getpid();
snprintf(token, len, "opaquelocktoken:%llx-%lx-%x", timestamp, pid, (unsigned int)rand());
return 0;
}
static lock_entry_t* find_lock(const char *path, int exact) {
pthread_mutex_lock(&lock_mutex); time_t now = time(NULL);
for (int i = 0; i < lock_count; ) {
if (locks[i].expires < now) {
memmove(&locks[i], &locks[i + 1], sizeof(lock_entry_t) * (lock_count - i - 1));
lock_count--; continue;
}
if (exact) {
if (strcmp(locks[i].path, path) == 0) {
lock_entry_t *result = &locks[i]; pthread_mutex_unlock(&lock_mutex); return result;
}
} else {
if (strncmp(path, locks[i].path, strlen(locks[i].path)) == 0) {
lock_entry_t *result = &locks[i]; pthread_mutex_unlock(&lock_mutex); return result;
}
}
i++;
}
pthread_mutex_unlock(&lock_mutex); return NULL;
}
static int add_lock(const char *path, lock_type_t type, const char *owner, int depth, int timeout, const char *token) {
REQUIRE_RET(path != NULL, "path cannot be NULL", -1);
pthread_mutex_lock(&lock_mutex);
if (lock_count >= 100) { pthread_mutex_unlock(&lock_mutex); return -1; }
lock_entry_t *entry = &locks[lock_count];
if (token && *token) {
if (strlen(token) >= sizeof(entry->token)) { pthread_mutex_unlock(&lock_mutex); return -1; }
strcpy(entry->token, token);
} else {
generate_lock_token(entry->token, sizeof(entry->token));
}
entry->type = type;
if (owner && strlen(owner) < sizeof(entry->owner)) strcpy(entry->owner, owner);
else entry->owner[0] = '\0';
if (strlen(path) < sizeof(entry->path)) strcpy(entry->path, path);
else { pthread_mutex_unlock(&lock_mutex); return -1; }
entry->created = time(NULL);
entry->expires = entry->created + (timeout > 0 ? timeout : LOCK_TIMEOUT_DEFAULT);
entry->depth = depth; lock_count++;
pthread_mutex_unlock(&lock_mutex); log_debug("Lock added: %s on %s", entry->token, path); return 0;
}
static int remove_lock(const char *token) {
REQUIRE_RET(token != NULL, "token cannot be NULL", -1);
pthread_mutex_lock(&lock_mutex);
for (int i = 0; i < lock_count; i++) {
if (strcmp(locks[i].token, token) == 0) {
memmove(&locks[i], &locks[i + 1], sizeof(lock_entry_t) * (lock_count - i - 1));
lock_count--; pthread_mutex_unlock(&lock_mutex);
log_debug("Lock removed: %s", token); return 0;
}
}
pthread_mutex_unlock(&lock_mutex); return -1;
}
/* ==================== RATE LIMITING ==================== */
static int check_rate_limit(const char *ip) {
REQUIRE_RET(ip != NULL, "ip cannot be NULL", 0);
pthread_mutex_lock(&rate_mutex); time_t now = time(NULL); rate_limit_entry_t *entry = NULL;
for (int i = 0; i < rate_count; i++) {
if (strcmp(rate_table[i].ip, ip) == 0) { entry = &rate_table[i]; break; }
}
if (!entry) {
if (rate_count >= 1000) { pthread_mutex_unlock(&rate_mutex); return 1; }
entry = &rate_table[rate_count++];
if (strlen(ip) < sizeof(entry->ip)) strcpy(entry->ip, ip);
else { pthread_mutex_unlock(&rate_mutex); return 1; }
entry->window_start = now; entry->request_count = 0;
pthread_mutex_init(&entry->mutex, NULL);
}
pthread_mutex_lock(&entry->mutex);
if (now - entry->window_start > RATE_LIMIT_WINDOW) {
entry->window_start = now; entry->request_count = 0;
}
entry->request_count++; int exceeded = (entry->request_count > MAX_REQ);
pthread_mutex_unlock(&entry->mutex); pthread_mutex_unlock(&rate_mutex);
if (exceeded) log_warn("Rate limit exceeded for %s: %d requests", ip, entry->request_count);
return exceeded;
}
/* ==================== HEADERS ==================== */
static void headers_init(headers_t *h) { REQUIRE(h != NULL, "headers cannot be NULL"); h->count = 0; }
static void headers_set(headers_t *h, const char *k, const char *v) {
REQUIRE(h != NULL, "headers cannot be NULL"); REQUIRE(k != NULL, "key cannot be NULL"); REQUIRE(v != NULL, "value cannot be NULL");
if (h->count >= 128) return;
strncpy(h->keys[h->count], k, SMALL_BUF - 1); h->keys[h->count][SMALL_BUF - 1] = '\0';
strncpy(h->vals[h->count], v, SMALL_BUF - 1); h->vals[h->count][SMALL_BUF - 1] = '\0';
trim_inplace(h->vals[h->count]); h->count++;
}
static const char *headers_get(headers_t *h, const char *k) {
REQUIRE_RET(h != NULL, "headers cannot be NULL", NULL); REQUIRE_RET(k != NULL, "key cannot be NULL", NULL);
for (int i = 0; i < h->count; i++) {
if (strcasecmp(h->keys[i], k) == 0) return h->vals[i];
}
return NULL;
}
/* ==================== BUFFER ==================== */
static int buffer_init(buffer_t *buf, size_t initial_capacity) {
REQUIRE_RET(buf != NULL, "buffer cannot be NULL", -1);
buf->data = (char *)malloc(initial_capacity); if (!buf->data) return -1;
buf->length = 0; buf->capacity = initial_capacity; return 0;
}
static void buffer_free(buffer_t *buf) {
if (!buf) return;
free(buf->data); buf->data = NULL; buf->length = 0; buf->capacity = 0;
}
static int buffer_append(buffer_t *buf, const void *data, size_t len) {
REQUIRE_RET(buf != NULL, "buffer cannot be NULL", -1); REQUIRE_RET(data != NULL || len == 0, "data cannot be NULL", -1);
if (buf->length + len > buf->capacity) {
size_t new_capacity = buf->capacity * 2;
while (new_capacity < buf->length + len) new_capacity *= 2;
char *new_data = (char *)realloc(buf->data, new_capacity); if (!new_data) return -1;
buf->data = new_data; buf->capacity = new_capacity;
}
if (len > 0) { memcpy(buf->data + buf->length, data, len); buf->length += len; }
return 0;
}
static int buffer_append_str(buffer_t *buf, const char *str) {
REQUIRE_RET(buf != NULL, "buffer cannot be NULL", -1); REQUIRE_RET(str != NULL, "str cannot be NULL", -1);
return buffer_append(buf, str, strlen(str));
}
/* ==================== IO UTILS ==================== */
static ssize_t send_all(int fd, const void *buf, size_t len, int timeout_sec) {
REQUIRE_RET(fd >= 0, "invalid fd", -1); REQUIRE_RET(buf != NULL || len == 0, "buf cannot be NULL", -1);
if (timeout_sec > 0) {
#ifdef _WIN32
int timeout_ms = timeout_sec * 1000;
setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, (int)sizeof(timeout_ms));
#else
struct timeval tv; tv.tv_sec = timeout_sec; tv.tv_usec = 0;
setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}
size_t total = 0; const char *p = (const char *)buf;
while (total < len) {
ssize_t sent = send(fd, p + total, len - total, MSG_NOSIGNAL);
if (sent <= 0) { if (errno == EINTR) continue; log_debug("Send failed: %s", strerror(errno)); return -1; }
total += (size_t)sent;
}
return (ssize_t)total;
}
static ssize_t recv_all(int fd, void *buf, size_t len, int timeout_sec) {
REQUIRE_RET(fd >= 0, "invalid fd", -1); REQUIRE_RET(buf != NULL || len == 0, "buf cannot be NULL", -1);
if (timeout_sec > 0) {
#ifdef _WIN32
int timeout_ms = timeout_sec * 1000;
setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, (int)sizeof(timeout_ms));
#else
struct timeval tv; tv.tv_sec = timeout_sec; tv.tv_usec = 0;
setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}
size_t total = 0; char *p = (char *)buf;
while (total < len) {
ssize_t received = recv(fd, p + total, len - total, 0);
if (received <= 0) { if (received == 0) return 0; if (errno == EINTR) continue; if (errno == EAGAIN || errno == EWOULDBLOCK) continue; log_debug("Recv failed: %s", strerror(errno)); return -1; }
total += (size_t)received;
}
return (ssize_t)total;
}
static ssize_t read_line(int fd, char *buf, size_t maxlen, int timeout_sec) {
REQUIRE_RET(fd >= 0, "invalid fd", -1); REQUIRE_RET(buf != NULL, "buf cannot be NULL", -1); REQUIRE_RET(maxlen > 0, "maxlen must be > 0", -1);
if (timeout_sec > 0) {
#ifdef _WIN32
int timeout_ms = timeout_sec * 1000;
setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, (int)sizeof(timeout_ms));
#else
struct timeval tv; tv.tv_sec = timeout_sec; tv.tv_usec = 0;
setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}
size_t i = 0; char c = 0;
while (i + 1 < maxlen) {
ssize_t n = recv(fd, &c, 1, 0);
if (n <= 0) { if (n == 0) return 0; if (errno == EINTR) continue; if (errno == EAGAIN || errno == EWOULDBLOCK) continue; log_debug("Recv line failed: %s", strerror(errno)); return -1; }
buf[i++] = c;
if (i >= 2 && buf[i - 2] == '\r' && buf[i - 1] == '\n') { buf[i] = '\0'; return (ssize_t)i; }
}
buf[maxlen - 1] = '\0'; return -1;
}
/* ==================== MIME TYPES ==================== */
static const char *guess_mime(const char *path) {
REQUIRE_RET(path != NULL, "path cannot be NULL", "application/octet-stream");
const char *ext = strrchr(path, '.'); if (!ext) return "application/octet-stream"; ext++;
struct mime_map { const char *ext; const char *mime; } mimes[] = {
{"html", "text/html; charset=utf-8"}, {"htm", "text/html; charset=utf-8"},
{"txt", "text/plain; charset=utf-8"}, {"css", "text/css; charset=utf-8"},
{"js", "application/javascript; charset=utf-8"}, {"json", "application/json; charset=utf-8"},
{"xml", "application/xml; charset=utf-8"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
{"png", "image/png"}, {"gif", "image/gif"}, {"bmp", "image/bmp"}, {"svg", "image/svg+xml"},
{"pdf", "application/pdf"}, {"zip", "application/zip"}, {"tar", "application/x-tar"},
{"gz", "application/gzip"}, {"mp3", "audio/mpeg"}, {"wav", "audio/wav"},
{"mp4", "video/mp4"}, {"webm", "video/webm"}, {NULL, NULL}
};
for (int i = 0; mimes[i].ext; i++) {
if (strcasecmp(ext, mimes[i].ext) == 0) return mimes[i].mime;
}
return "application/octet-stream";
}
/* ==================== FILE OPERATIONS ==================== */
static int create_parent_dirs(const char *path) {
REQUIRE_RET(path != NULL, "path cannot be NULL", -1);
char parent[MAX_PATH + 1]; strncpy(parent, path, sizeof(parent) - 1); parent[sizeof(parent) - 1] = '\0';
char *p = parent + strlen(parent); while (p > parent && *p != '/') p--;
if (p <= parent) return 0;
*p = '\0';
struct stat st; if (stat(parent, &st) == 0) { if (S_ISDIR(st.st_mode)) return 0; log_error("Parent not dir: %s", parent); return -1; }
if (create_parent_dirs(parent) != 0) return -1;
if (mkdir(parent, 0755) != 0 && errno != EEXIST) { log_error("mkdir failed: %s (%s)", parent, strerror(errno)); return -1; }
return 0;
}
static int recursive_delete(const char *path) {
REQUIRE_RET(path != NULL, "path cannot be NULL", -1);
struct stat st; if (lstat(path, &st) != 0) { if (errno == ENOENT) return 0; log_error("lstat failed: %s (%s)", path, strerror(errno)); return -1; }
if (!S_ISDIR(st.st_mode)) return unlink(path);
DIR *d = opendir(path); if (!d) { log_error("opendir failed: %s (%s)", path, strerror(errno)); return -1; }
struct dirent *e; while ((e = readdir(d))) {
if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
char sub[MAX_PATH + 1]; if (snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name) >= (int)sizeof(sub)) { closedir(d); return -1; }
if (recursive_delete(sub) != 0) { closedir(d); return -1; }
}
closedir(d); return rmdir(path);
}
static int recursive_copy(const char *src, const char *dest) {
REQUIRE_RET(src != NULL, "src cannot be NULL", -1); REQUIRE_RET(dest != NULL, "dest cannot be NULL", -1);
struct stat st; if (lstat(src, &st) != 0) return -1;
if (S_ISDIR(st.st_mode)) {
if (mkdir(dest, st.st_mode) != 0 && errno != EEXIST) return -1;
DIR *d = opendir(src); if (!d) return -1;
struct dirent *e; while ((e = readdir(d))) {
if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
char ssub[MAX_PATH + 1], dsub[MAX_PATH + 1];
if (snprintf(ssub, sizeof(ssub), "%s/%s", src, e->d_name) >= (int)sizeof(ssub) ||
snprintf(dsub, sizeof(dsub), "%s/%s", dest, e->d_name) >= (int)sizeof(dsub)) { closedir(d); return -1; }
if (recursive_copy(ssub, dsub) != 0) { closedir(d); return -1; }
}
closedir(d); return 0;
} else {
int fdsrc = open(src, O_RDONLY); if (fdsrc < 0) return -1;
int fddest = open(dest, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode); if (fddest < 0) { close(fdsrc); return -1; }
char buf[8192]; ssize_t r; while ((r = read(fdsrc, buf, sizeof(buf))) > 0) {
if (write(fddest, buf, (size_t)r) != r) { close(fdsrc); close(fddest); return -1; }
}
close(fdsrc); close(fddest); return 0;
}
}
/* ==================== HTTP RESPONSE ==================== */
static const char *status_reason(http_status_t code) {
switch (code) {
case HTTP_200_OK: return "OK"; case HTTP_201_CREATED: return "Created";
case HTTP_204_NO_CONTENT: return "No Content"; case HTTP_206_PARTIAL_CONTENT: return "Partial Content";
case HTTP_207_MULTI_STATUS: return "Multi-Status"; case HTTP_303_SEE_OTHER: return "See Other";
case HTTP_304_NOT_MODIFIED: return "Not Modified"; case HTTP_400_BAD_REQUEST: return "Bad Request";
case HTTP_401_UNAUTHORIZED: return "Unauthorized"; case HTTP_403_FORBIDDEN: return "Forbidden";
case HTTP_404_NOT_FOUND: return "Not Found"; case HTTP_405_METHOD_NOT_ALLOWED: return "Method Not Allowed";
case HTTP_409_CONFLICT: return "Conflict"; case HTTP_412_PRECONDITION_FAILED: return "Precondition Failed";
case HTTP_413_PAYLOAD_TOO_LARGE: return "Payload Too Large"; case HTTP_416_RANGE_NOT_SATISFIABLE: return "Range Not Satisfiable";
case HTTP_423_LOCKED: return "Locked"; case HTTP_429_TOO_MANY_REQUESTS: return "Too Many Requests";
case HTTP_500_INTERNAL_SERVER_ERROR: return "Internal Server Error"; case HTTP_501_NOT_IMPLEMENTED: return "Not Implemented";
case HTTP_503_SERVICE_UNAVAILABLE: return "Service Unavailable"; case HTTP_507_INSUFFICIENT_STORAGE: return "Insufficient Storage";
default: return "Unknown";
}
}
static bool append_header(char *buf, size_t buf_size, int *len, const char *fmt, ...) {
va_list ap; va_start(ap, fmt);
int written = vsnprintf(buf + *len, buf_size - (size_t)(*len), fmt, ap);
va_end(ap);
if (written < 0 || written >= (int)(buf_size - (size_t)(*len))) return false;
*len += written;
return true;
}
static void send_response(int fd, http_status_t code, headers_t *extra_headers, const char *body, size_t body_len) {
REQUIRE(fd >= 0, "invalid fd"); log_debug("Sending response: %d %s", code, status_reason(code));
char header_buf[8192]; int header_len = 0;
if (!append_header(header_buf, sizeof(header_buf), &header_len,
"HTTP/1.1 %d %s\r\nServer: mini-webdav/1.0\r\nDate: %s\r\n",
code, status_reason(code), http_time(time(NULL)))) return;
if (extra_headers) {
for (int i = 0; i < extra_headers->count; i++) {
if (!append_header(header_buf, sizeof(header_buf), &header_len, "%s: %s\r\n", extra_headers->keys[i], extra_headers->vals[i])) return;
}
}
if (!append_header(header_buf, sizeof(header_buf), &header_len,
"Content-Length: %zu\r\nConnection: close\r\n\r\n", body_len)) return;
send_all(fd, header_buf, (size_t)header_len, TIMEOUT);
if (body_len > 0 && body) send_all(fd, body, body_len, TIMEOUT);
}
static void send_error(int fd, http_status_t code, const char *message) {
REQUIRE(fd >= 0, "invalid fd");
headers_t headers; headers_init(&headers);
headers_set(&headers, "Content-Type", "text/plain; charset=utf-8");
send_response(fd, code, &headers, message, strlen(message));
}
/* ==================== HTML INTERFACE COM TAILWIND ==================== */
static void generate_directory_html(buffer_t *html, const char *reqpath, DIR *d, const char *fs_path) {
    REQUIRE(html != NULL, "html cannot be NULL");
    REQUIRE(reqpath != NULL, "reqpath cannot be NULL");
    
    // Favicon
    buffer_append_str(html, "<!DOCTYPE html>\n<html lang=\"pt-BR\">\n<head>\n");
    buffer_append_str(html, "    <meta charset=\"UTF-8\">\n");
    buffer_append_str(html, "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no\">\n");
    buffer_append_str(html, "    <meta name=\"theme-color\" content=\"#3b82f6\">\n");
    buffer_append_str(html, "    <title>WebDAV Server - ");
    buffer_append_str(html, reqpath);
    buffer_append_str(html, "</title>\n");
    buffer_append_str(html, "    <link rel=\"icon\" href=\"data:;base64,iVBORw0KGgo=\">\n");
    buffer_append_str(html, "    <script src=\"https://cdn.jsdelivr.net/npm/@tailwindcss/browser@4\"></script>\n");
    buffer_append_str(html, "    <link rel=\"stylesheet\" href=\"https://cdnjs.cloudflare.com/ajax/libs/font-awesome/7.0.1/css/all.min.css\">\n");
    buffer_append_str(html, "    <style>\n");
    buffer_append_str(html, "        @media (prefers-color-scheme: dark) {\n");
    buffer_append_str(html, "            body { background-color: #0f172a; color: #f1f5f9; }\n");
    buffer_append_str(html, "            .modal-content { background-color: #1e293b; color: #f1f5f9; }\n");
    buffer_append_str(html, "        }\n");
    buffer_append_str(html, "        @media (prefers-color-scheme: light) {\n");
    buffer_append_str(html, "            .modal-content { background-color: #ffffff; color: #1e293b; }\n");
    buffer_append_str(html, "        }\n");
    buffer_append_str(html, "        .action-btn {\n");
    buffer_append_str(html, "            transition: all 0.2s;\n");
    buffer_append_str(html, "            padding: 8px 12px;\n");
    buffer_append_str(html, "            border-radius: 6px;\n");
    buffer_append_str(html, "            margin: 2px;\n");
    buffer_append_str(html, "        }\n");
    buffer_append_str(html, "        .action-btn:hover { transform: scale(1.05); }\n");
    buffer_append_str(html, "        .action-btn i { font-size: 16px; }\n");
    buffer_append_str(html, "        .modal {\n");
    buffer_append_str(html, "            display: none;\n");
    buffer_append_str(html, "            position: fixed;\n");
    buffer_append_str(html, "            inset: 0;\n");
    buffer_append_str(html, "            background: rgba(0,0,0,0.7);\n");
    buffer_append_str(html, "            z-index: 1000;\n");
    buffer_append_str(html, "            align-items: center;\n");
    buffer_append_str(html, "            justify-content: center;\n");
    buffer_append_str(html, "            padding: 16px;\n");
    buffer_append_str(html, "        }\n");
    buffer_append_str(html, "        .modal-content {\n");
    buffer_append_str(html, "            padding: 24px;\n");
    buffer_append_str(html, "            border-radius: 12px;\n");
    buffer_append_str(html, "            width: 100%;\n");
    buffer_append_str(html, "            max-width: 450px;\n");
    buffer_append_str(html, "            box-shadow: 0 20px 25px -5px rgba(0,0,0,0.1), 0 10px 10px -5px rgba(0,0,0,0.04);\n");
    buffer_append_str(html, "            animation: modalFadeIn 0.3s ease-out;\n");
    buffer_append_str(html, "        }\n");
    buffer_append_str(html, "        @keyframes modalFadeIn {\n");
    buffer_append_str(html, "            from { opacity: 0; transform: translateY(-20px); }\n");
    buffer_append_str(html, "            to { opacity: 1; transform: translateY(0); }\n");
    buffer_append_str(html, "        }\n");
    buffer_append_str(html, "        .toast {\n");
    buffer_append_str(html, "            position: fixed;\n");
    buffer_append_str(html, "            bottom: 20px;\n");
    buffer_append_str(html, "            right: 20px;\n");
    buffer_append_str(html, "            left: 20px;\n");
    buffer_append_str(html, "            padding: 16px 24px;\n");
    buffer_append_str(html, "            border-radius: 8px;\n");
    buffer_append_str(html, "            color: white;\n");
    buffer_append_str(html, "            z-index: 2000;\n");
    buffer_append_str(html, "            min-width: 280px;\n");
    buffer_append_str(html, "            max-width: 90%;\n");
    buffer_append_str(html, "            margin: 0 auto;\n");
    buffer_append_str(html, "            box-shadow: 0 4px 6px rgba(0,0,0,0.1);\n");
    buffer_append_str(html, "            animation: toastSlideIn 0.3s ease-out;\n");
    buffer_append_str(html, "        }\n");
    buffer_append_str(html, "        @keyframes toastSlideIn {\n");
    buffer_append_str(html, "            from { opacity: 0; transform: translateY(50px); }\n");
    buffer_append_str(html, "            to { opacity: 1; transform: translateY(0); }\n");
    buffer_append_str(html, "        }\n");
    buffer_append_str(html, "        .toast-success { background: #10b981; }\n");
    buffer_append_str(html, "        .toast-error { background: #ef4444; }\n");
    buffer_append_str(html, "        .file-row.selected, .file-card.selected {\n");
    buffer_append_str(html, "            background-color: #dbeafe !important;\n");
    buffer_append_str(html, "        }\n");
    buffer_append_str(html, "        @media (prefers-color-scheme: dark) {\n");
    buffer_append_str(html, "            .file-row.selected, .file-card.selected {\n");
    buffer_append_str(html, "                background-color: #1e3a8a !important;\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "        }\n");
    buffer_append_str(html, "        @media (max-width: 768px) {\n");
    buffer_append_str(html, "            .desktop-only { display: none !important; }\n");
    buffer_append_str(html, "            .mobile-only { display: block !important; }\n");
    buffer_append_str(html, "            .mobile-hidden { display: none !important; }\n");
    buffer_append_str(html, "            .action-container {\n");
    buffer_append_str(html, "                position: fixed;\n");
    buffer_append_str(html, "                bottom: 0;\n");
    buffer_append_str(html, "                left: 0;\n");
    buffer_append_str(html, "                right: 0;\n");
    buffer_append_str(html, "                background: linear-gradient(to top, rgba(255,255,255,0.95) 80%, transparent);\n");
    buffer_append_str(html, "                padding: 12px 16px;\n");
    buffer_append_str(html, "                border-top: 1px solid #e2e8f0;\n");
    buffer_append_str(html, "                z-index: 100;\n");
    buffer_append_str(html, "                box-shadow: 0 -2px 10px rgba(0,0,0,0.1);\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            @media (prefers-color-scheme: dark) {\n");
    buffer_append_str(html, "                .action-container {\n");
    buffer_append_str(html, "                    background: linear-gradient(to top, rgba(15,23,42,0.95) 80%, transparent);\n");
    buffer_append_str(html, "                    border-top: 1px solid #334155;\n");
    buffer_append_str(html, "                }\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            .mobile-actions {\n");
    buffer_append_str(html, "                display: flex;\n");
    buffer_append_str(html, "                gap: 8px;\n");
    buffer_append_str(html, "                justify-content: center;\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            .mobile-action-btn {\n");
    buffer_append_str(html, "                flex: 1;\n");
    buffer_append_str(html, "                padding: 14px 8px;\n");
    buffer_append_str(html, "                border-radius: 10px;\n");
    buffer_append_str(html, "                font-size: 12px;\n");
    buffer_append_str(html, "                text-align: center;\n");
    buffer_append_str(html, "                cursor: pointer;\n");
    buffer_append_str(html, "                transition: all 0.2s;\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            .mobile-action-btn:disabled {\n");
    buffer_append_str(html, "                opacity: 0.5 !important;\n");
    buffer_append_str(html, "                pointer-events: none !important;\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            .file-card {\n");
    buffer_append_str(html, "                display: block;\n");
    buffer_append_str(html, "                padding: 16px;\n");
    buffer_append_str(html, "                margin-bottom: 8px;\n");
    buffer_append_str(html, "                background: white;\n");
    buffer_append_str(html, "                border-radius: 12px;\n");
    buffer_append_str(html, "                box-shadow: 0 2px 4px rgba(0,0,0,0.05);\n");
    buffer_append_str(html, "                transition: all 0.2s;\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            @media (prefers-color-scheme: dark) {\n");
    buffer_append_str(html, "                .file-card { background: #1e293b; }\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            .file-card:hover {\n");
    buffer_append_str(html, "                transform: translateY(-2px);\n");
    buffer_append_str(html, "                box-shadow: 0 4px 8px rgba(0,0,0,0.1);\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            .file-info {\n");
    buffer_append_str(html, "                display: flex;\n");
    buffer_append_str(html, "                align-items: center;\n");
    buffer_append_str(html, "                gap: 12px;\n");
    buffer_append_str(html, "                margin-bottom: 12px;\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            .file-details {\n");
    buffer_append_str(html, "                flex: 1;\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            .file-actions {\n");
    buffer_append_str(html, "                display: flex;\n");
    buffer_append_str(html, "                gap: 6px;\n");
    buffer_append_str(html, "                margin-top: 12px;\n");
    buffer_append_str(html, "                padding-top: 12px;\n");
    buffer_append_str(html, "                border-top: 1px solid #e2e8f0;\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            @media (prefers-color-scheme: dark) {\n");
    buffer_append_str(html, "                .file-actions { border-top: 1px solid #334155; }\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            .file-actions .action-btn {\n");
    buffer_append_str(html, "                padding: 6px 10px;\n");
    buffer_append_str(html, "                font-size: 12px;\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            .breadcrumb-item {\n");
    buffer_append_str(html, "                font-size: 13px;\n");
    buffer_append_str(html, "                padding: 4px 8px;\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            .breadcrumb-separator {\n");
    buffer_append_str(html, "                margin: 0 4px;\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            table { display: none; }\n");
    buffer_append_str(html, "        }\n");
    buffer_append_str(html, "        @media (min-width: 769px) {\n");
    buffer_append_str(html, "            .mobile-only { display: none !important; }\n");
    buffer_append_str(html, "            .mobile-hidden { display: block !important; }\n");
    buffer_append_str(html, "            .action-container { display: none !important; }\n");
    buffer_append_str(html, "            .file-card { display: none !important; }\n");
    buffer_append_str(html, "        }\n");
    buffer_append_str(html, "        .checkbox-cell {\n");
    buffer_append_str(html, "            min-width: 50px;\n");
    buffer_append_str(html, "            width: 50px;\n");
    buffer_append_str(html, "        }\n");
    buffer_append_str(html, "        .name-cell { min-width: 250px; }\n");
    buffer_append_str(html, "        .size-cell, .date-cell, .type-cell { min-width: 120px; }\n");
    buffer_append_str(html, "        .actions-cell { min-width: 200px; }\n");
    buffer_append_str(html, "        @media (max-width: 1024px) {\n");
    buffer_append_str(html, "            .name-cell { min-width: 200px; }\n");
    buffer_append_str(html, "            .size-cell, .date-cell, .type-cell { min-width: 100px; font-size: 13px; }\n");
    buffer_append_str(html, "            .actions-cell { min-width: 180px; }\n");
    buffer_append_str(html, "            .action-btn { padding: 4px 6px; }\n");
    buffer_append_str(html, "            .action-btn i { font-size: 14px; }\n");
    buffer_append_str(html, "        }\n");
    buffer_append_str(html, "        input[type=\"text\"], input[type=\"file\"] {\n");
    buffer_append_str(html, "            width: 100%;\n");
    buffer_append_str(html, "            padding: 12px;\n");
    buffer_append_str(html, "            font-size: 16px;\n");
    buffer_append_str(html, "            border-radius: 8px;\n");
    buffer_append_str(html, "            border: 1px solid #cbd5e1;\n");
    buffer_append_str(html, "            margin-bottom: 16px;\n");
    buffer_append_str(html, "        }\n");
    buffer_append_str(html, "        @media (prefers-color-scheme: dark) {\n");
    buffer_append_str(html, "            input[type=\"text\"], input[type=\"file\"] {\n");
    buffer_append_str(html, "                background-color: #1e293b;\n");
    buffer_append_str(html, "                border-color: #334155;\n");
    buffer_append_str(html, "                color: #f1f5f9;\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "        }\n");
    buffer_append_str(html, "        button, .action-btn {\n");
    buffer_append_str(html, "            font-size: 16px;\n");
    buffer_append_str(html, "            font-weight: 600;\n");
    buffer_append_str(html, "            cursor: pointer;\n");
    buffer_append_str(html, "            touch-action: manipulation;\n");
    buffer_append_str(html, "            -webkit-tap-highlight-color: transparent;\n");
    buffer_append_str(html, "        }\n");
    buffer_append_str(html, "    </style>\n");
    buffer_append_str(html, "</head>\n<body class=\"bg-gray-50 dark:bg-slate-900 dark:text-slate-100 transition-colors duration-200\">\n");
    
    // Header
    buffer_append_str(html, "    <header class=\"bg-gradient-to-r from-blue-600 to-indigo-700 text-white shadow-lg\">\n");
    buffer_append_str(html, "        <div class=\"container mx-auto px-4 py-4\">\n");
    buffer_append_str(html, "            <div class=\"flex items-center justify-between\">\n");
    buffer_append_str(html, "                <div class=\"flex items-center space-x-3\">\n");
    buffer_append_str(html, "                    <svg class=\"w-8 h-8\" fill=\"none\" stroke=\"currentColor\" viewBox=\"0 0 24 24\">\n");
    buffer_append_str(html, "                        <path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M3 7v10a2 2 0 002 2h14a2 2 0 002-2V9a2 2 0 00-2-2h-6l-2-2H5a2 2 0 00-2 2z\"></path>\n");
    buffer_append_str(html, "                    </svg>\n");
    buffer_append_str(html, "                    <h1 class=\"text-2xl font-bold hidden sm:block\">WebDAV Server</h1>\n");
    buffer_append_str(html, "                </div>\n");
    buffer_append_str(html, "                <div class=\"flex items-center space-x-2\">\n");
    buffer_append_str(html, "                    <button onclick=\"showCreateFolderModal()\" class=\"flex items-center bg-white text-blue-600 hover:bg-blue-50 px-3 py-2 rounded-lg shadow transition-colors text-sm\">\n");
    buffer_append_str(html, "                        <i class=\"fas fa-folder-plus mr-1 sm:mr-2\"></i>\n");
    buffer_append_str(html, "                        <span class=\"hidden xs:inline\">Nova Pasta</span>\n");
    buffer_append_str(html, "                    </button>\n");
    buffer_append_str(html, "                    <button onclick=\"document.getElementById('fileInput').click()\" class=\"flex items-center bg-green-500 hover:bg-green-600 text-white px-3 py-2 rounded-lg shadow transition-colors text-sm\">\n");
    buffer_append_str(html, "                        <i class=\"fas fa-upload mr-1 sm:mr-2\"></i>\n");
    buffer_append_str(html, "                        <span class=\"hidden xs:inline\">Upload</span>\n");
    buffer_append_str(html, "                    </button>\n");
    buffer_append_str(html, "                </div>\n");
    buffer_append_str(html, "            </div>\n");
    buffer_append_str(html, "            <div class=\"mt-3\">\n");
    buffer_append_str(html, "                <p class=\"text-sm opacity-90\">Diretório: <span class=\"font-mono bg-black bg-opacity-20 px-2 py-1 rounded text-xs sm:text-sm\">");
    buffer_append_str(html, reqpath);
    buffer_append_str(html, "</span></p>\n");
    buffer_append_str(html, "            </div>\n");
    buffer_append_str(html, "        </div>\n");
    buffer_append_str(html, "    </header>\n");
    
    // Hidden file input
    buffer_append_str(html, "    <input type=\"file\" id=\"fileInput\" multiple style=\"display:none\" onchange=\"handleFileUpload(event)\">\n");
    
    // Main Content
    buffer_append_str(html, "    <main class=\"container mx-auto px-3 sm:px-4 py-6\">\n");
    
    // Breadcrumb Navigation
    buffer_append_str(html, "        <nav class=\"mb-4\" aria-label=\"Breadcrumb\">\n");
    buffer_append_str(html, "            <ol class=\"flex flex-wrap items-center gap-1 text-xs sm:text-sm\">\n");
    buffer_append_str(html, "                <li class=\"breadcrumb-item\">\n");
    buffer_append_str(html, "                    <a href=\"/\" class=\"text-blue-600 hover:text-blue-800 dark:text-blue-400 dark:hover:text-blue-300 transition-colors flex items-center\">\n");
    buffer_append_str(html, "                        <svg class=\"w-3 h-3 sm:w-4 sm:h-4 inline-block mr-1\" fill=\"none\" stroke=\"currentColor\" viewBox=\"0 0 24 24\">\n");
    buffer_append_str(html, "                            <path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M3 12l2-2m0 0l7-7 7 7M5 10v10a1 1 0 001 1h3m10-11l2 2m-2-2v10a1 1 0 01-1 1h-3m-6 0a1 1 0 001-1v-4a1 1 0 011-1h2a1 1 0 011 1v4a1 1 0 001 1m-6 0h6\"></path>\n");
    buffer_append_str(html, "                        </svg>\n");
    buffer_append_str(html, "                        <span class=\"hidden xs:inline\">Início</span>\n");
    buffer_append_str(html, "                    </a>\n");
    buffer_append_str(html, "                </li>\n");
    
    // Gerar breadcrumbs dinamicamente
    if (strcmp(reqpath, "/") != 0) {
        char temp_path[MAX_PATH];
        strncpy(temp_path, reqpath, sizeof(temp_path) - 1);
        temp_path[sizeof(temp_path) - 1] = '\0';
        char *token = strtok(temp_path, "/");
        char current_path[MAX_PATH] = "/";
        
        while (token != NULL) {
            buffer_append_str(html, "                <li class=\"breadcrumb-separator\">\n");
            buffer_append_str(html, "                    <svg class=\"w-3 h-3 sm:w-4 sm:h-4 text-gray-400 dark:text-gray-500\" fill=\"currentColor\" viewBox=\"0 0 20 20\">\n");
            buffer_append_str(html, "                        <path fill-rule=\"evenodd\" d=\"M7.293 14.707a1 1 0 010-1.414L10.586 10 7.293 6.707a1 1 0 011.414-1.414l4 4a1 1 0 010 1.414l-4 4a1 1 0 01-1.414 0z\" clip-rule=\"evenodd\"></path>\n");
            buffer_append_str(html, "                    </svg>\n");
            buffer_append_str(html, "                </li>\n");
            buffer_append_str(html, "                <li class=\"breadcrumb-item\">\n");
            
            if (strlen(current_path) > 1 && current_path[strlen(current_path) - 1] != '/')
                strcat(current_path, "/");
            strcat(current_path, token);
            
            buffer_append_str(html, "                    <a href=\"");
            buffer_append_str(html, current_path);
            buffer_append_str(html, "\" class=\"text-blue-600 hover:text-blue-800 dark:text-blue-400 dark:hover:text-blue-300 transition-colors\">");
            buffer_append_str(html, token);
            buffer_append_str(html, "</a>\n");
            buffer_append_str(html, "                </li>\n");
            
            token = strtok(NULL, "/");
        }
    }
    
    buffer_append_str(html, "            </ol>\n");
    buffer_append_str(html, "        </nav>\n");
    
    // File List com checkboxes
    buffer_append_str(html, "        <div class=\"bg-white dark:bg-slate-800 rounded-xl shadow-lg overflow-hidden mb-4\">\n");
    buffer_append_str(html, "            <div class=\"p-3 sm:p-4 bg-gray-50 dark:bg-slate-700 border-b border-gray-200 dark:border-slate-600 flex flex-col sm:flex-row justify-between items-start sm:items-center gap-3\">\n");
    buffer_append_str(html, "                <div>\n");
    buffer_append_str(html, "                    <button onclick=\"deleteSelected()\" id=\"deleteBtn\" disabled class=\"bg-red-500 hover:bg-red-600 text-white px-3 sm:px-4 py-2 rounded-lg shadow transition-colors disabled:opacity-50 disabled:cursor-not-allowed text-sm\">\n");
    buffer_append_str(html, "                        <i class=\"fas fa-trash mr-1 sm:mr-2\"></i>\n");
    buffer_append_str(html, "                        <span class=\"hidden xs:inline\">Excluir Selecionados</span>\n");
    buffer_append_str(html, "                    </button>\n");
    buffer_append_str(html, "                </div>\n");
    buffer_append_str(html, "                <div class=\"flex items-center\">\n");
    buffer_append_str(html, "                    <span class=\"text-xs sm:text-sm text-gray-600 dark:text-slate-300\" id=\"itemCount\">0 itens selecionados</span>\n");
    buffer_append_str(html, "                </div>\n");
    buffer_append_str(html, "            </div>\n");
    
    // Versão Desktop - Tabela
    buffer_append_str(html, "            <div class=\"overflow-x-auto hidden md:block\">\n");
    buffer_append_str(html, "                <table class=\"min-w-full divide-y divide-gray-200 dark:divide-slate-700\">\n");
    buffer_append_str(html, "                    <thead class=\"bg-gray-50 dark:bg-slate-700\">\n");
    buffer_append_str(html, "                        <tr>\n");
    buffer_append_str(html, "                            <th class=\"px-3 sm:px-4 py-3 text-left text-xs font-medium text-gray-500 dark:text-slate-300 uppercase tracking-wider checkbox-cell\">\n");
    buffer_append_str(html, "                                <input type=\"checkbox\" id=\"selectAll\" onclick=\"toggleSelectAll()\" class=\"form-checkbox h-4 w-4 text-blue-600\">\n");
    buffer_append_str(html, "                            </th>\n");
    buffer_append_str(html, "                            <th class=\"px-4 sm:px-6 py-3 text-left text-xs font-medium text-gray-500 dark:text-slate-300 uppercase tracking-wider name-cell\">Nome</th>\n");
    buffer_append_str(html, "                            <th class=\"px-4 sm:px-6 py-3 text-left text-xs font-medium text-gray-500 dark:text-slate-300 uppercase tracking-wider size-cell\">Tamanho</th>\n");
    buffer_append_str(html, "                            <th class=\"px-4 sm:px-6 py-3 text-left text-xs font-medium text-gray-500 dark:text-slate-300 uppercase tracking-wider date-cell\">Última Modificação</th>\n");
    buffer_append_str(html, "                            <th class=\"px-4 sm:px-6 py-3 text-left text-xs font-medium text-gray-500 dark:text-slate-300 uppercase tracking-wider type-cell\">Tipo</th>\n");
    buffer_append_str(html, "                            <th class=\"px-4 sm:px-6 py-3 text-left text-xs font-medium text-gray-500 dark:text-slate-300 uppercase tracking-wider actions-cell\">Ações</th>\n");
    buffer_append_str(html, "                        </tr>\n");
    buffer_append_str(html, "                    </thead>\n");
    buffer_append_str(html, "                    <tbody class=\"bg-white dark:bg-slate-800 divide-y divide-gray-200 dark:divide-slate-700\">\n");
    
    // Parent directory link
    if (strcmp(reqpath, "/") != 0) {
        buffer_append_str(html, "                        <tr class=\"hover:bg-gray-50 dark:hover:bg-slate-700 transition-colors\">\n");
        buffer_append_str(html, "                            <td class=\"px-3 sm:px-4 py-3 sm:py-4 whitespace-nowrap checkbox-cell\">\n");
        buffer_append_str(html, "                                <input type=\"checkbox\" class=\"form-checkbox h-4 w-4 text-blue-600 file-checkbox\" disabled>\n");
        buffer_append_str(html, "                            </td>\n");
        buffer_append_str(html, "                            <td class=\"px-4 sm:px-6 py-3 sm:py-4 whitespace-nowrap\" colspan=\"5\">\n");
        buffer_append_str(html, "                                <a href=\"");
        
        char parent_path[MAX_PATH];
        strncpy(parent_path, reqpath, sizeof(parent_path) - 1);
        parent_path[sizeof(parent_path) - 1] = '\0';
        char *last_slash = strrchr(parent_path, '/');
        if (last_slash && last_slash != parent_path)
            *last_slash = '\0';
        else if (last_slash == parent_path)
            parent_path[1] = '\0';
        
        buffer_append_str(html, parent_path);
        buffer_append_str(html, "\" class=\"flex items-center text-blue-600 hover:text-blue-800 dark:text-blue-400 dark:hover:text-blue-300\">\n");
        buffer_append_str(html, "                                    <svg class=\"w-5 h-5 mr-2 text-gray-400\" fill=\"none\" stroke=\"currentColor\" viewBox=\"0 0 24 24\">\n");
        buffer_append_str(html, "                                        <path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M15 19l-7-7 7-7\"></path>\n");
        buffer_append_str(html, "                                    </svg>\n");
        buffer_append_str(html, "                                    .. (diretório pai)\n");
        buffer_append_str(html, "                                </a>\n");
        buffer_append_str(html, "                            </td>\n");
        buffer_append_str(html, "                        </tr>\n");
    }
    
    // Listar arquivos e diretórios - Desktop
    rewinddir(d);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        
        char full_path[MAX_PATH + 1];
        if (snprintf(full_path, sizeof(full_path), "%s/%s", fs_path, e->d_name) >= (int)sizeof(full_path))
            continue;
        
        struct stat est;
        if (stat(full_path, &est) != 0)
            continue;
        
        char encoded[MAX_ENCODED_PATH + 1];
        url_encode(encoded, sizeof(encoded), e->d_name);
        
        char href[MAX_ENCODED_PATH * 2 + 2];
        if (strcmp(reqpath, "/") == 0)
            snprintf(href, sizeof(href), "/%s", encoded);
        else
            snprintf(href, sizeof(href), "%s/%s", reqpath, encoded);
        
        char size_str[32];
        if (S_ISDIR(est.st_mode))
            strcpy(size_str, "-");
        else {
            off_t size = est.st_size;
            if (size < 1024)
                snprintf(size_str, sizeof(size_str), "%ld B", (long)size);
            else if (size < 1024 * 1024)
                snprintf(size_str, sizeof(size_str), "%.2f KB", size / 1024.0);
            else if (size < 1024 * 1024 * 1024)
                snprintf(size_str, sizeof(size_str), "%.2f MB", size / (1024.0 * 1024.0));
            else
                snprintf(size_str, sizeof(size_str), "%.2f GB", size / (1024.0 * 1024.0 * 1024.0));
        }
        
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%d/%m/%Y %H:%M", localtime(&est.st_mtime));
        
        // Versão Desktop - Linha da tabela
        buffer_append_str(html, "                        <tr class=\"hover:bg-gray-50 dark:hover:bg-slate-700 transition-colors file-row\" data-name=\"");
        buffer_append_str(html, encoded);
        buffer_append_str(html, "\" data-path=\"");
        buffer_append_str(html, href);
        buffer_append_str(html, "\" data-is-dir=\"");
        buffer_append_str(html, S_ISDIR(est.st_mode) ? "true" : "false");
        buffer_append_str(html, "\">\n");
        buffer_append_str(html, "                            <td class=\"px-3 sm:px-4 py-3 sm:py-4 whitespace-nowrap checkbox-cell\">\n");
        buffer_append_str(html, "                                <input type=\"checkbox\" class=\"form-checkbox h-4 w-4 text-blue-600 file-checkbox\">\n");
        buffer_append_str(html, "                            </td>\n");
        buffer_append_str(html, "                            <td class=\"px-4 sm:px-6 py-3 sm:py-4 whitespace-nowrap name-cell\">\n");
        buffer_append_str(html, "                                <a href=\"");
        buffer_append_str(html, href);
        buffer_append_str(html, "\" class=\"flex items-center text-blue-600 hover:text-blue-800 dark:text-blue-400 dark:hover:text-blue-300\">\n");
        
        // Ícone baseado no tipo
        if (S_ISDIR(est.st_mode)) {
            buffer_append_str(html, "                                    <svg class=\"w-5 h-5 mr-2 text-yellow-500\" fill=\"none\" stroke=\"currentColor\" viewBox=\"0 0 24 24\">\n");
            buffer_append_str(html, "                                        <path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M3 7v10a2 2 0 002 2h14a2 2 0 002-2V9a2 2 0 00-2-2h-6l-2-2H5a2 2 0 00-2 2z\"></path>\n");
            buffer_append_str(html, "                                    </svg>\n");
        } else {
            const char *ext = strrchr(e->d_name, '.');
            if (ext && (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".gif") == 0 || strcasecmp(ext, ".jpeg") == 0)) {
                buffer_append_str(html, "                                    <svg class=\"w-5 h-5 mr-2 text-green-500\" fill=\"none\" stroke=\"currentColor\" viewBox=\"0 0 24 24\">\n");
                buffer_append_str(html, "                                        <path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M4 16l4.586-4.586a2 2 0 012.828 0L16 16m-2-2l1.586-1.586a2 2 0 012.828 0L20 14m-6-6h.01M6 20h12a2 2 0 002-2V6a2 2 0 00-2-2H6a2 2 0 00-2 2v12a2 2 0 002 2z\"></path>\n");
                buffer_append_str(html, "                                    </svg>\n");
            } else if (ext && strcasecmp(ext, ".pdf") == 0) {
                buffer_append_str(html, "                                    <svg class=\"w-5 h-5 mr-2 text-red-500\" fill=\"none\" stroke=\"currentColor\" viewBox=\"0 0 24 24\">\n");
                buffer_append_str(html, "                                        <path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z\"></path>\n");
                buffer_append_str(html, "                                    </svg>\n");
            } else if (ext && (strcasecmp(ext, ".txt") == 0 || strcasecmp(ext, ".md") == 0)) {
                buffer_append_str(html, "                                    <svg class=\"w-5 h-5 mr-2 text-blue-500\" fill=\"none\" stroke=\"currentColor\" viewBox=\"0 0 24 24\">\n");
                buffer_append_str(html, "                                        <path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z\"></path>\n");
                buffer_append_str(html, "                                    </svg>\n");
            } else if (ext && (strcasecmp(ext, ".zip") == 0 || strcasecmp(ext, ".tar") == 0 || strcasecmp(ext, ".gz") == 0)) {
                buffer_append_str(html, "                                    <svg class=\"w-5 h-5 mr-2 text-purple-500\" fill=\"none\" stroke=\"currentColor\" viewBox=\"0 0 24 24\">\n");
                buffer_append_str(html, "                                        <path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M7 8h10M7 12h4m1 8l-4-4H5a2 2 0 01-2-2V6a2 2 0 012-2h14a2 2 0 012 2v8a2 2 0 01-2 2h-3l-4 4z\"></path>\n");
                buffer_append_str(html, "                                    </svg>\n");
            } else {
                buffer_append_str(html, "                                    <svg class=\"w-5 h-5 mr-2 text-gray-500\" fill=\"none\" stroke=\"currentColor\" viewBox=\"0 0 24 24\">\n");
                buffer_append_str(html, "                                        <path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z\"></path>\n");
                buffer_append_str(html, "                                    </svg>\n");
            }
        }
        
        buffer_append_str(html, e->d_name);
        if (S_ISDIR(est.st_mode))
            buffer_append_str(html, "/");
        buffer_append_str(html, "                                </a>\n");
        buffer_append_str(html, "                            </td>\n");
        buffer_append_str(html, "                            <td class=\"px-4 sm:px-6 py-3 sm:py-4 whitespace-nowrap text-sm text-gray-900 dark:text-slate-100 size-cell\">");
        buffer_append_str(html, size_str);
        buffer_append_str(html, "</td>\n");
        buffer_append_str(html, "                            <td class=\"px-4 sm:px-6 py-3 sm:py-4 whitespace-nowrap text-xs sm:text-sm text-gray-500 dark:text-slate-400 date-cell\">");
        buffer_append_str(html, time_str);
        buffer_append_str(html, "</td>\n");
        buffer_append_str(html, "                            <td class=\"px-4 sm:px-6 py-3 sm:py-4 whitespace-nowrap type-cell\">\n");
        if (S_ISDIR(est.st_mode))
            buffer_append_str(html, "                                <span class=\"px-2 py-1 text-xs font-semibold rounded-full bg-yellow-100 text-yellow-800 dark:bg-yellow-900 dark:text-yellow-200\">Diretório</span>\n");
        else
            buffer_append_str(html, "                                <span class=\"px-2 py-1 text-xs font-semibold rounded-full bg-blue-100 text-blue-800 dark:bg-blue-900 dark:text-blue-200\">Arquivo</span>\n");
        buffer_append_str(html, "                            </td>\n");
        buffer_append_str(html, "                            <td class=\"px-4 sm:px-6 py-3 sm:py-4 whitespace-nowrap actions-cell\">\n");
        buffer_append_str(html, "                                <div class=\"flex flex-wrap gap-1\">\n");
        
        // Botões de ação
        if (!S_ISDIR(est.st_mode)) {
            buffer_append_str(html, "                                    <a href=\"");
            buffer_append_str(html, href);
            buffer_append_str(html, "\" download class=\"action-btn bg-green-500 text-white hover:bg-green-600\" title=\"Baixar\">\n");
            buffer_append_str(html, "                                        <i class=\"fas fa-download\"></i>\n");
            buffer_append_str(html, "                                    </a>\n");
        }
        
        buffer_append_str(html, "                                    <button onclick=\"renameItem('");
        buffer_append_str(html, encoded);
        buffer_append_str(html, "', ");
        buffer_append_str(html, S_ISDIR(est.st_mode) ? "true" : "false");
        buffer_append_str(html, ")\" class=\"action-btn bg-blue-500 text-white hover:bg-blue-600\" title=\"Renomear\">\n");
        buffer_append_str(html, "                                        <i class=\"fas fa-edit\"></i>\n");
        buffer_append_str(html, "                                    </button>\n");
        
        if (!S_ISDIR(est.st_mode)) {
            buffer_append_str(html, "                                    <button onclick=\"copyItem('");
            buffer_append_str(html, encoded);
            buffer_append_str(html, "')\" class=\"action-btn bg-purple-500 text-white hover:bg-purple-600\" title=\"Copiar\">\n");
            buffer_append_str(html, "                                        <i class=\"fas fa-copy\"></i>\n");
            buffer_append_str(html, "                                    </button>\n");
        }
        
        buffer_append_str(html, "                                    <button onclick=\"deleteItem('");
        buffer_append_str(html, encoded);
        buffer_append_str(html, "', ");
        buffer_append_str(html, S_ISDIR(est.st_mode) ? "true" : "false");
        buffer_append_str(html, ")\" class=\"action-btn bg-red-500 text-white hover:bg-red-600\" title=\"Excluir\">\n");
        buffer_append_str(html, "                                        <i class=\"fas fa-trash\"></i>\n");
        buffer_append_str(html, "                                    </button>\n");
        buffer_append_str(html, "                                </div>\n");
        buffer_append_str(html, "                            </td>\n");
        buffer_append_str(html, "                        </tr>\n");
    }
    
    buffer_append_str(html, "                    </tbody>\n");
    buffer_append_str(html, "                </table>\n");
    buffer_append_str(html, "            </div>\n");
    
    // Versão Mobile - Cards
    buffer_append_str(html, "            <div class=\"md:hidden\">\n");
    rewinddir(d);
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        
        char full_path[MAX_PATH + 1];
        if (snprintf(full_path, sizeof(full_path), "%s/%s", fs_path, e->d_name) >= (int)sizeof(full_path))
            continue;
        
        struct stat est;
        if (stat(full_path, &est) != 0)
            continue;
        
        char encoded[MAX_ENCODED_PATH + 1];
        url_encode(encoded, sizeof(encoded), e->d_name);
        
        char href[MAX_ENCODED_PATH * 2 + 2];
        if (strcmp(reqpath, "/") == 0)
            snprintf(href, sizeof(href), "/%s", encoded);
        else
            snprintf(href, sizeof(href), "%s/%s", reqpath, encoded);
        
        char size_str[32];
        if (S_ISDIR(est.st_mode))
            strcpy(size_str, "-");
        else {
            off_t size = est.st_size;
            if (size < 1024)
                snprintf(size_str, sizeof(size_str), "%ld B", (long)size);
            else if (size < 1024 * 1024)
                snprintf(size_str, sizeof(size_str), "%.2f KB", size / 1024.0);
            else if (size < 1024 * 1024 * 1024)
                snprintf(size_str, sizeof(size_str), "%.2f MB", size / (1024.0 * 1024.0));
            else
                snprintf(size_str, sizeof(size_str), "%.2f GB", size / (1024.0 * 1024.0 * 1024.0));
        }
        
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%d/%m/%Y %H:%M", localtime(&est.st_mtime));
        
        buffer_append_str(html, "                <div class=\"file-card\" data-name=\"");
        buffer_append_str(html, encoded);
        buffer_append_str(html, "\" data-path=\"");
        buffer_append_str(html, href);
        buffer_append_str(html, "\" data-is-dir=\"");
        buffer_append_str(html, S_ISDIR(est.st_mode) ? "true" : "false");
        buffer_append_str(html, "\">\n");
        buffer_append_str(html, "                    <div class=\"file-info\">\n");
        buffer_append_str(html, "                        <input type=\"checkbox\" class=\"form-checkbox h-5 w-5 text-blue-600 file-checkbox\">\n");
        buffer_append_str(html, "                        <div class=\"file-details flex-1\">\n");
        buffer_append_str(html, "                            <a href=\"");
        buffer_append_str(html, href);
        buffer_append_str(html, "\" class=\"flex items-center text-blue-600 hover:text-blue-800 dark:text-blue-400 dark:hover:text-blue-300\">\n");
        
        if (S_ISDIR(est.st_mode)) {
            buffer_append_str(html, "                                <svg class=\"w-6 h-6 mr-2 text-yellow-500 flex-shrink-0\" fill=\"none\" stroke=\"currentColor\" viewBox=\"0 0 24 24\">\n");
            buffer_append_str(html, "                                    <path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M3 7v10a2 2 0 002 2h14a2 2 0 002-2V9a2 2 0 00-2-2h-6l-2-2H5a2 2 0 00-2 2z\"></path>\n");
            buffer_append_str(html, "                                </svg>\n");
        } else {
            const char *ext = strrchr(e->d_name, '.');
            if (ext && (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".gif") == 0 || strcasecmp(ext, ".jpeg") == 0)) {
                buffer_append_str(html, "                                <svg class=\"w-6 h-6 mr-2 text-green-500 flex-shrink-0\" fill=\"none\" stroke=\"currentColor\" viewBox=\"0 0 24 24\">\n");
                buffer_append_str(html, "                                    <path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M4 16l4.586-4.586a2 2 0 012.828 0L16 16m-2-2l1.586-1.586a2 2 0 012.828 0L20 14m-6-6h.01M6 20h12a2 2 0 002-2V6a2 2 0 00-2-2H6a2 2 0 00-2 2v12a2 2 0 002 2z\"></path>\n");
                buffer_append_str(html, "                                </svg>\n");
            } else if (ext && strcasecmp(ext, ".pdf") == 0) {
                buffer_append_str(html, "                                <svg class=\"w-6 h-6 mr-2 text-red-500 flex-shrink-0\" fill=\"none\" stroke=\"currentColor\" viewBox=\"0 0 24 24\">\n");
                buffer_append_str(html, "                                    <path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z\"></path>\n");
                buffer_append_str(html, "                                </svg>\n");
            } else if (ext && (strcasecmp(ext, ".txt") == 0 || strcasecmp(ext, ".md") == 0)) {
                buffer_append_str(html, "                                <svg class=\"w-6 h-6 mr-2 text-blue-500 flex-shrink-0\" fill=\"none\" stroke=\"currentColor\" viewBox=\"0 0 24 24\">\n");
                buffer_append_str(html, "                                    <path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z\"></path>\n");
                buffer_append_str(html, "                                </svg>\n");
            } else if (ext && (strcasecmp(ext, ".zip") == 0 || strcasecmp(ext, ".tar") == 0 || strcasecmp(ext, ".gz") == 0)) {
                buffer_append_str(html, "                                <svg class=\"w-6 h-6 mr-2 text-purple-500 flex-shrink-0\" fill=\"none\" stroke=\"currentColor\" viewBox=\"0 0 24 24\">\n");
                buffer_append_str(html, "                                    <path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M7 8h10M7 12h4m1 8l-4-4H5a2 2 0 01-2-2V6a2 2 0 012-2h14a2 2 0 012 2v8a2 2 0 01-2 2h-3l-4 4z\"></path>\n");
                buffer_append_str(html, "                                </svg>\n");
            } else {
                buffer_append_str(html, "                                <svg class=\"w-6 h-6 mr-2 text-gray-500 flex-shrink-0\" fill=\"none\" stroke=\"currentColor\" viewBox=\"0 0 24 24\">\n");
                buffer_append_str(html, "                                    <path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z\"></path>\n");
                buffer_append_str(html, "                                </svg>\n");
            }
        }
        
        buffer_append_str(html, "                                <span class=\"font-medium\">");
        buffer_append_str(html, e->d_name);
        if (S_ISDIR(est.st_mode))
            buffer_append_str(html, "/");
        buffer_append_str(html, "</span>\n");
        buffer_append_str(html, "                            </a>\n");
        buffer_append_str(html, "                            <div class=\"mt-2 text-xs text-gray-500 dark:text-slate-400\">\n");
        buffer_append_str(html, "                                <span class=\"mr-2\">");
        if (S_ISDIR(est.st_mode))
            buffer_append_str(html, "Diretório");
        else
            buffer_append_str(html, size_str);
        buffer_append_str(html, "</span>\n");
        buffer_append_str(html, "                                <span>");
        buffer_append_str(html, time_str);
        buffer_append_str(html, "</span>\n");
        buffer_append_str(html, "                            </div>\n");
        buffer_append_str(html, "                        </div>\n");
        buffer_append_str(html, "                    </div>\n");
        buffer_append_str(html, "                    <div class=\"file-actions\">\n");
        
        if (!S_ISDIR(est.st_mode)) {
            buffer_append_str(html, "                        <a href=\"");
            buffer_append_str(html, href);
            buffer_append_str(html, "\" download class=\"action-btn bg-green-500 text-white hover:bg-green-600 flex-1\">\n");
            buffer_append_str(html, "                            <i class=\"fas fa-download mr-1\"></i>Baixar\n");
            buffer_append_str(html, "                        </a>\n");
        }
        
        buffer_append_str(html, "                        <button onclick=\"renameItem('");
        buffer_append_str(html, encoded);
        buffer_append_str(html, "', ");
        buffer_append_str(html, S_ISDIR(est.st_mode) ? "true" : "false");
        buffer_append_str(html, ")\" class=\"action-btn bg-blue-500 text-white hover:bg-blue-600 flex-1\">\n");
        buffer_append_str(html, "                            <i class=\"fas fa-edit mr-1\"></i>Renomear\n");
        buffer_append_str(html, "                        </button>\n");
        
        if (!S_ISDIR(est.st_mode)) {
            buffer_append_str(html, "                        <button onclick=\"copyItem('");
            buffer_append_str(html, encoded);
            buffer_append_str(html, "')\" class=\"action-btn bg-purple-500 text-white hover:bg-purple-600 flex-1\">\n");
            buffer_append_str(html, "                            <i class=\"fas fa-copy mr-1\"></i>Copiar\n");
            buffer_append_str(html, "                        </button>\n");
        }
        
        buffer_append_str(html, "                        <button onclick=\"deleteItem('");
        buffer_append_str(html, encoded);
        buffer_append_str(html, "', ");
        buffer_append_str(html, S_ISDIR(est.st_mode) ? "true" : "false");
        buffer_append_str(html, ")\" class=\"action-btn bg-red-500 text-white hover:bg-red-600 flex-1\">\n");
        buffer_append_str(html, "                            <i class=\"fas fa-trash mr-1\"></i>Excluir\n");
        buffer_append_str(html, "                        </button>\n");
        buffer_append_str(html, "                    </div>\n");
        buffer_append_str(html, "                </div>\n");
    }
    
    buffer_append_str(html, "            </div>\n");
    buffer_append_str(html, "        </div>\n");
    
    // Upload Form
    buffer_append_str(html, "        <div class=\"mt-6 bg-white dark:bg-slate-800 rounded-xl shadow-lg p-4 sm:p-6\">\n");
    buffer_append_str(html, "            <h2 class=\"text-lg sm:text-xl font-bold mb-3 sm:mb-4 text-gray-900 dark:text-slate-100 flex items-center\">\n");
    buffer_append_str(html, "                <svg class=\"w-5 h-5 sm:w-6 sm:h-6 mr-2 text-green-500\" fill=\"none\" stroke=\"currentColor\" viewBox=\"0 0 24 24\">\n");
    buffer_append_str(html, "                    <path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M4 16v1a3 3 0 003 3h10a3 3 0 003-3v-1m-4-8l-4-4m0 0L8 8m4-4v12\"></path>\n");
    buffer_append_str(html, "                </svg>\n");
    buffer_append_str(html, "                Upload de Arquivo\n");
    buffer_append_str(html, "            </h2>\n");
    buffer_append_str(html, "            <form method=\"post\" enctype=\"multipart/form-data\" class=\"space-y-3 sm:space-y-4\">\n");
    buffer_append_str(html, "                <div>\n");
    buffer_append_str(html, "                    <label class=\"block text-sm font-medium text-gray-700 dark:text-slate-300 mb-2\">Selecionar arquivo</label>\n");
    buffer_append_str(html, "                    <input type=\"file\" name=\"file\" required class=\"w-full px-3 sm:px-4 py-2 border border-gray-300 dark:border-slate-600 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-blue-500 dark:bg-slate-700 dark:text-white\">\n");
    buffer_append_str(html, "                </div>\n");
    buffer_append_str(html, "                <button type=\"submit\" class=\"w-full bg-gradient-to-r from-green-500 to-green-600 hover:from-green-600 hover:to-green-700 text-white font-bold py-3 px-6 rounded-lg shadow-lg transition-all duration-200\">\n");
    buffer_append_str(html, "                    <div class=\"flex items-center justify-center\">\n");
    buffer_append_str(html, "                        <svg class=\"w-5 h-5 mr-2\" fill=\"none\" stroke=\"currentColor\" viewBox=\"0 0 24 24\">\n");
    buffer_append_str(html, "                            <path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M4 16v1a3 3 0 003 3h10a3 3 0 003-3v-1m-4-8l-4-4m0 0L8 8m4-4v12\"></path>\n");
    buffer_append_str(html, "                        </svg>\n");
    buffer_append_str(html, "                        Upload\n");
    buffer_append_str(html, "                    </div>\n");
    buffer_append_str(html, "                </button>\n");
    buffer_append_str(html, "            </form>\n");
    buffer_append_str(html, "        </div>\n");
    
    // Footer
    buffer_append_str(html, "    </main>\n");
    buffer_append_str(html, "    <footer class=\"bg-gray-800 dark:bg-slate-900 text-white py-6 mt-8\">\n");
    buffer_append_str(html, "        <div class=\"container mx-auto px-4 text-center\">\n");
    buffer_append_str(html, "            <p class=\"text-sm opacity-75\">Mini WebDAV Server Profissional &copy; 2026</p>\n");
    buffer_append_str(html, "            <p class=\"text-xs mt-2 opacity-50\">RFC 4918, RFC 2518, RFC 7230-7235</p>\n");
    buffer_append_str(html, "        </div>\n");
    buffer_append_str(html, "    </footer>\n");
    
    // Action Container for Mobile
    buffer_append_str(html, "    <div class=\"action-container md:hidden\">\n");
    buffer_append_str(html, "        <div class=\"mobile-actions\">\n");
    buffer_append_str(html, "            <button onclick=\"showCreateFolderModal()\" class=\"mobile-action-btn bg-white text-blue-600 hover:bg-blue-50 shadow\">\n");
    buffer_append_str(html, "                <i class=\"fas fa-folder-plus mb-1\"></i>\n");
    buffer_append_str(html, "                <div class=\"font-medium\">Nova Pasta</div>\n");
    buffer_append_str(html, "            </button>\n");
    buffer_append_str(html, "            <button onclick=\"document.getElementById('fileInput').click()\" class=\"mobile-action-btn bg-green-500 hover:bg-green-600 text-white shadow\">\n");
    buffer_append_str(html, "                <i class=\"fas fa-upload mb-1\"></i>\n");
    buffer_append_str(html, "                <div class=\"font-medium\">Upload</div>\n");
    buffer_append_str(html, "            </button>\n");
    buffer_append_str(html, "            <button onclick=\"deleteSelected()\" id=\"mobileDeleteBtn\" disabled class=\"mobile-action-btn bg-red-500 hover:bg-red-600 text-white shadow\">\n");
    buffer_append_str(html, "                <i class=\"fas fa-trash mb-1\"></i>\n");
    buffer_append_str(html, "                <div class=\"font-medium\">Excluir</div>\n");
    buffer_append_str(html, "            </button>\n");
    buffer_append_str(html, "        </div>\n");
    buffer_append_str(html, "    </div>\n");
    
    // Modals
    buffer_append_str(html, "    <!-- Create Folder Modal -->\n");
    buffer_append_str(html, "    <div id=\"createFolderModal\" class=\"modal\">\n");
    buffer_append_str(html, "        <div class=\"modal-content\">\n");
    buffer_append_str(html, "            <h3 class=\"text-xl font-bold mb-4\">Criar Nova Pasta</h3>\n");
    buffer_append_str(html, "            <input type=\"text\" id=\"folderName\" placeholder=\"Nome da pasta\" class=\"mb-4\">\n");
    buffer_append_str(html, "            <div class=\"flex flex-col sm:flex-row justify-end gap-2\">\n");
    buffer_append_str(html, "                <button onclick=\"closeModal('createFolderModal')\" class=\"px-4 py-2 bg-gray-300 hover:bg-gray-400 text-gray-800 dark:bg-slate-600 dark:hover:bg-slate-500 dark:text-slate-200 rounded-lg transition-colors\">Cancelar</button>\n");
    buffer_append_str(html, "                <button onclick=\"createFolder()\" class=\"px-4 py-2 bg-blue-600 hover:bg-blue-700 text-white rounded-lg transition-colors\">Criar</button>\n");
    buffer_append_str(html, "            </div>\n");
    buffer_append_str(html, "        </div>\n");
    buffer_append_str(html, "    </div>\n");
    
    buffer_append_str(html, "    <!-- Rename Modal -->\n");
    buffer_append_str(html, "    <div id=\"renameModal\" class=\"modal\">\n");
    buffer_append_str(html, "        <div class=\"modal-content\">\n");
    buffer_append_str(html, "            <h3 class=\"text-xl font-bold mb-4\">Renomear Item</h3>\n");
    buffer_append_str(html, "            <input type=\"text\" id=\"newName\" placeholder=\"Novo nome\" class=\"mb-4\">\n");
    buffer_append_str(html, "            <div class=\"flex flex-col sm:flex-row justify-end gap-2\">\n");
    buffer_append_str(html, "                <button onclick=\"closeModal('renameModal')\" class=\"px-4 py-2 bg-gray-300 hover:bg-gray-400 text-gray-800 dark:bg-slate-600 dark:hover:bg-slate-500 dark:text-slate-200 rounded-lg transition-colors\">Cancelar</button>\n");
    buffer_append_str(html, "                <button onclick=\"confirmRename()\" class=\"px-4 py-2 bg-blue-600 hover:bg-blue-700 text-white rounded-lg transition-colors\">Renomear</button>\n");
    buffer_append_str(html, "            </div>\n");
    buffer_append_str(html, "        </div>\n");
    buffer_append_str(html, "    </div>\n");
    
    buffer_append_str(html, "    <!-- Copy Modal -->\n");
    buffer_append_str(html, "    <div id=\"copyModal\" class=\"modal\">\n");
    buffer_append_str(html, "        <div class=\"modal-content\">\n");
    buffer_append_str(html, "            <h3 class=\"text-xl font-bold mb-4\">Copiar Arquivo</h3>\n");
    buffer_append_str(html, "            <input type=\"text\" id=\"copyDestination\" placeholder=\"Nome do arquivo copiado\" class=\"mb-4\">\n");
    buffer_append_str(html, "            <div class=\"flex flex-col sm:flex-row justify-end gap-2\">\n");
    buffer_append_str(html, "                <button onclick=\"closeModal('copyModal')\" class=\"px-4 py-2 bg-gray-300 hover:bg-gray-400 text-gray-800 dark:bg-slate-600 dark:hover:bg-slate-500 dark:text-slate-200 rounded-lg transition-colors\">Cancelar</button>\n");
    buffer_append_str(html, "                <button onclick=\"confirmCopy()\" class=\"px-4 py-2 bg-purple-600 hover:bg-purple-700 text-white rounded-lg transition-colors\">Copiar</button>\n");
    buffer_append_str(html, "            </div>\n");
    buffer_append_str(html, "        </div>\n");
    buffer_append_str(html, "    </div>\n");
    
    buffer_append_str(html, "    <!-- Delete Confirmation Modal -->\n");
    buffer_append_str(html, "    <div id=\"deleteModal\" class=\"modal\">\n");
    buffer_append_str(html, "        <div class=\"modal-content\">\n");
    buffer_append_str(html, "            <h3 class=\"text-xl font-bold mb-4 text-red-600\">Confirmar Exclusão</h3>\n");
    buffer_append_str(html, "            <p class=\"mb-4\" id=\"deleteMessage\">Tem certeza que deseja excluir este item?</p>\n");
    buffer_append_str(html, "            <div class=\"flex flex-col sm:flex-row justify-end gap-2\">\n");
    buffer_append_str(html, "                <button onclick=\"closeModal('deleteModal')\" class=\"px-4 py-2 bg-gray-300 hover:bg-gray-400 text-gray-800 dark:bg-slate-600 dark:hover:bg-slate-500 dark:text-slate-200 rounded-lg transition-colors\">Cancelar</button>\n");
    buffer_append_str(html, "                <button onclick=\"confirmDelete()\" class=\"px-4 py-2 bg-red-600 hover:bg-red-700 text-white rounded-lg transition-colors\">Excluir</button>\n");
    buffer_append_str(html, "            </div>\n");
    buffer_append_str(html, "        </div>\n");
    buffer_append_str(html, "    </div>\n");
    
    // Toast container
    buffer_append_str(html, "    <div id=\"toastContainer\" class=\"fixed bottom-4 right-4 z-50\"></div>\n");
    
    // JavaScript completo com correções
    buffer_append_str(html, "    <script>\n");
    buffer_append_str(html, "        const currentPath = '");
    buffer_append_str(html, reqpath);
    buffer_append_str(html, "';\n");
    buffer_append_str(html, "        const baseUrl = window.location.origin;\n");
    buffer_append_str(html, "        let currentItem = null;\n");
    buffer_append_str(html, "        let isDirectory = false;\n");
    buffer_append_str(html, "        let isBulkDelete = false;\n");
    buffer_append_str(html, "        let selectedItems = [];\n");
    
    // Função para fazer requisições WebDAV com autenticação
    buffer_append_str(html, "        function webdavRequest(method, path, headers = {}, body = null) {\n");
    buffer_append_str(html, "            const url = path.startsWith('http') ? path : baseUrl + path;\n");
    buffer_append_str(html, "            const options = {\n");
    buffer_append_str(html, "                method: method,\n");
    buffer_append_str(html, "                headers: headers,\n");
    buffer_append_str(html, "                body: body,\n");
    buffer_append_str(html, "                credentials: 'include'\n");
    buffer_append_str(html, "            };\n");
    buffer_append_str(html, "            return fetch(url, options);\n");
    buffer_append_str(html, "        }\n");
    
    // Renomear (MOVE com Overwrite: T)
    buffer_append_str(html, "        function renameItem(name, isDir) {\n");
    buffer_append_str(html, "            currentItem = decodeURIComponent(name);\n");
    buffer_append_str(html, "            isDirectory = isDir;\n");
    buffer_append_str(html, "            document.getElementById('newName').value = currentItem;\n");
    buffer_append_str(html, "            document.getElementById('renameModal').style.display = 'flex';\n");
    buffer_append_str(html, "        }\n");
    
    buffer_append_str(html, "        function confirmRename() {\n");
    buffer_append_str(html, "            const newName = document.getElementById('newName').value.trim();\n");
    buffer_append_str(html, "            if (!newName || newName === currentItem) {\n");
    buffer_append_str(html, "                showToast('Por favor, insira um novo nome válido', 'error');\n");
    buffer_append_str(html, "                return;\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            const sourcePath = currentPath + (currentPath.endsWith('/') ? '' : '/') + encodeURIComponent(currentItem);\n");
    buffer_append_str(html, "            const destPath = currentPath + (currentPath.endsWith('/') ? '' : '/') + encodeURIComponent(newName);\n");
    buffer_append_str(html, "            const destinationUrl = baseUrl + destPath;\n");
    buffer_append_str(html, "            webdavRequest('MOVE', sourcePath, {\n");
    buffer_append_str(html, "                'Destination': destinationUrl,\n");
    buffer_append_str(html, "                'Overwrite': 'T'\n");
    buffer_append_str(html, "            }).then(response => {\n");
    buffer_append_str(html, "                if (response.ok) {\n");
    buffer_append_str(html, "                    showToast('Renomeado com sucesso!', 'success');\n");
    buffer_append_str(html, "                    setTimeout(() => location.reload(), 500);\n");
    buffer_append_str(html, "                } else {\n");
    buffer_append_str(html, "                    response.text().then(text => {\n");
    buffer_append_str(html, "                        console.error('Erro:', text);\n");
    buffer_append_str(html, "                        showToast('Erro ao renomear: ' + response.status + ' ' + response.statusText, 'error');\n");
    buffer_append_str(html, "                    });\n");
    buffer_append_str(html, "                }\n");
    buffer_append_str(html, "                closeModal('renameModal');\n");
    buffer_append_str(html, "            }).catch(error => {\n");
    buffer_append_str(html, "                console.error('Erro de rede:', error);\n");
    buffer_append_str(html, "                showToast('Erro de rede: ' + error.message, 'error');\n");
    buffer_append_str(html, "                closeModal('renameModal');\n");
    buffer_append_str(html, "            });\n");
    buffer_append_str(html, "        }\n");
    
    // Copiar (COPY)
    buffer_append_str(html, "        function copyItem(name) {\n");
    buffer_append_str(html, "            currentItem = decodeURIComponent(name);\n");
    buffer_append_str(html, "            const defaultName = 'Copia_de_' + currentItem;\n");
    buffer_append_str(html, "            document.getElementById('copyDestination').value = defaultName;\n");
    buffer_append_str(html, "            document.getElementById('copyModal').style.display = 'flex';\n");
    buffer_append_str(html, "        }\n");
    
    buffer_append_str(html, "        function confirmCopy() {\n");
    buffer_append_str(html, "            const newName = document.getElementById('copyDestination').value.trim();\n");
    buffer_append_str(html, "            if (!newName) {\n");
    buffer_append_str(html, "                showToast('Por favor, insira um nome para a cópia', 'error');\n");
    buffer_append_str(html, "                return;\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            const sourcePath = currentPath + (currentPath.endsWith('/') ? '' : '/') + encodeURIComponent(currentItem);\n");
    buffer_append_str(html, "            const destPath = currentPath + (currentPath.endsWith('/') ? '' : '/') + encodeURIComponent(newName);\n");
    buffer_append_str(html, "            const destinationUrl = baseUrl + destPath;\n");
    buffer_append_str(html, "            webdavRequest('COPY', sourcePath, {\n");
    buffer_append_str(html, "                'Destination': destinationUrl,\n");
    buffer_append_str(html, "                'Overwrite': 'T'\n");
    buffer_append_str(html, "            }).then(response => {\n");
    buffer_append_str(html, "                if (response.ok) {\n");
    buffer_append_str(html, "                    showToast('Arquivo copiado com sucesso!', 'success');\n");
    buffer_append_str(html, "                    setTimeout(() => location.reload(), 500);\n");
    buffer_append_str(html, "                } else {\n");
    buffer_append_str(html, "                    response.text().then(text => {\n");
    buffer_append_str(html, "                        console.error('Erro:', text);\n");
    buffer_append_str(html, "                        showToast('Erro ao copiar: ' + response.status + ' ' + response.statusText, 'error');\n");
    buffer_append_str(html, "                    });\n");
    buffer_append_str(html, "                }\n");
    buffer_append_str(html, "                closeModal('copyModal');\n");
    buffer_append_str(html, "            }).catch(error => {\n");
    buffer_append_str(html, "                console.error('Erro de rede:', error);\n");
    buffer_append_str(html, "                showToast('Erro de rede: ' + error.message, 'error');\n");
    buffer_append_str(html, "                closeModal('copyModal');\n");
    buffer_append_str(html, "            });\n");
    buffer_append_str(html, "        }\n");
    
    // Criar pasta (MKCOL)
    buffer_append_str(html, "        function showCreateFolderModal() {\n");
    buffer_append_str(html, "            document.getElementById('createFolderModal').style.display = 'flex';\n");
    buffer_append_str(html, "        }\n");
    
    buffer_append_str(html, "        function createFolder() {\n");
    buffer_append_str(html, "            const folderName = document.getElementById('folderName').value.trim();\n");
    buffer_append_str(html, "            if (!folderName) {\n");
    buffer_append_str(html, "                showToast('Por favor, insira um nome para a pasta', 'error');\n");
    buffer_append_str(html, "                return;\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            const folderPath = currentPath + (currentPath.endsWith('/') ? '' : '/') + encodeURIComponent(folderName);\n");
    buffer_append_str(html, "            webdavRequest('MKCOL', folderPath).then(response => {\n");
    buffer_append_str(html, "                if (response.ok) {\n");
    buffer_append_str(html, "                    showToast('Pasta criada com sucesso!', 'success');\n");
    buffer_append_str(html, "                    setTimeout(() => location.reload(), 500);\n");
    buffer_append_str(html, "                } else {\n");
    buffer_append_str(html, "                    response.text().then(text => {\n");
    buffer_append_str(html, "                        console.error('Erro:', text);\n");
    buffer_append_str(html, "                        showToast('Erro ao criar pasta: ' + response.status + ' ' + response.statusText, 'error');\n");
    buffer_append_str(html, "                    });\n");
    buffer_append_str(html, "                }\n");
    buffer_append_str(html, "                closeModal('createFolderModal');\n");
    buffer_append_str(html, "            }).catch(error => {\n");
    buffer_append_str(html, "                console.error('Erro de rede:', error);\n");
    buffer_append_str(html, "                showToast('Erro de rede: ' + error.message, 'error');\n");
    buffer_append_str(html, "                closeModal('createFolderModal');\n");
    buffer_append_str(html, "            });\n");
    buffer_append_str(html, "        }\n");
    
    // Excluir (DELETE)
    buffer_append_str(html, "        function deleteItem(name, isDir) {\n");
    buffer_append_str(html, "            currentItem = decodeURIComponent(name);\n");
    buffer_append_str(html, "            isDirectory = isDir;\n");
    buffer_append_str(html, "            isBulkDelete = false;\n");
    buffer_append_str(html, "            const itemType = isDir ? 'pasta' : 'arquivo';\n");
    buffer_append_str(html, "            document.getElementById('deleteMessage').textContent = `Tem certeza que deseja excluir esta ${itemType}: ${currentItem}?`;\n");
    buffer_append_str(html, "            document.getElementById('deleteModal').style.display = 'flex';\n");
    buffer_append_str(html, "        }\n");
    
    // Upload de arquivo
    buffer_append_str(html, "        function handleFileUpload(event) {\n");
    buffer_append_str(html, "            const file = event.target.files[0];\n");
    buffer_append_str(html, "            if (!file) return;\n");
    buffer_append_str(html, "            const filePath = currentPath + (currentPath.endsWith('/') ? '' : '/') + encodeURIComponent(file.name);\n");
    buffer_append_str(html, "            webdavRequest('PUT', filePath, {\n");
    buffer_append_str(html, "                'Content-Type': file.type || 'application/octet-stream'\n");
    buffer_append_str(html, "            }, file).then(response => {\n");
    buffer_append_str(html, "                if (response.ok) {\n");
    buffer_append_str(html, "                    showToast('Arquivo enviado com sucesso!', 'success');\n");
    buffer_append_str(html, "                    setTimeout(() => location.reload(), 500);\n");
    buffer_append_str(html, "                } else {\n");
    buffer_append_str(html, "                    response.text().then(text => {\n");
    buffer_append_str(html, "                        console.error('Erro:', text);\n");
    buffer_append_str(html, "                        showToast('Erro no upload: ' + response.status + ' ' + response.statusText, 'error');\n");
    buffer_append_str(html, "                    });\n");
    buffer_append_str(html, "                }\n");
    buffer_append_str(html, "            }).catch(error => {\n");
    buffer_append_str(html, "                console.error('Erro de rede:', error);\n");
    buffer_append_str(html, "                showToast('Erro de rede: ' + error.message, 'error');\n");
    buffer_append_str(html, "            });\n");
    buffer_append_str(html, "        }\n");
    
    // Seleção múltipla
    buffer_append_str(html, "        function toggleSelectAll() {\n");
    buffer_append_str(html, "            const selectAll = document.getElementById('selectAll');\n");
    buffer_append_str(html, "            const checkboxes = document.querySelectorAll('.file-checkbox:not(:disabled)');\n");
    buffer_append_str(html, "            checkboxes.forEach(cb => {\n");
    buffer_append_str(html, "                cb.checked = selectAll.checked;\n");
    buffer_append_str(html, "                const row = cb.closest('.file-row, .file-card');\n");
    buffer_append_str(html, "                if (row) row.classList.toggle('selected', cb.checked);\n");
    buffer_append_str(html, "            });\n");
    buffer_append_str(html, "            updateDeleteButton();\n");
    buffer_append_str(html, "        }\n");
    
    buffer_append_str(html, "        function updateDeleteButton() {\n");
    buffer_append_str(html, "            const checkboxes = document.querySelectorAll('.file-checkbox:checked:not(:disabled)');\n");
    buffer_append_str(html, "            const deleteBtn = document.getElementById('deleteBtn');\n");
    buffer_append_str(html, "            const mobileDeleteBtn = document.getElementById('mobileDeleteBtn');\n");
    buffer_append_str(html, "            const itemCount = document.getElementById('itemCount');\n");
    buffer_append_str(html, "            const hasSelection = checkboxes.length > 0;\n");
    buffer_append_str(html, "            \n");
    buffer_append_str(html, "            // Atualizar botão desktop\n");
    buffer_append_str(html, "            if (deleteBtn) deleteBtn.disabled = !hasSelection;\n");
    buffer_append_str(html, "            \n");
    buffer_append_str(html, "            // Atualizar botão mobile\n");
    buffer_append_str(html, "            if (mobileDeleteBtn) {\n");
    buffer_append_str(html, "                mobileDeleteBtn.disabled = !hasSelection;\n");
    buffer_append_str(html, "                if (hasSelection) {\n");
    buffer_append_str(html, "                    mobileDeleteBtn.style.opacity = '1';\n");
    buffer_append_str(html, "                    mobileDeleteBtn.style.pointerEvents = 'auto';\n");
    buffer_append_str(html, "                } else {\n");
    buffer_append_str(html, "                    mobileDeleteBtn.style.opacity = '0.5';\n");
    buffer_append_str(html, "                    mobileDeleteBtn.style.pointerEvents = 'none';\n");
    buffer_append_str(html, "                }\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            \n");
    buffer_append_str(html, "            if (itemCount) itemCount.textContent = checkboxes.length + ' item(s) selecionados';\n");
    buffer_append_str(html, "        }\n");
    
    buffer_append_str(html, "        function deleteSelected() {\n");
    buffer_append_str(html, "            const checkboxes = document.querySelectorAll('.file-checkbox:checked:not(:disabled)');\n");
    buffer_append_str(html, "            if (checkboxes.length === 0) return;\n");
    buffer_append_str(html, "            selectedItems = [];\n");
    buffer_append_str(html, "            checkboxes.forEach(cb => {\n");
    buffer_append_str(html, "                const row = cb.closest('.file-row, .file-card');\n");
    buffer_append_str(html, "                if (row) {\n");
    buffer_append_str(html, "                    selectedItems.push({\n");
    buffer_append_str(html, "                        path: row.dataset.path,\n");
    buffer_append_str(html, "                        name: decodeURIComponent(row.dataset.name),\n");
    buffer_append_str(html, "                        isDir: row.dataset.isDir === 'true'\n");
    buffer_append_str(html, "                    });\n");
    buffer_append_str(html, "                }\n");
    buffer_append_str(html, "            });\n");
    buffer_append_str(html, "            isBulkDelete = true;\n");
    buffer_append_str(html, "            document.getElementById('deleteMessage').textContent = `Tem certeza que deseja excluir ${selectedItems.length} itens selecionados?`;\n");
    buffer_append_str(html, "            document.getElementById('deleteModal').style.display = 'flex';\n");
    buffer_append_str(html, "        }\n");
    
    // Confirmação de exclusão múltipla
    buffer_append_str(html, "        function confirmDelete() {\n");
    buffer_append_str(html, "            if (isBulkDelete) {\n");
    buffer_append_str(html, "                const promises = selectedItems.map(item => {\n");
    buffer_append_str(html, "                    return webdavRequest('DELETE', item.path);\n");
    buffer_append_str(html, "                });\n");
    buffer_append_str(html, "                Promise.all(promises).then(responses => {\n");
    buffer_append_str(html, "                    const failed = responses.filter(r => !r.ok);\n");
    buffer_append_str(html, "                    if (failed.length > 0) {\n");
    buffer_append_str(html, "                        showToast(`Alguns itens não puderam ser excluídos (${failed.length} falhas)`, 'error');\n");
    buffer_append_str(html, "                    } else {\n");
    buffer_append_str(html, "                        showToast('Itens excluídos com sucesso!', 'success');\n");
    buffer_append_str(html, "                    }\n");
    buffer_append_str(html, "                    setTimeout(() => location.reload(), 500);\n");
    buffer_append_str(html, "                    closeModal('deleteModal');\n");
    buffer_append_str(html, "                }).catch(error => {\n");
    buffer_append_str(html, "                    console.error('Erro de rede:', error);\n");
    buffer_append_str(html, "                    showToast('Erro de rede: ' + error.message, 'error');\n");
    buffer_append_str(html, "                    closeModal('deleteModal');\n");
    buffer_append_str(html, "                });\n");
    buffer_append_str(html, "            } else {\n");
    buffer_append_str(html, "                const itemPath = currentPath + (currentPath.endsWith('/') ? '' : '/') + encodeURIComponent(currentItem);\n");
    buffer_append_str(html, "                webdavRequest('DELETE', itemPath).then(response => {\n");
    buffer_append_str(html, "                    if (response.ok) {\n");
    buffer_append_str(html, "                        showToast('Excluído com sucesso!', 'success');\n");
    buffer_append_str(html, "                        setTimeout(() => location.reload(), 500);\n");
    buffer_append_str(html, "                    } else {\n");
    buffer_append_str(html, "                        response.text().then(text => {\n");
    buffer_append_str(html, "                            console.error('Erro:', text);\n");
    buffer_append_str(html, "                            showToast('Erro ao excluir: ' + response.status + ' ' + response.statusText, 'error');\n");
    buffer_append_str(html, "                        });\n");
    buffer_append_str(html, "                    }\n");
    buffer_append_str(html, "                    closeModal('deleteModal');\n");
    buffer_append_str(html, "                }).catch(error => {\n");
    buffer_append_str(html, "                    console.error('Erro de rede:', error);\n");
    buffer_append_str(html, "                    showToast('Erro de rede: ' + error.message, 'error');\n");
    buffer_append_str(html, "                    closeModal('deleteModal');\n");
    buffer_append_str(html, "                });\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "        }\n");
    
    // Atualizar botão ao clicar em checkboxes individuais
    buffer_append_str(html, "        document.addEventListener('DOMContentLoaded', function() {\n");
    buffer_append_str(html, "            document.querySelectorAll('.file-checkbox').forEach(cb => {\n");
    buffer_append_str(html, "                cb.addEventListener('change', function() {\n");
    buffer_append_str(html, "                    const row = this.closest('.file-row, .file-card');\n");
    buffer_append_str(html, "                    if (row) row.classList.toggle('selected', this.checked);\n");
    buffer_append_str(html, "                    updateDeleteButton();\n");
    buffer_append_str(html, "                });\n");
    buffer_append_str(html, "            });\n");
    buffer_append_str(html, "            \n");
    buffer_append_str(html, "            // Garantir que o botão mobile funcione corretamente\n");
    buffer_append_str(html, "            const mobileDeleteBtn = document.getElementById('mobileDeleteBtn');\n");
    buffer_append_str(html, "            if (mobileDeleteBtn) {\n");
    buffer_append_str(html, "                mobileDeleteBtn.addEventListener('click', function() {\n");
    buffer_append_str(html, "                    if (!this.disabled) {\n");
    buffer_append_str(html, "                        deleteSelected();\n");
    buffer_append_str(html, "                    }\n");
    buffer_append_str(html, "                });\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            \n");
    buffer_append_str(html, "            // Inicializar\n");
    buffer_append_str(html, "            setTimeout(updateDeleteButton, 100);\n");
    buffer_append_str(html, "        });\n");
    
    // Utilitários
    buffer_append_str(html, "        function closeModal(id) {\n");
    buffer_append_str(html, "            document.getElementById(id).style.display = 'none';\n");
    buffer_append_str(html, "        }\n");
    
    buffer_append_str(html, "        function showToast(message, type = 'success') {\n");
    buffer_append_str(html, "            const toast = document.createElement('div');\n");
    buffer_append_str(html, "            toast.className = `toast toast-${type}`;\n");
    buffer_append_str(html, "            toast.textContent = message;\n");
    buffer_append_str(html, "            toast.style.opacity = '1';\n");
    buffer_append_str(html, "            document.getElementById('toastContainer').appendChild(toast);\n");
    buffer_append_str(html, "            setTimeout(() => {\n");
    buffer_append_str(html, "                toast.style.opacity = '0';\n");
    buffer_append_str(html, "                toast.style.transition = 'opacity 0.5s';\n");
    buffer_append_str(html, "                setTimeout(() => toast.remove(), 500);\n");
    buffer_append_str(html, "            }, 3000);\n");
    buffer_append_str(html, "        }\n");
    
    // Fechar modais ao clicar fora
    buffer_append_str(html, "        document.querySelectorAll('.modal').forEach(modal => {\n");
    buffer_append_str(html, "            modal.addEventListener('click', (e) => {\n");
    buffer_append_str(html, "                if (e.target === modal) closeModal(modal.id);\n");
    buffer_append_str(html, "            });\n");
    buffer_append_str(html, "        });\n");
    
    // Tecla ESC para fechar modais
    buffer_append_str(html, "        document.addEventListener('keydown', (e) => {\n");
    buffer_append_str(html, "            if (e.key === 'Escape') {\n");
    buffer_append_str(html, "                document.querySelectorAll('.modal').forEach(modal => {\n");
    buffer_append_str(html, "                    if (modal.style.display === 'flex') closeModal(modal.id);\n");
    buffer_append_str(html, "                });\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "            if (e.key === 'Delete') {\n");
    buffer_append_str(html, "                deleteSelected();\n");
    buffer_append_str(html, "            }\n");
    buffer_append_str(html, "        });\n");
    
    // Suporte a toque longo para mobile
    buffer_append_str(html, "        if ('ontouchstart' in window) {\n");
    buffer_append_str(html, "            document.addEventListener('DOMContentLoaded', function() {\n");
    buffer_append_str(html, "                document.querySelectorAll('.file-card').forEach(card => {\n");
    buffer_append_str(html, "                    let longPressTimer = null;\n");
    buffer_append_str(html, "                    card.addEventListener('touchstart', function(e) {\n");
    buffer_append_str(html, "                        longPressTimer = setTimeout(() => {\n");
    buffer_append_str(html, "                            const checkbox = this.querySelector('.file-checkbox');\n");
    buffer_append_str(html, "                            if (checkbox) {\n");
    buffer_append_str(html, "                                checkbox.checked = !checkbox.checked;\n");
    buffer_append_str(html, "                                this.classList.toggle('selected', checkbox.checked);\n");
    buffer_append_str(html, "                                updateDeleteButton();\n");
    buffer_append_str(html, "                            }\n");
    buffer_append_str(html, "                        }, 500);\n");
    buffer_append_str(html, "                    });\n");
    buffer_append_str(html, "                    card.addEventListener('touchend', function() {\n");
    buffer_append_str(html, "                        clearTimeout(longPressTimer);\n");
    buffer_append_str(html, "                    });\n");
    buffer_append_str(html, "                    card.addEventListener('touchmove', function() {\n");
    buffer_append_str(html, "                        clearTimeout(longPressTimer);\n");
    buffer_append_str(html, "                    });\n");
    buffer_append_str(html, "                });\n");
    buffer_append_str(html, "            });\n");
    buffer_append_str(html, "        }\n");
    
    buffer_append_str(html, "    </script>\n");
    buffer_append_str(html, "</body>\n</html>\n");
}
/* ==================== HTTP HANDLERS ==================== */
static void handle_options(int fd) {
headers_t headers; headers_init(&headers);
headers_set(&headers, "Allow", "OPTIONS, GET, HEAD, POST, PUT, DELETE, MKCOL, PROPFIND, PROPPATCH, COPY, MOVE, LOCK, UNLOCK");
headers_set(&headers, "DAV", "1, 2"); headers_set(&headers, "MS-Author-Via", "DAV");
send_response(fd, HTTP_200_OK, &headers, NULL, 0);
}
static void handle_get_head(int fd, const char *reqpath, headers_t *req_headers, int send_body, int is_head) {
REQUIRE(fd >= 0, "invalid fd"); REQUIRE(reqpath != NULL, "reqpath cannot be NULL");
char fs_path[MAX_PATH + 1];
if (build_fs_path(ROOT_DIR, reqpath, fs_path, sizeof(fs_path)) != 0) { send_error(fd, HTTP_403_FORBIDDEN, "Forbidden"); return; }
struct stat st; if (stat(fs_path, &st) < 0) {
if (errno == ENOENT) send_error(fd, HTTP_404_NOT_FOUND, "Not Found");
else send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Internal Error");
return;
}
const char *if_modified = headers_get(req_headers, "If-Modified-Since");
if (if_modified) {
time_t if_time = 0;
if (parse_http_date_gmt(if_modified, &if_time) && st.st_mtime <= if_time) {
send_response(fd, HTTP_304_NOT_MODIFIED, NULL, NULL, 0); return;
}
}
const char *if_none_match = headers_get(req_headers, "If-None-Match");
if (if_none_match) {
char etag[128]; snprintf(etag, sizeof(etag), "\"%lx-%lx-%lx\"", (unsigned long)st.st_ino, (unsigned long)st.st_size, (unsigned long)st.st_mtime);
if (strcmp(if_none_match, etag) == 0 || strcmp(if_none_match, "*") == 0) { send_response(fd, HTTP_304_NOT_MODIFIED, NULL, NULL, 0); return; }
}
if (S_ISDIR(st.st_mode)) {
if (!is_head) {
DIR *d = opendir(fs_path); if (!d) { send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Cannot open directory"); return; }
buffer_t html; if (buffer_init(&html, 16384) != 0) { closedir(d); send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Internal Error"); return; }
generate_directory_html(&html, reqpath, d, fs_path); closedir(d);
headers_t resp_headers; headers_init(&resp_headers);
headers_set(&resp_headers, "Content-Type", "text/html; charset=utf-8");
send_response(fd, HTTP_200_OK, &resp_headers, html.data, html.length); buffer_free(&html);
} else {
headers_t resp_headers; headers_init(&resp_headers);
headers_set(&resp_headers, "Content-Type", "text/html; charset=utf-8");
send_response(fd, HTTP_200_OK, &resp_headers, NULL, 0);
}
return;
}
const char *range = headers_get(req_headers, "Range"); off_t range_start = 0, range_end = st.st_size - 1; int partial = 0;
if (range && strncmp(range, "bytes=", 6) == 0) {
const char *dash = strchr(range + 6, '-'); if (dash) {
range_start = atoll(range + 6);
if (*(dash + 1) != '\0') range_end = atoll(dash + 1);
else range_end = st.st_size - 1;
if (range_start < 0 || range_start >= st.st_size || range_end < range_start || range_end >= st.st_size) {
send_error(fd, HTTP_416_RANGE_NOT_SATISFIABLE, "Range Not Satisfiable"); return;
}
partial = 1;
}
}
headers_t resp_headers; headers_init(&resp_headers);
headers_set(&resp_headers, "Content-Type", guess_mime(fs_path));
headers_set(&resp_headers, "Last-Modified", http_time(st.st_mtime));
headers_set(&resp_headers, "Accept-Ranges", "bytes");
char etag[128]; snprintf(etag, sizeof(etag), "\"%lx-%lx-%lx\"", (unsigned long)st.st_ino, (unsigned long)st.st_size, (unsigned long)st.st_mtime);
headers_set(&resp_headers, "ETag", etag);
if (partial) {
char content_range[128]; snprintf(content_range, sizeof(content_range), "bytes %lld-%lld/%lld",
                                  (long long)range_start, (long long)range_end, (long long)st.st_size);
headers_set(&resp_headers, "Content-Range", content_range);
char content_length[32]; snprintf(content_length, sizeof(content_length), "%lld", (long long)(range_end - range_start + 1));
headers_set(&resp_headers, "Content-Length", content_length);
}
if (is_head) {
send_response(fd, partial ? HTTP_206_PARTIAL_CONTENT : HTTP_200_OK, &resp_headers, NULL, 0); return;
}
int file_fd = open(fs_path, O_RDONLY); if (file_fd < 0) { send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Cannot open file"); return; }
if (partial && lseek(file_fd, range_start, SEEK_SET) < 0) { close(file_fd); send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Seek failed"); return; }
char header_buf[1024]; int header_len = 0;
if (!append_header(header_buf, sizeof(header_buf), &header_len,
"HTTP/1.1 %d %s\r\nServer: mini-webdav/1.0\r\nDate: %s\r\nContent-Type: %s\r\nLast-Modified: %s\r\nAccept-Ranges: bytes\r\nETag: %s\r\n",
partial ? HTTP_206_PARTIAL_CONTENT : HTTP_200_OK, status_reason(partial ? HTTP_206_PARTIAL_CONTENT : HTTP_200_OK),
http_time(time(NULL)), guess_mime(fs_path), http_time(st.st_mtime), etag)) { close(file_fd); return; }
for (int i = 0; i < resp_headers.count; i++) {
if (!append_header(header_buf, sizeof(header_buf), &header_len, "%s: %s\r\n", resp_headers.keys[i], resp_headers.vals[i])) { close(file_fd); return; }
}
if (!append_header(header_buf, sizeof(header_buf), &header_len, "Connection: close\r\n\r\n")) { close(file_fd); return; }
send_all(fd, header_buf, (size_t)header_len, TIMEOUT);
if (send_body) {
size_t remaining = partial ? (size_t)(range_end - range_start + 1) : (size_t)st.st_size; char buf[SEND_BUF];
while (remaining > 0) {
size_t to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
ssize_t r = read(file_fd, buf, to_read); if (r <= 0) break;
if (send_all(fd, buf, (size_t)r, TIMEOUT) < 0) break;
remaining -= (size_t)r;
}
}
close(file_fd);
}
static void handle_put(int fd, const char *reqpath, headers_t *req_headers, long long content_length) {
REQUIRE(fd >= 0, "invalid fd"); REQUIRE(reqpath != NULL, "reqpath cannot be NULL");
    if (content_length < 0 || content_length > MAX_BODY) { send_error(fd, HTTP_413_PAYLOAD_TOO_LARGE, "Payload Too Large"); return; }
char fs_path[MAX_PATH + 1];
if (build_fs_path(ROOT_DIR, reqpath, fs_path, sizeof(fs_path)) != 0) { send_error(fd, HTTP_403_FORBIDDEN, "Forbidden"); return; }
const char *if_match = headers_get(req_headers, "If-Match");
if (if_match) {
struct stat st; if (stat(fs_path, &st) == 0) {
char etag[128]; snprintf(etag, sizeof(etag), "\"%lx-%lx-%lx\"", (unsigned long)st.st_ino, (unsigned long)st.st_size, (unsigned long)st.st_mtime);
if (strcmp(if_match, etag) != 0 && strcmp(if_match, "*") != 0) { send_error(fd, HTTP_412_PRECONDITION_FAILED, "Precondition Failed"); return; }
}
}
const char *if_none_match = headers_get(req_headers, "If-None-Match");
if (if_none_match && strcmp(if_none_match, "*") == 0) {
struct stat st; if (stat(fs_path, &st) == 0) { send_error(fd, HTTP_412_PRECONDITION_FAILED, "Precondition Failed"); return; }
}
if (create_parent_dirs(fs_path) != 0) { send_error(fd, HTTP_409_CONFLICT, "Cannot create parent directories"); return; }
int file_fd = open(fs_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
if (file_fd < 0) { log_error("Open failed for PUT: %s (%s)", fs_path, strerror(errno)); send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Cannot create file"); return; }
long long remaining = content_length; char buf[RECV_BUF];
while (remaining > 0) {
long long to_read = remaining > (long long)sizeof(buf) ? (long long)sizeof(buf) : remaining;
ssize_t r = recv_all(fd, buf, (size_t)to_read, TIMEOUT);
if (r <= 0) { log_error("Recv failed during PUT"); close(file_fd); unlink(fs_path); send_error(fd, HTTP_400_BAD_REQUEST, "Incomplete body"); return; }
if (write(file_fd, buf, (size_t)r) != r) { close(file_fd); unlink(fs_path); send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Write failed"); return; }
remaining -= r;
}
close(file_fd); struct stat st;
if (stat(fs_path, &st) == 0) {
char etag[128]; snprintf(etag, sizeof(etag), "\"%lx-%lx-%lx\"", (unsigned long)st.st_ino, (unsigned long)st.st_size, (unsigned long)st.st_mtime);
headers_t headers; headers_init(&headers); headers_set(&headers, "ETag", etag); headers_set(&headers, "Location", reqpath);
send_response(fd, HTTP_201_CREATED, &headers, "Created\n", 8);
} else send_response(fd, HTTP_201_CREATED, NULL, "Created\n", 8);
}
static void handle_mkcol(int fd, const char *reqpath) {
REQUIRE(fd >= 0, "invalid fd"); REQUIRE(reqpath != NULL, "reqpath cannot be NULL");
char fs_path[MAX_PATH + 1];
if (build_fs_path(ROOT_DIR, reqpath, fs_path, sizeof(fs_path)) != 0) { send_error(fd, HTTP_403_FORBIDDEN, "Forbidden"); return; }
struct stat st; if (stat(fs_path, &st) == 0) {
if (S_ISDIR(st.st_mode)) send_error(fd, HTTP_405_METHOD_NOT_ALLOWED, "Collection already exists");
else send_error(fd, HTTP_409_CONFLICT, "Resource already exists");
return;
}
if (create_parent_dirs(fs_path) != 0) { send_error(fd, HTTP_409_CONFLICT, "Cannot create parent directories"); return; }
if (mkdir(fs_path, 0755) < 0) { log_error("mkdir failed: %s (%s)", fs_path, strerror(errno)); send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Cannot create directory"); return; }
send_response(fd, HTTP_201_CREATED, NULL, "Created\n", 8);
}
static void handle_delete(int fd, const char *reqpath) {
REQUIRE(fd >= 0, "invalid fd"); REQUIRE(reqpath != NULL, "reqpath cannot be NULL");
char fs_path[MAX_PATH + 1];
if (build_fs_path(ROOT_DIR, reqpath, fs_path, sizeof(fs_path)) != 0) { send_error(fd, HTTP_403_FORBIDDEN, "Forbidden"); return; }
if (find_lock(fs_path, 0)) { send_error(fd, HTTP_423_LOCKED, "Resource is locked"); return; }
if (recursive_delete(fs_path) < 0) {
if (errno == ENOENT) send_error(fd, HTTP_404_NOT_FOUND, "Not Found");
else if (errno == EACCES) send_error(fd, HTTP_403_FORBIDDEN, "Forbidden");
else { log_error("Delete failed: %s (%s)", fs_path, strerror(errno)); send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Cannot delete"); }
return;
}
send_response(fd, HTTP_204_NO_CONTENT, NULL, NULL, 0);
}
static void handle_propfind(int fd, const char *reqpath, headers_t *req_headers, depth_t depth, long long content_length) {
REQUIRE(fd >= 0, "invalid fd"); REQUIRE(reqpath != NULL, "reqpath cannot be NULL");
(void)req_headers;
char *body = NULL; 
if (content_length > 0 && content_length < MAX_BODY) {
body = (char *)malloc((size_t)content_length + 1); if (body) {
long long received = 0; while (received < content_length) {
ssize_t r = recv_all(fd, body + received, (size_t)(content_length - received), TIMEOUT);
if (r <= 0) break;
received += r;
}
body[received] = '\0'; log_debug("PROPFIND body: %.200s", body);
}
}
char fs_path[MAX_PATH + 1];
if (build_fs_path(ROOT_DIR, reqpath, fs_path, sizeof(fs_path)) != 0) { free(body); send_error(fd, HTTP_403_FORBIDDEN, "Forbidden"); return; }

struct stat st;
if (stat(fs_path, &st) < 0) {
    free(body);
    if (errno == ENOENT)
        send_error(fd, HTTP_404_NOT_FOUND, "Not Found");
    else { send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Internal Error"); return; }
}

buffer_t xml;
if (buffer_init(&xml, 8192) != 0) { free(body); send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Internal Error"); return; }
buffer_append_str(&xml, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<D:multistatus xmlns:D=\"DAV:\">\n");
char curr_href[MAX_PATH + 1];
if (strcmp(reqpath, "/") == 0 || strlen(reqpath) == 0) strcpy(curr_href, "/");
else {
strncpy(curr_href, reqpath, sizeof(curr_href) - 1); curr_href[sizeof(curr_href) - 1] = '\0';
size_t len = strlen(curr_href); if (len > 1 && curr_href[len - 1] == '/') curr_href[len - 1] = '\0';
}
int is_dir = S_ISDIR(st.st_mode); const char *main_name;
if (strcmp(reqpath, "/") == 0) main_name = "root";
else {
main_name = strrchr(reqpath, '/'); if (main_name && *(main_name + 1) != '\0') main_name++;
else { main_name = reqpath[0] == '/' ? reqpath + 1 : reqpath; if (*main_name == '\0') main_name = "root"; }
}
char escaped_href[MAX_ENCODED_PATH + 1]; xml_escape(escaped_href, sizeof(escaped_href), curr_href);
char escaped_name[MAX_PATH * 3 + 1]; xml_escape(escaped_name, sizeof(escaped_name), main_name);
char etag[128]; snprintf(etag, sizeof(etag), "\"%lx-%lx-%lx\"", (unsigned long)st.st_ino, (unsigned long)st.st_size, (unsigned long)st.st_mtime);
char creationdate[64]; snprintf(creationdate, sizeof(creationdate), "%s", iso8601_time(st.st_ctime));
char getlastmodified[64]; snprintf(getlastmodified, sizeof(getlastmodified), "%s", http_time(st.st_mtime));
buffer_append_str(&xml, "  <D:response>\n<D:href>"); buffer_append_str(&xml, escaped_href);
buffer_append_str(&xml, "</D:href>\n<D:propstat>\n<D:prop>\n");
buffer_append_str(&xml, "        <D:displayname>"); buffer_append_str(&xml, escaped_name); buffer_append_str(&xml, "</D:displayname>\n");
buffer_append_str(&xml, "        <D:creationdate>"); buffer_append_str(&xml, creationdate); buffer_append_str(&xml, "</D:creationdate>\n");
buffer_append_str(&xml, "        <D:getlastmodified>"); buffer_append_str(&xml, getlastmodified); buffer_append_str(&xml, "</D:getlastmodified>\n");
buffer_append_str(&xml, "        <D:getetag>"); buffer_append_str(&xml, etag); buffer_append_str(&xml, "</D:getetag>\n");
if (is_dir) buffer_append_str(&xml, "        <D:resourcetype><D:collection/></D:resourcetype>\n");
else {
buffer_append_str(&xml, "        <D:resourcetype/>\n");
const char *mime = guess_mime(fs_path); buffer_append_str(&xml, "        <D:getcontenttype>");
buffer_append_str(&xml, mime); buffer_append_str(&xml, "</D:getcontenttype>\n");
char contentlen[32]; snprintf(contentlen, sizeof(contentlen), "%ld", (long)st.st_size);
buffer_append_str(&xml, "        <D:getcontentlength>"); buffer_append_str(&xml, contentlen);
buffer_append_str(&xml, "</D:getcontentlength>\n");
}
buffer_append_str(&xml, "        <D:supportedlock>\n");
buffer_append_str(&xml, "          <D:lockentry>\n");
buffer_append_str(&xml, "            <D:lockscope><D:exclusive/></D:lockscope>\n");
buffer_append_str(&xml, "            <D:locktype><D:write/></D:locktype>\n");
buffer_append_str(&xml, "          </D:lockentry>\n");
buffer_append_str(&xml, "          <D:lockentry>\n");
buffer_append_str(&xml, "            <D:lockscope><D:shared/></D:lockscope>\n");
buffer_append_str(&xml, "            <D:locktype><D:write/></D:locktype>\n");
buffer_append_str(&xml, "          </D:lockentry>\n");
buffer_append_str(&xml, "        </D:supportedlock>\n");
buffer_append_str(&xml, "        <D:lockdiscovery/>\n");
buffer_append_str(&xml, "      </D:prop>\n<D:status>HTTP/1.1 200 OK</D:status>\n");
buffer_append_str(&xml, "    </D:propstat>\n</D:response>\n");
if (is_dir && (depth > 0 || depth == DEPTH_INFINITY)) {
depth_t child_depth = (depth == DEPTH_INFINITY) ? DEPTH_INFINITY : (depth_t)(depth - 1);
char base_uri[MAX_ENCODED_PATH * 2 + 2];
if (strcmp(reqpath, "/") == 0) strcpy(base_uri, "/");
else {
if (snprintf(base_uri, sizeof(base_uri), "%s/", reqpath) >= (int)sizeof(base_uri)) {
buffer_free(&xml); free(body); send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Path too long"); return;
}
size_t blen = strlen(base_uri); if (blen > 1 && base_uri[blen - 2] == '/' && base_uri[blen - 1] == '/') base_uri[blen - 1] = '\0';
}
DIR *d = opendir(fs_path); if (d) {
struct dirent *e; while ((e = readdir(d))) {
if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
char child_fs[MAX_PATH + 1]; if (snprintf(child_fs, sizeof(child_fs), "%s/%s", fs_path, e->d_name) >= (int)sizeof(child_fs)) continue;
struct stat est; if (stat(child_fs, &est) != 0) continue;
char encoded_name[MAX_ENCODED_PATH + 1]; url_encode(encoded_name, sizeof(encoded_name), e->d_name);
char child_uri[MAX_ENCODED_PATH * 2 + 1]; if (snprintf(child_uri, sizeof(child_uri), "%s%s", base_uri, encoded_name) >= (int)sizeof(child_uri)) continue;
int is_child_dir = S_ISDIR(est.st_mode); const char *display_name = e->d_name;
char escaped_child[MAX_ENCODED_PATH + 1]; xml_escape(escaped_child, sizeof(escaped_child), child_uri);
char escaped_disp[MAX_PATH * 3 + 1]; xml_escape(escaped_disp, sizeof(escaped_disp), display_name);
char child_etag[128]; snprintf(child_etag, sizeof(child_etag), "\"%lx-%lx-%lx\"", (unsigned long)est.st_ino, (unsigned long)est.st_size, (unsigned long)est.st_mtime);
char child_creation[64]; snprintf(child_creation, sizeof(child_creation), "%s", iso8601_time(est.st_ctime));
char child_modified[64]; snprintf(child_modified, sizeof(child_modified), "%s", http_time(est.st_mtime));
buffer_append_str(&xml, "  <D:response>\n<D:href>"); buffer_append_str(&xml, escaped_child);
buffer_append_str(&xml, "</D:href>\n<D:propstat>\n<D:prop>\n");
buffer_append_str(&xml, "        <D:displayname>"); buffer_append_str(&xml, escaped_disp); buffer_append_str(&xml, "</D:displayname>\n");
buffer_append_str(&xml, "        <D:creationdate>"); buffer_append_str(&xml, child_creation); buffer_append_str(&xml, "</D:creationdate>\n");
buffer_append_str(&xml, "        <D:getlastmodified>"); buffer_append_str(&xml, child_modified); buffer_append_str(&xml, "</D:getlastmodified>\n");
buffer_append_str(&xml, "        <D:getetag>"); buffer_append_str(&xml, child_etag); buffer_append_str(&xml, "</D:getetag>\n");
if (is_child_dir) buffer_append_str(&xml, "        <D:resourcetype><D:collection/></D:resourcetype>\n");
else {
buffer_append_str(&xml, "        <D:resourcetype/>\n");
const char *mime = guess_mime(child_fs); buffer_append_str(&xml, "        <D:getcontenttype>");
buffer_append_str(&xml, mime); buffer_append_str(&xml, "</D:getcontenttype>\n");
char clen[32]; snprintf(clen, sizeof(clen), "%ld", (long)est.st_size);
buffer_append_str(&xml, "        <D:getcontentlength>"); buffer_append_str(&xml, clen);
buffer_append_str(&xml, "</D:getcontentlength>\n");
}
buffer_append_str(&xml, "        <D:supportedlock>\n");
buffer_append_str(&xml, "          <D:lockentry>\n");
buffer_append_str(&xml, "            <D:lockscope><D:exclusive/></D:lockscope>\n");
buffer_append_str(&xml, "            <D:locktype><D:write/></D:locktype>\n");
buffer_append_str(&xml, "          </D:lockentry>\n");
buffer_append_str(&xml, "          <D:lockentry>\n");
buffer_append_str(&xml, "            <D:lockscope><D:shared/></D:lockscope>\n");
buffer_append_str(&xml, "            <D:locktype><D:write/></D:locktype>\n");
buffer_append_str(&xml, "          </D:lockentry>\n");
buffer_append_str(&xml, "        </D:supportedlock>\n");
buffer_append_str(&xml, "        <D:lockdiscovery/>\n");
buffer_append_str(&xml, "      </D:prop>\n<D:status>HTTP/1.1 200 OK</D:status>\n");
buffer_append_str(&xml, "    </D:propstat>\n</D:response>\n");
if (is_child_dir && child_depth > 0) {
// Recursão manual simplificada - omitida por brevidade neste exemplo compacto
}
}
closedir(d);
}
}
buffer_append_str(&xml, "</D:multistatus>\n");
headers_t resp_headers; headers_init(&resp_headers);
headers_set(&resp_headers, "Content-Type", "application/xml; charset=utf-8");
send_response(fd, HTTP_207_MULTI_STATUS, &resp_headers, xml.data, xml.length);
buffer_free(&xml); free(body);
}
static void handle_proppatch(int fd, const char *reqpath, long long content_length) {
REQUIRE(fd >= 0, "invalid fd"); REQUIRE(reqpath != NULL, "reqpath cannot be NULL");
char fs_path[MAX_PATH + 1];
if (build_fs_path(ROOT_DIR, reqpath, fs_path, sizeof(fs_path)) != 0) { send_error(fd, HTTP_403_FORBIDDEN, "Forbidden"); return; }
struct stat st; if (stat(fs_path, &st) < 0) {
if (errno == ENOENT) send_error(fd, HTTP_404_NOT_FOUND, "Not Found");
else send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Internal Error");
return;
}
if (content_length < 0 || content_length > MAX_BODY) { send_error(fd, HTTP_413_PAYLOAD_TOO_LARGE, "Payload Too Large"); return; }
char *body = (char *)malloc((size_t)content_length + 1); if (!body) { send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Internal Error"); return; }
long long received = 0; while (received < content_length) {
ssize_t r = recv_all(fd, body + received, (size_t)(content_length - received), TIMEOUT);
if (r <= 0) break;
received += r;
}
body[received] = '\0'; log_debug("PROPPATCH body: %.200s", body); free(body);
char xml_response[2048]; int len = snprintf(xml_response, sizeof(xml_response),
"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<D:multistatus xmlns:D=\"DAV:\">\n<D:response>\n<D:href>%s</D:href>\n<D:propstat>\n<D:prop>\n</D:prop>\n<D:status>HTTP/1.1 200 OK</D:status>\n</D:propstat>\n</D:response>\n</D:multistatus>\n", reqpath);
headers_t headers; headers_init(&headers); headers_set(&headers, "Content-Type", "application/xml; charset=utf-8");
send_response(fd, HTTP_207_MULTI_STATUS, &headers, xml_response, (size_t)len);
}
static void handle_move_copy(int fd, const char *reqpath, headers_t *req_headers, int is_move) {
REQUIRE(fd >= 0, "invalid fd"); REQUIRE(reqpath != NULL, "reqpath cannot be NULL");
const char *dest = headers_get(req_headers, "Destination"); if (!dest) { send_error(fd, HTTP_400_BAD_REQUEST, "Missing Destination header"); return; }
if (strncasecmp(dest, "http://", 7) != 0 && strncasecmp(dest, "https://", 8) != 0) { send_error(fd, HTTP_400_BAD_REQUEST, "Invalid Destination scheme"); return; }
const char *path_start = strstr(dest, "://"); if (path_start) { path_start = strchr(path_start + 3, '/'); if (!path_start) path_start = ""; } else path_start = dest;
const char *host = headers_get(req_headers, "Host"); if (host) {
char dest_host[SMALL_BUF]; const char *auth_start = strstr(dest, "://");
if (auth_start) {
auth_start += 3; const char *path = strchr(auth_start, '/');
if (path) snprintf(dest_host, sizeof(dest_host), "%.*s", (int)(path - auth_start), auth_start);
else { strncpy(dest_host, auth_start, sizeof(dest_host) - 1); dest_host[sizeof(dest_host) - 1] = '\0'; }
char *port = strchr(dest_host, ':'); if (port) *port = '\0';
const char *colon = strchr(host, ':'); char host_no_port[SMALL_BUF];
if (colon) { snprintf(host_no_port, sizeof(host_no_port), "%.*s", (int)(colon - host), host); host_no_port[sizeof(host_no_port) - 1] = '\0'; }
else { strncpy(host_no_port, host, sizeof(host_no_port) - 1); host_no_port[sizeof(host_no_port) - 1] = '\0'; }
if (strcasecmp(dest_host, host_no_port) != 0) { send_error(fd, HTTP_400_BAD_REQUEST, "Destination not on this server"); return; }
}
}
const char *new_reqpath = path_start; char src_fs[MAX_PATH + 1], dest_fs[MAX_PATH + 1];
if (build_fs_path(ROOT_DIR, reqpath, src_fs, sizeof(src_fs)) != 0 || build_fs_path(ROOT_DIR, new_reqpath, dest_fs, sizeof(dest_fs)) != 0) {
send_error(fd, HTTP_403_FORBIDDEN, "Forbidden"); return;
}
if (strcmp(src_fs, dest_fs) == 0) { send_error(fd, HTTP_403_FORBIDDEN, "Source and destination are the same"); return; }
if (find_lock(src_fs, 0) || find_lock(dest_fs, 0)) { send_error(fd, HTTP_423_LOCKED, "Resource is locked"); return; }
struct stat st; int dest_exists = (stat(dest_fs, &st) == 0);
const char *overwrite = headers_get(req_headers, "Overwrite"); int do_overwrite = 1;
if (overwrite && strcasecmp(overwrite, "F") == 0) do_overwrite = 0;
if (dest_exists && !do_overwrite) { send_error(fd, HTTP_412_PRECONDITION_FAILED, "Destination exists and Overwrite is F"); return; }
if (dest_exists && recursive_delete(dest_fs) != 0) {
log_error("Delete existing dest failed: %s (%s)", dest_fs, strerror(errno));
send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Cannot delete existing resource"); return;
}
if (create_parent_dirs(dest_fs) != 0) { send_error(fd, HTTP_409_CONFLICT, "Cannot create parent directories"); return; }
int success; if (is_move) success = rename(src_fs, dest_fs);
else success = recursive_copy(src_fs, dest_fs);
if (success != 0) { send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Operation failed"); return; }
int code = dest_exists ? HTTP_204_NO_CONTENT : HTTP_201_CREATED;
send_response(fd, (http_status_t)code, NULL, NULL, 0);
}
static void handle_lock(int fd, const char *reqpath, headers_t *req_headers, long long content_length) {
REQUIRE(fd >= 0, "invalid fd"); REQUIRE(reqpath != NULL, "reqpath cannot be NULL");
char fs_path[MAX_PATH + 1];
if (build_fs_path(ROOT_DIR, reqpath, fs_path, sizeof(fs_path)) != 0) { send_error(fd, HTTP_403_FORBIDDEN, "Forbidden"); return; }
struct stat st; if (stat(fs_path, &st) < 0 && errno == ENOENT) {
if (create_parent_dirs(fs_path) != 0 || mkdir(fs_path, 0755) != 0) {
send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Cannot create lock-null resource"); return;
}
}
const char *if_header = headers_get(req_headers, "If");
if (if_header) {
char token[128]; if (sscanf(if_header, "<%127[^>]", token) == 1) {
if (remove_lock(token) != 0) { send_error(fd, HTTP_412_PRECONDITION_FAILED, "Lock token mismatch"); return; }
}
}
lock_type_t lock_type = LOCK_EXCLUSIVE;
const char *lock_type_header = headers_get(req_headers, "Lock-Type");
if (lock_type_header && strcasecmp(lock_type_header, "shared") == 0) lock_type = LOCK_SHARED;
const char *depth_str = headers_get(req_headers, "Depth"); depth_t depth = DEPTH_ZERO;
if (depth_str) {
if (strcasecmp(depth_str, "0") == 0) depth = DEPTH_ZERO;
else if (strcasecmp(depth_str, "1") == 0) depth = DEPTH_ONE;
else if (strcasecmp(depth_str, "infinity") == 0) depth = DEPTH_INFINITY;
}
const char *timeout_str = headers_get(req_headers, "Timeout"); int timeout = LOCK_TIMEOUT_DEFAULT;
if (timeout_str && strncasecmp(timeout_str, "Second-", 7) == 0) timeout = atoi(timeout_str + 7);
char owner[256] = ""; 
if (content_length > 0 && content_length < MAX_BODY) {
char *body = (char *)malloc((size_t)content_length + 1); if (body) {
long long received = 0; while (received < content_length) {
ssize_t r = recv_all(fd, body + received, (size_t)(content_length - received), TIMEOUT);
if (r <= 0) break;
received += r;
}
body[received] = '\0'; char *owner_start = strstr(body, "<D:owner>");
if (owner_start) {
owner_start += 9; char *owner_end = strstr(owner_start, "</D:owner>");
if (owner_end && (size_t)(owner_end - owner_start) < sizeof(owner)) {
size_t len = owner_end - owner_start; if (len >= sizeof(owner)) len = sizeof(owner) - 1;
memcpy(owner, owner_start, len); owner[len] = '\0';
}
}
free(body);
}
}
char lock_token[64]; generate_lock_token(lock_token, sizeof(lock_token));
if (add_lock(fs_path, lock_type, owner, depth, timeout, lock_token) != 0) { send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Cannot create lock"); return; }
char response[1024]; int len = snprintf(response, sizeof(response),
"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<D:prop xmlns:D=\"DAV:\">\n<D:lockdiscovery>\n<D:activelock>\n<D:locktype><D:write/></D:locktype>\n<D:lockscope><D:%s/></D:lockscope>\n<D:depth>%s</D:depth>\n<D:owner>%s</D:owner>\n<D:timeout>Second-%d</D:timeout>\n<D:locktoken>\n<D:href>%s</D:href>\n</D:locktoken>\n</D:activelock>\n</D:lockdiscovery>\n</D:prop>\n",
lock_type == LOCK_EXCLUSIVE ? "exclusive" : "shared",
depth == DEPTH_INFINITY ? "infinity" : (depth == DEPTH_ONE ? "1" : "0"),
owner, timeout, lock_token);
headers_t headers; headers_init(&headers); headers_set(&headers, "Content-Type", "application/xml; charset=utf-8");
headers_set(&headers, "Lock-Token", lock_token);
send_response(fd, HTTP_200_OK, &headers, response, (size_t)len);
}
static void handle_unlock(int fd, const char *reqpath, headers_t *req_headers) {
REQUIRE(fd >= 0, "invalid fd"); REQUIRE(reqpath != NULL, "reqpath cannot be NULL");
const char *lock_token = headers_get(req_headers, "Lock-Token");
if (!lock_token) { send_error(fd, HTTP_400_BAD_REQUEST, "Missing Lock-Token header"); return; }
if (lock_token[0] == '<') lock_token++;
char token_copy[128];
strncpy(token_copy, lock_token, sizeof(token_copy) - 1); token_copy[sizeof(token_copy) - 1] = '\0';
char *end = strchr(token_copy, '>'); if (end) *end = '\0';
if (remove_lock(token_copy) != 0) { send_error(fd, HTTP_412_PRECONDITION_FAILED, "Lock token does not match"); return; }
send_response(fd, HTTP_204_NO_CONTENT, NULL, NULL, 0);
}
static void handle_post(int fd, const char *reqpath, headers_t *req_headers, long long content_length) {
REQUIRE(fd >= 0, "invalid fd"); REQUIRE(reqpath != NULL, "reqpath cannot be NULL");
if (content_length < 0 || content_length > MAX_BODY) { send_error(fd, HTTP_413_PAYLOAD_TOO_LARGE, "Payload Too Large"); return; }
if (content_length > MAX_MULTIPART_BODY) { send_error(fd, HTTP_413_PAYLOAD_TOO_LARGE, "Multipart body too large"); return; }
char fs_path[MAX_PATH + 1];
if (build_fs_path(ROOT_DIR, reqpath, fs_path, sizeof(fs_path)) != 0) { send_error(fd, HTTP_403_FORBIDDEN, "Forbidden"); return; }
struct stat st; if (stat(fs_path, &st) < 0 || !S_ISDIR(st.st_mode)) { send_error(fd, HTTP_400_BAD_REQUEST, "POST target must be a directory"); return; }
const char *ctype = headers_get(req_headers, "Content-Type");
if (!ctype || strncasecmp(ctype, "multipart/form-data", 19) != 0) { send_error(fd, HTTP_400_BAD_REQUEST, "Content-Type must be multipart/form-data"); return; }
const char *bound_str = strstr(ctype, "boundary="); if (!bound_str) { send_error(fd, HTTP_400_BAD_REQUEST, "No boundary in Content-Type"); return; }
bound_str += 9; char boundary[SMALL_BUF]; strncpy(boundary, bound_str, sizeof(boundary) - 1); boundary[sizeof(boundary) - 1] = '\0';
if (boundary[0] == '"') { memmove(boundary, boundary + 1, strlen(boundary)); size_t blen = strlen(boundary); if (boundary[blen - 1] == '"') boundary[blen - 1] = '\0'; }
char *body = (char *)malloc((size_t)content_length + 1); if (!body) { send_error(fd, HTTP_500_INTERNAL_SERVER_ERROR, "Internal Error"); return; }
long long received = 0; while (received < content_length) {
ssize_t r = recv_all(fd, body + received, (size_t)(content_length - received), TIMEOUT);
if (r <= 0) break;
received += r;
}
body[received] = '\0'; char full_bound[SMALL_BUF]; snprintf(full_bound, sizeof(full_bound), "--%s", boundary);
char *p = body; char *end = body + received; int uploaded = 0;
while (p < end) {
char *part_start = strstr(p, full_bound); if (!part_start) break;
p = part_start + strlen(full_bound); if (strncmp(p, "--", 2) == 0) break;
if (strncmp(p, "\r\n", 2) == 0) p += 2; else continue;
char *h_end = strstr(p, "\r\n\r\n"); if (!h_end) break; p = h_end + 4;
char *content_end = strstr(p, full_bound); if (!content_end) content_end = end;
if (content_end - p >= 2 && *(content_end - 2) == '\r' && *(content_end - 1) == '\n') content_end -= 2;
size_t content_len = content_end - p; char filename[PATH_MAX];
char *fname_start = strstr(h_end - 100, "filename=\"");
if (fname_start) {
fname_start += 10; char *fname_end = strchr(fname_start, '"');
if (fname_end && fname_end < h_end) {
size_t fn_len = fname_end - fname_start; if (fn_len < sizeof(filename)) {
strncpy(filename, fname_start, fn_len); filename[fn_len] = '\0';
char file_path[MAX_PATH + 1];
if (snprintf(file_path, sizeof(file_path), "%s/%s", fs_path, filename) < (int)sizeof(file_path)) {
int file_fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
if (file_fd >= 0) {
if (write(file_fd, p, content_len) == (ssize_t)content_len) uploaded = 1;
close(file_fd);
}
}
}
}
}
p = content_end;
}
free(body); if (!uploaded) { send_error(fd, HTTP_400_BAD_REQUEST, "No file uploaded"); return; }
headers_t headers; headers_init(&headers); headers_set(&headers, "Location", reqpath);
send_response(fd, HTTP_303_SEE_OTHER, &headers, "See Other\n", 10);
}
/* ==================== CLIENT HANDLER ==================== */
static void* handle_client_thread(void *arg) {
int client_fd = *(int*)arg; free(arg);
struct sockaddr_in cli_addr; socklen_t addr_len = sizeof(cli_addr);
if (getpeername(client_fd, (struct sockaddr*)&cli_addr, &addr_len) < 0) { close_socket_fd(client_fd); conn_sem_post(&connection_sem); return NULL; }
char client_ip[INET_ADDRSTRLEN + 1];
if (!inet_ntop(AF_INET, &(cli_addr.sin_addr), client_ip, sizeof(client_ip))) { close_socket_fd(client_fd); conn_sem_post(&connection_sem); return NULL; }
if (check_rate_limit(client_ip)) { send_error(client_fd, HTTP_429_TOO_MANY_REQUESTS, "Too Many Requests"); close_socket_fd(client_fd); conn_sem_post(&connection_sem); return NULL; }
log_info("Connection from %s:%d", client_ip, ntohs(cli_addr.sin_port));
char line[RECV_BUF]; ssize_t n = read_line(client_fd, line, sizeof(line), TIMEOUT);
if (n <= 0) { close_socket_fd(client_fd); conn_sem_post(&connection_sem); return NULL; }
if (n >= 2) line[n - 2] = '\0';
log_debug("Request: %s", line);
char method[SMALL_BUF], path[MAX_PATH + 1], proto[SMALL_BUF];
if (sscanf(line, "%s %s %s", method, path, proto) != 3) { send_error(client_fd, HTTP_400_BAD_REQUEST, "Bad Request"); close_socket_fd(client_fd); conn_sem_post(&connection_sem); return NULL; }
log_debug("Method: %s Path: %s", method, path);
headers_t req_headers; headers_init(&req_headers);
while (1) {
ssize_t ln = read_line(client_fd, line, sizeof(line), TIMEOUT);
if (ln <= 0) break;
if (ln == 2) break;
line[ln - 2] = '\0'; char *colon = strchr(line, ':'); if (!colon) continue;
*colon = '\0'; char *key = line; char *val = colon + 1; while (*val == ' ') val++;
headers_set(&req_headers, key, val); log_debug("Header: %s: %s", key, val);
}
const char *auth_header = headers_get(&req_headers, "Authorization");
if (!check_auth(auth_header)) {
headers_t auth_headers; headers_init(&auth_headers);
headers_set(&auth_headers, "WWW-Authenticate", "Basic realm=\"WebDAV Server\"");
send_response(client_fd, HTTP_401_UNAUTHORIZED, &auth_headers, "Unauthorized\n", 13);
close_socket_fd(client_fd); conn_sem_post(&connection_sem); return NULL;
}
const char *cl = headers_get(&req_headers, "Content-Length"); long long content_length = cl ? atoll(cl) : 0;
const char *depth_str = headers_get(&req_headers, "Depth"); depth_t depth = DEPTH_ONE;
if (depth_str) {
if (strcasecmp(depth_str, "0") == 0) depth = DEPTH_ZERO;
else if (strcasecmp(depth_str, "1") == 0) depth = DEPTH_ONE;
else if (strcasecmp(depth_str, "infinity") == 0) depth = DEPTH_INFINITY;
}
log_debug("Content-Length: %lld Depth: %d", content_length, depth);
if (strcasecmp(method, "OPTIONS") == 0) handle_options(client_fd);
else if (strcasecmp(method, "GET") == 0) handle_get_head(client_fd, path, &req_headers, 1, 0);
else if (strcasecmp(method, "HEAD") == 0) handle_get_head(client_fd, path, &req_headers, 0, 1);
else if (strcasecmp(method, "POST") == 0) handle_post(client_fd, path, &req_headers, content_length);
else if (strcasecmp(method, "PUT") == 0) handle_put(client_fd, path, &req_headers, content_length);
else if (strcasecmp(method, "MKCOL") == 0) handle_mkcol(client_fd, path);
else if (strcasecmp(method, "DELETE") == 0) handle_delete(client_fd, path);
else if (strcasecmp(method, "PROPFIND") == 0) handle_propfind(client_fd, path, &req_headers, depth, content_length);
else if (strcasecmp(method, "PROPPATCH") == 0) handle_proppatch(client_fd, path, content_length);
else if (strcasecmp(method, "MOVE") == 0) handle_move_copy(client_fd, path, &req_headers, 1);
else if (strcasecmp(method, "COPY") == 0) handle_move_copy(client_fd, path, &req_headers, 0);
else if (strcasecmp(method, "LOCK") == 0) handle_lock(client_fd, path, &req_headers, content_length);
else if (strcasecmp(method, "UNLOCK") == 0) handle_unlock(client_fd, path, &req_headers);
else send_error(client_fd, HTTP_501_NOT_IMPLEMENTED, "Method Not Implemented");
close_socket_fd(client_fd); conn_sem_post(&connection_sem);
log_debug("Connection closed: %s:%d", client_ip, ntohs(cli_addr.sin_port));
return NULL;
}
/* ==================== MAIN ==================== */
static void cleanup(void) {
log_info("Cleaning up resources...");
if (ROOT_DIR) free(ROOT_DIR);
if (AUTH_USER) free(AUTH_USER);
if (AUTH_PASS) free(AUTH_PASS);
if (self_pipe[0] != -1) close(self_pipe[0]);
if (self_pipe[1] != -1) close(self_pipe[1]);
conn_sem_destroy(&connection_sem);
log_info("Cleanup complete");
}
int main(int argc, char **argv) {
signal(SIGINT, sigint_handler);
signal(SIGTERM, sigterm_handler);
#ifndef _WIN32
signal(SIGPIPE, sigpipe_handler);
signal(SIGCHLD, SIG_IGN);
#endif
atexit(cleanup);
setup_self_pipe();  // Configurar self-pipe trick
#ifdef _WIN32
if (windows_socket_init() != 0) exit(1);
pthread_mutex_init(&lock_mutex, NULL);
pthread_mutex_init(&rate_mutex, NULL);
#endif
ROOT_DIR = strdup(".");
int opt;
    while ((opt = getopt(argc, argv, "r:p:u:w:t:m:vh")) != -1) {
    switch (opt) {
    case 'r': free(ROOT_DIR); ROOT_DIR = strdup(optarg); break;
    case 'p': PORT = atoi(optarg); break;
    case 'u': AUTH_USER = strdup(optarg); break;
    case 'w': AUTH_PASS = strdup(optarg); break;
            case 't': TIMEOUT = atoi(optarg); if (TIMEOUT <= 0) TIMEOUT = DEFAULT_TIMEOUT; break;
            case 'm': MAX_REQ = atoi(optarg); if (MAX_REQ <= 0) MAX_REQ = RATE_LIMIT_MAX_REQ; break;
            case 'v': VERBOSE = 1; break;
            case 'h':
                printf("Usage: %s [-r root] [-p port] [-u user] [-w password] [-t timeout] [-m max_req] [-v]\n", argv[0]);
                printf("  -r root       Root directory (default: .)\n");
                printf("  -p port       Port number (default: 8080)\n");
                printf("  -u user       Username for Basic Auth\n");
                printf("  -w password   Password for Basic Auth\n");
                printf("  -t timeout    Timeout in seconds (default: %d)\n", DEFAULT_TIMEOUT);
                printf("  -m max_req    Max requests per minute for rate limiting (default: %d)\n", RATE_LIMIT_MAX_REQ);
                printf("  -v            Verbose mode\n");
                printf("  -h            Show this help\n");
                return 0;
        }
    }
    if ((AUTH_USER && !AUTH_PASS) || (!AUTH_USER && AUTH_PASS)) {
        fprintf(stderr, "Error: must provide both -u <user> and -w <password> to enable authentication.\n");
        exit(1);
    }
    size_t root_len = strlen(ROOT_DIR);
    if (root_len > 0 && ROOT_DIR[root_len - 1] != '/') {
        char *new_root = (char *)realloc(ROOT_DIR, root_len + 2);
        if (new_root) { ROOT_DIR = new_root; strcat(ROOT_DIR, "/"); }
    }
    if (conn_sem_init(&connection_sem, MAX_CONNECTIONS) != 0) { perror("conn_sem_init"); exit(1); }
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); exit(1); }
    int yes = 1;
#ifdef _WIN32
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, (int)sizeof(yes));
#else
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(PORT); addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); close_socket_fd(sock); exit(1); }
    if (listen(sock, BACKLOG) < 0) { perror("listen"); close_socket_fd(sock); exit(1); }
    char realroot[MAX_PATH + 1];
    if (realpath(ROOT_DIR, realroot)) {
        log_info("Server started on 0.0.0.0:%d", PORT);
        log_info("Root directory: %s", realroot);
        if (AUTH_USER) log_info("Authentication enabled for user: %s", AUTH_USER);
        else log_info("Authentication disabled (public access)");
        log_info("Timeout: %d seconds", TIMEOUT);
        log_info("Rate limit: %d requests per %d seconds", MAX_REQ, RATE_LIMIT_WINDOW);
        if (VERBOSE) log_info("Verbose mode enabled");
    } else {
        log_info("Server started on 0.0.0.0:%d", PORT);
        log_info("Root directory: %s", ROOT_DIR);
    }
    
    // Loop principal com espera multiplexada para permitir shutdown imediato
    while (RUNNING) {
#ifdef __COSMOPOLITAN__
        struct sockaddr_in cli_addr; socklen_t addr_len = sizeof(cli_addr);
        int client_fd = accept(sock, (struct sockaddr *)&cli_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR || !RUNNING) continue;
            perror("accept"); continue;
        }
        conn_sem_wait(&connection_sem);
        int *fd_ptr = (int *)malloc(sizeof(int)); if (!fd_ptr) { close_socket_fd(client_fd); conn_sem_post(&connection_sem); continue; }
        *fd_ptr = client_fd;
        // Cosmopolitan path: process synchronously to avoid runtime thread crashes.
        handle_client_thread(fd_ptr);
#elif defined(_WIN32)
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET((SOCKET)sock, &readfds);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int ret = select(sock + 1, &readfds, NULL, NULL, &tv);
#else
        struct pollfd fds[2];
        fds[0].fd = sock; fds[0].events = POLLIN;
        fds[1].fd = self_pipe[0]; fds[1].events = POLLIN;
        
        int ret = poll(fds, 2, 1000);  // Timeout de 1s para verificar RUNNING periodicamente
#endif
#ifndef __COSMOPOLITAN__
        if (ret < 0) {
            if (errno == EINTR) continue;  // Interrupção por sinal
            perror("poll"); break;
        }
#if !defined(_WIN32) && !defined(__COSMOPOLITAN__)
        // Verificar se há dados no self-pipe (sinal de shutdown)
        if (fds[1].revents & POLLIN) {
            char buf[10]; if (read(self_pipe[0], buf, sizeof(buf)) < 0) {}
            log_info("Shutdown triggered via self-pipe");
            break;
        }
#endif
        // Verificar se há nova conexão
#ifdef _WIN32
        if (ret > 0 && FD_ISSET(sock, &readfds)) {
#else
        if (fds[0].revents & POLLIN) {
#endif
            struct sockaddr_in cli_addr; socklen_t addr_len = sizeof(cli_addr);
            int client_fd = accept(sock, (struct sockaddr *)&cli_addr, &addr_len);
            if (client_fd < 0) {
                if (errno == EINTR || !RUNNING) break;
                perror("accept"); continue;
            }
            conn_sem_wait(&connection_sem);
            int *fd_ptr = (int *)malloc(sizeof(int)); if (!fd_ptr) { close_socket_fd(client_fd); conn_sem_post(&connection_sem); continue; }
            *fd_ptr = client_fd;
            pthread_t thread; if (pthread_create(&thread, NULL, handle_client_thread, fd_ptr) != 0) {
                perror("pthread_create"); close_socket_fd(client_fd); free(fd_ptr); conn_sem_post(&connection_sem); continue;
            }
            pthread_detach(thread);
        }
#endif
    }
    
    log_info("Shutting down server gracefully...");
    close_socket_fd(sock);
#if !defined(_WIN32) && !defined(__COSMOPOLITAN__)
    close(self_pipe[0]); close(self_pipe[1]);
#endif
    
    // Aguardar conexões ativas terminarem (timeout de 5 segundos)
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;
    while (conn_sem_trywait(&connection_sem) == -1 && errno == EAGAIN) {
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > ts.tv_sec || (now.tv_sec == ts.tv_sec && now.tv_nsec >= ts.tv_nsec)) {
            log_warn("Timeout waiting for active connections to close");
            break;
        }
        usleep(100000);  // Esperar 100ms
    }
    
    log_info("Server shutdown complete");
#ifdef _WIN32
WSACleanup();
#endif
    return 0;
}
