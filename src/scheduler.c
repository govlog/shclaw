/*
 * scheduler.c — one-shot + recurring task scheduler
 */

#include "../include/tc.h"

static const struct { const char *name; int seconds; } INTERVALS[] = {
    {"every_30min", 1800},
    {"hourly",      3600},
    {"every_6h",    21600},
    {"every_12h",   43200},
    {"daily",       86400},
    {"weekly",      604800},
    {NULL, 0},
};

static int interval_seconds(const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; INTERVALS[i].name; i++)
        if (strcmp(INTERVALS[i].name, name) == 0)
            return INTERVALS[i].seconds;
    return 0;
}

static cJSON *load_tasks(scheduler_t *s) {
    return json_load_array(s->path);
}

static void save_tasks(scheduler_t *s, cJSON *tasks) {
    json_save_atomic(s->path, tasks, 1);
}

/* Parse ISO 8601 datetime to time_t.
 * Trailing 'Z' or '+00:00' → UTC (timegm).
 * No timezone suffix → local time (mktime). */
static time_t parse_iso(const char *iso) {
    struct tm tm = {0};
    if (!iso || !iso[0] || strlen(iso) < 19) return 0;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;

    /* Look for UTC marker after the time portion (offset 19) */
    const char *tz = iso + 19;
    if (*tz == 'Z' ||
        strncmp(tz, "+00:00", 6) == 0 ||
        strncmp(tz, "+0000", 5) == 0)
        return timegm(&tm);

    /* No UTC marker — treat as local time */
    tm.tm_isdst = -1;
    return mktime(&tm);
}

void scheduler_init(scheduler_t *s, const char *json_path) {
    snprintf(s->path, sizeof(s->path), "%s", json_path);
    pthread_mutex_init(&s->lock, NULL);
    mkdirs_for(json_path);
}

const char *sched_add(scheduler_t *s, const char *run_at, const char *desc,
                      const char *prompt, const char *interval,
                      char *out, size_t out_sz) {
    char tid[10], now[32];
    uuid_short(tid, 8);
    now_iso(now, sizeof(now));

    if (!run_at || !run_at[0] || !desc || !desc[0]) {
        snprintf(out, out_sz, "Error: run_at and description are required");
        return out;
    }
    if (parse_iso(run_at) == 0) {
        snprintf(out, out_sz, "Error: invalid run_at");
        return out;
    }
    if (interval && interval[0] && !interval_seconds(interval)) {
        snprintf(out, out_sz, "Error: invalid recurring interval");
        return out;
    }

    cJSON *task = cJSON_CreateObject();
    cJSON_AddStringToObject(task, "id", tid);
    cJSON_AddStringToObject(task, "run_at", run_at);
    cJSON_AddStringToObject(task, "description", desc);
    cJSON_AddStringToObject(task, "prompt", prompt ? prompt : "");
    if (interval && interval[0] && interval_seconds(interval))
        cJSON_AddStringToObject(task, "recurring", interval);
    else
        cJSON_AddNullToObject(task, "recurring");
    cJSON_AddStringToObject(task, "created_at", now);

    pthread_mutex_lock(&s->lock);
    cJSON *tasks = load_tasks(s);
    cJSON_AddItemToArray(tasks, task);
    save_tasks(s, tasks);
    cJSON_Delete(tasks);
    pthread_mutex_unlock(&s->lock);

    snprintf(out, out_sz, "Task scheduled (id=%s): %s at %s%s%s",
             tid, desc, run_at,
             interval ? " recurring " : "",
             interval ? interval : "");
    return out;
}

const char *sched_list(scheduler_t *s, char *out, size_t out_sz) {
    pthread_mutex_lock(&s->lock);
    cJSON *tasks = load_tasks(s);
    pthread_mutex_unlock(&s->lock);

    char *json = cJSON_Print(tasks);
    snprintf(out, out_sz, "%s", json ? json : "[]");
    free(json);
    cJSON_Delete(tasks);
    return out;
}

const char *sched_update(scheduler_t *s, const char *id, const char *run_at,
                         const char *desc, const char *prompt,
                         const char *interval, char *out, size_t out_sz) {
    if (!id || !id[0]) {
        snprintf(out, out_sz, "Error: task_id is required");
        return out;
    }
    if (run_at && run_at[0] && parse_iso(run_at) == 0) {
        snprintf(out, out_sz, "Error: invalid run_at");
        return out;
    }
    if (interval && interval[0] && !interval_seconds(interval)) {
        snprintf(out, out_sz, "Error: invalid recurring interval");
        return out;
    }

    int found = 0;
    pthread_mutex_lock(&s->lock);
    cJSON *tasks = load_tasks(s);

    cJSON *task;
    cJSON_ArrayForEach(task, tasks) {
        const char *tid = j_str(task, "id");
        if (tid && strcmp(tid, id) == 0) {
            if (run_at && run_at[0])
                cJSON_SetValuestring(cJSON_GetObjectItem(task, "run_at"), run_at);
            if (desc && desc[0])
                cJSON_SetValuestring(cJSON_GetObjectItem(task, "description"), desc);
            if (prompt)
                cJSON_SetValuestring(cJSON_GetObjectItem(task, "prompt"), prompt);
            if (interval) {
                cJSON_DeleteItemFromObject(task, "recurring");
                if (interval[0] && interval_seconds(interval))
                    cJSON_AddStringToObject(task, "recurring", interval);
                else
                    cJSON_AddNullToObject(task, "recurring");
            }
            found = 1;
            break;
        }
    }

    if (found) save_tasks(s, tasks);
    cJSON_Delete(tasks);
    pthread_mutex_unlock(&s->lock);

    snprintf(out, out_sz, found ? "Task %s updated." : "Task %s not found.", id);
    return out;
}

const char *sched_cancel(scheduler_t *s, const char *id,
                         char *out, size_t out_sz) {
    if (!id || !id[0]) {
        snprintf(out, out_sz, "Error: task_id is required");
        return out;
    }

    int found = 0;
    pthread_mutex_lock(&s->lock);
    cJSON *tasks = load_tasks(s);

    cJSON *new_tasks = cJSON_CreateArray();
    cJSON *task;
    cJSON_ArrayForEach(task, tasks) {
        const char *tid = j_str(task, "id");
        if (tid && strcmp(tid, id) == 0)
            found = 1;
        else
            cJSON_AddItemToArray(new_tasks, cJSON_Duplicate(task, 1));
    }

    if (found) save_tasks(s, new_tasks);
    cJSON_Delete(tasks);
    cJSON_Delete(new_tasks);
    pthread_mutex_unlock(&s->lock);

    snprintf(out, out_sz, found ? "Task %s cancelled." : "Task %s not found.", id);
    return out;
}

int sched_get_due(scheduler_t *s, cJSON **out) {
    time_t now = time(NULL);

    pthread_mutex_lock(&s->lock);
    cJSON *tasks = load_tasks(s);
    pthread_mutex_unlock(&s->lock);

    cJSON *due = cJSON_CreateArray();
    int count = 0;

    cJSON *task;
    cJSON_ArrayForEach(task, tasks) {
        const char *run_at = j_str(task, "run_at");
        if (!run_at) continue;
        time_t t = parse_iso(run_at);
        if (t > 0 && t <= now) {
            cJSON_AddItemToArray(due, cJSON_Duplicate(task, 1));
            count++;
        }
    }

    cJSON_Delete(tasks);
    *out = due;
    return count;
}

void sched_mark_done(scheduler_t *s, cJSON *ids) {
    if (!ids || cJSON_GetArraySize(ids) == 0) return;

    pthread_mutex_lock(&s->lock);
    cJSON *tasks = load_tasks(s);
    cJSON *remaining = cJSON_CreateArray();
    time_t now = time(NULL);

    cJSON *task;
    cJSON_ArrayForEach(task, tasks) {
        const char *tid = j_str(task, "id");
        if (!tid) continue;

        /* Check if this task is in the done list */
        int is_done = 0;
        cJSON *id_item;
        cJSON_ArrayForEach(id_item, ids) {
            const char *iv = cJSON_GetStringValue(id_item);
            if (iv && strcmp(iv, tid) == 0) { is_done = 1; break; }
        }

        if (is_done) {
            const char *recur = j_str(task, "recurring");
            if (recur) {
                int secs = interval_seconds(recur);
                if (secs > 0) {
                    /* Advance to next run */
                    time_t next = parse_iso(j_str(task, "run_at"));
                    while (next <= now) next += secs;

                    char next_iso[32];
                    struct tm tm;
                    gmtime_r(&next, &tm);
                    snprintf(next_iso, sizeof(next_iso), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                             tm.tm_hour, tm.tm_min, tm.tm_sec);

                    cJSON *dup = cJSON_Duplicate(task, 1);
                    cJSON_SetValuestring(cJSON_GetObjectItem(dup, "run_at"), next_iso);
                    cJSON_AddItemToArray(remaining, dup);
                }
                /* else: one-shot, drop it */
            }
        } else {
            cJSON_AddItemToArray(remaining, cJSON_Duplicate(task, 1));
        }
    }

    save_tasks(s, remaining);
    cJSON_Delete(tasks);
    cJSON_Delete(remaining);
    pthread_mutex_unlock(&s->lock);
}
