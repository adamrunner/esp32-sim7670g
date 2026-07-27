#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Portable journal primitives shared by firmware and host-side fault tests.
// All functions return quickly except event_core_append(), which is called only
// by the dedicated journal worker task in firmware.

bool event_core_redact_json(const char *input, char *output, size_t output_len);

bool event_core_rate_limited(
    bool previously_emitted,
    uint64_t last_emitted_ms,
    uint64_t now_ms,
    uint32_t interval_ms
);

// Remove an incomplete final line left by an interrupted append. Returns 1
// when a tail was repaired, 0 when no repair was necessary, and -1 on error.
int event_core_repair_tail(const char *path);

// Append exactly one JSONL record. Rotation is deterministic: events.jsonl is
// active, then events.1.jsonl through events.(file_count-1).jsonl. `critical`
// adds fsync after fflush. Returns 0 on success and -1 on any failure.
int event_core_append(
    const char *directory,
    const char *json,
    size_t json_len,
    bool critical,
    size_t max_file_bytes,
    unsigned file_count
);
