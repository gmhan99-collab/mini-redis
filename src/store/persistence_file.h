#ifndef MINI_REDIS_PERSISTENCE_FILE_H
#define MINI_REDIS_PERSISTENCE_FILE_H

#include <stdint.h>
#include <stdio.h>

/* Paths are relative to a single locked directory. */
typedef struct {
    const char *directory;
    FILE *lock;
} persistence_files;

int persistence_files_create(const char *directory);
int persistence_files_lock(persistence_files *files, int create);
FILE *persistence_file_open(const persistence_files *files, const char *name, const char *mode);
int persistence_file_missing(const persistence_files *files, const char *name);
int persistence_file_size(FILE *file, uint64_t *size);
int persistence_file_seek_end(FILE *file);
int persistence_file_sync(FILE *file, const char *operation);

typedef int (*persistence_file_writer)(FILE *, void *);
/* Returns -1 if publication failed and the old destination could not be restored. */
int persistence_file_replace(const persistence_files *files, const char *name,
                             const char *operation, persistence_file_writer write_file,
                             void *context, uint64_t *size);

#endif
