// object.c — Content-addressable object store v2
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // Step 1: Build the header string
    const char *type_str;
    if (type == OBJ_BLOB)        type_str = "blob";
    else if (type == OBJ_TREE)   type_str = "tree";
    else if (type == OBJ_COMMIT) type_str = "commit";
    else return -1;

    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    // header_len does NOT include the null terminator, but we want to store it
    // The format is: "<type> <size>\0<data>"
    size_t full_len = (size_t)header_len + 1 + len; // +1 for the '\0'

    // Step 2: Allocate and build the full object (header + \0 + data)
    uint8_t *full = malloc(full_len);
    if (!full) return -1;
    memcpy(full, header, (size_t)header_len);
    full[header_len] = '\0';
    memcpy(full + header_len + 1, data, len);

    // Step 3: Compute SHA-256 of the full object
    ObjectID id;
    compute_hash(full, full_len, &id);
    if (id_out) *id_out = id;

    // Step 4: Deduplication — if object already exists, skip writing
    if (object_exists(&id)) {
        free(full);
        return 0;
    }

    // Step 5: Create shard directory (.pes/objects/XX/)
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&id, hex);

    char shard_dir[256];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(shard_dir, 0755); // Ignore error if already exists

    // Step 6: Write to a temp file in the shard directory
    char final_path[512];
    object_path(&id, final_path, sizeof(final_path));

    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(full);
        return -1;
    }

    size_t written = 0;
    while (written < full_len) {
        ssize_t n = write(fd, full + written, full_len - written);
        if (n < 0) {
            close(fd);
            free(full);
            return -1;
        }
        written += (size_t)n;
    }

    // Step 7: fsync the temp file to ensure data reaches disk
    fsync(fd);
    close(fd);
    free(full);

    // Step 8: Atomically rename temp file to final path
    if (rename(tmp_path, final_path) != 0) return -1;

    // Step 9: fsync the shard directory to persist the rename
    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    return 0;
}

// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // Step 1: Get the file path
    char path[512];
    object_path(id, path, sizeof(path));

    // Step 2: Open and read the entire file
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0) { fclose(f); return -1; }

    uint8_t *buf = malloc((size_t)file_size);
    if (!buf) { fclose(f); return -1; }

    if (fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);

    // Step 3: Verify integrity — recompute hash and compare to filename
    ObjectID computed;
    compute_hash(buf, (size_t)file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buf);
        return -1; // Corrupted object
    }

    // Step 4: Parse the header — find the '\0' separating header from data
    uint8_t *null_byte = memchr(buf, '\0', (size_t)file_size);
    if (!null_byte) { free(buf); return -1; }

    // Step 5: Parse type string
    if (strncmp((char *)buf, "blob ", 5) == 0)        *type_out = OBJ_BLOB;
    else if (strncmp((char *)buf, "tree ", 5) == 0)   *type_out = OBJ_TREE;
    else if (strncmp((char *)buf, "commit ", 7) == 0) *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }

    // Step 6: Parse size from header
    char *space = memchr(buf, ' ', null_byte - buf);
    if (!space) { free(buf); return -1; }
    size_t data_len = (size_t)strtoul(space + 1, NULL, 10);

    // Step 7: Allocate output buffer and copy data portion
    uint8_t *data_start = null_byte + 1;
    *data_out = malloc(data_len + 1); // +1 for safe null terminator
    if (!*data_out) { free(buf); return -1; }
    memcpy(*data_out, data_start, data_len);
    ((uint8_t *)*data_out)[data_len] = '\0';
    *len_out = data_len;

    free(buf);
    return 0;
}
