// board, create, inspect and validate `.brd` device-identity extension files.
//
// a `.brd` carries a cosmetic device label: brand, manufacturer, model,
// codename, device name, plus the self-reported name of whoever authored it.
// the host app reads one to relabel the device it reports, it can do nothing
// else. the technical identity (soc, board, gpu) belongs to the host app and
// is not expressible in this format at all. see brd_format.h.
//
// fully local: no network, no accounts, no telemetry, no server. it reads and
// writes files in front of you and nothing else.

#include "brd_format.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const brd_field_id kPromptOrder[] = {
    BRD_FIELD_BRAND, BRD_FIELD_MANUFACTURER, BRD_FIELD_MODEL,
    BRD_FIELD_CODENAME, BRD_FIELD_DEVICE_NAME, BRD_FIELD_AUTHOR
};

static void usage(FILE *out) {
    fprintf(out,
        "board: create, inspect and validate .brd device-identity files\n"
        "\n"
        "usage:\n"
        "  board create [options]     write a new .brd (interactive unless every field is given)\n"
        "  board inspect <file>       print what a .brd contains, in plain text\n"
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
        "A .brd can set ONLY the six fields above. It cannot set the SoC, board,\n"
        "hardware, bootloader, API level or GPU identity: those belong to the host\n"
        "app that loads these files, and this format has no way to express them.\n"
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
        case BRD_FIELD_BRAND:        return identity->brand;
        case BRD_FIELD_MANUFACTURER: return identity->manufacturer;
        case BRD_FIELD_MODEL:        return identity->model;
        case BRD_FIELD_CODENAME:     return identity->codename;
        case BRD_FIELD_DEVICE_NAME:  return identity->device_name;
        case BRD_FIELD_AUTHOR:       return identity->author;
    }
    return NULL;
}

static int command_create(int argc, char **argv) {
    brd_identity identity;
    memset(&identity, 0, sizeof(identity));
    const char *output = NULL;

    static const struct { const char *flag; brd_field_id field; } kFlags[] = {
        { "--brand",        BRD_FIELD_BRAND },
        { "--manufacturer", BRD_FIELD_MANUFACTURER },
        { "--model",        BRD_FIELD_MODEL },
        { "--codename",     BRD_FIELD_CODENAME },
        { "--name",         BRD_FIELD_DEVICE_NAME },
        { "--author",       BRD_FIELD_AUTHOR },
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

    /* prompt for whatever the flags did not supply. with every field given this
       loop does nothing, which is what makes `board create` scriptable. */
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

static int command_inspect(const char *path) {
    brd_identity identity;
    brd_status status = brd_read_file(path, &identity);
    if (status != BRD_OK) {
        fprintf(stderr, "board: %s: %s\n", path, brd_status_message(status));
        return 1;
    }
    print_identity(&identity, path);
    return 0;
}

static int command_validate(const char *path) {
    brd_identity identity;
    brd_status status = brd_read_file(path, &identity);
    if (status != BRD_OK) {
        fprintf(stderr, "board: %s: INVALID: %s\n", path, brd_status_message(status));
        return 1;
    }
    printf("%s: valid (format version %u, %u fields)\n", path, BRD_FORMAT_VERSION, BRD_FIELD_COUNT);
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
    if (strcmp(command, "inspect") == 0 || strcmp(command, "validate") == 0) {
        if (argc != 3) { fprintf(stderr, "board: %s needs exactly one file\n", command); return 2; }
        return strcmp(command, "inspect") == 0 ? command_inspect(argv[2]) : command_validate(argv[2]);
    }
    fprintf(stderr, "board: unknown command '%s'\n\n", command);
    usage(stderr);
    return 2;
}
