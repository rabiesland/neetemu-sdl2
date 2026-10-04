#ifndef NEETEMU_BUILD_H
#define NEETEMU_BUILD_H

#include <stdbool.h>
#include <stddef.h>

#define BUILD_MAX_ENTRY 1024
#define BUILD_MAX_PARTS 128

typedef struct {
    char path[128];
    bool readonly;
    bool hidden;
    int source;
} BuildPart;

bool build_get_boot(const char *disk_root, char *out, size_t n);

bool build_get_parts(const char *disk_root, BuildPart *parts, int *nparts);

bool build_entry_to_host(const char *disk_root, const char *entry,
                         char *out, size_t n);

bool build_entry_valid(const char *disk_root, const char *entry);

bool build_set_boot(const char *disk_root, const char *entry);

bool build_add_partition(const char *disk_root, const char *name);
bool build_remove_partition(const char *disk_root, const char *name);

bool build_same_path(const char *a, const char *b);

#endif
