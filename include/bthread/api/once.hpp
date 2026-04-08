#pragma once

#include "bthread.h"

#ifdef __cplusplus
extern "C" {
#endif

int bthread_once(bthread_once_t* once, void (*init_routine)(void));

#ifdef __cplusplus
}
#endif