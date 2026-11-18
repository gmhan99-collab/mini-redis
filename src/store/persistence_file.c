#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif

#include "persistence_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

static char *file_path(const persistence_files *files, const char *name, const char *suffix)
{
    size_t dir_len = strlen(files->directory);
    size_t tail_len = strlen(name) + strlen(suffix) + 2;
    if (dir_len > SIZE_MAX - tail_len) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    char *path = malloc(dir_len + tail_len);
    if (path) {
        snprintf(path, dir_len + tail_len, "%s/%s%s", files->directory, name, suffix);
    }
    return path;
}

#ifndef _WIN32
static int sync_directory(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return 0;
    }
    int synced = fsync(fd) == 0;
    close(fd);
    return synced;
}
#endif

int persistence_files_create(const char *directory)
{
#ifdef _WIN32
    return _mkdir(directory) == 0;
#else
    size_t length = strlen(directory);
    while (length && directory[length - 1] == '/') {
        length--;
    }
    const char *end = directory + length;
    while (end > directory && end[-1] != '/') {
        end--;
    }
    char *parent = end == directory ? strdup(".") : strndup(directory, (size_t)(end - directory));
    if (!parent) {
        return 0;
    }
    int created = mkdir(directory, 0700) == 0 && sync_directory(parent);
    free(parent);
    return created;
#endif
}

static FILE *open_exclusive(const char *path)
{
#ifdef _WIN32
    int fd = _open(path, _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        return NULL;
    }
    FILE *file = _fdopen(fd, "w+b");
    if (!file) {
        const int saved_errno = errno;
        _close(fd);
        remove(path);
        errno = saved_errno;
    }
#else
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return NULL;
    }
    FILE *file = fdopen(fd, "w+b");
    if (!file) {
        const int saved_errno = errno;
        close(fd);
        remove(path);
        errno = saved_errno;
    }
#endif
    return file;
}

FILE *persistence_file_open(const persistence_files *files, const char *name, const char *mode)
{
    char *path = file_path(files, name, "");
    if (!path) {
        return NULL;
    }
    FILE *file = mode ? fopen(path, mode) : open_exclusive(path);
    int saved_errno = errno;
    free(path);
    errno = saved_errno;
    return file;
}

int persistence_file_missing(const persistence_files *files, const char *name)
{
    char *path = file_path(files, name, "");
    if (!path) {
        return 0;
    }
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    int missing = attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
#else
    struct stat attributes;
    int missing = lstat(path, &attributes) != 0 && errno == ENOENT;
#endif
    free(path);
    return missing;
}

int persistence_files_lock(persistence_files *files, int create)
{
    files->lock = persistence_file_open(files, "lock", create ? NULL : "r+b");
    if (!files->lock) {
        return 0;
    }
#ifdef _WIN32
    OVERLAPPED position = {0};
    HANDLE handle = (HANDLE)_get_osfhandle(_fileno(files->lock));
    return LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                      0, 1, 0, &position) != 0;
#else
    return flock(fileno(files->lock), LOCK_EX | LOCK_NB) == 0;
#endif
}

int persistence_file_size(FILE *file, uint64_t *size)
{
#ifdef _WIN32
    struct _stat64 info;
    if (_fstat64(_fileno(file), &info) != 0) {
        return 0;
    }
#else
    struct stat info;
    if (fstat(fileno(file), &info) != 0) {
        return 0;
    }
#endif
#ifdef _WIN32
    int regular = (info.st_mode & _S_IFMT) == _S_IFREG;
#else
    int regular = S_ISREG(info.st_mode);
#endif
    if (!regular || info.st_size < 0) {
        return 0;
    }
    *size = (uint64_t)info.st_size;
    return 1;
}

int persistence_file_seek_end(FILE *file)
{
#ifdef _WIN32
    return _fseeki64(file, 0, SEEK_END) == 0;
#else
    return fseeko(file, 0, SEEK_END) == 0;
#endif
}

static int fail_sync(const char *operation)
{
#ifdef PERSISTENCE_TEST_HOOKS
    const char *failure = getenv("MINI_REDIS_TEST_FAIL_SYNC");
    return failure && !strcmp(failure, operation);
#else
    (void)operation;
    return 0;
#endif
}

int persistence_file_sync(FILE *file, const char *operation)
{
    if (fflush(file) != 0 || fail_sync(operation)) {
        return 0;
    }
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

static void report_file_collision(const char *operation, const char *path)
{
    fprintf(stderr, "Persistence operation '%s' blocked because private file '%s' already exists. "
                    "Inspect the file before retrying.\n", operation, path);
}

#ifndef _WIN32
static int replace_and_sync(const char *temp, const char *path, const char *backup,
                            const char *directory, const char *operation)
{
    const int had_previous = link(path, backup) == 0;
    if (!had_previous && errno != ENOENT) {
        if (errno == EEXIST) {
            report_file_collision(operation, backup);
        }
        return 0;
    }
    if (!sync_directory(directory) || rename(temp, path) != 0) {
        if (had_previous) {
            unlink(backup);
        }
        return 0;
    }
    if ((!strcmp(operation, "snapshot") && fail_sync("snapshot-directory")) ||
        !sync_directory(directory)) {
        int restored = had_previous ? rename(backup, path) == 0 : unlink(path) == 0;
        return restored && sync_directory(directory) ? 0 : -1;
    }
    if (had_previous) {
        /* The destination is already durable. The spare link is housekeeping. */
        unlink(backup);
    }
    return 1;
}
#endif

int persistence_file_replace(const persistence_files *files, const char *name,
                             const char *operation, persistence_file_writer write_file,
                             void *context, uint64_t *size)
{
    char *path = file_path(files, name, "");
    char *temp = file_path(files, name, ".tmp");
#ifndef _WIN32
    char *backup = file_path(files, name, ".previous");
    if (!backup) {
        free(temp);
        free(path);
        return 0;
    }
#endif
    FILE *file = temp ? open_exclusive(temp) : NULL;
    if (temp && !file && errno == EEXIST) {
        report_file_collision(operation, temp);
    }
    int result = 0;
    uint64_t bytes;
    if (file) {
        int ready = path && write_file(file, context) && persistence_file_sync(file, operation)
                    && persistence_file_size(file, &bytes);
        if (fclose(file) == 0 && ready) {
#ifdef _WIN32
            result = MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
            result = replace_and_sync(temp, path, backup, files->directory, operation);
#endif
        }
        if (result != 1) {
            remove(temp);
        } else if (size) {
            *size = bytes;
        }
    }
#ifndef _WIN32
    free(backup);
#endif
    free(temp);
    free(path);
    return result;
}
