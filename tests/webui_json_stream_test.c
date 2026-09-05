#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "webui_json_stream.h"

typedef struct {
    char output[4096];
    size_t length;
    size_t calls;
    size_t max_chunk;
    size_t fail_on_call;
} collecting_sink_t;

static bool collect(void *context, const char *data, size_t length)
{
    collecting_sink_t *sink = context;
    sink->calls++;
    if (sink->fail_on_call && sink->calls == sink->fail_on_call) {
        return false;
    }
    assert(length <= WEBUI_JSON_STREAM_CHUNK_BYTES);
    assert(sink->length + length < sizeof(sink->output));
    memcpy(sink->output + sink->length, data, length);
    sink->length += length;
    sink->output[sink->length] = '\0';
    if (length > sink->max_chunk) {
        sink->max_chunk = length;
    }
    return true;
}

static cJSON *sample_value(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "escaped", "quote \" slash \\ line\n\t\x01");
    cJSON_AddNumberToObject(root, "number", 12.5);
    cJSON_AddNullToObject(root, "nothing");
    cJSON_AddBoolToObject(root, "enabled", true);
    cJSON *nested = cJSON_AddObjectToObject(root, "nested");
    cJSON *array = cJSON_AddArrayToObject(nested, "values");
    cJSON_AddItemToArray(array, cJSON_CreateNumber(-3));
    cJSON_AddItemToArray(array, cJSON_CreateString("value"));
    return root;
}

static void test_values_and_types(void)
{
    collecting_sink_t sink = {0};
    webui_json_stream_t stream;
    webui_json_stream_init(&stream, collect, &sink);
    cJSON *root = sample_value();
    assert(webui_json_stream_value(&stream, root));
    assert(webui_json_stream_flush(&stream));
    cJSON_Delete(root);

    cJSON *parsed = cJSON_Parse(sink.output);
    assert(cJSON_IsObject(parsed));
    assert(strcmp(
        cJSON_GetObjectItemCaseSensitive(parsed, "escaped")->valuestring,
        "quote \" slash \\ line\n\t\x01"
    ) == 0);
    assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(parsed, "number")));
    assert(cJSON_GetObjectItemCaseSensitive(parsed, "number")->valuedouble == 12.5);
    assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(parsed, "nothing")));
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(parsed, "enabled")));
    cJSON *nested = cJSON_GetObjectItemCaseSensitive(parsed, "nested");
    cJSON *array = cJSON_GetObjectItemCaseSensitive(nested, "values");
    assert(cJSON_IsArray(array));
    assert(cJSON_GetArraySize(array) == 2);
    assert(cJSON_GetArrayItem(array, 0)->valuedouble == -3);
    assert(strcmp(cJSON_GetArrayItem(array, 1)->valuestring, "value") == 0);
    cJSON_Delete(parsed);
    printf("encoded-json:%s\n", sink.output);
}

static void test_chunk_boundary_and_sink_failure(void)
{
    char long_value[1301];
    memset(long_value, 'x', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';

    collecting_sink_t sink = {0};
    webui_json_stream_t stream;
    webui_json_stream_init(&stream, collect, &sink);
    assert(webui_json_stream_string(&stream, long_value));
    assert(webui_json_stream_flush(&stream));
    assert(sink.calls == 3);
    assert(sink.max_chunk == WEBUI_JSON_STREAM_CHUNK_BYTES);
    cJSON *parsed = cJSON_Parse(sink.output);
    assert(cJSON_IsString(parsed));
    assert(strlen(parsed->valuestring) == strlen(long_value));
    cJSON_Delete(parsed);

    collecting_sink_t failing = {.fail_on_call = 2};
    webui_json_stream_init(&stream, collect, &failing);
    assert(!webui_json_stream_string(&stream, long_value));
    assert(stream.failed);
    assert(!webui_json_stream_flush(&stream));
    assert(failing.calls == 2);
}

static size_t outstanding_allocations;
static size_t fragment_entry_allocations[2];

static void *tracked_malloc(size_t size)
{
    void *memory = malloc(size);
    if (memory) outstanding_allocations++;
    return memory;
}

static void tracked_free(void *memory)
{
    if (memory) outstanding_allocations--;
    free(memory);
}

static void first_fragment(cJSON *root)
{
    fragment_entry_allocations[0] = outstanding_allocations;
    cJSON_AddStringToObject(root, "first", "one");
}

static void second_fragment(cJSON *root)
{
    fragment_entry_allocations[1] = outstanding_allocations;
    cJSON_AddNumberToObject(root, "second", 2);
}

static void test_one_fragment_lifetime(void)
{
    cJSON_Hooks hooks = {
        .malloc_fn = tracked_malloc,
        .free_fn = tracked_free,
    };
    cJSON_InitHooks(&hooks);
    collecting_sink_t sink = {0};
    webui_json_stream_t stream;
    webui_json_stream_init(&stream, collect, &sink);
    const webui_json_fragment_builder_t builders[] = {
        first_fragment,
        second_fragment,
    };
    assert(webui_json_stream_object_fragments(
        &stream, builders, sizeof(builders) / sizeof(builders[0])
    ));
    assert(webui_json_stream_flush(&stream));
    assert(fragment_entry_allocations[0] == 1);
    assert(fragment_entry_allocations[1] == 1);
    assert(outstanding_allocations == 0);
    assert(strcmp(sink.output, "{\"first\":\"one\",\"second\":2}") == 0);
    cJSON_InitHooks(NULL);
}

int main(void)
{
    test_values_and_types();
    test_chunk_boundary_and_sink_failure();
    test_one_fragment_lifetime();
    puts("webui JSON stream tests: ok");
    return 0;
}
