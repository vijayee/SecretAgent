//
// Created by victor on 5/22/26.
//

#include "validation.h"
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

static bool _is_printable_or_special(int chr) {
  return isprint(chr) || chr == '/' || chr == '-' || chr == '+' || chr == '.';
}

int validate_content_type(const char* content_type) {
  if (content_type == NULL) return -1;

  size_t length = strlen(content_type);
  if (length == 0 || length > OFFS_MAX_CONTENT_TYPE_LEN) return -1;

  for (size_t index = 0; index < length; index++) {
    if (!_is_printable_or_special((unsigned char)content_type[index])) return -1;
  }
  return 0;
}

int validate_file_name(const char* file_name) {
  if (file_name == NULL) return -1;

  size_t length = strlen(file_name);
  if (length == 0 || length > OFFS_MAX_FILE_NAME_LEN) return -1;

  if (strstr(file_name, "..") != NULL) return -1;
  if (strchr(file_name, '/') != NULL) return -1;

  for (size_t index = 0; index < length; index++) {
    if (!isprint((unsigned char)file_name[index])) return -1;
  }
  return 0;
}

int validate_ori_string(const char* ori) {
  if (ori == NULL) return -1;

  size_t length = strlen(ori);
  if (length == 0 || length > OFFS_MAX_ORI_STRING_LEN) return -1;

  if (strncmp(ori, "https://", 8) != 0 && strncmp(ori, "http://", 7) != 0) {
    return -1;
  }

  if (strstr(ori, "/offsystem/v3/") == NULL) return -1;

  return 0;
}
