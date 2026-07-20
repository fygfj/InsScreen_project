#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void diag_log_init(void);
size_t diag_log_buffer_used(void);
esp_err_t diag_log_export_to_sd(char *path_out, size_t path_out_len, size_t *bytes_out);

#ifdef __cplusplus
}
#endif
