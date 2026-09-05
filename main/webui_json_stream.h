#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEBUI_JSON_STREAM_CHUNK_BYTES 512

typedef bool (*webui_json_stream_sink_t)(
    void *context,
    const char *data,
    size_t length
);

typedef void (*webui_json_fragment_builder_t)(cJSON *root);

typedef struct {
    webui_json_stream_sink_t sink;
    void *sink_context;
    char chunk[WEBUI_JSON_STREAM_CHUNK_BYTES];
    size_t used;
    bool failed;
    bool allocation_failed;
} webui_json_stream_t;

void webui_json_stream_init(
    webui_json_stream_t *stream,
    webui_json_stream_sink_t sink,
    void *sink_context
);

bool webui_json_stream_flush(webui_json_stream_t *stream);
bool webui_json_stream_write(
    webui_json_stream_t *stream,
    const char *data,
    size_t length
);
bool webui_json_stream_literal(
    webui_json_stream_t *stream,
    const char *literal
);
bool webui_json_stream_string(
    webui_json_stream_t *stream,
    const char *value
);
bool webui_json_stream_value(
    webui_json_stream_t *stream,
    const cJSON *item
);
bool webui_json_stream_object_members(
    webui_json_stream_t *stream,
    const cJSON *object,
    bool *first
);
bool webui_json_stream_object_fragments(
    webui_json_stream_t *stream,
    const webui_json_fragment_builder_t *builders,
    size_t builder_count
);

#ifdef __cplusplus
}
#endif
