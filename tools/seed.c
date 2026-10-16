/*
 * seed — write a realistic TEST database through the core API, so the UI
 * has something to show: a few dozen tasks covering due dates (overdue,
 * today, ahead, with and without a time), recurrence, subtasks, notes,
 * completed and in-progress work, high priority, attachments, Notes action
 * items (in a tagged note and not), and tags — each task's area, the wider area
 * it belongs to, a few extra tags across areas, emojis, two favorites.
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
 * EARLIER entry by its 1-based position in the table (0 = top level),
 * which keeps the table readable without ids.  A subtask takes its
 * parent's tags. */
typedef struct {
    const char *area;               /* its tag (seed_areas names it)      */
    int         parent;             /* table position of its parent, or 0 */
    const char *title;
    const char *notes;              /* "" for none                        */
    int         due_in_days;        /* relative to today, or NO_DUE       */
    int         due_time;           /* minutes past midnight; 0 = default */
    TaskStatus  status;
    int         priority;
    int         attachments;        /* how many sample attachments        */
    int         recur_interval;     /* 0 = does not repeat                */
    TaskRecurUnit recur_unit;
} seed_task_t;

/* The areas a task's `area` names: a tag with its emoji, and the wider
 * area's tag every task of it also carries (NULL for none) — the shape
 * the schema's conversion gives lists that were in a group. */
typedef struct {
    const char *name;
    const char *emoji;              /* "" for none                        */
    const char *group;              /* the wider area, or NULL            */
} seed_area_t;

static const seed_area_t seed_areas[] = {
    { "Deployments", "🚀", "Work" },
    { "Customers",   "🤝", "Work" },
    { "Admin",       "🗂️", "Work" },
    { "House",       "🏠", "Home" },
    { "Errands",     "🛒", "Home" },
    { "Reading",     "📚", NULL   },
    { "Someday",     "",   NULL   },
};

static const seed_task_t seed_tasks[] = {
    /* 1 */ { "Deployments", NO_PARENT, "Cut the 4.3 release branch",
              "Freeze main on Thursday.\nTag after the smoke tests pass.\nAnnounce in #releases.",
              0, 14 * 60 + 30, TASK_STATUS_IN_PROGRESS, 1, 2, 0, TASK_RECUR_DAY },
    /* 2 */ { "Deployments", 1, "Update the changelog", "", NO_DUE, 0, TASK_STATUS_DONE, 0, 0, 0, TASK_RECUR_DAY },
    /* 3 */ { "Deployments", 1, "Run the smoke tests", "", NO_DUE, 0, TASK_STATUS_IN_PROGRESS, 0, 0, 0, TASK_RECUR_DAY },
    /* 4 */ { "Deployments", 1, "Tag and push", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },
    /* 5 */ { "Deployments", NO_PARENT, "Rotate the staging certificates",
              "Expires at the end of the month.", -3, 0, TASK_STATUS_NEW, 1, 0, 0, TASK_RECUR_DAY },
    /* 6 */ { "Deployments", NO_PARENT, "Review collector resource limits",
              "", 2, 0, TASK_STATUS_NEW, 0, 1, 0, TASK_RECUR_DAY },
    /* 7 */ { "Deployments", NO_PARENT, "Weekly deployment health check",
              "Dashboards first, then the error budget.", 1, 9 * 60, TASK_STATUS_NEW, 0, 0, 1, TASK_RECUR_WEEK },
    /* 8 */ { "Deployments", NO_PARENT, "Decommission the old build host",
              "", NO_DUE, 0, TASK_STATUS_DONE, 0, 0, 0, TASK_RECUR_DAY },
    /* 9 */ { "Deployments", NO_PARENT, "Write the rollback runbook",
              "\n\n   Starts after two blank lines, to exercise the notes preview.  This line is deliberately long so that the preview has to be cut: it runs well past one hundred and twenty bytes of text.",
              7, 0, TASK_STATUS_IN_PROGRESS, 0, 0, 0, TASK_RECUR_DAY },

    /* 10 */ { "Customers", NO_PARENT, "Prepare the quarterly review for Northwind",
               "Slides, usage numbers, open tickets.", 0, 0, TASK_STATUS_NEW, 0, 1, 0, TASK_RECUR_DAY },
    /* 11 */ { "Customers", 10, "Pull the usage numbers", "", NO_DUE, 0, TASK_STATUS_DONE, 0, 0, 0, TASK_RECUR_DAY },
    /* 12 */ { "Customers", 10, "Draft the slides", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },
    /* 13 */ { "Customers", 10, "List the open tickets", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },
    /* 14 */ { "Customers", 10, "Book the room", "", NO_DUE, 0, TASK_STATUS_DONE, 0, 0, 0, TASK_RECUR_DAY },
    /* 15 */ { "Customers", 10, "Send the agenda", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },
    /* 16 */ { "Customers", 10, "Confirm attendees", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },
    /* 17 */ { "Customers", NO_PARENT, "Reply to Contoso about the PoV timeline",
               "They asked for dates before Friday.", -1, 0, TASK_STATUS_NEW, 1, 0, 0, TASK_RECUR_DAY },
    /* 18 */ { "Customers", NO_PARENT, "Call Fabrikam — renewal",
               "Café at 3?  Ask for Zoë.", 3, 15 * 60, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },
    /* 19 */ { "Customers", NO_PARENT, "Send the onboarding checklist",
               "", NO_DUE, 0, TASK_STATUS_DONE, 0, 0, 0, TASK_RECUR_DAY },
    /* 20 */ { "Customers", NO_PARENT, "Monthly account review",
               "", 12, 0, TASK_STATUS_NEW, 0, 0, 1, TASK_RECUR_MONTH },

    /* 21 */ { "Admin", NO_PARENT, "Submit expenses", "", 0, 17 * 60, TASK_STATUS_NEW, 0, 3, 0, TASK_RECUR_DAY },
    /* 22 */ { "Admin", NO_PARENT, "Timesheet", "", 1, 0, TASK_STATUS_NEW, 0, 0, 1, TASK_RECUR_WEEK },
    /* 23 */ { "Admin", NO_PARENT, "Renew the code-signing certificate",
               "", 30, 0, TASK_STATUS_NEW, 0, 0, 1, TASK_RECUR_YEAR },
    /* 24 */ { "Admin", NO_PARENT, "", "A task with no title, to show the stand-in.", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },

    /* 25 */ { "House", NO_PARENT, "Fix the garden gate", "Needs two new hinges.", -10, 0, TASK_STATUS_IN_PROGRESS, 0, 0, 0, TASK_RECUR_DAY },
    /* 26 */ { "House", 25, "Buy hinges", "", NO_DUE, 0, TASK_STATUS_DONE, 0, 0, 0, TASK_RECUR_DAY },
    /* 27 */ { "House", 25, "Sand and repaint", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },
    /* 28 */ { "House", NO_PARENT, "Water the plants", "", 0, 0, TASK_STATUS_DONE, 0, 0, 2, TASK_RECUR_DAY },
    /* 29 */ { "House", NO_PARENT, "Book the boiler service", "", 5, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },
    /* 30 */ { "House", NO_PARENT, "Clear the gutters", "", NO_DUE, 0, TASK_STATUS_DONE, 0, 0, 0, TASK_RECUR_DAY },
    /* 31 */ { "House", NO_PARENT, "Replace the smoke alarm batteries", "", 60, 0, TASK_STATUS_NEW, 0, 0, 6, TASK_RECUR_MONTH },

    /* 32 */ { "Errands", NO_PARENT, "Groceries", "Milk, eggs, coffee, 🍋 lemons.", 0, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },
    /* 33 */ { "Errands", NO_PARENT, "Collect the parcel", "", -1, 0, TASK_STATUS_DONE, 0, 0, 0, TASK_RECUR_DAY },
    /* 34 */ { "Errands", NO_PARENT, "Pharmacy", "", 1, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },
    /* 35 */ { "Errands", NO_PARENT, "Return the library books", "", 2, 10 * 60, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },

    /* 36 */ { "Reading", NO_PARENT, "The Mythical Man-Month", "Chapters 1–4 this week.", 4, 0, TASK_STATUS_IN_PROGRESS, 0, 0, 0, TASK_RECUR_DAY },
    /* 37 */ { "Reading", NO_PARENT, "SQLite: the WAL design notes", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },
    /* 38 */ { "Reading", NO_PARENT, "A Philosophy of Software Design", "", NO_DUE, 0, TASK_STATUS_DONE, 0, 0, 0, TASK_RECUR_DAY },
    /* 39 */ { "Reading", NO_PARENT, "日本語のタイトル — a non-Latin title", "ノートのプレビュー。", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },

    /* 40 */ { "Someday", NO_PARENT, "Learn to sail", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },
    /* 41 */ { "Someday", NO_PARENT, "Build a workbench", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },
    /* 42 */ { "Someday", NO_PARENT, "Digitise the photo albums", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },
    /* 43 */ { "Someday", NO_PARENT, "Captain's Log - lock down my diet RE keto", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },
    /* 44 */ { "Customers", NO_PARENT, "Quad - Ian to create a diagram", "", 6, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },
    /* 45 */ { "Errands", NO_PARENT, "Pick up the dry cleaning", "", NO_DUE, 0, TASK_STATUS_NEW, 0, 0, 0, TASK_RECUR_DAY },
};

/* Notes action items: tasks of the table above that a note "wrote", as
 * Notes links them (a note, and a note_task row naming the task), so the
 * "From note" line and the action tag have something to show.  Two notes
 * carry a tag of their own (a tag Tasks offers but, until a task carries
 * it, does not show), one carries none. */
typedef struct {
    int         task;               /* table position of the task (1-based) */
    const char *tag;                /* the note's tag (its Tags field), or
                                     * NULL for none                        */
    const char *note;               /* the note's title                     */
} seed_link_t;

static const seed_link_t seed_links[] = {
    { 43, "Captain's Log", "Log, stardate today" },
    { 44, "Quad",          "Quad weekly sync"    },
    { 45, NULL,            "Loose ends"          },
};

/* Tags: tasks of the table above and a tag each carries, so the tag
 * buttons and their proto-lists have something to show — one tag across
 * lists of both groups (a subtask among them), one on a single task. */
typedef struct {
    int         task;               /* table position of the task (1-based) */
    const char *tag;                /* the tag's name                       */
} seed_tag_t;

static const seed_tag_t seed_tags[] = {
    {  5, "urgent" },
    { 17, "urgent" },
    { 25, "urgent" },
    { 12, "urgent" },
    { 37, "deep work" },
};

/* The tags shown in the Favorites bar. */
static const char * const seed_favorites[] = { "Customers", "Reading" };

#define COUNT(a) (sizeof (a) / sizeof (a)[0])

/*
 * area_of — a seeded area by name.
 *
 * Inputs:
 *   name — the area's name
 *
 * Output:
 *   the table entry, or NULL when no area has that name.
 */
static const seed_area_t *
area_of(const char *name)
{
    for (size_t i = 0; i < COUNT(seed_areas); i++) {
        if (strcmp(seed_areas[i].name, name) == 0) return &seed_areas[i];
    }
    return NULL;
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
 *   parent_id   — its parent's id, or 0 (a top-level task is tagged with
 *                 its area and the area's group)
 *   attachments — the sample attachment paths
 *
 * Output:
 *   the new task's id, or 0 on failure.
 */
static gint64
seed_one(TaskApp *app, const seed_task_t *spec, gint64 parent_id,
         gchar * const attachments[3])
{
    const seed_area_t *area = area_of(spec->area);
    gint64 id = task_db_task_create(app->db, parent_id, spec->title,
                                    parent_id == 0 ? spec->area : NULL);
    if (id == 0) return 0;
    if (parent_id == 0 && area != NULL && area->group != NULL &&
        task_db_task_tag_add(app->db, id, area->group) == 0) return 0;

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

    if (spec->priority) task_db_task_set_priority(app->db, id, TRUE);
    for (int i = 0; i < spec->attachments && i < 3; i++) {
        task_db_attachment_add(app->db, id, attachments[i]);
    }
    return id;
}

/*
 * seed_link — make a seeded task a Notes action item, as Notes writes one:
 * a note (with its tag, when given), and the note_task row naming the
 * task, with the uid sequence moved past it.  The note's content is left
 * empty: Tasks reads only the link and the note's title.
 *
 * Inputs:
 *   app     — the context
 *   link    — the table entry
 *   task_id — the task's database id
 *
 * Output:
 *   0 on success, -1 when a statement failed (reported).
 */
static int
seed_link(TaskApp *app, const seed_link_t *link, gint64 task_id)
{
    char *tag = link->tag != NULL
        ? sqlite3_mprintf("INSERT OR IGNORE INTO tags (name) VALUES (%Q);"
                          "INSERT INTO note_tags (note_id, tag_id, in_field)"
                          " SELECT (SELECT MAX(id) FROM notes), id, 1 FROM tags"
                          " WHERE name = %Q;", link->tag, link->tag)
        : sqlite3_mprintf("%s", "");
    char *sql = sqlite3_mprintf(
        "INSERT INTO notes (title, created_at, updated_at)"
        " VALUES (%Q, strftime('%%s','now'), strftime('%%s','now'));"
        "%s"
        "INSERT INTO note_task (task_id, note_id, ord, seen_title)"
        " SELECT %lld, (SELECT MAX(id) FROM notes), 0, title FROM tasks WHERE id = %lld;"
        "UPDATE action_uid_seq SET next = (SELECT max(uid) + 1 FROM note_task);",
        link->note, tag, (long long)task_id, (long long)task_id);
    char *err = NULL;
    int   rc  = sqlite3_exec(shared_db_handle(app->db->shared), sql, NULL, NULL, &err);

    if (rc != SQLITE_OK) fprintf(stderr, "seed: linking a note item: %s\n", err);
    sqlite3_free(err);
    sqlite3_free(sql);
    sqlite3_free(tag);
    return rc == SQLITE_OK ? 0 : -1;
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

    char  *data_dir = task_data_dir();
    gchar *attachments[3];
    if (make_attachments(data_dir, attachments) != 0) return 1;
    free(data_dir);

    gint64 task_ids[COUNT(seed_tasks)];
    for (size_t i = 0; i < COUNT(seed_tasks); i++) {
        const seed_task_t *spec = &seed_tasks[i];
        gint64 parent = spec->parent != NO_PARENT ? task_ids[spec->parent - 1] : 0;
        task_ids[i] = seed_one(app, spec, parent, attachments);
        if (task_ids[i] == 0) {
            fprintf(stderr, "seed: could not create \"%s\"\n", spec->title);
            return 1;
        }
    }

    for (size_t i = 0; i < COUNT(seed_links); i++) {
        if (seed_link(app, &seed_links[i], task_ids[seed_links[i].task - 1]) != 0) return 1;
    }

    for (size_t i = 0; i < COUNT(seed_tags); i++) {
        if (task_db_task_tag_add(app->db, task_ids[seed_tags[i].task - 1], seed_tags[i].tag) == 0) {
            fprintf(stderr, "seed: could not tag a task \"%s\"\n", seed_tags[i].tag);
            return 1;
        }
    }

    for (size_t i = 0; i < COUNT(seed_areas); i++) {
        TaskTag *tag = task_db_tag_find(app->db, seed_areas[i].name);
        if (tag != NULL && !task_db_tag_set_emoji(app->db, tag->id, seed_areas[i].emoji)) return 1;
        task_tag_free(tag);
    }
    for (size_t i = 0; i < COUNT(seed_favorites); i++) {
        TaskTag *tag = task_db_tag_find(app->db, seed_favorites[i]);
        if (tag == NULL || !task_db_tag_set_favorite(app->db, tag->id, TRUE)) return 1;
        task_tag_free(tag);
    }

    gint n_tasks = 0, n_tags = 0;
    task_db_totals(app->db, &n_tasks, &n_tags);
    printf("seed: %d tasks with %d tags -> %s\n", n_tasks, n_tags, path);

    for (int i = 0; i < 3; i++) g_free(attachments[i]);
    free(path);
    task_app_free(app);
    task_config_close();
    return 0;
}
