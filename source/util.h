#pragma once

#include <switch.h>

#define sizearray(a) (sizeof(a) / sizeof((a)[0]))

#define TITLE_ID 0x4242424242424242
#define CONFIGPATH "/config/sys-httpd/config.ini"

#define R_ASSERT(res_expr)            \
    ({                                \
        const Result rc = (res_expr); \
        if (R_FAILED(rc))             \
        {                             \
            fatalThrow(rc);           \
        }                             \
    })

Result pauseInit();
void pauseExit();
bool isPaused();
void setPaused(bool newPaused);
