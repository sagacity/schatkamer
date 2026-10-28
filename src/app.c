// Schatkamer for PS5: a loopback server for the PS5 media web app. It searches
// the site, resolves a programme at play time and relays the CDN to Shaka.
// It never decrypts media or touches licenses. SPDX-License-Identifier: GPL-3.0-or-later
#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>
#include "console_curl.h"
#include "install.h"
#include "shaka.inc"
#include "webui.inc"

#define SITE "https://schatkamer.beeldengeluid.nl"
#define CDN "https://sk-video.cdn.beeldengeluid.nl"
#define CDN_AUDIO "https://sk-audio.cdn.beeldengeluid.nl"
#define PORT 48731
#define MAX_PAGE (8u * 1024u * 1024u)
#define MAX_RELAY (16u * 1024u * 1024u)
#define MAX_RESULTS 60

// ---------------------------------------------------------------- buffers

struct buf { char *data; size_t len; size_t limit; };

static int buf_add(struct buf *b, const void *p, size_t n)
{
    if (n > b->limit - b->len) return 0;
    char *next = realloc(b->data, b->len + n + 1);
    if (!next) return 0;
    b->data = next;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
    return 1;
}

static int buf_str(struct buf *b, const char *s) { return buf_add(b, s, strlen(s)); }

static int buf_json(struct buf *b, const char *s)
{
    if (!buf_str(b, "\"")) return 0;
    for (; *s; ++s) {
        char e[8];
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') snprintf(e, sizeof(e), "\\%c", c);
        else if (c < 0x20) snprintf(e, sizeof(e), "\\u%04x", c);
        else { e[0] = (char)c; e[1] = 0; }
        if (!buf_str(b, e)) return 0;
    }
    return buf_str(b, "\"");
}

static size_t curl_to_buf(char *data, size_t size, size_t count, void *ctx)
{
    if (size && count > SIZE_MAX / size) return 0;
    return buf_add(ctx, data, size * count) ? size * count : 0;
}

// ------------------------------------------------------------------- text

static void html_unescape(char *s)
{
    static const struct { const char *from; char to; } table[] = {
        {"&amp;", '&'}, {"&quot;", '"'}, {"&#x27;", '\''}, {"&#39;", '\''}, {"&lt;", '<'}, {"&gt;", '>'},
    };
    char *out = s;
    while (*s) {
        int hit = 0;
        if (*s == '&')
            for (size_t i = 0; i < sizeof(table) / sizeof(table[0]) && !hit; ++i) {
                size_t n = strlen(table[i].from);
                if (!strncmp(s, table[i].from, n)) { *out++ = table[i].to; s += n; hit = 1; }
            }
        if (!hit) *out++ = *s++;
    }
    *out = 0;
}

static int hexval(int c) { return isdigit(c) ? c - '0' : (tolower(c) >= 'a' && tolower(c) <= 'f') ? tolower(c) - 'a' + 10 : -1; }

// Decodes %XX and '+' in place.
static void url_decode(char *s)
{
    char *out = s;
    while (*s) {
        if (*s == '%' && hexval((unsigned char)s[1]) >= 0 && hexval((unsigned char)s[2]) >= 0) {
            *out++ = (char)(hexval((unsigned char)s[1]) * 16 + hexval((unsigned char)s[2]));
            s += 3;
        } else if (*s == '+') { *out++ = ' '; ++s; }
        else *out++ = *s++;
    }
    *out = 0;
}

// Copies the value of query parameter `name` (still encoded) into out.
static int query_raw(const char *query, const char *name, char *out, size_t cap)
{
    size_t name_len = strlen(name);
    while (*query) {
        const char *end = strchr(query, '&');
        if (!end) end = query + strlen(query);
        if ((size_t)(end - query) > name_len && !strncmp(query, name, name_len) && query[name_len] == '=') {
            size_t n = (size_t)(end - query) - name_len - 1;
            if (!n || n >= cap) return 0;
            memcpy(out, query + name_len + 1, n);
            out[n] = 0;
            return 1;
        }
        if (!*end) break;
        query = end + 1;
    }
    return 0;
}

static void url_encode(struct buf *b, const char *s)
{
    for (; *s; ++s) {
        char e[4];
        if (isalnum((unsigned char)*s) || strchr("-_.~", *s)) { e[0] = *s; e[1] = 0; }
        else snprintf(e, sizeof(e), "%%%02X", (unsigned char)*s);
        buf_str(b, e);
    }
}

// A programme page path: /programma/<id...> or /serie/<id>/aflevering/<id>.
static int valid_programme_path(const char *p)
{
    int serie = !strncmp(p, "/serie/", 7);
    if (!serie && strncmp(p, "/programma/", 11)) return 0;
    if (strlen(p) > 300 || strstr(p, "//")) return 0;
    for (const char *c = p; *c; ++c)
        if (!isalnum((unsigned char)*c) && !strchr("/-_", *c)) return 0;
    return !serie || strstr(p, "/aflevering/") != NULL;
}

// ---------------------------------------------------------------- network

static CURL *new_handle(void)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    console_curl_setup(curl);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (PlayStation) SchatkamerPS5/1.0");
    return curl;
}

// GETs a page; returns the HTTP status or 0 on transport failure. With rsc
// set, the site answers with its React Server Components data (plain JSON
// lines) instead of HTML.
static long fetch(const char *url, struct buf *out, int rsc)
{
    CURL *curl = new_handle();
    if (!curl) return 0;
    out->limit = MAX_PAGE;
    struct curl_slist *headers = rsc ? curl_slist_append(NULL, "rsc: 1") : NULL;
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_to_buf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    long status = 0;
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    printf("Fetch: curl=%d status=%ld\n", (int)rc, status);
    fflush(stdout);
    return rc == CURLE_OK ? status : 0;
}

static long fetch_page(const char *url, struct buf *out) { return fetch(url, out, 0); }

// ----------------------------------------------------------------- search

static void put_utf8(char *out, size_t *o, size_t cap, unsigned v)
{
    if (v < 0x80 && *o + 1 < cap) out[(*o)++] = (char)v;
    else if (v < 0x800 && *o + 2 < cap) { out[(*o)++] = (char)(0xc0 | v >> 6); out[(*o)++] = (char)(0x80 | (v & 0x3f)); }
    else if (*o + 3 < cap) { out[(*o)++] = (char)(0xe0 | v >> 12); out[(*o)++] = (char)(0x80 | (v >> 6 & 0x3f)); out[(*o)++] = (char)(0x80 | (v & 0x3f)); }
}

static unsigned hex4(const char *p)
{
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) { int h = hexval((unsigned char)p[i]); if (h < 0) return '?'; v = v * 16 + (unsigned)h; }
    return v;
}

// Reads the string value of "key" from the page's result data between `from`
// and `limit`. The data is JSON inside a JavaScript string, so it is escaped
// twice: a quote of the JSON is \" in the page and a quote inside a value \\\".
static int escaped_field(const char *from, const char *limit, const char *key, char *out, size_t cap)
{
    char marker[48];
    snprintf(marker, sizeof(marker), "\\\"%s\\\":\\\"", key);
    const char *p = strstr(from, marker);
    if (!p || p >= limit) return 0;
    p += strlen(marker);
    char level1[4096];
    size_t n = 0;
    while (*p && p < limit && n + 4 < sizeof(level1)) {
        if (p[0] != '\\' || !p[1]) { level1[n++] = *p++; continue; }
        char c = p[1];
        if (c == '"') {
            size_t slashes = 0;
            while (slashes < n && level1[n - 1 - slashes] == '\\') ++slashes;
            if (slashes % 2 == 0) break; // the end of the value
        }
        if (c == 'u' && hexval((unsigned char)p[2]) >= 0) { put_utf8(level1, &n, sizeof(level1), hex4(p + 2)); p += 6; continue; }
        level1[n++] = c;
        p += 2;
    }
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < cap; ++i) {
        if (level1[i] != '\\' || i + 1 >= n) { out[o++] = level1[i]; continue; }
        char c = level1[++i];
        if (c == 'n' || c == 'r' || c == 't') out[o++] = ' ';
        else if (c == 'u' && i + 4 < n) { put_utf8(out, &o, cap, hex4(level1 + i + 1)); i += 4; }
        else out[o++] = c;
    }
    out[o] = 0;
    return 1;
}

static void json_member(struct buf *json, const char *name, const char *value, int first)
{
    if (!first) buf_str(json, ",");
    buf_str(json, "\"");
    buf_str(json, name);
    buf_str(json, "\":");
    buf_json(json, value);
}

// Writes the playable video results as a JSON array of {title, series,
// description, date, broadcaster, image, path}. Each result in the page's
// data holds "mediaType" between its text fields and its programme URL.
static int search(const char *query, int number, const char *sort, struct buf *json)
{
    struct buf url = {0}, page = {0};
    url.limit = 4096;
    buf_str(&url, SITE "/zoeken?q=");
    url_encode(&url, query);
    if (number > 1) { char more[24]; snprintf(more, sizeof(more), "&pagina=%d", number); buf_str(&url, more); }
    if (*sort) { buf_str(&url, "&sorteren="); buf_str(&url, sort); }
    long status = url.data ? fetch_page(url.data, &page) : 0;
    free(url.data);
    if (status != 200 || !page.data) { free(page.data); return 0; }

    static const char media[] = "\\\"mediaType\\\":\\\"";
    static const char object[] = "{\\\"id\\\":\\\"";
    char *seen[MAX_RESULTS];
    int count = 0;
    json->limit = 512 * 1024;
    buf_str(json, "[");
    for (char *m = page.data; count < MAX_RESULTS && (m = strstr(m, media)); m += sizeof(media) - 1) {
        // The result's own object starts at the nearest "{"id":" before its media type.
        char *start = NULL;
        for (char *s = m; s > page.data && m - s < 16000; --s)
            if (!strncmp(s, object, sizeof(object) - 1)) { start = s; break; }
        char *next = strstr(m + 1, media);
        char *end = next ? next : m + strlen(m);
        if (!start || strncmp(m + sizeof(media) - 1, "video", 5)) continue;
        char link[600], path[320], title[512], series[256], description[2048], date[32], broadcaster[128], image[600] = "";
        if (!escaped_field(m, end, "url", link, sizeof(link)) || strncmp(link, SITE "/", sizeof(SITE)) ||
            !escaped_field(start, m, "title", title, sizeof(title))) continue;
        snprintf(path, sizeof(path), "%s", link + sizeof(SITE) - 1);
        path[strcspn(path, "?#")] = 0;
        if (!valid_programme_path(path)) continue;
        int dup = 0;
        for (int i = 0; i < count; ++i) if (!strcmp(seen[i], path)) dup = 1;
        if (dup) continue;
        if (!escaped_field(start, m, "seriesTitle", series, sizeof(series))) series[0] = 0;
        if (!escaped_field(start, m, "description", description, sizeof(description))) description[0] = 0;
        if (!escaped_field(m, end, "date", date, sizeof(date))) date[0] = 0;
        if (!escaped_field(m, end, "broadcaster", broadcaster, sizeof(broadcaster))) broadcaster[0] = 0;
        char *img = strstr(start, "\\\"image\\\":{");
        if (img && img < m && !escaped_field(img, m, "url", image, sizeof(image))) image[0] = 0;
        seen[count] = strdup(path);
        if (!seen[count]) break;
        if (count++) buf_str(json, ",");
        buf_str(json, "{");
        json_member(json, "title", title, 1);
        json_member(json, "series", series, 0);
        json_member(json, "description", description, 0);
        json_member(json, "date", date, 0);
        json_member(json, "broadcaster", broadcaster, 0);
        json_member(json, "image", image, 0);
        json_member(json, "path", path, 0);
        buf_str(json, "}");
    }
    buf_str(json, "]");
    for (int i = 0; i < count; ++i) free(seen[i]);
    free(page.data);
    return json->data != NULL;
}

// ------------------------------------------------------------------- browse

// Minimal reading of the plain JSON in the site's RSC data: skip a value,
// and find a string member of an object at its top level.

static const char *skip_space(const char *p)
{
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') ++p;
    return p;
}

// Returns the character after the JSON value at p.
static const char *skip_value(const char *p)
{
    if (*p == '"') {
        for (++p; *p && *p != '"'; ++p)
            if (*p == '\\' && p[1]) ++p;
        return *p ? p + 1 : p;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; *p; ++p) {
            if (*p == '"') { p = skip_value(p) - 1; continue; }
            if (*p == '{' || *p == '[') ++depth;
            else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') ++p;
    return p;
}

// Decodes the JSON string at p (pointing at its opening quote) into out.
static void decode_string(const char *p, char *out, size_t cap)
{
    size_t o = 0;
    for (++p; *p && *p != '"' && o + 4 < cap; ++p) {
        if (*p != '\\' || !p[1]) { out[o++] = *p; continue; }
        char c = *++p;
        if (c == 'n' || c == 'r' || c == 't') out[o++] = ' ';
        else if (c == 'u' && hexval((unsigned char)p[1]) >= 0) { put_utf8(out, &o, cap, hex4(p + 1)); p += 4; }
        else out[o++] = c;
    }
    out[o] = 0;
}

// The value of member `key` of the object at obj, or NULL.
static const char *member(const char *obj, const char *key)
{
    if (*obj != '{') return NULL;
    size_t key_len = strlen(key);
    for (const char *p = skip_space(obj + 1); *p == '"';) {
        const char *name = p + 1;
        const char *after = skip_value(p);
        if (*after != ':') return NULL;
        const char *value = after + 1;
        if ((size_t)(after - 1 - name) == key_len && !strncmp(name, key, key_len)) return value;
        p = skip_space(skip_value(value));
    }
    return NULL;
}

static int member_string(const char *obj, const char *key, char *out, size_t cap)
{
    const char *value = obj ? member(obj, key) : NULL;
    out[0] = 0;
    if (!value || *value != '"') return 0;
    decode_string(value, out, cap);
    return 1;
}

// A folder the title can open: a series, broadcaster, person or story page.
static int valid_folder_path(const char *p)
{
    if (strncmp(p, "/serie/", 7) && strncmp(p, "/omroep/", 8) && strncmp(p, "/persoon/", 9) && strncmp(p, "/verhaal/", 9))
        return 0;
    if (strlen(p) > 300 || strstr(p, "//") || strstr(p, "/aflevering/")) return 0;
    for (const char *c = p; *c; ++c)
        if (!isalnum((unsigned char)*c) && !strchr("/-_", *c)) return 0;
    return 1;
}

// Writes one item of a row or folder as a flat JSON object. kind is "play"
// for a programme and "folder" for a page with programmes of its own.
// With path set, that path is used instead of the item's "url".
static int write_item_at(struct buf *json, const char *item, const char *row, int *count, const char *given_path)
{
    char link[600], path[320], title[512], series[256], description[2048], date[32], broadcaster[128], image[600], media[16];
    if (!member_string(item, "title", title, sizeof(title))) return 0;
    if (given_path) snprintf(path, sizeof(path), "%s", given_path);
    else if (!member_string(item, "url", link, sizeof(link)) || strncmp(link, SITE "/", sizeof(SITE))) return 0;
    else snprintf(path, sizeof(path), "%s", link + sizeof(SITE) - 1);
    if (member_string(item, "mediaType", media, sizeof(media)) && strcmp(media, "video")) return 0;
    const char *playable = member(item, "isPlayable");
    if (playable && !strncmp(playable, "false", 5)) return 0;
    path[strcspn(path, "?#")] = 0;
    const char *kind = valid_programme_path(path) ? "play" : valid_folder_path(path) ? "folder" : NULL;
    if (!kind) return 0;
    if (!member_string(item, "seriesTitle", series, sizeof(series))) member_string(item, "subTitle", series, sizeof(series));
    member_string(item, "description", description, sizeof(description));
    member_string(item, "date", date, sizeof(date));
    member_string(item, "broadcaster", broadcaster, sizeof(broadcaster));
    member_string(member(item, "image"), "url", image, sizeof(image));
    if ((*count)++) buf_str(json, ",");
    buf_str(json, "{");
    json_member(json, "title", title, 1);
    json_member(json, "series", series, 0);
    json_member(json, "description", description, 0);
    json_member(json, "date", date, 0);
    json_member(json, "broadcaster", broadcaster, 0);
    json_member(json, "image", image, 0);
    json_member(json, "row", row, 0);
    json_member(json, "kind", kind, 0);
    json_member(json, "path", path, 0);
    buf_str(json, "}");
    return 1;
}

static int write_item(struct buf *json, const char *item, const char *row, int *count)
{
    return write_item_at(json, item, row, count, NULL);
}

// The page path of a programme from its route ("urlObject"), for programmes
// that carry no "url": /series/[id]/[slug]/program/[programId] becomes
// /serie/<id>/<slug>/aflevering/<programId>, /program/[id]/[[...slug]] /programma/<id>/<slug>.
static int route_path(const char *program, char *out, size_t cap)
{
    const char *route = member(program, "urlObject");
    const char *params = route ? member(route, "params") : NULL;
    char pathname[96], id[48], slug[200] = "", program_id[48];
    if (!params || !member_string(route, "pathname", pathname, sizeof(pathname)) || !member_string(params, "id", id, sizeof(id)))
        return 0;
    const char *slug_value = member(params, "slug");
    if (slug_value && *slug_value == '[') slug_value = skip_space(slug_value + 1);
    if (slug_value && *slug_value == '"') decode_string(slug_value, slug, sizeof(slug));
    if (!strcmp(pathname, "/series/[id]/[slug]/program/[programId]") && member_string(params, "programId", program_id, sizeof(program_id)))
        return snprintf(out, cap, "/serie/%s/%s/aflevering/%s", id, slug, program_id) < (int)cap;
    if (!strncmp(pathname, "/program/[id]", 13))
        return snprintf(out, cap, slug[0] ? "/programma/%s/%s" : "/programma/%s", id, slug) < (int)cap;
    return 0;
}

// Writes the objects of the JSON array at p as items.
static void write_array(struct buf *json, const char *p, const char *row, int *count)
{
    const char *end = skip_value(p);
    for (const char *q = skip_space(p + 1); q < end && *q == '{'; q = skip_space(skip_value(q)))
        if (*count < 400) write_item(json, q, row, count);
}

// The home page's rows ("swimlanes") as one flat array of items with their row title.
static int home(struct buf *json)
{
    struct buf page = {0};
    long status = fetch(SITE "/", &page, 1);
    if (status != 200 || !page.data) { free(page.data); return 0; }
    static const char lane[] = "\"type\":\"swimlane\"";
    int count = 0;
    json->limit = 1024 * 1024;
    buf_str(json, "[");
    for (const char *m = page.data; (m = strstr(m, lane)); m += sizeof(lane) - 1) {
        // The row's object starts at the nearest "{"id":"module-" before its type.
        const char *start = NULL;
        for (const char *s = m; s > page.data && m - s < 4000; --s)
            if (!strncmp(s, "{\"id\":\"module-", 14)) { start = s; break; }
        if (!start) continue;
        char row[256];
        const char *items = member(start, "items");
        if (!member_string(start, "title", row, sizeof(row)) || !items || *items != '[') continue;
        write_array(json, items, row, &count);
    }
    buf_str(json, "]");
    free(page.data);
    return json->data != NULL;
}

// The programmes on a folder page: every "results" list in its data.
static int folder(const char *path, int number, const char *sort, struct buf *json)
{
    struct buf url = {0}, page = {0};
    url.limit = 1024;
    buf_str(&url, SITE);
    buf_str(&url, path);
    char join = '?';
    if (number > 1) { char more[24]; snprintf(more, sizeof(more), "?pagina=%d", number); buf_str(&url, more); join = '&'; }
    if (*sort) { char key[16]; snprintf(key, sizeof(key), "%csorteren=", join); buf_str(&url, key); buf_str(&url, sort); }
    long status = url.data ? fetch(url.data, &page, 1) : 0;
    free(url.data);
    if (status != 200 || !page.data) { free(page.data); return 0; }
    static const char results[] = "\"results\":[";
    static const char embedded[] = "\"type\":\"program-media\"";
    static const char lane[] = "\"type\":\"swimlane\"";
    int count = 0;
    json->limit = 1024 * 1024;
    buf_str(json, "[");
    // Series, broadcaster and person pages list their programmes as results.
    for (const char *m = page.data; (m = strstr(m, results)); m += sizeof(results) - 1)
        write_array(json, m + sizeof(results) - 2, "", &count);
    // Story pages embed programmes in modules, and sometimes a row of more.
    for (const char *m = page.data; (m = strstr(m, embedded)); m += sizeof(embedded) - 1) {
        const char *start = NULL;
        for (const char *s = m; s > page.data && m - s < 4000; --s)
            if (!strncmp(s, "{\"id\":\"module-", 14)) { start = s; break; }
        const char *program = start ? member(start, "program") : NULL;
        char path[320];
        if (program && *program == '{' && route_path(program, path, sizeof(path)) && count < 400)
            write_item_at(json, program, "", &count, path);
    }
    for (const char *m = page.data; (m = strstr(m, lane)); m += sizeof(lane) - 1) {
        const char *start = NULL;
        for (const char *s = m; s > page.data && m - s < 4000; --s)
            if (!strncmp(s, "{\"id\":\"module-", 14)) { start = s; break; }
        const char *items = start ? member(start, "items") : NULL;
        if (items && *items == '[') write_array(json, items, "", &count);
    }
    buf_str(json, "]");
    free(page.data);
    return json->data != NULL;
}

// ------------------------------------------------------------------ details

// The full description of a programme: the longest "description" on its page
// that begins with `starts` (the cut-off text the list data carries).
static int details(const char *path, const char *starts, struct buf *json)
{
    struct buf url = {0}, page = {0};
    url.limit = 1024;
    buf_str(&url, SITE);
    buf_str(&url, path);
    long status = url.data ? fetch(url.data, &page, 1) : 0;
    free(url.data);
    if (status != 200 || !page.data) { free(page.data); return 0; }
    static const char key[] = "\"description\":\"";
    char best[4096], text[4096]; // requests run on their own threads: no shared buffers
    best[0] = 0;
    size_t starts_len = strlen(starts);
    for (const char *m = page.data; (m = strstr(m, key)); m += sizeof(key) - 1) {
        decode_string(m + sizeof(key) - 2, text, sizeof(text));
        if (starts_len && strncmp(text, starts, starts_len)) continue;
        if (strlen(text) > strlen(best)) memcpy(best, text, strlen(text) + 1);
    }
    free(page.data);
    json->limit = 8192;
    buf_str(json, "{");
    json_member(json, "description", best, 1);
    buf_str(json, "}");
    return json->data != NULL;
}

// ------------------------------------------------------------ play session

// The selected programme's CDN access. Cookies stay in memory only. Requests
// run on their own threads: read and replace it under session_lock.
struct session { char origin[64], manifest[1024], dir[1024], cookie[4096], path[400], title[256]; int active, id; };
static struct session session;
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;


static int query_token(const char *query, const char *name, char *out, size_t cap)
{
    if (!query_raw(query, name, out, cap)) return 0;
    for (const char *c = out; *c; ++c)
        if (!isalnum((unsigned char)*c) && !strchr("-_~", *c)) return 0;
    return 1;
}

// Length of the CDN origin ("https://sk-video.cdn.beeldengeluid.nl") that `s`
// starts with, or 0. Video and audio programmes use different hosts.
static size_t cdn_prefix(const char *s)
{
    static const char *const origins[] = { CDN, CDN_AUDIO };
    for (size_t i = 0; i < sizeof(origins) / sizeof(origins[0]); ++i) {
        size_t n = strlen(origins[i]);
        if (!strncmp(s, origins[i], n) && s[n] == '/') return n;
    }
    return 0;
}

// Finds the signed manifest URL in the programme page's Next.js payload.
static char *find_signed_url(char *page)
{
    html_unescape(page);
    // Unescape JSON-style & and \/ in place.
    char *w = page;
    for (char *r = page; *r;) {
        if (!strncmp(r, "\\u0026", 6)) { *w++ = '&'; r += 6; }
        else if (!strncmp(r, "\\/", 2)) { *w++ = '/'; r += 2; }
        else *w++ = *r++;
    }
    *w = 0;
    // Flight text records carry a hex byte length: ":T<hex>,<text>".
    for (char *p = page; (p = strstr(p, ":T")); p += 2) {
        char *num = p + 2, *comma;
        unsigned long size = strtoul(num, &comma, 16);
        if (comma == num || *comma != ',' || !cdn_prefix(comma + 1)) continue;
        if (size >= 8192 || strlen(comma + 1) < size) continue;
        char *url = strndup(comma + 1, size);
        if (url && strstr(url, ".m3u8") && !strchr(url, '\n')) return url;
        free(url);
    }
    for (char *p = page; (p = strstr(p, "https://sk-")); ++p) {
        if (!cdn_prefix(p)) continue;
        size_t n = strcspn(p, " \t\r\n\"\\<>");
        char *url = strndup(p, n);
        if (url && strstr(url, ".m3u8")) return url;
        free(url);
    }
    return NULL;
}

// Resolves a programme page into a relay session. Returns an HTTP status
// (200 on success) and a short message for failures.
static int open_session(const char *path, const char *title, const char **message)
{
    struct buf url = {0}, page = {0};
    url.limit = 1024;
    buf_str(&url, SITE);
    buf_str(&url, path);
    long status = url.data ? fetch_page(url.data, &page) : 0;
    free(url.data);
    static struct session next; // large: kept off the thread's stack, built under build_lock
    static pthread_mutex_t build_lock = PTHREAD_MUTEX_INITIALIZER;
    if (status != 200 || !page.data) {
        free(page.data);
        *message = status == 404 ? "Programme not found" : "Could not reach Schatkamer";
        return 502;
    }
    pthread_mutex_lock(&build_lock);
    pthread_mutex_lock(&session_lock);
    next = session;
    pthread_mutex_unlock(&session_lock);
    char *signed_url = find_signed_url(page.data);
    free(page.data);
    if (!signed_url) { pthread_mutex_unlock(&build_lock); *message = "No playable stream (rights or sign-in required?)"; return 404; }

    char policy[2048], signature[2048], key[256];
    char *query = strchr(signed_url, '?');
    size_t origin_len = cdn_prefix(signed_url);
    const char *p = signed_url + origin_len;
    int ok = query && origin_len && origin_len < sizeof(next.origin) && (size_t)(query - p) < sizeof(next.manifest);
    if (ok) {
        snprintf(next.origin, sizeof(next.origin), "%.*s", (int)origin_len, signed_url);
        *query++ = 0;
        ok = query_token(query, "CloudFront-Policy", policy, sizeof(policy)) &&
             query_token(query, "CloudFront-Signature", signature, sizeof(signature)) &&
             query_token(query, "Key-Pair-Id", key, sizeof(key));
    }
    if (ok) {
        ok = snprintf(next.cookie, sizeof(next.cookie), "CloudFront-Policy=%s; CloudFront-Signature=%s; CloudFront-Key-Pair-Id=%s", policy, signature, key) < (int)sizeof(next.cookie);
        snprintf(next.manifest, sizeof(next.manifest), "%s", p);
        char *slash = strrchr(next.manifest, '/');
        if (slash) { snprintf(next.dir, sizeof(next.dir), "%.*s", (int)(slash - next.manifest + 1), next.manifest); }
        else ok = 0;
    }
    free(signed_url);
    if (!ok) { pthread_mutex_unlock(&build_lock); *message = "Unrecognised stream URL"; return 502; }
    snprintf(next.path, sizeof(next.path), "%s", path);
    if (title) snprintf(next.title, sizeof(next.title), "%s", title);
    next.active = 1;
    pthread_mutex_lock(&session_lock);
    next.id = session.id + 1;
    session = next;
    pthread_mutex_unlock(&session_lock);
    pthread_mutex_unlock(&build_lock);
    return 200;
}

// ------------------------------------------------------------------ relay

struct transfer { struct buf body; char content_range[128]; };

static size_t relay_header(char *data, size_t size, size_t count, void *ctx)
{
    struct transfer *t = ctx;
    size_t n = size * count;
    static const char name[] = "Content-Range:";
    if (n > sizeof(name) - 1 && n < sizeof(t->content_range) && !strncasecmp(data, name, sizeof(name) - 1)) {
        while (n && (data[n - 1] == '\r' || data[n - 1] == '\n')) --n;
        memcpy(t->content_range, data, n);
        t->content_range[n] = 0;
    }
    return size * count;
}

static int send_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        int n = (int)send(fd, p, len, 0);
        if (n <= 0) return 0;
        p += n; len -= (size_t)n;
    }
    return 1;
}

static void respond(int fd, int status, const char *type, const void *body, size_t length)
{
    const char *reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 404 ? "Not Found" : "Bad Gateway";
    char head[256];
    int n = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", status, reason, type, length);
    send_all(fd, head, (size_t)n);
    if (length) send_all(fd, body, length);
}

static void respond_text(int fd, int status, const char *text) { respond(fd, status, "text/plain; charset=utf-8", text, strlen(text)); }

// Fetches one CDN object and answers `fd`. Only paths under the session's
// directory are served.
static void relay(int fd, const char *path, const char *request)
{
    struct { char origin[64], dir[1024], cookie[4096]; int active; } s;
    pthread_mutex_lock(&session_lock);
    memcpy(s.origin, session.origin, sizeof(s.origin));
    memcpy(s.dir, session.dir, sizeof(s.dir));
    memcpy(s.cookie, session.cookie, sizeof(s.cookie));
    s.active = session.active;
    pthread_mutex_unlock(&session_lock);
    if (!s.active || strncmp(path, s.dir, strlen(s.dir)) || strstr(path, "..")) { respond_text(fd, 404, "Not found"); return; }
    for (const char *c = path; *c; ++c)
        if (!isalnum((unsigned char)*c) && !strchr("/-_.", *c)) { respond_text(fd, 404, "Not found"); return; }

    char url[2048], range_value[96] = {0};
    snprintf(url, sizeof(url), "%s%s", s.origin, path);
    const char *range = strcasestr(request, "\r\nRange: bytes=");
    if (range) {
        range = strchr(range + 2, ':') + 2;
        const char *end = strstr(range, "\r\n");
        size_t n = end ? (size_t)(end - range) : 0;
        if (n <= 6 || n >= sizeof(range_value)) { respond_text(fd, 400, "Bad range"); return; }
        for (size_t i = 6; i < n; ++i)
            if (!isdigit((unsigned char)range[i]) && !strchr("-,", range[i])) { respond_text(fd, 400, "Bad range"); return; }
        memcpy(range_value, range + 6, n - 6);
    }

    CURL *curl = new_handle();
    if (!curl) { respond_text(fd, 502, "Upstream error"); return; }
    struct transfer t = {0};
    t.body.limit = MAX_RELAY;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_COOKIE, s.cookie);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_to_buf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, relay_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
    if (range_value[0]) curl_easy_setopt(curl, CURLOPT_RANGE, range_value);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    printf("Relay: curl=%d status=%ld bytes=%zu\n", (int)rc, status, t.body.len);
    fflush(stdout);

    if (rc != CURLE_OK || (status != 200 && status != 206) || (status == 206 && !t.content_range[0])) {
        respond_text(fd, 502, "Upstream error");
    } else {
        const char *type = strstr(path, ".m3u8") ? "application/vnd.apple.mpegurl" : "application/octet-stream";
        char head[512];
        int n = snprintf(head, sizeof(head), "HTTP/1.1 %ld %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%s%sCache-Control: no-store\r\nConnection: close\r\n\r\n",
                         status, status == 206 ? "Partial Content" : "OK", type, t.body.len, t.content_range, t.content_range[0] ? "\r\n" : "");
        if (n > 0 && (size_t)n < sizeof(head) && send_all(fd, head, (size_t)n) && t.body.len) send_all(fd, t.body.data, t.body.len);
    }
    free(t.body.data);
}

// Seconds since start, for log lines that need to be put in order.
static double uptime(void)
{
    static struct timespec start;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!start.tv_sec) start = now;
    return (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
}

// ----------------------------------------------------------------- server

// The "sort" query parameter as the site's value: "nieuwste", "oudste", or "" for the default.
static const char *sort_param(const char *query)
{
    char value[16];
    if (!query_raw(query, "sort", value, sizeof(value))) return "";
    if (!strcmp(value, "newest")) return "nieuwste";
    if (!strcmp(value, "oldest")) return "oudste";
    return "";
}

// The "page" query parameter: 1 when absent, at most 1000.
static int page_param(const char *query)
{
    char value[8];
    if (!query_raw(query, "page", value, sizeof(value))) return 1;
    int page = atoi(value);
    return page < 1 ? 1 : page > 1000 ? 1000 : page;
}

static int handle(int fd)
{
    char request[4096] = {0};
    int n = (int)recv(fd, request, sizeof(request) - 1, 0);
    if (n <= 0) return 1;
    if (strncmp(request, "GET ", 4)) { respond_text(fd, 400, "GET only"); return 1; }
    char *target = request + 4, *end = strchr(target, ' ');
    if (!end) { respond_text(fd, 400, "Bad request"); return 1; }
    *end = 0;
    char *query = strchr(target, '?');
    if (query) *query++ = 0; else query = target + strlen(target);

    if (!strcmp(target, "/") || !strcmp(target, "/web")) respond(fd, 200, "text/html; charset=utf-8", webui, webui_len);
    else if (!strcmp(target, "/shaka.js")) respond(fd, 200, "application/javascript", shaka, shaka_len);
    else if (!strcmp(target, "/stop")) { respond_text(fd, 200, "Stopped"); return 0; }
    else if (!strcmp(target, "/report")) { printf("UI: %s\n", query); fflush(stdout); respond(fd, 200, "text/plain", "", 0); }
    else if (!strcmp(target, "/api/search")) {
        char raw[600];
        struct buf json = {0};
        if (!query_raw(query, "q", raw, sizeof(raw))) { respond_text(fd, 400, "Missing query"); return 1; }
        url_decode(raw);
        if (search(raw, page_param(query), sort_param(query), &json)) respond(fd, 200, "application/json", json.data, json.len);
        else respond_text(fd, 502, "Search failed");
        free(json.data);
    } else if (!strcmp(target, "/api/home")) {
        struct buf json = {0};
        if (home(&json)) respond(fd, 200, "application/json", json.data, json.len);
        else respond_text(fd, 502, "Home unavailable");
        free(json.data);
    } else if (!strcmp(target, "/api/details")) {
        char path[400], starts[200];
        struct buf json = {0};
        if (!query_raw(query, "path", path, sizeof(path))) { respond_text(fd, 400, "Missing path"); return 1; }
        url_decode(path);
        if (!valid_programme_path(path)) { respond_text(fd, 400, "Invalid programme path"); return 1; }
        if (query_raw(query, "starts", starts, sizeof(starts))) url_decode(starts);
        else starts[0] = 0;
        if (details(path, starts, &json)) respond(fd, 200, "application/json", json.data, json.len);
        else respond_text(fd, 502, "Details unavailable");
        free(json.data);
    } else if (!strcmp(target, "/api/folder")) {
        char path[400];
        struct buf json = {0};
        if (!query_raw(query, "path", path, sizeof(path))) { respond_text(fd, 400, "Missing path"); return 1; }
        url_decode(path);
        if (!valid_folder_path(path)) { respond_text(fd, 400, "Invalid folder"); return 1; }
        if (folder(path, page_param(query), sort_param(query), &json)) respond(fd, 200, "application/json", json.data, json.len);
        else respond_text(fd, 502, "Folder unavailable");
        free(json.data);
    } else if (!strcmp(target, "/api/current")) {
        struct buf json = {0};
        char id[16], manifest[1100];
        json.limit = 4096;
        pthread_mutex_lock(&session_lock);
        int active = session.active;
        if (active) {
            snprintf(id, sizeof(id), "%d", session.id);
            snprintf(manifest, sizeof(manifest), "http://localhost:%d/relay%s", PORT, session.manifest);
            buf_str(&json, "{\"id\":");
            buf_str(&json, id);
            buf_str(&json, ",\"manifest\":");
            buf_json(&json, manifest);
            buf_str(&json, ",\"title\":");
            buf_json(&json, session.title);
            buf_str(&json, "}");
        }
        pthread_mutex_unlock(&session_lock);
        if (!active) respond_text(fd, 404, "No programme selected");
        else if (json.data) respond(fd, 200, "application/json", json.data, json.len);
        else respond_text(fd, 502, "Out of memory");
        free(json.data);
    } else if (!strcmp(target, "/api/refresh")) {
        // Signed CDN cookies expire: sign the same programme again, keeping its id
        // so the player continues instead of starting over.
        const char *message = "";
        char path[400];
        pthread_mutex_lock(&session_lock);
        int id = session.id;
        snprintf(path, sizeof(path), "%s", session.path);
        pthread_mutex_unlock(&session_lock);
        int status = path[0] ? open_session(path, NULL, &message) : 404;
        char body[1100];
        pthread_mutex_lock(&session_lock);
        if (status == 200) session.id = id;
        int len = snprintf(body, sizeof(body), "{\"manifest\":\"http://localhost:%d/relay%s\"}", PORT, session.manifest);
        pthread_mutex_unlock(&session_lock);
        if (status != 200) respond_text(fd, status, path[0] ? message : "No programme selected");
        else respond(fd, 200, "application/json", body, (size_t)len);
    } else if (!strcmp(target, "/api/play")) {
        char path[400], title[256];
        const char *message = "";
        if (!query_raw(query, "path", path, sizeof(path))) { respond_text(fd, 400, "Missing path"); return 1; }
        url_decode(path);
        if (!valid_programme_path(path)) { respond_text(fd, 400, "Invalid programme path"); return 1; }
        if (query_raw(query, "title", title, sizeof(title))) url_decode(title);
        else title[0] = 0;
        printf("[%7.2f] play requested\n", uptime());
        fflush(stdout);
        int status = open_session(path, title, &message);
        printf("[%7.2f] play resolved -> %d\n", uptime(), status);
        fflush(stdout);
        if (status != 200) { respond_text(fd, status, message); return 1; }
        respond_text(fd, 200, "Ready");    } else if (!strncmp(target, "/relay/", 7)) relay(fd, target + 6, request);
    else respond_text(fd, 404, "Not found");
    return 1;
}

static void *serve_connection(void *arg)
{
    int fd = (int)(intptr_t)arg;
    int keep_running = handle(fd);
    close(fd);
    if (!keep_running) {
        // A newer instance asked for the port.
        fflush(stdout);
        exit(0);
    }
    return NULL;
}

// Asks an earlier instance to exit so the port can be reused.
static void stop_previous(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd >= 0 && !connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        const char req[] = "GET /stop HTTP/1.1\r\n\r\n";
        send(fd, req, sizeof(req) - 1, 0);
        char junk[64];
        recv(fd, junk, sizeof(junk), 0);
    }
    if (fd >= 0) close(fd);
    sleep(1);
}

static int listen_loopback(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 8)) { close(fd); return -1; }
    return fd;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
    install_player();
    int server = listen_loopback();
    if (server < 0) { stop_previous(); server = listen_loopback(); }
    if (server < 0) { perror("listen"); return 1; }
    printf("Schatkamer ready on 127.0.0.1:%d\n", PORT);
    fflush(stdout);
    // One thread per connection: a client that stops reading (a suspended
    // player mid-segment) must not hold up anyone else.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    for (;;) {
        int fd = accept(server, NULL, NULL);
        if (fd < 0) continue;
        // Reads and writes give up after a while, so a stalled client only costs its own thread.
        struct timeval receive = {5, 0}, send_limit = {20, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive, sizeof(receive));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_limit, sizeof(send_limit));
        pthread_t thread;
        if (pthread_create(&thread, &attr, serve_connection, (void *)(intptr_t)fd)) close(fd);
    }
}
