#ifndef ESPCLAW_WORKSPACE_H
#define ESPCLAW_WORKSPACE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *relative_path;
    const char *default_content;
} espclaw_workspace_file_t;

size_t espclaw_workspace_file_count(void);
const espclaw_workspace_file_t *espclaw_workspace_file_at(size_t index);
const espclaw_workspace_file_t *espclaw_find_workspace_file(const char *relative_path);
bool espclaw_workspace_is_control_file(const char *relative_path);
int espclaw_workspace_resolve_path(
    const char *workspace_root,
    const char *relative_path,
    char *buffer,
    size_t buffer_size
);
int espclaw_workspace_bootstrap(const char *workspace_root);
int espclaw_workspace_read_file(
    const char *workspace_root,
    const char *relative_path,
    char *buffer,
    size_t buffer_size
);
int espclaw_workspace_write_file(
    const char *workspace_root,
    const char *relative_path,
    const char *content
);

#endif
