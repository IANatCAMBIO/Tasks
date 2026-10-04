/*
 * task_health — what the Settings window's Database plate says about the
 * open database, as toolkit-neutral data: the words of the health verdict,
 * and the background probe that reads the facts worth reading off the main
 * thread (the integrity check, the file's size and its SHA-256).
 *
 * The plate describes ONE file, so the probe answers about one file: the
 * path is copied when the probe starts, every fact is gathered from that
 * path on a background thread, and the answer is dropped if the app has
 * opened a different database by the time it comes back.
 *
 * The wording is carried from the GTK build's settings_window.c.
 *
 * Plain C: strings returned here are malloc'd and freed with free().
 * task_health_stamp and task_health_describe are safe from any thread;
 * task_health_probe_start is MAIN THREAD ONLY, and so is its callback.
 */

#ifndef TASK_HEALTH_H
#define TASK_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#include "task_app.h"

/* The plate's indicator light.  It is green ONLY for a check that ran and
 * passed: there is no green for "not checked yet", because an indicator
 * that goes green before anything has looked is the "all good" that was
 * never established. */
typedef enum {
    task_health_unknown = 0,        /* nothing has checked this database   */
    task_health_good,               /* a check ran and passed              */
    task_health_bad                 /* a check found problems, or could
                                     * not be run                          */
} task_health_light_t;

/*
 * task_health_stamp — the moment a check was made, in as few words as carry
 * it: "13:24 today" on the same local day, otherwise "Sep 8 at 09:02".
 *
 * Inputs:
 *   when — unix time of the check
 *   now  — unix time to judge "today" against (the caller's clock, so that
 *          the answer can be tested)
 *
 * Output:
 *   new string (free it); "an unknown time" when `when` cannot be shown as
 *   a local time.  NULL only when memory runs out.
 */
char *task_health_stamp(int64_t when, int64_t now);

/*
 * task_health_describe — the health line of the plate, from the verdict the
 * database handle holds (task_db_health).
 *
 * The three failures are three different sentences on purpose: "problems
 * found" and "check did not complete" send someone looking for different
 * things — a locked or unreadable file reading as merely unhealthy has
 * them hunting for corruption that may not be there.
 *
 * Inputs:
 *   health  — the stored verdict, or NULL when no check has been made
 *   now     — unix time to judge "today" against
 *   summary — receives the line: "Healthy — checked 13:24 today",
 *             "Problems found — …", "Check did not complete — …" or
 *             "Not checked" (new string, free it)
 *   detail  — receives the longer explanation for a tooltip: sqlite's own
 *             words when there are any, else what the verdict means (new
 *             string, free it)
 *
 * Output:
 *   which light to show.  *summary and *detail are NULL only when memory
 *   runs out.
 */
task_health_light_t task_health_describe(const TaskDbHealth *health, int64_t now,
                                         char **summary, char **detail);

/*
 * What a probe found.  Valid only during the callback.
 *
 *   current — the app still has the probed database open.  When false the
 *             app opened another one while the probe ran: nothing was
 *             recorded, and size and sha256 describe a file that is no
 *             longer the one on show — ignore them.
 *   size    — the file's size on disk in bytes (what it occupies, free
 *             pages included), or -1 when it cannot be read.
 *   sha256  — hex SHA-256 of the file as it stood, or NULL when it cannot
 *             be read.  Borrowed.
 */
typedef struct {
    bool        current;
    int64_t     size;
    const char *sha256;
} task_health_probe_t;

/* A probe's completion; runs on the main thread, exactly once per
 * task_health_probe_start. */
typedef void (*task_health_done_fn)(TaskApp *app, const task_health_probe_t *probe,
                                    void *user_data);

/*
 * task_health_probe_start — read the open database's file on a background
 * thread and report back on the main thread.
 *
 * With `check`, the two integrity PRAGMAs are run first (on a read-only
 * connection of the probe's own, so the file can never be created or
 * changed by it) and, if the app still has that database open when the
 * probe returns, the verdict is recorded on the handle: task_db_health
 * answers with it from inside `done` onwards.  Without `check` only the
 * size and digest are read and the stored verdict is left alone.
 *
 * A probe cannot be cancelled.  `done` is always called (before this
 * returns, with nothing read, in the one case where memory for the probe
 * cannot be had), so a caller whose window may have closed meanwhile
 * passes something it can still judge by then: the Settings window passes
 * a generation number, not a pointer.  Probes may overlap; each reports
 * for itself.
 *
 * Inputs:
 *   app       — the application context, with a database open; it must
 *               stay alive until done has run (as for a backup pass)
 *   check     — also run the integrity check and record its verdict
 *   done      — called with the result, not NULL
 *   user_data — passed through to done (borrowed, never freed here)
 */
void task_health_probe_start(TaskApp *app, bool check, task_health_done_fn done,
                             void *user_data);

#endif /* TASK_HEALTH_H */
