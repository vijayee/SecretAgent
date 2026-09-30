#define _DEFAULT_SOURCE

#include "platform_file.h"

#ifndef _WIN32
  #include <stdlib.h>
  #include <stdio.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <string.h>
  #include <errno.h>

  extern int mkdir_p(char* path);

  struct platform_file_t {
    int fd;
  };

  platform_file_t* platform_file_open(const char* path, int flags, int mode) {
    int fd = open(path, flags, mode);
    if (fd < 0) {
      return NULL;
    }
    platform_file_t* file = (platform_file_t*)calloc(1, sizeof(platform_file_t));
    if (file == NULL) {
      close(fd);
      return NULL;
    }
    file->fd = fd;
    return file;
  }

  void platform_file_close(platform_file_t* file) {
    if (file == NULL) {
      return;
    }
    close(file->fd);
    free(file);
  }

  ssize_t platform_file_read(platform_file_t* file, void* buf, size_t count) {
    return read(file->fd, buf, count);
  }

  ssize_t platform_file_write(platform_file_t* file, const void* buf, size_t count) {
    return write(file->fd, buf, count);
  }

  ssize_t platform_file_pread(platform_file_t* file, void* buf, size_t count, uint64_t offset) {
    return pread(file->fd, buf, count, (off_t)offset);
  }

  ssize_t platform_file_pwrite(platform_file_t* file, const void* buf, size_t count, uint64_t offset) {
    return pwrite(file->fd, buf, count, (off_t)offset);
  }

  int64_t platform_file_seek(platform_file_t* file, int64_t offset, int whence) {
    int w = (whence == PLATFORM_SEEK_SET) ? SEEK_SET :
            (whence == PLATFORM_SEEK_CUR) ? SEEK_CUR : SEEK_END;
    off_t result = lseek(file->fd, (off_t)offset, w);
    return (int64_t)result;
  }

  int platform_file_sync(platform_file_t* file) {
    if (file == NULL) return -1;
    return fsync(file->fd);
  }

  int platform_file_truncate(platform_file_t* file, uint64_t length) {
    if (file == NULL) return -1;
    return ftruncate(file->fd, (off_t)length);
  }

  int platform_file_exists(const char* path) {
    return access(path, F_OK) == 0 ? 1 : 0;
  }

  int platform_file_unlink(const char* path) {
    return unlink(path);
  }

  int platform_file_rename(const char* old_path, const char* new_path) {
    return rename(old_path, new_path);
  }

  int platform_mkdir(const char* path) {
    return mkdir_p((char*)path);
  }

  char* platform_temp_file_write(const uint8_t* data, size_t len) {
    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/liboffs_XXXXXX");
    int fd = mkstemp(tmpl);
    if (fd < 0) return NULL;
    ssize_t written = write(fd, data, len);
    close(fd);
    if (written != (ssize_t)len) {
      unlink(tmpl);
      return NULL;
    }
    char* path = strdup(tmpl);
    if (path == NULL) {
      unlink(tmpl);
      return NULL;
    }
    return path;
  }
#else
  /* Windows implementation — Win32 File API */
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <stdlib.h>
  #include <string.h>
  #include <errno.h>

  struct platform_file_t {
    HANDLE handle;
  };

  platform_file_t* platform_file_open(const char* path, int flags, int mode) {
    (void)mode;
    DWORD dwDesiredAccess = 0;
    DWORD dwCreationDisposition = OPEN_EXISTING;

    if (flags & PLATFORM_O_RDWR) {
      dwDesiredAccess = GENERIC_READ | GENERIC_WRITE;
    } else if (flags & PLATFORM_O_WRONLY) {
      dwDesiredAccess = GENERIC_WRITE;
    } else {
      dwDesiredAccess = GENERIC_READ;
    }

    if (flags & PLATFORM_O_CREAT) {
      if (flags & PLATFORM_O_TRUNC) {
        dwCreationDisposition = CREATE_ALWAYS;
      } else {
        dwCreationDisposition = OPEN_ALWAYS;
      }
    } else if (flags & PLATFORM_O_TRUNC) {
      dwCreationDisposition = TRUNCATE_EXISTING;
    }

    /* Convert UTF-8 path to wide string */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    WCHAR* wpath = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    if (wpath == NULL) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);

    HANDLE h = CreateFileW(wpath, dwDesiredAccess,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, dwCreationDisposition,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) {
      return NULL;
    }
    platform_file_t* file = (platform_file_t*)calloc(1, sizeof(platform_file_t));
    if (file == NULL) {
      CloseHandle(h);
      return NULL;
    }
    file->handle = h;
    return file;
  }

  void platform_file_close(platform_file_t* file) {
    if (file == NULL) return;
    CloseHandle(file->handle);
    free(file);
  }

  ssize_t platform_file_read(platform_file_t* file, void* buf, size_t count) {
    DWORD bytes_read = 0;
    if (!ReadFile(file->handle, buf, (DWORD)count, &bytes_read, NULL)) {
      return -1;
    }
    return (ssize_t)bytes_read;
  }

  ssize_t platform_file_write(platform_file_t* file, const void* buf, size_t count) {
    DWORD bytes_written = 0;
    if (!WriteFile(file->handle, buf, (DWORD)count, &bytes_written, NULL)) {
      return -1;
    }
    return (ssize_t)bytes_written;
  }

  ssize_t platform_file_pread(platform_file_t* file, void* buf, size_t count, uint64_t offset) {
    OVERLAPPED overlapped;
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.Offset = (DWORD)(offset & 0xFFFFFFFF);
    overlapped.OffsetHigh = (DWORD)(offset >> 32);
    DWORD bytes_read = 0;
    if (!ReadFile(file->handle, buf, (DWORD)count, &bytes_read, &overlapped)) {
      return -1;
    }
    return (ssize_t)bytes_read;
  }

  ssize_t platform_file_pwrite(platform_file_t* file, const void* buf, size_t count, uint64_t offset) {
    OVERLAPPED overlapped;
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.Offset = (DWORD)(offset & 0xFFFFFFFF);
    overlapped.OffsetHigh = (DWORD)(offset >> 32);
    DWORD bytes_written = 0;
    if (!WriteFile(file->handle, buf, (DWORD)count, &bytes_written, &overlapped)) {
      return -1;
    }
    return (ssize_t)bytes_written;
  }

  int64_t platform_file_seek(platform_file_t* file, int64_t offset, int whence) {
    DWORD move_method;
    switch (whence) {
      case PLATFORM_SEEK_SET: move_method = FILE_BEGIN; break;
      case PLATFORM_SEEK_CUR: move_method = FILE_CURRENT; break;
      case PLATFORM_SEEK_END: move_method = FILE_END; break;
      default: return -1;
    }
    LARGE_INTEGER li_offset;
    li_offset.QuadPart = offset;
    LARGE_INTEGER li_new;
    if (!SetFilePointerEx(file->handle, li_offset, &li_new, move_method)) {
      return -1;
    }
    return (int64_t)li_new.QuadPart;
  }

  int platform_file_sync(platform_file_t* file) {
    if (file == NULL) return -1;
    return FlushFileBuffers(file->handle) ? 0 : -1;
  }

  int platform_file_truncate(platform_file_t* file, uint64_t length) {
    if (file == NULL) return -1;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)length;
    if (!SetFilePointerEx(file->handle, li, NULL, FILE_BEGIN)) return -1;
    return SetEndOfFile(file->handle) ? 0 : -1;
  }

  int platform_file_exists(const char* path) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return 0;
    WCHAR* wpath = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    if (wpath == NULL) return 0;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
    DWORD attrs = GetFileAttributesW(wpath);
    free(wpath);
    return (attrs != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
  }

  int platform_file_unlink(const char* path) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return -1;
    WCHAR* wpath = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    if (wpath == NULL) return -1;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
    int result = DeleteFileW(wpath) ? 0 : -1;
    free(wpath);
    return result;
  }

  int platform_file_rename(const char* old_path, const char* new_path) {
    int old_wlen = MultiByteToWideChar(CP_UTF8, 0, old_path, -1, NULL, 0);
    int new_wlen = MultiByteToWideChar(CP_UTF8, 0, new_path, -1, NULL, 0);
    if (old_wlen <= 0 || new_wlen <= 0) return -1;
    WCHAR* wold = (WCHAR*)malloc(old_wlen * sizeof(WCHAR));
    WCHAR* wnew = (WCHAR*)malloc(new_wlen * sizeof(WCHAR));
    if (wold == NULL || wnew == NULL) {
      free(wold);
      free(wnew);
      return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, old_path, -1, wold, old_wlen);
    MultiByteToWideChar(CP_UTF8, 0, new_path, -1, wnew, new_wlen);
    int result = MoveFileExW(wold, wnew, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
    free(wold);
    free(wnew);
    return result;
  }

  /* Recursive mkdir using CreateDirectoryW */
  static int _mkdir_p_win(const char* path) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return -1;
    WCHAR* wpath = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    if (wpath == NULL) return -1;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);

    /* Walk the path creating each component */
    for (int i = 0; wpath[i] != L'\0'; i++) {
      if (wpath[i] == L'\\' || wpath[i] == L'/') {
        if (i == 0) continue; /* skip leading slash */
        WCHAR saved = wpath[i];
        wpath[i] = L'\0';
        CreateDirectoryW(wpath, NULL); /* ignore error if already exists */
        wpath[i] = saved;
      }
    }
    /* Create the final component */
    int result = CreateDirectoryW(wpath, NULL) ? 0 : -1;
    if (result != 0 && GetLastError() == ERROR_ALREADY_EXISTS) {
      result = 0;
    }
    free(wpath);
    return result;
  }

  int platform_mkdir(const char* path) {
    return _mkdir_p_win(path);
  }

  char* platform_temp_file_write(const uint8_t* data, size_t len) {
    char tmp_path[MAX_PATH];
    char tmp_file[MAX_PATH];
    if (GetTempPathA(sizeof(tmp_path), tmp_path) == 0) return NULL;
    if (GetTempFileNameA(tmp_path, "lof", 0, tmp_file) == 0) return NULL;

    HANDLE h = CreateFileA(tmp_file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    DWORD bytes_written = 0;
    BOOL ok = WriteFile(h, data, (DWORD)len, &bytes_written, NULL);
    CloseHandle(h);

    if (!ok || bytes_written != (DWORD)len) {
      DeleteFileA(tmp_file);
      return NULL;
    }

    char* path = strdup(tmp_file);
    if (path == NULL) {
      DeleteFileA(tmp_file);
      return NULL;
    }
    return path;
  }
#endif
