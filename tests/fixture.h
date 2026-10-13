/*
 * fixture.h — a throwaway application context for the tests that need a
 * database: a fresh sandbox directory, the config loaded from it, the
 * built-in views registered, and an empty shared database in it
 * (CAMBIO_SHARED_DB, which check_scratch_dir points there) opened through
 * the core.
 */

#ifndef TESTS_FIXTURE_H
#define TESTS_FIXTURE_H

#include "check.h"
#include "core_views.h"
#include "task_app.h"
#include "task_config.h"
#include "task_paths.h"

/*
 * fixture_open — build the context.  Exits with status 2 when the sandbox
 * cannot be set up.
 *
 * Output:
 *   a TaskApp with an empty database; release with fixture_close.
 */
static inline TaskApp *
fixture_open(void)
{
    char scratch[1024];

    check_scratch_dir(scratch, sizeof scratch);
    setenv(TASK_DATA_DIR_ENV, scratch, 1);
    if (!task_config_init()) {
        fprintf(stderr, "fixture: cannot initialise the config in %s\n", scratch);
        exit(2);
    }
    task_core_views_init();

    TaskApp *app  = task_app_new();
    char    *path = shared_db_path(NULL);
    app->db = path != NULL ? task_db_open(path, NULL) : NULL;
    free(path);
    if (app->db == NULL) {
        fprintf(stderr, "fixture: cannot open a database in %s\n", scratch);
        exit(2);
    }
    return app;
}

/*
 * fixture_close — close the database and the config.
 *
 * Inputs:
 *   app — the context from fixture_open
 */
static inline void
fixture_close(TaskApp *app)
{
    task_app_free(app);
    task_config_close();
}

#endif /* TESTS_FIXTURE_H */
