#ifndef OFFS_PLATFORM_FILE_H
#define OFFS_PLATFORM_FILE_H

#include <stdint.h>
#include <stddef.h>
#include "platform_posix_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct platform_file_t platform_file_t;

/* Open flags — values match POSIX. Windows implementation maps them internally. */
#define PLATFORM_O_RDONLY  0
#define PLATFORM_O_WRONLY  1
#define PLATFORM_O_RDWR    2
#define PLATFORM_O_CREAT   0100
#define PLATFORM_O_TRUNC   01000
#define PLATFORM_O_BINARY  0  /* no-op on POSIX */

platform_file_t* platform_file_open(const char* path, int flags, int mode);
void platform_file_close(platform_file_t* file);
ssize_t platform_file_read(platform_file_t* file, void* buf, size_t count);
ssize_t platform_file_write(platform_file_t* file, const void* buf, size_t count);
ssize_t platform_file_pread(platform_file_t* file, void* buf, size_t count, uint64_t offset);
ssize_t platform_file_pwrite(platform_file_t* file, const void* buf, size_t count, uint64_t offset);
int64_t platform_file_seek(platform_file_t* file, int64_t offset, int whence);
int platform_file_sync(platform_file_t* file);
int platform_file_truncate(platform_file_t* file, uint64_t length);

#define PLATFORM_SEEK_SET  0
#define PLATFORM_SEEK_CUR  1
#define PLATFORM_SEEK_END  2

int platform_file_exists(const char* path);
int platform_file_unlink(const char* path);
/* Rename/move old_path to new_path, replacing new_path when it already
   exists (POSIX rename() / Win32 MoveFileExW MOVEFILE_REPLACE_EXISTING).
   Returns 0 on success, -1 on failure. */
int platform_file_rename(const char* old_path, const char* new_path);
int platform_mkdir(const char* path);  /* recursive */

// Write data to a uniquely-named temporary file. Returns a strdup'd path
// the caller must free (and unlink when done), or NULL on failure.
char* platform_temp_file_write(const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* OFFS_PLATFORM_FILE_H */
