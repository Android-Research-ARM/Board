// brd_format.h, the `.brd` device-identity extension format.
//
// one implementation, two consumers: the `board` cli (Board/board.c) and the
// host app that loads `.brd` files at runtime. `board validate` and the app's
// pre-apply check therefore cannot drift apart, they are literally the same
// function. do not reimplement any of this on either side.
//
// the hard rule this file enforces structurally. a .brd file may carry only
// cosmetic label fields: brand, manufacturer, model, device codename, device
// name, and the author's self-reported name. it is incapable of carrying
// anything else, not because a well-behaved file wouldn't, but because
// `brd_identity` has no other members and brd_decode() rejects any field id it
// does not know. the host app's technical identity, soc/board/hardware/gpu
// (phonePropBoard, phonePropHardware, phonePropSocManufacturer,
// phonePropSocModel, phonePropBootloader, phonePropFirstApiLevel, gpuPropModel,
// and every ro.hardware*/ro.board.* equivalent), is set by the host app and is
// not reachable from this format at all.
//
// why binary and not json, and why not encrypted. `board` is open source, so
// encryption would buy no real secrecy, the key would ship beside the reader.
// the honest goal is "not accidentally hand-editable in a text editor", which a
// compact versioned binary layout achieves without pretending to be a security
// boundary. the security boundary is brd_decode()'s validation, not obscurity.
//
// layout (all integers little-endian, no padding, no alignment assumptions,
// every field is read byte-by-byte so this is portable and endian-independent):
//
//     offset  size  meaning
//     0       8     magic, the 8 ascii bytes "ARASBRD1" (no nul)
//     8       2     format_version, u16, currently 1
//     10      2     field_count, u16, number of records below
//     12      4     payload_length, u32, bytes of records
//     16      n     the records
//     16+n    4     crc32, u32, ieee, over bytes [0, 16+n)
//
// each record:
//     0       1     field id (brd_field_id)
//     1       2     value length, u16, in bytes (never 0)
//     3       len   value bytes, utf-8, not nul-terminated
//
// a file is exactly 20 + payload_length bytes. trailing bytes are a hard
// rejection, so a .brd cannot smuggle an appended payload past the parser.
//
// evolving it later. bump BRD_FORMAT_VERSION and keep decoding version 1,
// that is the entire reason the version field exists. adding a field means a
// new id plus a new member on brd_identity; never repurpose an existing id.

#ifndef ARAS_BRD_FORMAT_H
#define ARAS_BRD_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRD_MAGIC             "ARASBRD1"
#define BRD_MAGIC_LENGTH      8u
#define BRD_HEADER_LENGTH     16u
#define BRD_TRAILER_LENGTH    4u   /* crc32 */
#define BRD_FORMAT_VERSION    1u
#define BRD_MAX_VALUE_BYTES   64u
#define BRD_MAX_FILE_BYTES    4096u
#define BRD_FIELD_COUNT       6u

/// every field a .brd may contain. an id outside this set is a decode error,
/// this is what makes "smuggle a technical field in" structurally impossible
/// rather than merely discouraged.
typedef enum {
    BRD_FIELD_BRAND        = 1,  /* ro.product.brand,        token charset */
    BRD_FIELD_MANUFACTURER = 2,  /* ro.product.manufacturer, text charset  */
    BRD_FIELD_MODEL        = 3,  /* ro.product.model,        token charset */
    BRD_FIELD_CODENAME     = 4,  /* ro.product.device,       token charset */
    BRD_FIELD_DEVICE_NAME  = 5,  /* human-facing name,       text charset  */
    BRD_FIELD_AUTHOR       = 6   /* self-reported, unverified text charset */
} brd_field_id;

/// the complete data model. six cosmetic strings and nothing else, deliberately
/// not a dictionary, a key-value list, or anything else open-ended, so there is
/// no representation in which an unexpected field could survive decoding.
/// values are nul-terminated for c convenience; the wire format is length-prefixed.
typedef struct {
    char brand[BRD_MAX_VALUE_BYTES + 1];
    char manufacturer[BRD_MAX_VALUE_BYTES + 1];
    char model[BRD_MAX_VALUE_BYTES + 1];
    char codename[BRD_MAX_VALUE_BYTES + 1];
    char device_name[BRD_MAX_VALUE_BYTES + 1];
    char author[BRD_MAX_VALUE_BYTES + 1];
} brd_identity;

typedef enum {
    BRD_OK = 0,
    BRD_ERR_IO,                /* could not read/write the file             */
    BRD_ERR_TOO_SMALL,         /* shorter than a header + trailer           */
    BRD_ERR_TOO_LARGE,         /* over BRD_MAX_FILE_BYTES                   */
    BRD_ERR_BAD_MAGIC,         /* not a .brd at all                         */
    BRD_ERR_BAD_VERSION,       /* a format version this build cannot read   */
    BRD_ERR_BAD_LENGTH,        /* declared length disagrees with the file   */
    BRD_ERR_TRAILING_BYTES,    /* extra bytes after the declared payload    */
    BRD_ERR_BAD_CRC,           /* corrupt or tampered                       */
    BRD_ERR_UNKNOWN_FIELD,     /* a field id outside brd_field_id           */
    BRD_ERR_DUPLICATE_FIELD,   /* the same field id twice                   */
    BRD_ERR_MISSING_FIELD,     /* a required field absent                   */
    BRD_ERR_EMPTY_VALUE,       /* zero-length value (author included)       */
    BRD_ERR_VALUE_TOO_LONG,    /* over BRD_MAX_VALUE_BYTES                  */
    BRD_ERR_BAD_CHARACTER,     /* control byte, non-ASCII, or wrong charset */
    BRD_ERR_BAD_PADDING        /* leading/trailing whitespace in a value    */
} brd_status;

/// human-readable, single-line, no trailing period. never null.
const char *brd_status_message(brd_status status);

/// lowercase display label for a field ("brand", "manufacturer", ...), or
/// "unknown" for an id outside the enum. never null.
const char *brd_field_label(brd_field_id field);

/// the value a decoded identity holds for `field`, or null for an unknown id.
const char *brd_identity_value(const brd_identity *identity, brd_field_id field);

/// check one value against `field`'s charset and length rules, without needing a
/// whole file. `board create` uses this to reject bad input at the prompt; the
/// decoder applies the identical check to every field it reads.
brd_status brd_validate_value(brd_field_id field, const char *value);

/// parse and fully validate `length` bytes. on BRD_OK, *out holds every field,
/// nul-terminated. on any error *out is zeroed, a partially-populated identity
/// is never handed back, so a caller that ignores the status still cannot apply
/// half of a malformed file.
brd_status brd_decode(const uint8_t *bytes, size_t length, brd_identity *out);

/// serialize a fully-validated identity. writes at most `capacity` bytes and
/// sets *out_length. every field is validated first, so encode can never
/// produce a file that decode would reject.
brd_status brd_encode(const brd_identity *identity, uint8_t *out, size_t capacity, size_t *out_length);

/// read and decode a file. applies BRD_MAX_FILE_BYTES before allocating, so a
/// hostile path cannot make the reader consume arbitrary memory.
brd_status brd_read_file(const char *path, brd_identity *out);

/// encode and write atomically (temp file in the same directory, then rename),
/// mode 0644.
brd_status brd_write_file(const char *path, const brd_identity *identity);

#ifdef __cplusplus
}
#endif

#endif /* ARAS_BRD_FORMAT_H */
