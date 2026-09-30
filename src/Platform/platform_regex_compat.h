//
// PCRE2-backed POSIX regex shim. The Linux/POSIX path uses <regex.h>;
// this header is the Windows equivalent, providing the same symbols
// (`regcomp`, `regexec`, `regfree`, `regerror`, `regex_t`, `regmatch_t`,
// `REG_EXTENDED`, `REG_NOSUB`, `REG_NOMATCH`, ...) by way of PCRE2's
// POSIX compatibility wrapper (pcre2posix.h). The rest of liboffs
// includes this header on `_WIN32` and <regex.h> elsewhere — the API
// surface is identical, so no caller changes are required.
//
// The actual PCRE2 build is wired in the top-level CMakeLists.txt
// (deps/pcre2 git submodule, pcre2-8-static + pcre2-posix-static libs).
// The struct field `regex_t.re_nsub` and the regmatch_t.rm_so/rm_eo
// fields used by http_route.c are present in PCRE2's regex_t/regmatch_t.
//
#ifdef _WIN32
  #include <pcre2posix.h>
#endif
