#include "webui_json_stream.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void webui_json_stream_init(
    webui_json_stream_t *stream,
    webui_json_stream_sink_t sink,
    void *sink_context
)
{
    memset(stream, 0, sizeof(*stream));
    stream->sink = sink;
    stream->sink_context = sink_context;
}

bool webui_json_stream_flush(webui_json_stream_t *stream)
{
    if (stream->failed) {
        return false;
    }
    if (stream->used == 0) {
        return true;
    }
    bool accepted = stream->sink && stream->sink(
        stream->sink_context, stream->chunk, stream->used
    );
    stream->used = 0;
    if (!accepted) {
        stream->failed = true;
    }
    return accepted;
}

bool webui_json_stream_write(
    webui_json_stream_t *stream,
    const char *data,
    size_t length
)
{
    while (length > 0) {
        size_t available = sizeof(stream->chunk) - stream->used;
        if (available == 0 && !webui_json_stream_flush(stream)) {
            return false;
        }
        available = sizeof(stream->chunk) - stream->used;
        size_t count = length < available ? length : available;
        memcpy(stream->chunk + stream->used, data, count);
        stream->used += count;
        data += count;
        length -= count;
    }
    return true;
}

bool webui_json_stream_literal(
    webui_json_stream_t *stream,
    const char *literal
)
{
    return webui_json_stream_write(stream, literal, strlen(literal));
}

bool webui_json_stream_string(
    webui_json_stream_t *stream,
    const char *value
)
{
    if (!webui_json_stream_literal(stream, "\"")) {
        return false;
    }
    const unsigned char *cursor =
        (const unsigned char *)(value ? value : "");
    while (*cursor) {
        const char *escape = NULL;
        switch (*cursor) {
        case '"':  escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\b': escape = "\\b";  break;
        case '\f': escape = "\\f";  break;
        case '\n': escape = "\\n";  break;
        case '\r': escape = "\\r";  break;
        case '\t': escape = "\\t";  break;
        default: break;
        }
        if (escape) {
            if (!webui_json_stream_literal(stream, escape)) {
                return false;
            }
        } else if (*cursor < 0x20) {
            char encoded[7];
            snprintf(encoded, sizeof(encoded), "\\u%04x", *cursor);
            if (!webui_json_stream_literal(stream, encoded)) {
                return false;
            }
        } else {
            char byte = (char)*cursor;
            if (!webui_json_stream_write(stream, &byte, 1)) {
                return false;
            }
        }
        cursor++;
    }
    return webui_json_stream_literal(stream, "\"");
}

bool webui_json_stream_value(
    webui_json_stream_t *stream,
    const cJSON *item
)
{
    if (!item || cJSON_IsNull(item) || cJSON_IsInvalid(item)) {
        return webui_json_stream_literal(stream, "null");
    }
    if (cJSON_IsFalse(item)) {
        return webui_json_stream_literal(stream, "false");
    }
    if (cJSON_IsTrue(item)) {
        return webui_json_stream_literal(stream, "true");
    }
    if (cJSON_IsNumber(item)) {
        if (!isfinite(item->valuedouble)) {
            return webui_json_stream_literal(stream, "null");
        }
        char number[32];
        int length = snprintf(
            number, sizeof(number), "%.17g", item->valuedouble
        );
        return length > 0 && (size_t)length < sizeof(number) &&
               webui_json_stream_write(stream, number, (size_t)length);
    }
    if (cJSON_IsString(item)) {
        return webui_json_stream_string(stream, item->valuestring);
    }
    if (cJSON_IsRaw(item)) {
        return webui_json_stream_literal(
            stream, item->valuestring ? item->valuestring : "null"
        );
    }
    if (cJSON_IsArray(item)) {
        if (!webui_json_stream_literal(stream, "[")) {
            return false;
        }
        const cJSON *child = item->child;
        bool first = true;
        while (child) {
            if ((!first && !webui_json_stream_literal(stream, ",")) ||
                !webui_json_stream_value(stream, child)) {
                return false;
            }
            first = false;
            child = child->next;
        }
        return webui_json_stream_literal(stream, "]");
    }
    if (cJSON_IsObject(item)) {
        if (!webui_json_stream_literal(stream, "{")) {
            return false;
        }
        bool first = true;
        if (!webui_json_stream_object_members(stream, item, &first)) {
            return false;
        }
        return webui_json_stream_literal(stream, "}");
    }
    return webui_json_stream_literal(stream, "null");
}

bool webui_json_stream_object_members(
    webui_json_stream_t *stream,
    const cJSON *object,
    bool *first
)
{
    const cJSON *child = object ? object->child : NULL;
    while (child) {
        if ((!*first && !webui_json_stream_literal(stream, ",")) ||
            !webui_json_stream_string(stream, child->string) ||
            !webui_json_stream_literal(stream, ":") ||
            !webui_json_stream_value(stream, child)) {
            return false;
        }
        *first = false;
        child = child->next;
    }
    return true;
}

bool webui_json_stream_object_fragments(
    webui_json_stream_t *stream,
    const webui_json_fragment_builder_t *builders,
    size_t builder_count
)
{
    bool first = true;
    bool encoded = webui_json_stream_literal(stream, "{");
    for (size_t i = 0; encoded && i < builder_count; i++) {
        cJSON *fragment = cJSON_CreateObject();
        if (!fragment) {
            stream->allocation_failed = true;
            stream->failed = true;
            return false;
        }
        builders[i](fragment);
        encoded = webui_json_stream_object_members(
            stream, fragment, &first
        );
        cJSON_Delete(fragment);
    }
    return encoded && webui_json_stream_literal(stream, "}");
}
