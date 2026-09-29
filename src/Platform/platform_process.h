#ifndef SA_PLATFORM_PROCESS_H
#define SA_PLATFORM_PROCESS_H

#ifdef __cplusplus
extern "C" {
#endif

int platform_getpid(void);
int platform_core_count(void);

#ifdef __cplusplus
}
#endif

#endif /* SA_PLATFORM_PROCESS_H */
