#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "build.h"

#define BUILD_FILE_MAX (256u * 1024u)

typedef struct {
    char entrypoint[BUILD_MAX_ENTRY];
    bool has_entrypoint;
    char language[64];
    BuildPart parts[BUILD_MAX_PARTS];
    int nparts;
} BuildDoc;

static void build_path(const char *disk_root, char *out, size_t n) {
    snprintf(out, n, "%s/build.json", disk_root);
}

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_reg(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static char *read_whole(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > BUILD_FILE_MAX) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    if (len_out) *len_out = got;
    return buf;
}

typedef struct {
    const char *p;
    const char *end;
} Cur;

static void skip_ws(Cur *c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' ||
                             *c->p == '\n' || *c->p == '\r'))
        c->p++;
}

static bool parse_string(Cur *c, char *out, size_t n) {
    if (c->p >= c->end || *c->p != '"') return false;
    c->p++;
    size_t w = 0;
    while (c->p < c->end) {
        char ch = *c->p++;
        if (ch == '"') {
            if (w >= n) return false;
            out[w] = 0;
            return true;
        }
        if (ch == '\\') {
            if (c->p >= c->end) return false;
            char e = *c->p++;
            switch (e) {
            case '"': ch = '"'; break;
            case '\\': ch = '\\'; break;
            case '/': ch = '/'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'u':
                for (int i = 0; i < 4; i++) {
                    if (c->p >= c->end || !isxdigit((unsigned char)*c->p))
                        return false;
                    c->p++;
                }
                ch = '?';
                break;
            default: return false;
            }
        }
        if (w + 1 >= n) return false;
        out[w++] = ch;
    }
    return false;
}

static bool parse_bool(Cur *c, bool *out) {
    if ((size_t)(c->end - c->p) >= 4 && memcmp(c->p, "true", 4) == 0) {
        c->p += 4;
        *out = true;
        return true;
    }
    if ((size_t)(c->end - c->p) >= 5 && memcmp(c->p, "false", 5) == 0) {
        c->p += 5;
        *out = false;
        return true;
    }
    return false;
}

static bool parse_number(Cur *c, long *out) {
    const char *s = c->p;
    bool neg = false;
    if (s < c->end && (*s == '-' || *s == '+')) {
        neg = *s == '-';
        s++;
    }
    if (s >= c->end || !isdigit((unsigned char)*s)) return false;
    long v = 0;
    while (s < c->end && isdigit((unsigned char)*s))
        v = v * 10 + (*s++ - '0');
    c->p = s;
    *out = neg ? -v : v;
    return true;
}

static bool skip_string(Cur *c) {
    if (c->p >= c->end || *c->p != '"') return false;
    c->p++;
    while (c->p < c->end) {
        char ch = *c->p++;
        if (ch == '"') return true;
        if (ch == '\\') {
            if (c->p >= c->end) return false;
            if (*c->p == 'u') {
                c->p++;
                for (int i = 0; i < 4; i++) {
                    if (c->p >= c->end ||
                        !isxdigit((unsigned char)*c->p))
                        return false;
                    c->p++;
                }
            } else {
                c->p++;
            }
        }
    }
    return false;
}

static bool skip_value(Cur *c) {
    skip_ws(c);
    if (c->p >= c->end) return false;
    if (*c->p == '"') return skip_string(c);
    if (*c->p == '{') {
        c->p++;
        skip_ws(c);
        if (c->p < c->end && *c->p == '}') { c->p++; return true; }
        for (;;) {
            skip_ws(c);
            char key[256];
            if (!parse_string(c, key, sizeof(key))) return false;
            (void)key;
            skip_ws(c);
            if (c->p >= c->end || *c->p != ':') return false;
            c->p++;
            if (!skip_value(c)) return false;
            skip_ws(c);
            if (c->p >= c->end) return false;
            if (*c->p == ',') { c->p++; continue; }
            if (*c->p == '}') { c->p++; return true; }
            return false;
        }
    }
    if (*c->p == '[') {
        c->p++;
        skip_ws(c);
        if (c->p < c->end && *c->p == ']') { c->p++; return true; }
        for (;;) {
            if (!skip_value(c)) return false;
            skip_ws(c);
            if (c->p >= c->end) return false;
            if (*c->p == ',') { c->p++; continue; }
            if (*c->p == ']') { c->p++; return true; }
            return false;
        }
    }
    bool b;
    long num;
    if (parse_bool(c, &b)) return true;
    if (parse_number(c, &num)) return true;
    if ((size_t)(c->end - c->p) >= 4 && memcmp(c->p, "null", 4) == 0) {
        c->p += 4;
        return true;
    }
    return false;
}

static bool valid_part_name(const char *s) {
    if (!s || !*s) return false;
    for (const char *c = s; *c; c++) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
            (*c >= '0' && *c <= '9') || *c == '_' || *c == '-')
            continue;
        return false;
    }
    return true;
}

static bool parse_partition_body(Cur *c, BuildPart *part) {
    memset(part, 0, sizeof(*part));
    skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        c->p++;
        return part->path[0] != 0;
    }
    for (;;) {
        skip_ws(c);
        char key[64];
        if (!parse_string(c, key, sizeof(key))) return false;
        skip_ws(c);
        if (c->p >= c->end || *c->p != ':') return false;
        c->p++;
        skip_ws(c);
        if (strcmp(key, "path") == 0) {
            char v[128];
            if (!parse_string(c, v, sizeof(v))) return false;
            if (!valid_part_name(v)) return false;
            snprintf(part->path, sizeof(part->path), "%s", v);
        } else if (strcmp(key, "readonly") == 0) {
            if (!parse_bool(c, &part->readonly)) return false;
        } else if (strcmp(key, "hidden") == 0) {
            if (!parse_bool(c, &part->hidden)) return false;
        } else if (strcmp(key, "source") == 0) {
            long v;
            if (!parse_number(c, &v)) {
                if (!skip_value(c)) return false;
            } else {
                part->source = (int)v;
            }
        } else {
            if (!skip_value(c)) return false;
        }
        skip_ws(c);
        if (c->p >= c->end) return false;
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == '}') { c->p++; return part->path[0] != 0; }
        return false;
    }
}

static bool parse_doc(const char *text, size_t len, BuildDoc *doc) {
    memset(doc, 0, sizeof(*doc));
    snprintf(doc->language, sizeof(doc->language), "Lua");
    Cur c = { text, text + len };
    skip_ws(&c);
    if (c.p >= c.end || *c.p != '{') return false;
    c.p++;
    skip_ws(&c);
    if (c.p < c.end && *c.p == '}') { c.p++; return true; }
    for (;;) {
        skip_ws(&c);
        char key[64];
        if (!parse_string(&c, key, sizeof(key))) return false;
        skip_ws(&c);
        if (c.p >= c.end || *c.p != ':') return false;
        c.p++;
        skip_ws(&c);
        if (strcmp(key, "entrypoint") == 0) {
            char v[BUILD_MAX_ENTRY];
            if (c.p < c.end && *c.p == '"') {
                if (!parse_string(&c, v, sizeof(v))) return false;
                snprintf(doc->entrypoint, sizeof(doc->entrypoint), "%s", v);
                doc->has_entrypoint = true;
            } else if ((size_t)(c.end - c.p) >= 4 &&
                       memcmp(c.p, "null", 4) == 0) {
                c.p += 4;
            } else {
                return false;
            }
        } else if (strcmp(key, "language") == 0) {
            char v[64];
            if (!parse_string(&c, v, sizeof(v))) return false;
            snprintf(doc->language, sizeof(doc->language), "%s", v);
        } else if (strcmp(key, "partitions") == 0) {
            if (c.p >= c.end || *c.p != '[') return false;
            c.p++;
            skip_ws(&c);
            if (c.p < c.end && *c.p == ']') { c.p++; }
            else {
                for (;;) {
                    skip_ws(&c);
                    if (c.p >= c.end || *c.p != '{') return false;
                    c.p++;
                    if (doc->nparts < BUILD_MAX_PARTS) {
                        if (!parse_partition_body(
                                &c, &doc->parts[doc->nparts]))
                            return false;
                        doc->nparts++;
                    } else {
                        BuildPart scratch;
                        if (!parse_partition_body(&c, &scratch))
                            return false;
                    }
                    skip_ws(&c);
                    if (c.p >= c.end) return false;
                    if (*c.p == ',') { c.p++; continue; }
                    if (*c.p == ']') { c.p++; break; }
                    return false;
                }
            }
        } else {
            if (!skip_value(&c)) return false;
        }
        skip_ws(&c);
        if (c.p >= c.end) return false;
        if (*c.p == ',') { c.p++; continue; }
        if (*c.p == '}') { c.p++; return true; }
        return false;
    }
}

static bool load_doc(const char *disk_root, BuildDoc *doc, bool *present) {
    char path[4096];
    build_path(disk_root, path, sizeof(path));
    size_t len = 0;
    char *text = read_whole(path, &len);
    if (!text) {
        if (present) *present = false;
        return false;
    }
    if (present) *present = true;
    bool ok = parse_doc(text, len, doc);
    free(text);
    return ok;
}

bool build_entry_to_host(const char *disk_root, const char *entry,
                         char *out, size_t n) {
    if (!disk_root || !entry || !out || n == 0) return false;
    const char *colon = strchr(entry, ':');
    if (!colon || colon == entry) return false;
    if (strchr(colon + 1, ':') != NULL) return false;
    size_t plen = (size_t)(colon - entry);
    if (plen == 0 || plen >= 128) return false;
    char part[128];
    memcpy(part, entry, plen);
    part[plen] = 0;
    if (!valid_part_name(part)) return false;

    char clean[1024];
    size_t w = 0;
    bool last_slash = true;
    for (const char *s = colon + 1; *s && w + 1 < sizeof(clean); s++) {
        char ch = *s == '\\' ? '/' : *s;
        if (ch == '/') {
            if (last_slash) continue;
            last_slash = true;
            clean[w++] = '/';
        } else {
            last_slash = false;
            clean[w++] = ch;
        }
    }
    clean[w] = 0;
    if (clean[0] == 0) return false;
    if (strstr(clean, "..") != NULL) return false;

    int need = snprintf(out, n, "%s/%s/%s", disk_root, part, clean);
    return need > 0 && (size_t)need < n;
}

static bool part_listed(const BuildDoc *doc, const char *part) {
    for (int i = 0; i < doc->nparts; i++)
        if (strcmp(doc->parts[i].path, part) == 0) return true;
    return false;
}

bool build_entry_valid(const char *disk_root, const char *entry) {
    char part[128];
    const char *colon = entry ? strchr(entry, ':') : NULL;
    if (!colon || colon == entry) return false;
    size_t plen = (size_t)(colon - entry);
    if (plen == 0 || plen >= sizeof(part)) return false;
    memcpy(part, entry, plen);
    part[plen] = 0;
    if (!valid_part_name(part)) return false;

    char host[4096];
    if (!build_entry_to_host(disk_root, entry, host, sizeof(host)))
        return false;

    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/%s", disk_root, part);
    bool dir_ok = is_dir(dir);
    if (!dir_ok) {
        BuildDoc doc;
        bool present = false;
        if (!load_doc(disk_root, &doc, &present) || !present)
            return false;
        if (!part_listed(&doc, part)) return false;
    }
    return is_reg(host);
}

bool build_get_boot(const char *disk_root, char *out, size_t n) {
    BuildDoc doc;
    bool present = false;
    if (!load_doc(disk_root, &doc, &present) || !present)
        return false;
    if (!doc.has_entrypoint || !doc.entrypoint[0]) return false;
    if (strlen(doc.entrypoint) + 1 > n) return false;
    snprintf(out, n, "%s", doc.entrypoint);
    return true;
}

bool build_get_parts(const char *disk_root, BuildPart *parts, int *nparts) {
    if (!nparts || *nparts <= 0) return false;
    BuildDoc doc;
    bool present = false;
    if (!load_doc(disk_root, &doc, &present) || !present) return false;
    int want = *nparts < doc.nparts ? *nparts : doc.nparts;
    for (int i = 0; i < want; i++) parts[i] = doc.parts[i];
    *nparts = want;
    return true;
}

bool build_same_path(const char *a, const char *b) {
    char na[1024], nb[1024];
    size_t wa = 0, wb = 0;
    bool last = true;
    for (const char *s = a ? a : ""; *s && wa + 1 < sizeof(na); s++) {
        char ch = *s == '\\' ? '/' : *s;
        if (ch == '/') {
            if (last) continue;
            last = true;
            na[wa++] = '/';
        } else {
            last = false;
            na[wa++] = ch;
        }
    }
    na[wa] = 0;
    last = true;
    for (const char *s = b ? b : ""; *s && wb + 1 < sizeof(nb); s++) {
        char ch = *s == '\\' ? '/' : *s;
        if (ch == '/') {
            if (last) continue;
            last = true;
            nb[wb++] = '/';
        } else {
            last = false;
            nb[wb++] = ch;
        }
    }
    nb[wb] = 0;
    const char *pa = (na[0] == '/') ? na + 1 : na;
    const char *pb = (nb[0] == '/') ? nb + 1 : nb;
    const char *ca = strchr(pa, ':');
    const char *cb = strchr(pb, ':');
    if (!ca || !cb) return strcmp(pa, pb) == 0;
    if ((size_t)(ca - pa) != (size_t)(cb - pb)) return false;
    for (const char *x = pa, *y = pb; x < ca; x++, y++)
        if (tolower((unsigned char)*x) != tolower((unsigned char)*y))
            return false;
    const char *ra = (*ca == ':') ? ca + 1 : ca;
    const char *rb = (*cb == ':') ? cb + 1 : cb;
    while (*ra == '/') ra++;
    while (*rb == '/') rb++;
    return strcmp(ra, rb) == 0;
}

static void json_escape(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (*p < 0x20) fprintf(f, "\\u%04x", *p);
            else fputc(*p, f);
        }
    }
    fputc('"', f);
}

static int part_cmp(const void *a, const void *b) {
    return strcmp(((const BuildPart *)a)->path, ((const BuildPart *)b)->path);
}

static void synth_parts_from_disk(const char *disk_root, BuildDoc *doc) {
    doc->nparts = 0;
    DIR *d = opendir(disk_root);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (!valid_part_name(de->d_name)) continue;
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", disk_root, de->d_name);
        if (!is_dir(full)) continue;
        if (doc->nparts >= BUILD_MAX_PARTS) break;
        BuildPart *p = &doc->parts[doc->nparts++];
        snprintf(p->path, sizeof(p->path), "%s", de->d_name);
        p->readonly = false;
        p->hidden = false;
        p->source = 0;
    }
    closedir(d);
    qsort(doc->parts, (size_t)doc->nparts, sizeof(doc->parts[0]), part_cmp);
}

static bool write_doc(const char *disk_root, const BuildDoc *doc) {
    char path[4096], tmp[4112];
    build_path(disk_root, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fputs("{\n", f);
    if (doc->has_entrypoint && doc->entrypoint[0]) {
        fputs("    \"entrypoint\" : ", f);
        json_escape(f, doc->entrypoint);
        fputs(",\n", f);
        fputs("    \"language\" : ", f);
        json_escape(f, doc->language[0] ? doc->language : "Lua");
        fputs(",\n", f);
    }
    fputs("    \"partitions\" : [", f);
    for (int i = 0; i < doc->nparts; i++) {
        const BuildPart *p = &doc->parts[i];
        fputs(i == 0 ? "\n" : ",\n", f);
        fputs("        {\n", f);
        fputs("            \"path\" : ", f);
        json_escape(f, p->path);
        if (p->source != 0) {
            fprintf(f, ",\n            \"source\" : %d", p->source);
        }
        fprintf(f, ",\n            \"readonly\" : %s",
                p->readonly ? "true" : "false");
        fprintf(f, ",\n            \"hidden\" : %s",
                p->hidden ? "true" : "false");
        fputs("\n        }", f);
    }
    fputs(doc->nparts > 0 ? "\n    ]\n}\n" : "]\n}\n", f);
    if (fclose(f) != 0) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

bool build_set_boot(const char *disk_root, const char *entry) {
    if (!disk_root || !disk_root[0]) return false;
    if (entry && !build_entry_valid(disk_root, entry)) return false;

    BuildDoc doc;
    bool present = false;
    if (load_doc(disk_root, &doc, &present) && present) {
    } else if (present) {
        return false;
    } else {
        memset(&doc, 0, sizeof(doc));
        snprintf(doc.language, sizeof(doc.language), "Lua");
        synth_parts_from_disk(disk_root, &doc);
    }
    if (!entry) {
        return false;
    }
    snprintf(doc.entrypoint, sizeof(doc.entrypoint), "%s", entry);
    doc.has_entrypoint = true;
    if (!doc.language[0])
        snprintf(doc.language, sizeof(doc.language), "Lua");
    return write_doc(disk_root, &doc);
}

bool build_add_partition(const char *disk_root, const char *name) {
    if (!valid_part_name(name)) return false;
    BuildDoc doc;
    bool present = false;
    if (!load_doc(disk_root, &doc, &present) || !present) return true;
    for (int i = 0; i < doc.nparts; i++)
        if (strcmp(doc.parts[i].path, name) == 0) return true;
    if (doc.nparts >= BUILD_MAX_PARTS) return false;
    BuildPart *p = &doc.parts[doc.nparts++];
    snprintf(p->path, sizeof(p->path), "%s", name);
    p->readonly = false;
    p->hidden = false;
    p->source = 0;
    return write_doc(disk_root, &doc);
}

bool build_remove_partition(const char *disk_root, const char *name) {
    if (!name) return false;
    BuildDoc doc;
    bool present = false;
    if (!load_doc(disk_root, &doc, &present) || !present) return true;
    int w = 0;
    for (int i = 0; i < doc.nparts; i++) {
        if (strcmp(doc.parts[i].path, name) == 0) continue;
        if (w != i) doc.parts[w] = doc.parts[i];
        w++;
    }
    if (w == doc.nparts) return true;
    doc.nparts = w;
    return write_doc(disk_root, &doc);
}
