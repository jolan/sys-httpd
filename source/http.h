// This file is under the terms of the unlicense (https://github.com/DavidBuchanan314/ftpd/blob/master/LICENSE)

#pragma once

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*! Loop status */
typedef enum
{
    LOOP_CONTINUE, /*!< Continue looping */
    LOOP_RESTART,  /*!< Reinitialize */
    LOOP_EXIT,     /*!< Terminate looping */
} loop_status_t;

void http_pre_init(void);
int http_init(void);
loop_status_t http_loop(void);
void http_exit(void);
void http_post_exit(void);
