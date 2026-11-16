/* Shared set-up of the program (paths, settings, environment test aids). */
#ifndef CTU_APP_H
#define CTU_APP_H

#include <windows.h>
#include "engine.h"

typedef struct {
    wchar_t data_dir[CTU_PATH_MAX];   /* %APPDATA%\ClaudeTaskbarUsage, or CTU_DATA_DIR */
    wchar_t ini_path[CTU_PATH_MAX];
    wchar_t sample_path[CTU_PATH_MAX]; /* CTU_SAMPLE, "" when not set */
    int interval_min;                  /* 5, 10, 15 or 30 */
    int show_remaining;
} app_config;

void app_load_config(app_config *c);
void app_save_config(const app_config *c);
/* Sets the paths, the fetch function and the sample mode of an engine; loads last.json. */
void app_setup_engine(const app_config *c, ctu_engine *e);
/* The stop event that ends a running login renewal (the widget's worker); call before the worker starts. */
void app_set_renew_stop(HANDLE stop);

int widget_run(HINSTANCE inst, app_config *cfg);

#endif
