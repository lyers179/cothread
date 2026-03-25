#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct bthread_once_t;

int bthread_once(bthread_once_t* once, void (*init_routine)(void));

#ifdef __cplusplus
}
#endif