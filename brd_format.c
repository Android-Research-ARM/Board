// brd_format.c, see brd_format.h for the format, the field set, and why this
// is one implementation shared by the `board` cli and the host app's loader.
//
// plain c99, no dependencies beyond libc, no allocation on the decode path.
// it is compiled into the cli and into the host app's binaries, so it must
// stay free of objective-c, appkit, and anything that would drag a framework in.

#include "brd_format.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------- charsets */

/* two charsets, both strict ascii. non-ascii is refused outright rather than
   validated as utf-8: every one of these values ends up in a plist-shaped
   string, a json config, a process environment variable and (historically) a
   kernel command line, and "which of those is utf-8-clean today" is not a
   question worth betting a silent boot failure on. */

/* tokens (brand, model, codename) are the values that pair with a guest build
   and get compared verbatim by app compatibility checks: letters, digits, and
   the three separators android itself uses. no spaces, that rule has always
   applied to exactly these fields. */
static int brd_is_token_byte(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

/* text (manufacturer, device name, author, and the three chip fields) is
   printable ascii, spaces allowed because real values contain them ("Pocket P1",
   "Pocket Silicon P1"). control bytes, del and
   anything >= 0x80 are refused; so is a leading or trailing space, which is
   invisible in `board inspect` output and would otherwise let two visually
   identical files disagree. */
static int brd_is_text_byte(unsigned char c) {
    return c >= 0x20 && c <= 0x7E;
}

static int brd_field_is_token(brd_field_id field) {
    return field == BRD_FIELD_BRAND || field == BRD_FIELD_MODEL || field == BRD_FIELD_CODENAME ||
           field == BRD_FIELD_BOARD || field == BRD_FIELD_HARDWARE || field == BRD_FIELD_OPENGLES_VERSION;
}

static int brd_field_is_known(unsigned id) {
    return id >= BRD_FIELD_BRAND && id <= BRD_FIELD_OPENGLES_VERSION;
}

/* the optional chip group. one definition of "which three are paired", used by
   decode, encode and the diagnostic below, so the rule cannot be stated twice
   and then drift. */
static const brd_field_id kBrdSocGroup[BRD_FIELD_SOC_GROUP] = BRD_SOC_GROUP_IDS;

static int brd_field_is_soc(unsigned id) {
    return id >= BRD_FIELD_SOC_MANUFACTURER && id <= BRD_FIELD_GPU_MODEL;
}

static int brd_field_is_v3(unsigned id) {
    return id >= BRD_FIELD_BOARD && id <= BRD_FIELD_OPENGLES_VERSION;
}

/* ------------------------------------------------------------------- crc32 */

/* ieee 802.3 crc-32, bitwise. no table: this runs twice per file at most, over
   a few hundred bytes, and a 1 kb table in a format library is not worth the
   .data. */
static uint32_t brd_crc32(const uint8_t *bytes, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------- little-endian accessors */

static uint16_t brd_read_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t brd_read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void brd_write_u16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value & 0xFFu);
    p[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void brd_write_u32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & 0xFFu);
    p[1] = (uint8_t)((value >> 8) & 0xFFu);
    p[2] = (uint8_t)((value >> 16) & 0xFFu);
    p[3] = (uint8_t)((value >> 24) & 0xFFu);
}

/* ------------------------------------------------------------- diagnostics */

const char *brd_status_message(brd_status status) {
    switch (status) {
        case BRD_OK:                  return "ok";
        case BRD_ERR_IO:              return "could not read or write the file";
        case BRD_ERR_TOO_SMALL:       return "file is too small to be a .brd";
        case BRD_ERR_TOO_LARGE:       return "file is larger than a .brd may be";
        case BRD_ERR_BAD_MAGIC:       return "not a .brd file (wrong magic bytes)";
        case BRD_ERR_BAD_VERSION:     return "unsupported .brd format version";
        case BRD_ERR_BAD_LENGTH:      return "declared payload length does not match the file";
        case BRD_ERR_TRAILING_BYTES:  return "unexpected extra bytes after the payload";
        case BRD_ERR_BAD_CRC:         return "checksum mismatch: the file is corrupt or was modified";
        case BRD_ERR_UNKNOWN_FIELD:   return "file contains a field this format does not allow";
        case BRD_ERR_DUPLICATE_FIELD: return "file contains the same field twice";
        case BRD_ERR_MISSING_FIELD:   return "file is missing a required field";
        case BRD_ERR_EMPTY_VALUE:     return "a field is empty and none may be";
        case BRD_ERR_VALUE_TOO_LONG:  return "a field value is too long";
        case BRD_ERR_BAD_CHARACTER:   return "a field value contains a character this field does not allow";
        case BRD_ERR_BAD_PADDING:     return "a field value has leading or trailing whitespace";
        case BRD_ERR_INCOMPLETE_SOC_SET:
            return "chip identity is incomplete: soc manufacturer, soc model and gpu model "
                   "must be set together, or not at all";
    }
    return "unknown error";
}

const char *brd_field_label(brd_field_id field) {
    switch (field) {
        case BRD_FIELD_BRAND:            return "brand";
        case BRD_FIELD_MANUFACTURER:     return "manufacturer";
        case BRD_FIELD_MODEL:            return "model";
        case BRD_FIELD_CODENAME:         return "codename";
        case BRD_FIELD_DEVICE_NAME:      return "device name";
        case BRD_FIELD_AUTHOR:           return "author";
        case BRD_FIELD_SOC_MANUFACTURER: return "soc manufacturer";
        case BRD_FIELD_SOC_MODEL:        return "soc model";
        case BRD_FIELD_GPU_MODEL:        return "gpu model";
        case BRD_FIELD_BOARD:            return "board";
        case BRD_FIELD_HARDWARE:         return "hardware";
        case BRD_FIELD_BUILD_ID:         return "build id";
        case BRD_FIELD_OPENGLES_VERSION: return "gles version";
    }
    return "unknown";
}

const char *brd_identity_value(const brd_identity *identity, brd_field_id field) {
    if (!identity) return NULL;
    switch (field) {
        case BRD_FIELD_BRAND:            return identity->brand;
        case BRD_FIELD_MANUFACTURER:     return identity->manufacturer;
        case BRD_FIELD_MODEL:            return identity->model;
        case BRD_FIELD_CODENAME:         return identity->codename;
        case BRD_FIELD_DEVICE_NAME:      return identity->device_name;
        case BRD_FIELD_AUTHOR:           return identity->author;
        case BRD_FIELD_SOC_MANUFACTURER: return identity->soc_manufacturer;
        case BRD_FIELD_SOC_MODEL:        return identity->soc_model;
        case BRD_FIELD_GPU_MODEL:        return identity->gpu_model;
        case BRD_FIELD_BOARD:            return identity->board;
        case BRD_FIELD_HARDWARE:         return identity->hardware;
        case BRD_FIELD_BUILD_ID:         return identity->build_id;
        case BRD_FIELD_OPENGLES_VERSION: return identity->opengles_version;
    }
    return NULL;
}

/* writable counterpart, private: keeps brd_decode free of an arm switch that
   would have to be kept in step with the one above. */
static char *brd_identity_slot(brd_identity *identity, brd_field_id field) {
    switch (field) {
        case BRD_FIELD_BRAND:            return identity->brand;
        case BRD_FIELD_MANUFACTURER:     return identity->manufacturer;
        case BRD_FIELD_MODEL:            return identity->model;
        case BRD_FIELD_CODENAME:         return identity->codename;
        case BRD_FIELD_DEVICE_NAME:      return identity->device_name;
        case BRD_FIELD_AUTHOR:           return identity->author;
        case BRD_FIELD_SOC_MANUFACTURER: return identity->soc_manufacturer;
        case BRD_FIELD_SOC_MODEL:        return identity->soc_model;
        case BRD_FIELD_GPU_MODEL:        return identity->gpu_model;
        case BRD_FIELD_BOARD:            return identity->board;
        case BRD_FIELD_HARDWARE:         return identity->hardware;
        case BRD_FIELD_BUILD_ID:         return identity->build_id;
        case BRD_FIELD_OPENGLES_VERSION: return identity->opengles_version;
    }
    return NULL;
}

/* -------------------------------------------------------------- validation */

brd_status brd_validate_value(brd_field_id field, const char *value) {
    if (!brd_field_is_known((unsigned)field)) return BRD_ERR_UNKNOWN_FIELD;
    if (!value) return BRD_ERR_EMPTY_VALUE;

    size_t length = strlen(value);
    if (length == 0) return BRD_ERR_EMPTY_VALUE;
    if (length > BRD_MAX_VALUE_BYTES) return BRD_ERR_VALUE_TOO_LONG;

    int token = brd_field_is_token(field);
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)value[i];
        if (token ? !brd_is_token_byte(c) : !brd_is_text_byte(c)) return BRD_ERR_BAD_CHARACTER;
    }
    /* token fields cannot contain a space by charset; text fields may, but not
       at either end. */
    if (!token && (value[0] == ' ' || value[length - 1] == ' ')) return BRD_ERR_BAD_PADDING;
    return BRD_OK;
}

/* ------------------------------------------------------------------ decode */

brd_status brd_decode(const uint8_t *bytes, size_t length, brd_identity *out) {
    return brd_decode_detail(bytes, length, out, NULL, 0, NULL);
}

brd_status brd_decode_detail(const uint8_t *bytes, size_t length, brd_identity *out,
                             brd_field_id *missing, size_t missing_capacity, size_t *missing_count) {
    if (missing_count) *missing_count = 0;
    if (!out) return BRD_ERR_IO;
    memset(out, 0, sizeof(*out));
    if (!bytes) return BRD_ERR_IO;

    if (length > (size_t)BRD_MAX_FILE_BYTES) return BRD_ERR_TOO_LARGE;
    if (length < (size_t)BRD_HEADER_LENGTH + (size_t)BRD_TRAILER_LENGTH) return BRD_ERR_TOO_SMALL;

    if (memcmp(bytes, BRD_MAGIC, BRD_MAGIC_LENGTH) != 0) return BRD_ERR_BAD_MAGIC;

    /* every version from BRD_FORMAT_VERSION_MIN up to the current one is read.
       a file older than this build is not an error; a file newer than it is,
       because this build cannot know what a later version's records mean. */
    uint16_t version = brd_read_u16(bytes + 8);
    if (version < BRD_FORMAT_VERSION_MIN || version > BRD_FORMAT_VERSION) return BRD_ERR_BAD_VERSION;

    uint16_t field_count = brd_read_u16(bytes + 10);
    uint32_t payload_length = brd_read_u32(bytes + 12);

    /* the file must be exactly header + payload + crc. checked before any
       record is read, and computed so it cannot overflow on a hostile length. */
    if (payload_length > (uint32_t)(BRD_MAX_FILE_BYTES - BRD_HEADER_LENGTH - BRD_TRAILER_LENGTH))
        return BRD_ERR_BAD_LENGTH;
    size_t expected = (size_t)BRD_HEADER_LENGTH + (size_t)payload_length + (size_t)BRD_TRAILER_LENGTH;
    if (length < expected) return BRD_ERR_BAD_LENGTH;
    if (length > expected) return BRD_ERR_TRAILING_BYTES;

    uint32_t stored_crc = brd_read_u32(bytes + BRD_HEADER_LENGTH + payload_length);
    if (stored_crc != brd_crc32(bytes, (size_t)BRD_HEADER_LENGTH + (size_t)payload_length))
        return BRD_ERR_BAD_CRC;

    /* deliberately not `field_count != BRD_FIELD_COUNT -> MISSING_FIELD`. that
       early equality check rejected a file carrying a seventh, unknown field,
       but with the wrong reason, and without ever reaching the unknown-id check
       below, so the enforcement that actually matters went untested. parse what
       the file claims and let the per-record checks give the true reason; the
       "all six present" sweep after the loop still catches a short file. the
       loop cannot run away: each record needs at least 3 bytes and overrunning
       the payload returns BRD_ERR_BAD_LENGTH. */
    int seen[BRD_FIELD_OPENGLES_VERSION + 1];
    memset(seen, 0, sizeof(seen));

    size_t offset = BRD_HEADER_LENGTH;
    size_t end = (size_t)BRD_HEADER_LENGTH + (size_t)payload_length;
    for (uint16_t i = 0; i < field_count; i++) {
        if (offset + 3 > end) return BRD_ERR_BAD_LENGTH;
        unsigned id = bytes[offset];
        uint16_t value_length = brd_read_u16(bytes + offset + 1);
        offset += 3;

        /* the enforcement point. an id outside the enum is rejected here, before
           anything is copied anywhere, a hostile file naming a technical
           property cannot get as far as having a value read, let alone applied. */
        if (!brd_field_is_known(id)) return BRD_ERR_UNKNOWN_FIELD;
        /* the chip group arrived in format version 2. a version 1 file carrying
           one is a forgery or a corruption, not an older file being generous:
           reject it by the same rule that rejects an id outside the enum. */
        if (brd_field_is_soc(id) && version < 2u) return BRD_ERR_UNKNOWN_FIELD;
        /* extended technical properties arrived in format version 3. */
        if (brd_field_is_v3(id) && version < 3u) return BRD_ERR_UNKNOWN_FIELD;
        if (seen[id]) return BRD_ERR_DUPLICATE_FIELD;
        seen[id] = 1;

        if (value_length == 0) return BRD_ERR_EMPTY_VALUE;
        if (value_length > BRD_MAX_VALUE_BYTES) return BRD_ERR_VALUE_TOO_LONG;
        if (offset + value_length > end) return BRD_ERR_BAD_LENGTH;

        /* an embedded nul has to be caught here, before the value becomes a c
           string. brd_validate_value() works on a nul-terminated string, so it
           would see "pocket" in "pocket\0evil", pass it, and let the tail be
           silently dropped, the classic truncation trick, and the reason the
           test suite forges exactly that file. (an earlier revision of this
           comment claimed the charset check covered it. it did not: strlen
           stops at the nul before any charset check runs.) */
        if (memchr(bytes + offset, 0, value_length) != NULL) {
            memset(out, 0, sizeof(*out));
            return BRD_ERR_BAD_CHARACTER;
        }

        /* copy into a fixed slot and nul-terminate before validating, so the
           charset check runs on exactly the bytes that would be applied. */
        char scratch[BRD_MAX_VALUE_BYTES + 1];
        memcpy(scratch, bytes + offset, value_length);
        scratch[value_length] = '\0';
        offset += value_length;

        brd_status status = brd_validate_value((brd_field_id)id, scratch);
        if (status != BRD_OK) { memset(out, 0, sizeof(*out)); return status; }

        char *slot = brd_identity_slot(out, (brd_field_id)id);
        if (!slot) { memset(out, 0, sizeof(*out)); return BRD_ERR_UNKNOWN_FIELD; }
        memcpy(slot, scratch, (size_t)value_length + 1);
    }

    if (offset != end) { memset(out, 0, sizeof(*out)); return BRD_ERR_BAD_LENGTH; }

    for (unsigned id = BRD_FIELD_BRAND; id <= BRD_FIELD_AUTHOR; id++)
        if (!seen[id]) { memset(out, 0, sizeof(*out)); return BRD_ERR_MISSING_FIELD; }

    /* THE PAIRING RULE. app compatibility checks read the chip model and the gpu
       model as a matched pair, so a file that set one without the other would
       describe a device that does not exist. checked here, in the one function
       both the cli's `validate` and the host app's pre-apply check call, so the
       rule cannot be enforced in one of them and forgotten in the other. zero of
       the three is fine and is the ordinary case; one or two never is. */
    size_t present = 0;
    for (size_t i = 0; i < BRD_FIELD_SOC_GROUP; i++)
        if (seen[kBrdSocGroup[i]]) present++;
    if (present != 0 && present != BRD_FIELD_SOC_GROUP) {
        for (size_t i = 0; i < BRD_FIELD_SOC_GROUP; i++) {
            if (seen[kBrdSocGroup[i]]) continue;
            if (missing && missing_count && *missing_count < missing_capacity)
                missing[(*missing_count)++] = kBrdSocGroup[i];
        }
        memset(out, 0, sizeof(*out));
        return BRD_ERR_INCOMPLETE_SOC_SET;
    }

    return BRD_OK;
}

/* ------------------------------------------------------------------ encode */

/* which ids this identity will actually be written as: always the six required
   ones, plus the chip group when it is set, plus any optional v3 fields. an empty
   member means "not set", which is why the members are zeroed for a caller that
   never touches them. returns BRD_ERR_INCOMPLETE_SOC_SET rather than silently
   writing a partial group, so encode can never produce a file decode would reject. */
static brd_status brd_encode_field_plan(const brd_identity *identity,
                                        brd_field_id *plan, size_t *plan_count) {
    size_t count = 0;
    for (unsigned id = BRD_FIELD_BRAND; id <= BRD_FIELD_AUTHOR; id++)
        plan[count++] = (brd_field_id)id;

    size_t present = 0;
    for (size_t i = 0; i < BRD_FIELD_SOC_GROUP; i++) {
        const char *value = brd_identity_value(identity, kBrdSocGroup[i]);
        if (value && value[0]) present++;
    }
    if (present != 0 && present != BRD_FIELD_SOC_GROUP) return BRD_ERR_INCOMPLETE_SOC_SET;
    if (present == BRD_FIELD_SOC_GROUP)
        for (size_t i = 0; i < BRD_FIELD_SOC_GROUP; i++) plan[count++] = kBrdSocGroup[i];

    for (unsigned id = BRD_FIELD_BOARD; id <= BRD_FIELD_OPENGLES_VERSION; id++) {
        const char *value = brd_identity_value(identity, (brd_field_id)id);
        if (value && value[0]) plan[count++] = (brd_field_id)id;
    }

    *plan_count = count;
    return BRD_OK;
}

brd_status brd_encode(const brd_identity *identity, uint8_t *out, size_t capacity, size_t *out_length) {
    if (!identity || !out || !out_length) return BRD_ERR_IO;
    *out_length = 0;

    brd_field_id plan[BRD_FIELD_COUNT];
    size_t plan_count = 0;
    brd_status planned = brd_encode_field_plan(identity, plan, &plan_count);
    if (planned != BRD_OK) return planned;

    for (size_t i = 0; i < plan_count; i++) {
        brd_status status = brd_validate_value(plan[i], brd_identity_value(identity, plan[i]));
        if (status != BRD_OK) return status;
    }

    size_t payload_length = 0;
    for (size_t i = 0; i < plan_count; i++)
        payload_length += 3 + strlen(brd_identity_value(identity, plan[i]));

    size_t total = (size_t)BRD_HEADER_LENGTH + payload_length + (size_t)BRD_TRAILER_LENGTH;
    if (total > (size_t)BRD_MAX_FILE_BYTES) return BRD_ERR_TOO_LARGE;
    if (total > capacity) return BRD_ERR_IO;

    /* version 1 for a label-only file; version 2 for chip group; version 3 for extended fields */
    uint16_t version = (uint16_t)BRD_FORMAT_VERSION_MIN;
    for (size_t i = 0; i < plan_count; i++) {
        if (brd_field_is_v3((unsigned)plan[i])) {
            version = (uint16_t)BRD_FORMAT_VERSION;
            break;
        }
        if (brd_field_is_soc((unsigned)plan[i])) {
            version = 2u;
        }
    }
    memcpy(out, BRD_MAGIC, BRD_MAGIC_LENGTH);
    brd_write_u16(out + 8, version);
    brd_write_u16(out + 10, (uint16_t)plan_count);
    brd_write_u32(out + 12, (uint32_t)payload_length);

    size_t offset = BRD_HEADER_LENGTH;
    for (size_t i = 0; i < plan_count; i++) {
        const char *value = brd_identity_value(identity, plan[i]);
        size_t value_length = strlen(value);
        out[offset] = (uint8_t)plan[i];
        brd_write_u16(out + offset + 1, (uint16_t)value_length);
        memcpy(out + offset + 3, value, value_length);
        offset += 3 + value_length;
    }

    brd_write_u32(out + offset, brd_crc32(out, offset));
    *out_length = total;
    return BRD_OK;
}

/* --------------------------------------------------------------------- I/O */

brd_status brd_read_file(const char *path, brd_identity *out) {
    return brd_read_file_detail(path, out, NULL, 0, NULL);
}

brd_status brd_read_file_detail(const char *path, brd_identity *out,
                                brd_field_id *missing, size_t missing_capacity, size_t *missing_count) {
    if (missing_count) *missing_count = 0;
    if (!out) return BRD_ERR_IO;
    memset(out, 0, sizeof(*out));
    if (!path || !path[0]) return BRD_ERR_IO;

    FILE *file = fopen(path, "rb");
    if (!file) return BRD_ERR_IO;

    /* bounded read: one byte more than the maximum, so "too large" is detected
       without ever sizing a buffer from the file itself. */
    uint8_t buffer[BRD_MAX_FILE_BYTES + 1];
    size_t length = fread(buffer, 1, sizeof(buffer), file);
    int failed = ferror(file);
    fclose(file);
    if (failed) return BRD_ERR_IO;
    if (length > (size_t)BRD_MAX_FILE_BYTES) return BRD_ERR_TOO_LARGE;

    return brd_decode_detail(buffer, length, out, missing, missing_capacity, missing_count);
}

brd_status brd_write_file(const char *path, const brd_identity *identity) {
    if (!path || !path[0] || !identity) return BRD_ERR_IO;

    uint8_t buffer[BRD_MAX_FILE_BYTES];
    size_t length = 0;
    brd_status status = brd_encode(identity, buffer, sizeof(buffer), &length);
    if (status != BRD_OK) return status;

    /* atomic: write a sibling temp file, fsync, rename. a half-written .brd on
       a full disk would otherwise be indistinguishable from a corrupt one. */
    char temp[PATH_MAX];
    int written = snprintf(temp, sizeof(temp), "%s.tmp-%d", path, (int)getpid());
    if (written < 0 || (size_t)written >= sizeof(temp)) return BRD_ERR_IO;

    FILE *file = fopen(temp, "wb");
    if (!file) return BRD_ERR_IO;
    int ok = fwrite(buffer, 1, length, file) == length;
    if (ok) ok = fflush(file) == 0;
    if (ok) ok = fsync(fileno(file)) == 0;
    if (fclose(file) != 0) ok = 0;
    if (!ok || rename(temp, path) != 0) { unlink(temp); return BRD_ERR_IO; }
    return BRD_OK;
}
