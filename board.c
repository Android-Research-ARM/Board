// board, create, inspect and validate `.brd` device-identity extension files.
//
// a `.brd` carries a device label: brand, manufacturer, model, codename, device
// name, plus the self-reported name of whoever authored it. it may also carry
// an optional chip identity: soc manufacturer, soc model and gpu model, which
// are valid only as a set of three because app compatibility checks read the
// chip model and the gpu model as a matched pair. everything else in the
// technical identity (board, hardware, bootloader, api level) belongs to the
// host app and is not expressible in this format at all. see brd_format.h.
//
// fully local: no network, no accounts, no telemetry, no server. it reads and
// writes files in front of you and nothing else.

#include "brd_format.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* the six required fields, in the order `create` asks for them. the chip group
   is deliberately NOT here: it is optional, and prompting every author for a
   chip identity would turn "you may set this" into "you must". it is authored
   with flags instead. */
static const brd_field_id kPromptOrder[] = {
    BRD_FIELD_BRAND, BRD_FIELD_MANUFACTURER, BRD_FIELD_MODEL,
    BRD_FIELD_CODENAME, BRD_FIELD_DEVICE_NAME, BRD_FIELD_AUTHOR
};

static const brd_field_id kSocGroup[BRD_FIELD_SOC_GROUP] = BRD_SOC_GROUP_IDS;

static void usage(FILE *out) {
    fprintf(out,
        "board: create, inspect and validate .brd device-identity files\n"
        "\n"
        "usage:\n"
        "  board create [options]     write a new .brd (interactive unless every field is given)\n"
        "  board inspect <file>       print what a .brd contains, in plain text\n"
        "  board view <file>          show detailed .brd file information\n"
        "  board validate <file>      check a .brd is well-formed; exit 1 if not\n"
        "  board help                 this message\n"
        "\n"
        "create options (any omitted field is prompted for):\n"
        "  --brand <token>            e.g. pocket        [A-Za-z0-9._-]\n"
        "  --manufacturer <text>      e.g. Pocket\n"
        "  --model <token>            e.g. POCKET_P1_A   [A-Za-z0-9._-]\n"
        "  --codename <token>         e.g. POCKET_P1     [A-Za-z0-9._-]\n"
        "  --name <text>              e.g. Pocket P1     (the human-facing name)\n"
        "  --author <text>            who made this file; defaults to your login name\n"
        "  --output <path>            where to write it; defaults to <codename>.brd\n"
        "\n"
        "optional chip identity (all three together, or none of them):\n"
        "  --soc-manufacturer <text>  e.g. Pocket Silicon\n"
        "  --soc-model <text>         e.g. Pocket Silicon P1\n"
        "  --gpu-model <text>         e.g. Pocket Graphics P1\n"
        "\n"
        "optional technical identity (format version 3):\n"
        "  --board <token>            e.g. sun           (ro.product.board / platform)\n"
        "  --hardware <token>         e.g. qcom          (ro.hardware / ro.boot.hardware)\n"
        "  --build-id <text>          e.g. ASUS_AI2501H-user 15 (ro.build.display.id)\n"
        "  --gles-version <token>     e.g. 196610        (ro.opengles.version, 196610 = ES 3.2)\n"
        "\n"
        "optional carrier identity (format version 4):\n"
        "  --carrier <token>          e.g. XTC           (ro.boot.carrierid)\n"
        "  --sales-code <token>       e.g. XTC           (ro.boot.sales_code / csc)\n"
        "\n"
        "The chip and GPU identity must be set together, or not at all: app\n"
        "compatibility checks read the chip model and the GPU model as a matched\n"
        "pair, so setting one without the other would describe a device that does\n"
        "not exist. Giving one of the three flags means giving all three. Giving\n"
        "none of them is normal, and leaves the host app reporting its own chip.\n"
        "\n"
        "The author field is self-reported and is never verified by anything.\n");
}

static void print_identity(const brd_identity *identity, const char *path) {
    if (path) printf("%s\n", path);
    printf("  brand         %s\n", identity->brand);
    printf("  manufacturer  %s\n", identity->manufacturer);
    printf("  model         %s\n", identity->model);
    printf("  codename      %s\n", identity->codename);
    printf("  device name   %s\n", identity->device_name);
    printf("  author        %s   (self-reported, not verified)\n", identity->author);
    /* the chip group prints only when the file carries it. a file without one
       is the ordinary case, and three blank lines would read as "set to
       nothing" rather than "not set". */
    if (identity->soc_model[0]) {
        printf("  soc manuf     %s\n", identity->soc_manufacturer);
        printf("  soc model     %s\n", identity->soc_model);
        printf("  gpu model     %s\n", identity->gpu_model);
    } else {
        printf("  chip identity not set (the host app reports its own)\n");
    }
    if (identity->board[0]) printf("  board         %s\n", identity->board);
    if (identity->hardware[0]) printf("  hardware      %s\n", identity->hardware);
    if (identity->build_id[0]) printf("  build id      %s\n", identity->build_id);
    if (identity->opengles_version[0]) printf("  gles version  %s\n", identity->opengles_version);
    if (identity->carrier[0]) printf("  carrier       %s\n", identity->carrier);
    if (identity->sales_code[0]) printf("  sales code    %s\n", identity->sales_code);
}

/* ------------------------------------------------------------------ create */

static const char *default_author(void) {
    /* $USER first, then the passwd entry, `id -un`'s own source, so this works
       in a shell that does not export USER. never returns an empty string, the
       caller still requires a non-empty answer either way. */
    const char *user = getenv("USER");
    if (user && user[0]) return user;
    struct passwd *entry = getpwuid(getuid());
    if (entry && entry->pw_name && entry->pw_name[0]) return entry->pw_name;
    return "Anonymous";
}

/* read one line from stdin into `buffer`, stripping the newline. returns 0 on
   eof, which is what ctrl-d gives, treated as "abort", never as "empty". */
static int read_line(char *buffer, size_t capacity) {
    if (!fgets(buffer, (int)capacity, stdin)) return 0;
    size_t length = strlen(buffer);
    while (length > 0 && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r'))
        buffer[--length] = '\0';
    return 1;
}

/* prompt until the answer passes the same validation the decoder applies, so a
   file this tool writes can never be one the host app then rejects. `fallback`
   is offered as the default when non-null and the user just presses return. */
static int prompt_field(brd_field_id field, const char *fallback, char *out, size_t capacity) {
    char line[512];
    for (;;) {
        if (fallback && fallback[0]) printf("%s [%s]: ", brd_field_label(field), fallback);
        else printf("%s: ", brd_field_label(field));
        fflush(stdout);

        if (!read_line(line, sizeof(line))) { printf("\naborted\n"); return 0; }

        const char *answer = line;
        if (!line[0] && fallback && fallback[0]) answer = fallback;

        brd_status status = brd_validate_value(field, answer);
        if (status == BRD_OK) {
            snprintf(out, capacity, "%s", answer);
            return 1;
        }
        /* the author field is required and must never end up blank, say so in
           those words rather than showing the generic "a field is empty". */
        if (status == BRD_ERR_EMPTY_VALUE && field == BRD_FIELD_AUTHOR)
            fprintf(stderr, "  author cannot be blank, put your name, a handle, or 'Anonymous'\n");
        else
            fprintf(stderr, "  %s\n", brd_status_message(status));
        if (status == BRD_ERR_BAD_CHARACTER)
            fprintf(stderr, "  allowed here: %s\n",
                    (field == BRD_FIELD_BRAND || field == BRD_FIELD_MODEL || field == BRD_FIELD_CODENAME)
                        ? "letters, digits, dot, underscore, hyphen, no spaces"
                        : "printable ASCII, spaces allowed inside");
    }
}

static char *slot_for(brd_identity *identity, brd_field_id field) {
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
        case BRD_FIELD_CARRIER:          return identity->carrier;
        case BRD_FIELD_SALES_CODE:       return identity->sales_code;
    }
    return NULL;
}

static int command_create(int argc, char **argv) {
    brd_identity identity;
    memset(&identity, 0, sizeof(identity));
    const char *output = NULL;

    static const struct { const char *flag; brd_field_id field; } kFlags[] = {
        { "--brand",            BRD_FIELD_BRAND },
        { "--manufacturer",     BRD_FIELD_MANUFACTURER },
        { "--model",            BRD_FIELD_MODEL },
        { "--codename",         BRD_FIELD_CODENAME },
        { "--name",             BRD_FIELD_DEVICE_NAME },
        { "--author",           BRD_FIELD_AUTHOR },
        { "--soc-manufacturer", BRD_FIELD_SOC_MANUFACTURER },
        { "--soc-model",        BRD_FIELD_SOC_MODEL },
        { "--gpu-model",        BRD_FIELD_GPU_MODEL },
        { "--board",            BRD_FIELD_BOARD },
        { "--hardware",         BRD_FIELD_HARDWARE },
        { "--build-id",         BRD_FIELD_BUILD_ID },
        { "--gles-version",     BRD_FIELD_OPENGLES_VERSION },
        { "--carrier",          BRD_FIELD_CARRIER },
        { "--sales-code",       BRD_FIELD_SALES_CODE },
    };

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--output") == 0 || strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) { fprintf(stderr, "board: --output needs a path\n"); return 2; }
            output = argv[i];
            continue;
        }
        int matched = 0;
        for (size_t f = 0; f < sizeof(kFlags) / sizeof(kFlags[0]); f++) {
            if (strcmp(argv[i], kFlags[f].flag) != 0) continue;
            if (++i >= argc) { fprintf(stderr, "board: %s needs a value\n", kFlags[f].flag); return 2; }
            brd_status status = brd_validate_value(kFlags[f].field, argv[i]);
            if (status != BRD_OK) {
                fprintf(stderr, "board: %s: %s\n", kFlags[f].flag, brd_status_message(status));
                return 2;
            }
            snprintf(slot_for(&identity, kFlags[f].field), BRD_MAX_VALUE_BYTES + 1, "%s", argv[i]);
            matched = 1;
            break;
        }
        if (!matched) { fprintf(stderr, "board: unknown option '%s'\n", argv[i]); return 2; }
    }

    /* THE PAIRING RULE, caught at the prompt rather than at the write. brd_encode()
       refuses a partial chip group too and is the real enforcement, but an author
       who typed two of the three flags deserves to be told which one they left
       out, by name, before anything else happens. */
    {
        size_t given = 0;
        for (size_t i = 0; i < BRD_FIELD_SOC_GROUP; i++)
            if (slot_for(&identity, kSocGroup[i])[0]) given++;
        if (given != 0 && given != BRD_FIELD_SOC_GROUP) {
            fprintf(stderr, "board: %s\n", brd_status_message(BRD_ERR_INCOMPLETE_SOC_SET));
            for (size_t i = 0; i < BRD_FIELD_SOC_GROUP; i++)
                if (!slot_for(&identity, kSocGroup[i])[0])
                    fprintf(stderr, "  missing: %s\n", brd_field_label(kSocGroup[i]));
            return 2;
        }
    }

    /* prompt for whatever the flags did not supply. with every field given this
       loop does nothing, which is what makes `board create` scriptable. only the
       six required fields are prompted; the chip group stays flags-only. */
    for (size_t i = 0; i < sizeof(kPromptOrder) / sizeof(kPromptOrder[0]); i++) {
        brd_field_id field = kPromptOrder[i];
        char *slot = slot_for(&identity, field);
        if (slot[0]) continue;
        const char *fallback = (field == BRD_FIELD_AUTHOR) ? default_author() : NULL;
        if (!prompt_field(field, fallback, slot, BRD_MAX_VALUE_BYTES + 1)) return 1;
    }

    char derived[BRD_MAX_VALUE_BYTES + 8];
    if (!output) {
        snprintf(derived, sizeof(derived), "%s.brd", identity.codename);
        output = derived;
    }

    brd_status status = brd_write_file(output, &identity);
    if (status != BRD_OK) {
        fprintf(stderr, "board: could not write %s: %s\n", output, brd_status_message(status));
        return 1;
    }
    printf("\nwrote %s\n", output);
    print_identity(&identity, NULL);
    return 0;
}

/* --------------------------------------------------------- inspect/validate */

/* the shared failure rendering. BRD_ERR_INCOMPLETE_SOC_SET is the one status
   that can name specific fields, and brd_read_file_detail() is what hands them
   over -- the same walk brd_read_file() does, not a second parse. */
static void print_failure(const char *path, const char *label, brd_status status,
                          const brd_field_id *missing, size_t missing_count) {
    fprintf(stderr, "board: %s: %s%s\n", path, label, brd_status_message(status));
    for (size_t i = 0; i < missing_count; i++)
        fprintf(stderr, "  missing: %s\n", brd_field_label(missing[i]));
}

static int command_inspect(const char *path) {
    brd_identity identity;
    brd_field_id missing[BRD_FIELD_SOC_GROUP];
    size_t missing_count = 0;
    brd_status status = brd_read_file_detail(path, &identity, missing,
                                             sizeof(missing) / sizeof(missing[0]), &missing_count);
    if (status != BRD_OK) {
        print_failure(path, "", status, missing, missing_count);
        return 1;
    }
    print_identity(&identity, path);
    return 0;
}

/* --------------------------------------------------------------- view */

static int command_view(const char *path) {
    brd_identity identity;
    brd_field_id missing[BRD_FIELD_SOC_GROUP];
    size_t missing_count = 0;
    brd_status status = brd_read_file_detail(path, &identity, missing,
                                             sizeof(missing) / sizeof(missing[0]), &missing_count);
    if (status != BRD_OK) {
        print_failure(path, "", status, missing, missing_count);
        return 1;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "board: %s: could not stat file\n", path);
        return 1;
    }

    int has_chip = identity.soc_model[0] != '\0';
    int has_v3 = identity.board[0] != '\0' || identity.hardware[0] != '\0' ||
                 identity.build_id[0] != '\0' || identity.opengles_version[0] != '\0';
    int has_v4 = identity.carrier[0] != '\0' || identity.sales_code[0] != '\0';
    unsigned version = has_v4 ? BRD_FORMAT_VERSION : (has_v3 ? 3u : (has_chip ? 2u : BRD_FORMAT_VERSION_MIN));
    unsigned fields = BRD_FIELD_REQUIRED + (has_chip ? BRD_FIELD_SOC_GROUP : 0u);
    if (identity.board[0]) fields++;
    if (identity.hardware[0]) fields++;
    if (identity.build_id[0]) fields++;
    if (identity.opengles_version[0]) fields++;
    if (identity.carrier[0]) fields++;
    if (identity.sales_code[0]) fields++;

    printf("=== .brd file details ===\n");
    printf("File:           %s\n", path);
    printf("Size:           %ld bytes\n", (long)st.st_size);
    printf("Format version: %u\n", version);
    printf("Fields:         %u\n", fields);
    printf("Chip identity:  %s\n", has_chip ? "set" : "not set (host app reports its own)");
    printf("CRC:            valid\n");
    printf("\n=== identity fields ===\n");
    print_identity(&identity, NULL);
    printf("\n=== field details ===\n");
    printf("  brand         id=%d  len=%zu  charset=token\n", BRD_FIELD_BRAND, strlen(identity.brand));
    printf("  manufacturer  id=%d  len=%zu  charset=text\n", BRD_FIELD_MANUFACTURER, strlen(identity.manufacturer));
    printf("  model         id=%d  len=%zu  charset=token\n", BRD_FIELD_MODEL, strlen(identity.model));
    printf("  codename      id=%d  len=%zu  charset=token\n", BRD_FIELD_CODENAME, strlen(identity.codename));
    printf("  device name   id=%d  len=%zu  charset=text\n", BRD_FIELD_DEVICE_NAME, strlen(identity.device_name));
    printf("  author        id=%d  len=%zu  charset=text\n", BRD_FIELD_AUTHOR, strlen(identity.author));
    if (has_chip) {
        printf("  soc manuf     id=%d  len=%zu  charset=text\n",
               BRD_FIELD_SOC_MANUFACTURER, strlen(identity.soc_manufacturer));
        printf("  soc model     id=%d  len=%zu  charset=text\n",
               BRD_FIELD_SOC_MODEL, strlen(identity.soc_model));
        printf("  gpu model     id=%d  len=%zu  charset=text\n",
               BRD_FIELD_GPU_MODEL, strlen(identity.gpu_model));
    }
    if (identity.board[0])
        printf("  board         id=%d  len=%zu  charset=token\n", BRD_FIELD_BOARD, strlen(identity.board));
    if (identity.hardware[0])
        printf("  hardware      id=%d  len=%zu  charset=token\n", BRD_FIELD_HARDWARE, strlen(identity.hardware));
    if (identity.build_id[0])
        printf("  build id      id=%d  len=%zu  charset=text\n", BRD_FIELD_BUILD_ID, strlen(identity.build_id));
    if (identity.opengles_version[0])
        printf("  gles version  id=%d  len=%zu  charset=token\n", BRD_FIELD_OPENGLES_VERSION, strlen(identity.opengles_version));
    if (identity.carrier[0])
        printf("  carrier       id=%d  len=%zu  charset=token\n", BRD_FIELD_CARRIER, strlen(identity.carrier));
    if (identity.sales_code[0])
        printf("  sales code    id=%d  len=%zu  charset=token\n", BRD_FIELD_SALES_CODE, strlen(identity.sales_code));

    return 0;
}

static int command_validate(const char *path) {
    brd_identity identity;
    brd_field_id missing[BRD_FIELD_SOC_GROUP];
    size_t missing_count = 0;
    brd_status status = brd_read_file_detail(path, &identity, missing,
                                             sizeof(missing) / sizeof(missing[0]), &missing_count);
    if (status != BRD_OK) {
        print_failure(path, "INVALID: ", status, missing, missing_count);
        return 1;
    }
    /* the counts are the FILE's, not the format's. */
    int has_chip = identity.soc_model[0] != '\0';
    int has_v3 = identity.board[0] != '\0' || identity.hardware[0] != '\0' ||
                 identity.build_id[0] != '\0' || identity.opengles_version[0] != '\0';
    int has_v4 = identity.carrier[0] != '\0' || identity.sales_code[0] != '\0';
    unsigned version = has_v4 ? BRD_FORMAT_VERSION : (has_v3 ? 3u : (has_chip ? 2u : BRD_FORMAT_VERSION_MIN));
    unsigned fields = BRD_FIELD_REQUIRED + (has_chip ? BRD_FIELD_SOC_GROUP : 0u);
    if (identity.board[0]) fields++;
    if (identity.hardware[0]) fields++;
    if (identity.build_id[0]) fields++;
    if (identity.opengles_version[0]) fields++;
    if (identity.carrier[0]) fields++;
    if (identity.sales_code[0]) fields++;

    printf("%s: valid (format version %u, %u fields%s%s%s)\n", path,
           version, fields,
           has_chip ? ", chip identity set" : "",
           has_v3 ? ", extended identity set" : "",
           has_v4 ? ", carrier identity set" : "");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(stderr); return 2; }

    const char *command = argv[1];
    if (strcmp(command, "help") == 0 || strcmp(command, "--help") == 0 || strcmp(command, "-h") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(command, "create") == 0)
        return command_create(argc - 2, argv + 2);
    if (strcmp(command, "inspect") == 0 || strcmp(command, "validate") == 0 || strcmp(command, "view") == 0) {
        if (argc != 3) { fprintf(stderr, "board: %s needs exactly one file\n", command); return 2; }
        if (strcmp(command, "inspect") == 0) return command_inspect(argv[2]);
        if (strcmp(command, "view") == 0) return command_view(argv[2]);
        return command_validate(argv[2]);
    }
    fprintf(stderr, "board: unknown command '%s'\n\n", command);
    usage(stderr);
    return 2;
}
