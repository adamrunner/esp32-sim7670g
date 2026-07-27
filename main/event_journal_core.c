#include "event_journal_core.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>


static bool sensitive_key(const char *key, size_t key_len)
{
    static const char *const keys[] = {
        "password", "passwd", "passphrase", "psk", "secret", "token",
        "authorization", "username", "mqtt_uri", "broker_uri",
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strlen(keys[i]) == key_len &&
            strncasecmp(key, keys[i], key_len) == 0) {
            return true;
        }
    }
    return false;
}

static const char *skip_json_string(const char *cursor)
{
    if (*cursor != '"') {
        return cursor;
    }
    cursor++;
    while (*cursor) {
        if (*cursor == '\\' && cursor[1]) {
            cursor += 2;
        } else if (*cursor == '"') {
            return cursor + 1;
        } else {
            cursor++;
        }
    }
    return cursor;
}

static const char *skip_json_value(const char *cursor)
{
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor == '"') {
        return skip_json_string(cursor);
    }
    if (*cursor == '{' || *cursor == '[') {
        char opening = *cursor;
        char closing = opening == '{' ? '}' : ']';
        int depth = 0;
        do {
            if (*cursor == '"') {
                cursor = skip_json_string(cursor);
                continue;
            }
            if (*cursor == opening) {
                depth++;
            } else if (*cursor == closing) {
                depth--;
            }
            cursor++;
        } while (*cursor && depth > 0);
        return cursor;
    }
    while (*cursor && *cursor != ',' && *cursor != '}') {
        cursor++;
    }
    return cursor;
}

static bool append_bytes(
    char *output,
    size_t output_len,
    size_t *used,
    const char *source,
    size_t source_len
)
{
    if (*used + source_len >= output_len) {
        return false;
    }
    memcpy(output + *used, source, source_len);
    *used += source_len;
    output[*used] = '\0';
    return true;
}

bool event_core_redact_json(const char *input, char *output, size_t output_len)
{
    if (!input || !output || output_len == 0) {
        return false;
    }
    size_t used = 0;
    output[0] = '\0';
    const char *cursor = input;

    while (*cursor) {
        if (*cursor != '"') {
            if (!append_bytes(output, output_len, &used, cursor, 1)) {
                return false;
            }
            cursor++;
            continue;
        }

        const char *token_end = skip_json_string(cursor);
        if (token_end <= cursor + 1 || token_end[-1] != '"') {
            return false;
        }
        const char *after = token_end;
        while (isspace((unsigned char)*after)) {
            after++;
        }
        bool is_key = *after == ':';
        size_t key_len = (size_t)(token_end - cursor - 2);
        if (!is_key || !sensitive_key(cursor + 1, key_len)) {
            if (!append_bytes(
                    output, output_len, &used, cursor,
                    (size_t)(token_end - cursor))) {
                return false;
            }
            cursor = token_end;
            continue;
        }

        if (!append_bytes(
                output, output_len, &used, cursor,
                (size_t)(after - cursor + 1))) {
            return false;
        }
        cursor = after + 1;
        while (isspace((unsigned char)*cursor)) {
            if (!append_bytes(output, output_len, &used, cursor, 1)) {
                return false;
            }
            cursor++;
        }
        static const char replacement[] = "\"[redacted]\"";
        if (!append_bytes(
                output, output_len, &used, replacement,
                sizeof(replacement) - 1)) {
            return false;
        }
        cursor = skip_json_value(cursor);
    }
    return true;
}

bool event_core_rate_limited(
    bool previously_emitted,
    uint64_t last_emitted_ms,
    uint64_t now_ms,
    uint32_t interval_ms
)
{
    return interval_ms > 0 && previously_emitted &&
           now_ms >= last_emitted_ms &&
           now_ms - last_emitted_ms < interval_ms;
}

int event_core_repair_tail(const char *path)
{
    FILE *file = fopen(path, "r+");
    if (!file) {
        return errno == ENOENT ? 0 : -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    long size = ftell(file);
    if (size <= 0) {
        fclose(file);
        return 0;
    }
    if (fseek(file, -1, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    if (fgetc(file) == '\n') {
        fclose(file);
        return 0;
    }

    long last_newline = -1;
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    for (long position = 0; position < size; position++) {
        if (fgetc(file) == '\n') {
            last_newline = position;
        }
    }
    int fd = fileno(file);
    int result = ftruncate(fd, last_newline + 1);
    if (result == 0) {
        fflush(file);
        fsync(fd);
    }
    fclose(file);
    return result == 0 ? 1 : -1;
}

static void journal_path(
    char *output,
    size_t output_len,
    const char *directory,
    unsigned generation
)
{
    if (generation == 0) {
        snprintf(output, output_len, "%s/events.jsonl", directory);
    } else {
        snprintf(
            output, output_len, "%s/events.%u.jsonl", directory, generation
        );
    }
}

static int rotate_files(const char *directory, unsigned file_count)
{
    char source[192];
    char destination[192];
    for (unsigned generation = file_count - 1; generation > 0; generation--) {
        journal_path(source, sizeof(source), directory, generation - 1);
        journal_path(destination, sizeof(destination), directory, generation);
        if (unlink(destination) != 0 && errno != ENOENT) {
            return -1;
        }
        if (rename(source, destination) != 0 && errno != ENOENT) {
            return -1;
        }
    }
    return 0;
}

int event_core_append(
    const char *directory,
    const char *json,
    size_t json_len,
    bool critical,
    size_t max_file_bytes,
    unsigned file_count
)
{
    if (!directory || !json || json_len == 0 || max_file_bytes == 0 ||
        file_count < 2) {
        errno = EINVAL;
        return -1;
    }
    if (mkdir(directory, 0775) != 0 && errno != EEXIST) {
        return -1;
    }

    char active_path[192];
    journal_path(active_path, sizeof(active_path), directory, 0);
    struct stat info;
    if (stat(active_path, &info) == 0 &&
        (size_t)info.st_size + json_len + 1 > max_file_bytes) {
        if (rotate_files(directory, file_count) != 0) {
            return -1;
        }
    }

    FILE *file = fopen(active_path, "a");
    if (!file) {
        return -1;
    }
    bool ok = fwrite(json, 1, json_len, file) == json_len &&
              fputc('\n', file) != EOF &&
              fflush(file) == 0;
    if (ok && critical) {
        ok = fsync(fileno(file)) == 0;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    return ok ? 0 : -1;
}
