// index.c — Staging area implementation
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// Forward declaration
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++)
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
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
    int sc = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        sc++;
    }
    if (sc == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int uc = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            uc++;
        } else if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                   st.st_size  != (off_t)index->entries[i].size) {
            printf("  modified:   %s\n", index->entries[i].path);
            uc++;
        }
    }
    if (uc == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int tc = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o")) continue;
            int tracked = 0;
            for (int i = 0; i < index->count; i++)
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    tracked = 1;
                    break;
                }
            if (!tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    tc++;
                }
            }
        }
        closedir(dir);
    }
    if (tc == 0) printf("  (nothing to show)\n");
    printf("\n");
    return 0;
}

// ─── TODO (implemented) ──────────────────────────────────────────────────────

static int cmp_path(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0; // no index file yet is fine

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        if (index->count >= MAX_INDEX_ENTRIES) { fclose(f); return -1; }

        IndexEntry *e = &index->entries[index->count];
        char hex[HASH_HEX_SIZE + 1];
        unsigned long long mtime, size;

        if (sscanf(line, "%o %64s %llu %llu %511s",
                   &e->mode, hex, &mtime, &size, e->path) != 5) {
            fclose(f);
            return -1;
        }
        if (hex_to_hash(hex, &e->hash) != 0) { fclose(f); return -1; }
        e->mtime_sec = (uint64_t)mtime;
        e->size      = (uint64_t)size;
        index->count++;
    }
    fclose(f);
    return 0;
}

int index_save(const Index *index) {
    Index sorted = *index;
    qsort(sorted.entries, (size_t)sorted.count, sizeof(IndexEntry), cmp_path);

    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", INDEX_FILE);

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

    for (int i = 0; i < sorted.count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted.entries[i].hash, hex);
        fprintf(f, "%o %s %llu %llu %s\n",
                sorted.entries[i].mode,
                hex,
                (unsigned long long)sorted.entries[i].mtime_sec,
                (unsigned long long)sorted.entries[i].size,
                sorted.entries[i].path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return rename(tmp, INDEX_FILE);
}

int index_add(Index *index, const char *path) {
    // Read file contents
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz < 0) { fclose(f); return -1; }

    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }

    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    if ((long)nread != sz) { free(buf); return -1; }

    // Write blob to object store
    ObjectID bid;
    if (object_write(OBJ_BLOB, buf, (size_t)sz, &bid) != 0) {
        free(buf);
        return -1;
    }
    free(buf);

    // Get file metadata
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    uint32_t mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;

    // Update or insert index entry
    IndexEntry *e = index_find(index, path);
    if (e) {
        e->hash      = bid;
        e->mtime_sec = (uint64_t)st.st_mtime;
        e->size      = (uint64_t)st.st_size;
        e->mode      = mode;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        e = &index->entries[index->count++];
        e->hash      = bid;
        e->mtime_sec = (uint64_t)st.st_mtime;
        e->size      = (uint64_t)st.st_size;
        e->mode      = mode;
        snprintf(e->path, sizeof(e->path), "%s", path);
    }

    return index_save(index);
}
