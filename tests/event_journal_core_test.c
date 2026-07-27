#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "event_journal_core.h"


static void read_file(const char *path, char *output, size_t output_len)
{
    FILE *file = fopen(path, "r");
    assert(file);
    size_t count = fread(output, 1, output_len - 1, file);
    output[count] = '\0';
    fclose(file);
}

static void test_redaction(void)
{
    const char *input =
        "{\"ssid\":\"field-net\",\"password\":\"wifi-secret\","
        "\"username\":\"broker-user\",\"token\":\"abc\",\"count\":4}";
    char output[256];
    assert(event_core_redact_json(input, output, sizeof(output)));
    assert(strstr(output, "field-net"));
    assert(!strstr(output, "wifi-secret"));
    assert(!strstr(output, "broker-user"));
    assert(!strstr(output, "\"abc\""));
    assert(strstr(output, "\"password\":\"[redacted]\""));
    assert(strstr(output, "\"count\":4"));
}

static void test_rate_limit(void)
{
    assert(!event_core_rate_limited(false, 0, 0, 1000));
    assert(event_core_rate_limited(true, 1000, 1500, 1000));
    assert(!event_core_rate_limited(true, 1000, 2000, 1000));
    assert(!event_core_rate_limited(true, 1000, 1001, 0));
}

static void test_rotation_repair_ordering_and_failure(void)
{
    char template[] = "/tmp/event-journal-test.XXXXXX";
    char *root = mkdtemp(template);
    assert(root);

    char events_dir[256];
    snprintf(events_dir, sizeof(events_dir), "%s/events", root);
    const char *event_one =
        "{\"boot_id\":\"boot-a\",\"event_sequence\":1,\"wall_time\":null}";
    const char *event_two =
        "{\"boot_id\":\"boot-a\",\"event_sequence\":2,\"wall_time\":0}";
    assert(event_core_append(
        events_dir, event_one, strlen(event_one), false, 100, 3
    ) == 0);
    assert(event_core_append(
        events_dir, event_two, strlen(event_two), true, 100, 3
    ) == 0);

    char active[256];
    char rotated[256];
    snprintf(active, sizeof(active), "%s/events.jsonl", events_dir);
    snprintf(rotated, sizeof(rotated), "%s/events.1.jsonl", events_dir);
    char contents[512];
    read_file(rotated, contents, sizeof(contents));
    assert(strstr(contents, "\"event_sequence\":1"));
    read_file(active, contents, sizeof(contents));
    assert(strstr(contents, "\"event_sequence\":2"));

    FILE *file = fopen(active, "a");
    assert(file);
    fputs("{\"partial\":", file);
    fclose(file);
    assert(event_core_repair_tail(active) == 1);
    read_file(active, contents, sizeof(contents));
    assert(strcmp(contents, "{\"boot_id\":\"boot-a\",\"event_sequence\":2,"
                            "\"wall_time\":0}\n") == 0);

    char blocked[256];
    snprintf(blocked, sizeof(blocked), "%s/not-a-directory", root);
    file = fopen(blocked, "w");
    assert(file);
    fputs("x", file);
    fclose(file);
    assert(event_core_append(
        blocked, event_one, strlen(event_one), false, 100, 3
    ) == -1);
}

int main(void)
{
    test_redaction();
    test_rate_limit();
    test_rotation_repair_ordering_and_failure();
    puts("event journal core tests: ok");
    return 0;
}
