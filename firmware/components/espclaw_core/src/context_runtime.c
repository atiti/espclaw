#include "espclaw/context_runtime.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "espclaw/workspace.h"

#define ESPCLAW_CONTEXT_DEFAULT_CHUNK_BYTES 2048U
#define ESPCLAW_CONTEXT_MAX_CHUNK_BYTES 8192U
#define ESPCLAW_CONTEXT_DEFAULT_SEARCH_LIMIT 3U
#define ESPCLAW_CONTEXT_MAX_SEARCH_LIMIT 8U
#define ESPCLAW_CONTEXT_MAX_TERMS 8U

typedef struct {
    size_t index;
    size_t start;
    size_t length;
    size_t score;
    char *text;
} espclaw_context_search_hit_t;

static bool context_path_valid(const char *relative_path)
{
    size_t index;

    if (relative_path == NULL || relative_path[0] == '\0' || relative_path[0] == '/') {
        return false;
    }
    if (strstr(relative_path, "..") != NULL) {
        return false;
    }
    for (index = 0; relative_path[index] != '\0'; ++index) {
        unsigned char c = (unsigned char)relative_path[index];

        if (!(isalnum(c) || c == '_' || c == '-' || c == '.' || c == '/')) {
            return false;
        }
    }
    return true;
}

static size_t normalize_chunk_bytes(size_t chunk_bytes)
{
    if (chunk_bytes == 0U) {
        return ESPCLAW_CONTEXT_DEFAULT_CHUNK_BYTES;
    }
    if (chunk_bytes > ESPCLAW_CONTEXT_MAX_CHUNK_BYTES) {
        return ESPCLAW_CONTEXT_MAX_CHUNK_BYTES;
    }
    return chunk_bytes;
}

static size_t normalize_search_limit(size_t limit)
{
    if (limit == 0U) {
        return ESPCLAW_CONTEXT_DEFAULT_SEARCH_LIMIT;
    }
    if (limit > ESPCLAW_CONTEXT_MAX_SEARCH_LIMIT) {
        return ESPCLAW_CONTEXT_MAX_SEARCH_LIMIT;
    }
    return limit;
}

static size_t append_json_escaped(char *buffer, size_t buffer_size, size_t used, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)(text != NULL ? text : "");

    if (buffer == NULL || buffer_size == 0 || used >= buffer_size) {
        return used;
    }
    buffer[used++] = '"';
    while (*cursor != '\0' && used + 2 < buffer_size) {
        if (*cursor == '\\' || *cursor == '"') {
            buffer[used++] = '\\';
            buffer[used++] = (char)*cursor;
        } else if (*cursor == '\n') {
            buffer[used++] = '\\';
            buffer[used++] = 'n';
        } else if (*cursor == '\r') {
            buffer[used++] = '\\';
            buffer[used++] = 'r';
        } else if (*cursor == '\t') {
            buffer[used++] = '\\';
            buffer[used++] = 't';
        } else {
            buffer[used++] = (char)*cursor;
        }
        cursor++;
    }
    if (used < buffer_size) {
        buffer[used++] = '"';
    }
    if (used < buffer_size) {
        buffer[used] = '\0';
    } else {
        buffer[buffer_size - 1] = '\0';
    }
    return used;
}

static int open_context_file(
    const char *workspace_root,
    const char *relative_path,
    FILE **handle_out,
    size_t *total_bytes_out
)
{
    char absolute_path[512];
    struct stat file_stat;
    FILE *handle;

    if (handle_out == NULL || total_bytes_out == NULL || workspace_root == NULL || !context_path_valid(relative_path) ||
        espclaw_workspace_resolve_path(workspace_root, relative_path, absolute_path, sizeof(absolute_path)) != 0 ||
        stat(absolute_path, &file_stat) != 0) {
        return -1;
    }
    handle = fopen(absolute_path, "rb");
    if (handle == NULL) {
        return -1;
    }
    *handle_out = handle;
    *total_bytes_out = (size_t)file_stat.st_size;
    return 0;
}

static size_t chunk_count_for_size(size_t total_bytes, size_t chunk_bytes)
{
    if (chunk_bytes == 0U) {
        return 0U;
    }
    if (total_bytes == 0U) {
        return 1U;
    }
    return (total_bytes + chunk_bytes - 1U) / chunk_bytes;
}

static int load_chunk_text(
    const char *workspace_root,
    const char *relative_path,
    size_t chunk_index,
    size_t chunk_bytes,
    char *text_out,
    size_t text_out_size,
    size_t *total_bytes_out,
    size_t *chunk_count_out,
    size_t *chunk_start_out,
    size_t *chunk_length_out
)
{
    FILE *handle = NULL;
    size_t total_bytes = 0;
    size_t chunk_count;
    size_t offset;
    size_t bytes_to_read;
    size_t bytes_read;

    if (text_out == NULL || text_out_size == 0 || total_bytes_out == NULL || chunk_count_out == NULL ||
        chunk_start_out == NULL || chunk_length_out == NULL) {
        return -1;
    }
    text_out[0] = '\0';
    chunk_bytes = normalize_chunk_bytes(chunk_bytes);

    if (open_context_file(workspace_root, relative_path, &handle, &total_bytes) != 0) {
        return -1;
    }

    chunk_count = chunk_count_for_size(total_bytes, chunk_bytes);
    if (chunk_index >= chunk_count) {
        fclose(handle);
        return -1;
    }

    offset = chunk_index * chunk_bytes;
    bytes_to_read = total_bytes > offset ? total_bytes - offset : 0U;
    if (bytes_to_read > chunk_bytes) {
        bytes_to_read = chunk_bytes;
    }
    if (bytes_to_read >= text_out_size) {
        bytes_to_read = text_out_size - 1U;
    }

    if (fseek(handle, (long)offset, SEEK_SET) != 0) {
        fclose(handle);
        return -1;
    }
    bytes_read = fread(text_out, 1U, bytes_to_read, handle);
    fclose(handle);
    text_out[bytes_read] = '\0';

    *total_bytes_out = total_bytes;
    *chunk_count_out = chunk_count;
    *chunk_start_out = offset;
    *chunk_length_out = bytes_read;
    return 0;
}

static void lower_in_place(char *text)
{
    size_t index;

    if (text == NULL) {
        return;
    }
    for (index = 0; text[index] != '\0'; ++index) {
        text[index] = (char)tolower((unsigned char)text[index]);
    }
}

static size_t split_query_terms(const char *query, char terms[][32], size_t max_terms)
{
    char scratch[ESPCLAW_CONTEXT_QUERY_MAX + 1];
    char *token;
    char *save_ptr = NULL;
    size_t count = 0;

    if (query == NULL || query[0] == '\0' || max_terms == 0U) {
        return 0U;
    }
    snprintf(scratch, sizeof(scratch), "%s", query);
    lower_in_place(scratch);
    token = strtok_r(scratch, " \t\r\n", &save_ptr);
    while (token != NULL && count < max_terms) {
        if (token[0] != '\0') {
            snprintf(terms[count], 32U, "%s", token);
            count++;
        }
        token = strtok_r(NULL, " \t\r\n", &save_ptr);
    }
    return count;
}

static size_t count_term_hits(const char *haystack_lower, const char *needle_lower)
{
    const char *cursor = haystack_lower;
    size_t count = 0U;
    size_t needle_length;

    if (haystack_lower == NULL || needle_lower == NULL || needle_lower[0] == '\0') {
        return 0U;
    }
    needle_length = strlen(needle_lower);
    while ((cursor = strstr(cursor, needle_lower)) != NULL) {
        count++;
        cursor += needle_length;
    }
    return count;
}

static void maybe_record_search_hit(
    espclaw_context_search_hit_t *hits,
    size_t max_hits,
    size_t chunk_index,
    size_t chunk_start,
    const char *text,
    size_t text_length,
    size_t score
)
{
    size_t slot = max_hits;
    size_t worst_score = (size_t)-1;
    size_t index;

    if (hits == NULL || max_hits == 0U || text == NULL || score == 0U) {
        return;
    }
    for (index = 0; index < max_hits; ++index) {
        if (hits[index].text == NULL) {
            slot = index;
            break;
        }
        if (hits[index].score < worst_score) {
            worst_score = hits[index].score;
            slot = index;
        }
    }
    if (slot >= max_hits) {
        return;
    }
    if (hits[slot].text != NULL && hits[slot].score >= score) {
        return;
    }
    if (hits[slot].text != NULL) {
        free(hits[slot].text);
        hits[slot].text = NULL;
    }
    hits[slot].text = (char *)calloc(text_length + 1U, 1U);
    if (hits[slot].text == NULL) {
        return;
    }
    memcpy(hits[slot].text, text, text_length);
    hits[slot].text[text_length] = '\0';
    hits[slot].index = chunk_index;
    hits[slot].start = chunk_start;
    hits[slot].length = text_length;
    hits[slot].score = score;
}

static int compare_hits_desc(const void *lhs, const void *rhs)
{
    const espclaw_context_search_hit_t *a = (const espclaw_context_search_hit_t *)lhs;
    const espclaw_context_search_hit_t *b = (const espclaw_context_search_hit_t *)rhs;

    if (a->text == NULL && b->text == NULL) {
        return 0;
    }
    if (a->text == NULL) {
        return 1;
    }
    if (b->text == NULL) {
        return -1;
    }
    if (a->score < b->score) {
        return 1;
    }
    if (a->score > b->score) {
        return -1;
    }
    if (a->index > b->index) {
        return 1;
    }
    if (a->index < b->index) {
        return -1;
    }
    return 0;
}

int espclaw_context_render_chunks_json(
    const char *workspace_root,
    const char *relative_path,
    size_t chunk_bytes,
    char *buffer,
    size_t buffer_size
)
{
    FILE *handle = NULL;
    size_t total_bytes = 0;
    size_t chunk_count;
    size_t used = 0;

    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }
    buffer[0] = '\0';
    chunk_bytes = normalize_chunk_bytes(chunk_bytes);
    if (open_context_file(workspace_root, relative_path, &handle, &total_bytes) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"context file not found\"}");
        return -1;
    }
    fclose(handle);
    chunk_count = chunk_count_for_size(total_bytes, chunk_bytes);

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"ok\":true,\"path\":");
    used = append_json_escaped(buffer, buffer_size, used, relative_path);
    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        ",\"chunk_bytes\":%lu,\"total_bytes\":%lu,\"chunk_count\":%lu}",
        (unsigned long)chunk_bytes,
        (unsigned long)total_bytes,
        (unsigned long)chunk_count
    );
    return 0;
}

int espclaw_context_render_chunk_json(
    const char *workspace_root,
    const char *relative_path,
    size_t chunk_index,
    size_t chunk_bytes,
    char *buffer,
    size_t buffer_size
)
{
    char *text = NULL;
    size_t total_bytes = 0;
    size_t chunk_count = 0;
    size_t chunk_start = 0;
    size_t chunk_length = 0;
    size_t used = 0;
    int rc = -1;

    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }
    buffer[0] = '\0';
    chunk_bytes = normalize_chunk_bytes(chunk_bytes);
    text = (char *)calloc(chunk_bytes + 1U, 1U);
    if (text == NULL) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"out of memory\"}");
        return -1;
    }
    if (load_chunk_text(
            workspace_root,
            relative_path,
            chunk_index,
            chunk_bytes,
            text,
            chunk_bytes + 1U,
            &total_bytes,
            &chunk_count,
            &chunk_start,
            &chunk_length) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"context chunk not found\"}");
        goto cleanup;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"ok\":true,\"path\":");
    used = append_json_escaped(buffer, buffer_size, used, relative_path);
    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        ",\"chunk_index\":%lu,\"chunk_bytes\":%lu,\"chunk_start\":%lu,\"chunk_length\":%lu,\"chunk_count\":%lu,\"total_bytes\":%lu,\"text\":",
        (unsigned long)chunk_index,
        (unsigned long)chunk_bytes,
        (unsigned long)chunk_start,
        (unsigned long)chunk_length,
        (unsigned long)chunk_count,
        (unsigned long)total_bytes
    );
    used = append_json_escaped(buffer, buffer_size, used, text);
    used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    rc = 0;

cleanup:
    free(text);
    return rc;
}

int espclaw_context_search_json(
    const char *workspace_root,
    const char *relative_path,
    const char *query,
    size_t chunk_bytes,
    size_t limit,
    char *buffer,
    size_t buffer_size
)
{
    FILE *handle = NULL;
    size_t total_bytes = 0;
    size_t chunk_count;
    size_t chunk_index;
    char terms[ESPCLAW_CONTEXT_MAX_TERMS][32];
    size_t term_count;
    espclaw_context_search_hit_t hits[ESPCLAW_CONTEXT_MAX_SEARCH_LIMIT];
    char *text = NULL;
    char *lower = NULL;
    size_t used = 0;
    size_t index;
    int rc = -1;

    if (buffer == NULL || buffer_size == 0 || query == NULL || query[0] == '\0') {
        return -1;
    }
    buffer[0] = '\0';
    chunk_bytes = normalize_chunk_bytes(chunk_bytes);
    limit = normalize_search_limit(limit);
    term_count = split_query_terms(query, terms, ESPCLAW_CONTEXT_MAX_TERMS);
    if (term_count == 0U) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing query\"}");
        return -1;
    }
    if (open_context_file(workspace_root, relative_path, &handle, &total_bytes) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"context file not found\"}");
        return -1;
    }
    fclose(handle);
    chunk_count = chunk_count_for_size(total_bytes, chunk_bytes);
    memset(hits, 0, sizeof(hits));

    text = (char *)calloc(chunk_bytes + 1U, 1U);
    lower = (char *)calloc(chunk_bytes + 1U, 1U);
    if (text == NULL || lower == NULL) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"out of memory\"}");
        goto cleanup;
    }

    for (chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        size_t loaded_total = 0;
        size_t loaded_chunk_count = 0;
        size_t chunk_start = 0;
        size_t chunk_length = 0;
        size_t score = 0U;
        size_t term_index;

        if (load_chunk_text(
                workspace_root,
                relative_path,
                chunk_index,
                chunk_bytes,
                text,
                chunk_bytes + 1U,
                &loaded_total,
                &loaded_chunk_count,
                &chunk_start,
                &chunk_length) != 0) {
            continue;
        }
        memcpy(lower, text, chunk_length + 1U);
        lower_in_place(lower);
        for (term_index = 0; term_index < term_count; ++term_index) {
            score += count_term_hits(lower, terms[term_index]);
        }
        maybe_record_search_hit(hits, limit, chunk_index, chunk_start, text, chunk_length, score);
    }

    qsort(hits, limit, sizeof(hits[0]), compare_hits_desc);
    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"ok\":true,\"path\":");
    used = append_json_escaped(buffer, buffer_size, used, relative_path);
    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        ",\"query\":"
    );
    used = append_json_escaped(buffer, buffer_size, used, query);
    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        ",\"chunk_bytes\":%lu,\"total_bytes\":%lu,\"chunk_count\":%lu,\"results\":[",
        (unsigned long)chunk_bytes,
        (unsigned long)total_bytes,
        (unsigned long)chunk_count
    );
    for (index = 0; index < limit && hits[index].text != NULL && used + 64 < buffer_size; ++index) {
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "%s{\"chunk_index\":%lu,\"chunk_start\":%lu,\"chunk_length\":%lu,\"score\":%lu,\"text\":",
            index == 0 ? "" : ",",
            (unsigned long)hits[index].index,
            (unsigned long)hits[index].start,
            (unsigned long)hits[index].length,
            (unsigned long)hits[index].score
        );
        used = append_json_escaped(buffer, buffer_size, used, hits[index].text);
        used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]}");
    rc = 0;

cleanup:
    for (index = 0; index < ESPCLAW_CONTEXT_MAX_SEARCH_LIMIT; ++index) {
        free(hits[index].text);
    }
    free(lower);
    free(text);
    return rc;
}
