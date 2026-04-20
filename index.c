// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>

// Forward declaration from object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

static int compare_index_entries(const void *a, const void *b) {
    const IndexEntry *ea = (const IndexEntry *)a;
    const IndexEntry *eb = (const IndexEntry *)b;
    return strcmp(ea->path, eb->path);
}

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }

            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) {
        if (errno == ENOENT) return 0;
        return -1;
    }

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fclose(f);
            return -1;
        }

        IndexEntry *e = &index->entries[index->count];
        char hash_hex[HASH_HEX_SIZE + 1];
        hash_hex[0] = '\0';

        uint32_t mode;
        uint64_t mtime;
        uint32_t size;
        char path[512];

        int n = sscanf(line, "%o %64s %" SCNu64 " %u %511[^\n]",
                       &mode, hash_hex, &mtime, &size, path);
        if (n != 5) {
            fclose(f);
            return -1;
        }
        if (hex_to_hash(hash_hex, &e->hash) != 0) {
            fclose(f);
            return -1;
        }

        e->mode = mode;
        e->mtime_sec = mtime;
        e->size = size;
        snprintf(e->path, sizeof(e->path), "%s", path);
        index->count++;
    }

    fclose(f);
    return 0;
}

int index_save(const Index *index) {
    Index *sorted = malloc(sizeof(Index));
    if (!sorted) return -1;
    *sorted = *index;
    qsort(sorted->entries, sorted->count, sizeof(IndexEntry), compare_index_entries);

    const char *tmp_path = INDEX_FILE ".tmp";
    FILE *f = fopen(tmp_path, "w");
    if (!f) { free(sorted); return -1; }

    for (int i = 0; i < sorted->count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted->entries[i].hash, hex);
        fprintf(f, "%o %s %" PRIu64 " %u %s\n",
                sorted->entries[i].mode,
                hex,
                sorted->entries[i].mtime_sec,
                sorted->entries[i].size,
                sorted->entries[i].path);
    }

    fflush(f);
    if (fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp_path);
        free(sorted);
        return -1;
    }
    fclose(f);
    free(sorted);

    if (rename(tmp_path, INDEX_FILE) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

int index_add(Index *index, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    size_t file_size = (size_t)st.st_size;
    void *buf = malloc(file_size > 0 ? file_size : 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    if (file_size > 0 && fread(buf, 1, file_size, f) != file_size) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    ObjectID blob;
    if (object_write(OBJ_BLOB, buf, file_size, &blob) != 0) {
        free(buf);
        return -1;
    }
    free(buf);

    IndexEntry *entry = index_find(index, path);
    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        entry = &index->entries[index->count++];
    }

    entry->mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    entry->hash = blob;
    entry->mtime_sec = (uint64_t)st.st_mtime;
    entry->size = (uint32_t)st.st_size;
    snprintf(entry->path, sizeof(entry->path), "%s", path);

    return index_save(index);
}
