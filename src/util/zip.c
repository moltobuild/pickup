#include <pickup/util/zip.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The signature every local file header opens with: "PK\3\4". */
#define LOCAL_HEADER_SIGNATURE 0x04034b50u

/* Fixed part of a local file header, after the signature. */
#define LOCAL_HEADER_REST 26

/* Where each field sits inside that fixed part. */
#define FIELD_FLAGS        2
#define FIELD_METHOD       4
#define FIELD_COMPRESSED  14
#define FIELD_UNCOMPRESSED 18
#define FIELD_NAME_LENGTH 22
#define FIELD_EXTRA_LENGTH 24

/* The only compression method understood: none. */
#define METHOD_STORED 0

/* Set when the sizes are not in the header but in a descriptor after the data,
   which would make a forward walk impossible. Conda does not write these. */
#define FLAG_DATA_DESCRIPTOR 0x0008

/* A 32-bit size of all-ones means the real one is in the zip64 extra field. */
#define SIZE_IN_ZIP64 0xFFFFFFFFu

/* The extra field that carries 64-bit sizes, and its smallest useful length:
   uncompressed and compressed, eight bytes each. */
#define EXTRA_ID_ZIP64 0x0001
#define EXTRA_ZIP64_MIN 16

/* Largest extra field read. The format allows 64 KB; conda writes 20 bytes. */
#define EXTRA_MAX 4096

/* How much is copied at a time. */
#define COPY_CHUNK 65536

/* Little-endian readers. The format is little-endian regardless of the host,
   so the bytes are assembled rather than cast. */
static uint16_t read_u16(const unsigned char *bytes) {
    return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static uint32_t read_u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8)
         | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t read_u64(const unsigned char *bytes) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--)
        value = (value << 8) | bytes[i];
    return value;
}

/* Find the compressed size in a zip64 extra field, when there is one.

   The field packs uncompressed size first, then compressed. Both are present
   in a local header, which is what makes reading the second one safe here. */
static bool zip64_compressed_size(const unsigned char *extra, size_t length,
                                  uint64_t *out) {
    size_t offset = 0;
    while (offset + 4 <= length) {
        uint16_t id = read_u16(extra + offset);
        uint16_t size = read_u16(extra + offset + 2);
        if (offset + 4 + size > length)
            return false;

        if (id == EXTRA_ID_ZIP64 && size >= EXTRA_ZIP64_MIN) {
            *out = read_u64(extra + offset + 4 + 8);
            return true;
        }
        offset += 4 + size;
    }
    return false;
}

/* Read one local file header, leaving the file positioned at the member's
   data. Returns false at the end of the entries or on anything unreadable. */
static bool read_header(FILE *file, zip_member *out) {
    unsigned char signature[4];
    if (fread(signature, 1, sizeof signature, file) != sizeof signature)
        return false;
    if (read_u32(signature) != LOCAL_HEADER_SIGNATURE)
        return false; /* the central directory, which ends the walk */

    unsigned char header[LOCAL_HEADER_REST];
    if (fread(header, 1, sizeof header, file) != sizeof header)
        return false;

    /* Sizes after the data cannot be walked forward, and nothing that writes
       conda packages produces them. Refused rather than guessed at. */
    if ((read_u16(header + FIELD_FLAGS) & FLAG_DATA_DESCRIPTOR) != 0)
        return false;
    if (read_u16(header + FIELD_METHOD) != METHOD_STORED)
        return false;

    uint32_t compressed = read_u32(header + FIELD_COMPRESSED);
    uint16_t name_length = read_u16(header + FIELD_NAME_LENGTH);
    uint16_t extra_length = read_u16(header + FIELD_EXTRA_LENGTH);
    if (name_length == 0 || name_length >= ZIP_NAME_MAX || extra_length > EXTRA_MAX)
        return false;

    if (fread(out->name, 1, name_length, file) != name_length)
        return false;
    out->name[name_length] = '\0';

    unsigned char extra[EXTRA_MAX];
    if (fread(extra, 1, extra_length, file) != extra_length)
        return false;

    uint64_t size = compressed;
    if (compressed == SIZE_IN_ZIP64
        && !zip64_compressed_size(extra, extra_length, &size))
        return false;

    long position = ftell(file);
    if (position < 0)
        return false;
    out->offset = position;
    out->size = (long long)size;
    return true;
}

bool zip_find_member(const char *path, const char *prefix, zip_member *out) {
    if (path == NULL || prefix == NULL)
        return false;

    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return false;

    bool found = false;
    zip_member member;
    while (read_header(file, &member)) {
        if (strncmp(member.name, prefix, strlen(prefix)) == 0) {
            *out = member;
            found = true;
            break;
        }
        /* Past the data of the entry that was not wanted, to the next header. */
        if (fseek(file, (long)member.size, SEEK_CUR) != 0)
            break;
    }

    fclose(file);
    return found;
}

bool zip_extract_member(const char *path, const zip_member *member,
                        const char *destination) {
    FILE *source = fopen(path, "rb");
    if (source == NULL)
        return false;
    if (fseek(source, (long)member->offset, SEEK_SET) != 0) {
        fclose(source);
        return false;
    }

    FILE *target = fopen(destination, "wb");
    if (target == NULL) {
        fclose(source);
        return false;
    }

    bool ok = true;
    long long left = member->size;
    unsigned char buffer[COPY_CHUNK];
    while (ok && left > 0) {
        size_t wanted = left < (long long)sizeof buffer
            ? (size_t)left : sizeof buffer;
        size_t got = fread(buffer, 1, wanted, source);
        /* A short read means the member runs past the end of the file, which
           makes the whole reading of it wrong. */
        ok = got == wanted && fwrite(buffer, 1, got, target) == got;
        left -= (long long)got;
    }

    ok = fclose(target) == 0 && ok;
    fclose(source);
    if (!ok)
        (void)remove(destination);
    return ok;
}
