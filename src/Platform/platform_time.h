#ifndef SA_PLATFORM_TIME_H
#define SA_PLATFORM_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void platform_sleep_ms(unsigned int ms);
uint64_t platform_monotonic_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* SA_PLATFORM_TIME_H */
