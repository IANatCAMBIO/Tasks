/*
 * seed — write a realistic TEST database through the core API, so the UI
 * has something to show: list groups, lists, and a few dozen tasks covering
 * due dates (overdue, today, ahead, with and without a time), recurrence,
 * subtasks, notes, completed and in-progress work, favourites, high
 * priority and attachments.
 *
 * Run through `make seed`, which empties the sandbox first and sets
 * TASKS_DATA_DIR and CAMBIO_SHARED_DB.  The tool refuses to run without
 * both variables and refuses to touch a database that already exists, so
 * it can never write into the per-user data directory, the per-user
 * shared database or over real data.
 *
 * Output: exit status 0 on success, 1 on any refusal or failure.
 */

#include "db.h"
#include "recur.h"
#include "task_app.h"
#include "task_config.h"
#include "task_dates.h"
#include "task_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define NO_DUE   (-1000)            /* due_in_days value meaning "no date" */
#define NO_PARENT 0

/* One task to create.  Tasks are created in table order; `parent` names an
 * EARLIER entry of the same list by its 1-based position in the table (0 =
 * top level), which keeps the table readable without ids. */
typedef struct {
    const char *list;               /* name of the list it goes in        */
    int         parent;             /* table position of its parent, or 0 */
    const char *title;
    const char *notes;              /* "" for none                        */
    int         due_in_days;        /* relative to today, or NO_DUE       */
    int         due_time;           /* minutes past midnight; 0 = default */
    TaskStatus  status;
    int         pinned;
    int         priority;
    int         attachments;        /* how many sample attachments        */
    int         recur_interval;     /* 0 = does not repeat                */
    TaskRecurUnit recur_unit;
} seed_task_t;

/* One list to create. */
typedef struct {
    const char *name;
    const char *emoji;              /* "" for none                        */
    const char *group;              /* group name, or NULL for ungrouped  */
} seed_list_t;

static const char * const seed_groups[] = { "Work", "Home" };

static const seed_list_t seed_lists[] = {
    { "Deployments", "🚀", "Work" },
    { "Customers",   "🤝", "Work" },
    { "Admin",       "🗂️", "Work" },
    { "House",       "🏠", "Home" },
    { "Errands",     "🛒", "Home" },
    { "Reading",     "📚", NULL   },
    { "Someday",     "",   NULL   },
    { "Empty List",  "🫙", NULL   },
};

static const seed_task_t seed_tasks[] = {
    /* 1 */ { "Deployments", NO_PARENT, "Cut the 4.3 release branch",
              "Freeze main on Thursday.\nTag after the smoke tests pass.\nAnnounce in #releases.",
              0, 14 * 60 + 30, TASK_STATUS_IN_PROGRESS, 1, 1, 2, 0, TASK_RECUR_DAY },
    /* 2 */ { "Deployments", 1, "Update the changelog", "", NO_DUE, 0, TASK_STATUS_DONE, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 3 */ { "Deployments", 1, "Run the smoke tests", "", NO_DUE, 0, TASK_STATUS_IN_PROGRESS, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 4 */ { "Deployments", 1, "Tag and push", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 5 */ { "Deployments", NO_PARENT, "Rotate the staging certificates",
              "Expires at the end of the month.", -3, 0, TASK_STATUS_NEW, 0, 1, 0, 0, TASK_RECUR_DAY },
    /* 6 */ { "Deployments", NO_PARENT, "Review collector resource limits",
              "", 2, 0, TASK_STATUS_NEW, 0, 0, 1, 0, TASK_RECUR_DAY },
    /* 7 */ { "Deployments", NO_PARENT, "Weekly deployment health check",
              "Dashboards first, then the error budget.", 1, 9 * 60, TASK_STATUS_NEW, 0, 0, 0, 1, TASK_RECUR_WEEK },
    /* 8 */ { "Deployments", NO_PARENT, "Decommission the old build host",
              "", NO_DUE, 0, TASK_STATUS_DONE, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 9 */ { "Deployments", NO_PARENT, "Write the rollback runbook",
              "\n\n   Starts after two blank lines, to exercise the notes preview.  This line is deliberately long so that the preview has to be cut: it runs well past one hundred and twenty bytes of text.",
              7, 0, TASK_STATUS_IN_PROGRESS, 0, 0, 0, 0, TASK_RECUR_DAY },

    /* 10 */ { "Customers", NO_PARENT, "Prepare the quarterly review for Northwind",
               "Slides, usage numbers, open tickets.", 0, 0, TASK_STATUS_NEW, 1, 0, 1, 0, TASK_RECUR_DAY },
    /* 11 */ { "Customers", 10, "Pull the usage numbers", "", NO_DUE, 0, TASK_STATUS_DONE, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 12 */ { "Customers", 10, "Draft the slides", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 13 */ { "Customers", 10, "List the open tickets", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 14 */ { "Customers", 10, "Book the room", "", NO_DUE, 0, TASK_STATUS_DONE, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 15 */ { "Customers", 10, "Send the agenda", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 16 */ { "Customers", 10, "Confirm attendees", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 17 */ { "Customers", NO_PARENT, "Reply to Contoso about the PoV timeline",
               "They asked for dates before Friday.", -1, 0, TASK_STATUS_NEW, 0, 1, 0, 0, TASK_RECUR_DAY },
    /* 18 */ { "Customers", NO_PARENT, "Call Fabrikam — renewal",
               "Café at 3?  Ask for Zoë.", 3, 15 * 60, TASK_STATUS_NEW, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 19 */ { "Customers", NO_PARENT, "Send the onboarding checklist",
               "", NO_DUE, 0, TASK_STATUS_DONE, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 20 */ { "Customers", NO_PARENT, "Monthly account review",
               "", 12, 0, TASK_STATUS_NEW, 0, 0, 0, 1, TASK_RECUR_MONTH },

    /* 21 */ { "Admin", NO_PARENT, "Submit expenses", "", 0, 17 * 60, TASK_STATUS_NEW, 0, 0, 3, 0, TASK_RECUR_DAY },
    /* 22 */ { "Admin", NO_PARENT, "Timesheet", "", 1, 0, TASK_STATUS_NEW, 0, 0, 0, 1, TASK_RECUR_WEEK },
    /* 23 */ { "Admin", NO_PARENT, "Renew the code-signing certificate",
               "", 30, 0, TASK_STATUS_NEW, 0, 0, 0, 1, TASK_RECUR_YEAR },
    /* 24 */ { "Admin", NO_PARENT, "", "A task with no title, to show the stand-in.", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, 0, TASK_RECUR_DAY },

    /* 25 */ { "House", NO_PARENT, "Fix the garden gate", "Needs two new hinges.", -10, 0, TASK_STATUS_IN_PROGRESS, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 26 */ { "House", 25, "Buy hinges", "", NO_DUE, 0, TASK_STATUS_DONE, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 27 */ { "House", 25, "Sand and repaint", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 28 */ { "House", NO_PARENT, "Water the plants", "", 0, 0, TASK_STATUS_DONE, 0, 0, 0, 2, TASK_RECUR_DAY },
    /* 29 */ { "House", NO_PARENT, "Book the boiler service", "", 5, 0, TASK_STATUS_NEW, 1, 0, 0, 0, TASK_RECUR_DAY },
    /* 30 */ { "House", NO_PARENT, "Clear the gutters", "", NO_DUE, 0, TASK_STATUS_DONE, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 31 */ { "House", NO_PARENT, "Replace the smoke alarm batteries", "", 60, 0, TASK_STATUS_NEW, 0, 0, 0, 6, TASK_RECUR_MONTH },

    /* 32 */ { "Errands", NO_PARENT, "Groceries", "Milk, eggs, coffee, 🍋 lemons.", 0, 0, TASK_STATUS_NEW, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 33 */ { "Errands", NO_PARENT, "Collect the parcel", "", -1, 0, TASK_STATUS_DONE, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 34 */ { "Errands", NO_PARENT, "Pharmacy", "", 1, 0, TASK_STATUS_NEW, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 35 */ { "Errands", NO_PARENT, "Return the library books", "", 2, 10 * 60, TASK_STATUS_NEW, 0, 0, 0, 0, TASK_RECUR_DAY },

    /* 36 */ { "Reading", NO_PARENT, "The Mythical Man-Month", "Chapters 1–4 this week.", 4, 0, TASK_STATUS_IN_PROGRESS, 1, 0, 0, 0, TASK_RECUR_DAY },
    /* 37 */ { "Reading", NO_PARENT, "SQLite: the WAL design notes", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 38 */ { "Reading", NO_PARENT, "A Philosophy of Software Design", "", NO_DUE, 0, TASK_STATUS_DONE, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 39 */ { "Reading", NO_PARENT, "日本語のタイトル — a non-Latin title", "ノートのプレビュー。", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, 0, TASK_RECUR_DAY },

    /* 40 */ { "Someday", NO_PARENT, "Learn to sail", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 41 */ { "Someday", NO_PARENT, "Build a workbench", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, 0, TASK_RECUR_DAY },
    /* 42 */ { "Someday", NO_PARENT, "Digitise the photo albums", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, 0, TASK_RECUR_DAY },
};

#define COUNT(a) (sizeof (a) / sizeof (a)[0])

/*
 * list_id_of — the database id of a seeded list.
 *
 * Inputs:
 *   ids  — ids of seed_lists, in table order
 *   name — the list's name
 *
 * Output:
 *   the id, or 0 when no seeded list has that name.
 */
static gint64
list_id_of(const gint64 *ids, const char *name)
{
    for (size_t i = 0; i < COUNT(seed_lists); i++) {
        if (strcmp(seed_lists[i].name, name) == 0) return ids[i];
    }
    return 0;
}

/*
 * make_attachments — write the sample files tasks attach, inside the
 * sandbox, so "Open" has something real to open.
 *
 * Inputs:
 *   data_dir — the sandbox directory
 *   paths    — receives the three files' paths (g_free each)
 *
 * Output:
 *   0 on success, -1 when a file cannot be written.
 */
static int
make_attachments(const char *data_dir, gchar *paths[3])
{
    static const char * const names[3] = {
        "release-notes.txt", "receipt-october.txt", "floor-plan.txt",
    };
    gchar *dir = g_build_filename(data_dir, "attachments", NULL);

    if (mkdir(dir, 0755) != 0) {
        perror(dir);
        g_free(dir);
        return -1;
    }
    for (int i = 0; i < 3; i++) {
        paths[i] = g_build_filename(dir, names[i], NULL);
        FILE *f = fopen(paths[i], "w");
        if (f == NULL) {
            perror(paths[i]);
            g_free(dir);
            return -1;
        }
        fprintf(f, "Sample attachment %d for the Tasks test database.\n", i + 1);
        fclose(f);
    }
    g_free(dir);
    return 0;
}

/*
 * seed_one — create one task of the table and apply its fields.
 *
 * Inputs:
 *   app         — the context
 *   spec        — the table entry
 *   list_id     — the list to create it in
 *   parent_id   — its parent's id, or 0
 *   attachments — the sample attachment paths
 *
 * Output:
 *   the new task's id, or 0 on failure.
 */
static gint64
seed_one(TaskApp *app, const seed_task_t *spec, gint64 list_id,
         gint64 parent_id, gchar * const attachments[3])
{
    gint64 id = task_db_task_create(app->db, list_id, parent_id, spec->title);
    if (id == 0) return 0;

    Task *before = task_db_task_get(app->db, id);
    Task *t      = task_dup(before);
    g_free(t->notes);
    t->notes = g_strdup(spec->notes);
    if (spec->due_in_days != NO_DUE) {
        gint64 lo, hi;
        task_day_bounds(spec->due_in_days, &lo, &hi);
        t->due = lo;
        if (spec->due_time != 0) t->due_time = spec->due_time;
    }
    if (spec->recur_interval > 0) {
        t->recur_interval = spec->recur_interval;
        t->recur_unit     = spec->recur_unit;
        t->recur_lead     = task_recur_lead_default(spec->recur_unit,
                                                    spec->recur_interval);
    }
    t->status = spec->status;
    task_db_task_update(app->db, before, t);
    task_free(before);
    task_free(t);

    if (spec->pinned) task_db_task_set_pinned(app->db, id, TRUE);
    if (spec->priority) task_db_task_set_priority(app->db, id, TRUE);
    for (int i = 0; i < spec->attachments && i < 3; i++) {
        task_db_attachment_add(app->db, id, attachments[i]);
    }
    return id;
}

/*
 * main — build the test database in $TASKS_DATA_DIR.
 *
 * Output:
 *   0 on success; 1 when the sandbox variable is missing, a database is
 *   already there, or anything fails.
 */
int
main(void)
{
    const char *sandbox = getenv(TASK_DATA_DIR_ENV);
    const char *shared  = getenv(SHARED_DB_ENV_VAR);
    if (sandbox == NULL || sandbox[0] == '\0' || shared == NULL || shared[0] == '\0') {
        fprintf(stderr, "seed: %s and %s must both be set; run `make seed`\n",
                TASK_DATA_DIR_ENV, SHARED_DB_ENV_VAR);
        return 1;
    }
    if (!task_config_init()) {
        fprintf(stderr, "seed: cannot use %s as the data directory\n", sandbox);
        return 1;
    }

    char *path = shared_db_path(NULL);
    if (path == NULL || g_file_test(path, G_FILE_TEST_EXISTS)) {
        fprintf(stderr, "seed: %s already exists; refusing to write into it\n", path);
        return 1;
    }

    GError  *err = NULL;
    TaskApp *app = task_app_new();
    app->db = task_db_open(path, &err);
    if (app->db == NULL) {
        fprintf(stderr, "seed: %s\n", err->message);
        return 1;
    }

    /* The sandbox's backups stay in the sandbox: the shared module's default
     * folder is the per-user one (backup.h). */
    char  *data_dir = task_data_dir();
    gchar *backups  = g_build_filename(data_dir, "backups", NULL);
    g_mkdir_with_parents(backups, 0700);
    if (shared_settings_set(app->db->shared, SHARED_SETTING_BACKUP_DIR, backups, NULL) != 0) {
        fprintf(stderr, "seed: cannot set the backup folder\n");
        return 1;
    }
    g_free(backups);

    gchar *attachments[3];
    if (make_attachments(data_dir, attachments) != 0) return 1;
    free(data_dir);

    gint64 group_ids[COUNT(seed_groups)];
    for (size_t i = 0; i < COUNT(seed_groups); i++) {
        group_ids[i] = task_db_group_create(app->db, seed_groups[i]);
    }

    gint64 list_ids[COUNT(seed_lists)];
    for (size_t i = 0; i < COUNT(seed_lists); i++) {
        list_ids[i] = task_db_list_create(app->db, seed_lists[i].name,
                                          seed_lists[i].emoji);
        for (size_t g = 0; g < COUNT(seed_groups); g++) {
            if (seed_lists[i].group != NULL &&
                strcmp(seed_lists[i].group, seed_groups[g]) == 0) {
                task_db_list_set_group(app->db, list_ids[i], group_ids[g]);
            }
        }
    }

    gint64 task_ids[COUNT(seed_tasks)];
    for (size_t i = 0; i < COUNT(seed_tasks); i++) {
        const seed_task_t *spec = &seed_tasks[i];
        gint64 parent = spec->parent != NO_PARENT ? task_ids[spec->parent - 1] : 0;
        task_ids[i] = seed_one(app, spec, list_id_of(list_ids, spec->list),
                               parent, attachments);
        if (task_ids[i] == 0) {
            fprintf(stderr, "seed: could not create \"%s\"\n", spec->title);
            return 1;
        }
    }

    gint n_tasks = 0, n_lists = 0;
    task_db_totals(app->db, &n_tasks, &n_lists);
    printf("seed: %d tasks in %d lists, %zu groups -> %s\n", n_tasks, n_lists,
           COUNT(seed_groups), path);

    for (int i = 0; i < 3; i++) g_free(attachments[i]);
    free(path);
    task_app_free(app);
    task_config_close();
    return 0;
}
