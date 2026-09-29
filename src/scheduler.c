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

#define TIME_HELP "Use local time as YYYY-MM-DDTHH:MM:SS, e.g. 2026-01-31T18:30:00"
#define INTERVAL_HELP "every_30min, hourly, every_6h, every_12h, daily, weekly"

static int interval_seconds(const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; INTERVALS[i].name; i++)
        if (strcmp(INTERVALS[i].name, name) == 0)
            return INTERVALS[i].seconds;
    return 0;
}

/* NULL when scheduler.json is corrupt: it must not be overwritten then */
static cJSON *load_tasks(scheduler_t *s) {
    return json_load(s->path, 1);
}

static const char *corrupt(scheduler_t *s, char *out, size_t out_sz) {
    snprintf(out, out_sz, "Error: %s is not valid JSON: fix or delete it", s->path);
    return out;
}

static int two_digits(const char *p) {
    return p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9'
        ? (p[0] - '0') * 10 + (p[1] - '0') : -1;
}

/* ISO 8601: no zone = local time, 'Z' or ±HH[:MM] as given. Also takes a
 * space for 'T', no seconds, and fractional seconds. 0 if invalid.
 * Parsed by hand: sscanf scansets are not portable (Cosmopolitan). */
time_t sched_parse_time(const char *iso) {
    struct tm tm = {0};
    const char *p = iso;
    if (!p) return 0;
    for (int i = 0; i < 4; i++, p++) {
        if (*p < '0' || *p > '9') return 0;
        tm.tm_year = tm.tm_year * 10 + (*p - '0');
    }
    if (*p != '-' || (tm.tm_mon = two_digits(p + 1)) < 0 || p[3] != '-' ||
        (tm.tm_mday = two_digits(p + 4)) < 0 || (p[6] != 'T' && p[6] != ' ') ||
        (tm.tm_hour = two_digits(p + 7)) < 0 || p[9] != ':' ||
        (tm.tm_min = two_digits(p + 10)) < 0)
        return 0;
    p += 12;
    if (*p == ':') {
        tm.tm_sec = two_digits(p + 1);
        if (tm.tm_sec < 0) return 0;
        p += 3;
        if (*p == '.')
            for (p++; *p >= '0' && *p <= '9'; p++) {}
    }
    if (tm.tm_mon < 1 || tm.tm_mon > 12 || tm.tm_mday < 1 || tm.tm_mday > 31 ||
        tm.tm_hour > 23 || tm.tm_min > 59 || tm.tm_sec > 60)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;

    if (*p == 'Z' || *p == 'z')
        return timegm(&tm);
    if (*p == '+' || *p == '-') {
        int sign = *p == '-' ? -1 : 1;
        int oh = two_digits(p + 1), om = 0;
        if (oh < 0) return 0;
        p += 3;
        if (*p == ':') p++;
        if (two_digits(p) >= 0) om = two_digits(p);
        return timegm(&tm) - sign * (oh * 3600 + om * 60);
    }
    tm.tm_isdst = -1;
    return mktime(&tm);
}

void scheduler_init(scheduler_t *s, const char *json_path) {
    snprintf(s->path, sizeof(s->path), "%s", json_path);
    pthread_mutex_init(&s->lock, NULL);
    mkdirs_for(json_path);
}

/* Validate run_at and interval; NULL when fine, else the error in out.
 * A past one-shot time would fire at once: small models do that to
 * "reschedule" a task that is due, and loop. A past recurring start moves
 * to its next occurrence, written to next and returned through *run_at. */
static const char *check_task(const char **run_at, const char *interval,
                              char next[32], char *out, size_t out_sz) {
    time_t t = 0, now = time(NULL);
    if (*run_at && (*run_at)[0] && (t = sched_parse_time(*run_at)) == 0) {
        snprintf(out, out_sz, "Error: invalid run_at '%s'. " TIME_HELP, *run_at);
        return out;
    }
    if (interval && interval[0] && !interval_seconds(interval)) {
        snprintf(out, out_sz, "Error: invalid interval '%s'. Use one of: " INTERVAL_HELP,
                 interval);
        return out;
    }
    if (t && t <= now) {
        if (!interval || !interval[0]) {
            char now_str[32];
            format_time(now, 1, now_str, sizeof(now_str));
            snprintf(out, out_sz, "Error: run_at %s is not in the future (local time is "
                     "now %s). Pick a later time, or do the task now instead of "
                     "scheduling it.", *run_at, now_str);
            return out;
        }
        while (t <= now) t += interval_seconds(interval);
        format_time(t, 0, next, 32);
        *run_at = next;
    }
    return NULL;
}

static void set_string(cJSON *obj, const char *key, const char *value) {
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddStringToObject(obj, key, value);
}

const char *sched_add(scheduler_t *s, const char *run_at, const char *desc,
                      const char *prompt, const char *interval,
                      char *out, size_t out_sz) {
    char tid[10], now[32], next[32];
    uuid_short(tid, 8);
    now_iso(now, sizeof(now));

    if (!run_at || !run_at[0] || !desc || !desc[0]) {
        snprintf(out, out_sz, "Error: run_at and description are required");
        return out;
    }
    if (check_task(&run_at, interval, next, out, out_sz))
        return out;
    int recurring = interval && interval[0];

    cJSON *task = cJSON_CreateObject();
    cJSON_AddStringToObject(task, "id", tid);
    cJSON_AddStringToObject(task, "run_at", run_at);
    cJSON_AddStringToObject(task, "description", desc);
    cJSON_AddStringToObject(task, "prompt", prompt ? prompt : "");
    if (recurring)
        cJSON_AddStringToObject(task, "recurring", interval);
    else
        cJSON_AddNullToObject(task, "recurring");
    cJSON_AddStringToObject(task, "created_at", now);

    pthread_mutex_lock(&s->lock);
    cJSON *tasks = load_tasks(s);
    int rc = -2;
    if (tasks) {
        cJSON_AddItemToArray(tasks, task);
        task = NULL;
        rc = json_save_atomic(s->path, tasks, 1);
        cJSON_Delete(tasks);
    }
    pthread_mutex_unlock(&s->lock);
    cJSON_Delete(task);

    if (rc == -2)
        return corrupt(s, out, out_sz);
    if (rc != 0)
        snprintf(out, out_sz, "Error: cannot save the task");
    else
        snprintf(out, out_sz, "Task scheduled (id=%s): %s at %s%s%s", tid, desc, run_at,
                 recurring ? ", recurring " : "", recurring ? interval : "");
    return out;
}

/* Tasks sorted by run time, one line each ("" when none) */
const char *sched_list(scheduler_t *s, char *out, size_t out_sz) {
    pthread_mutex_lock(&s->lock);
    cJSON *tasks = load_tasks(s);
    pthread_mutex_unlock(&s->lock);
    if (!tasks)
        return corrupt(s, out, out_sz);

    int n = cJSON_GetArraySize(tasks);
    cJSON **sorted = calloc((size_t)n + 1, sizeof(cJSON *));
    int count = 0;
    cJSON *task;
    cJSON_ArrayForEach(task, tasks) {
        if (!sorted) break;
        time_t t = sched_parse_time(j_str(task, "run_at"));
        int pos = count++;
        while (pos > 0 && sched_parse_time(j_str(sorted[pos - 1], "run_at")) > t) {
            sorted[pos] = sorted[pos - 1];
            pos--;
        }
        sorted[pos] = task;
    }

    size_t off = 0;
    out[0] = '\0';
    for (int i = 0; i < count; i++) {
        const char *id = j_str(sorted[i], "id");
        const char *desc = j_str(sorted[i], "description");
        const char *run_at = j_str(sorted[i], "run_at");
        const char *recur = j_str(sorted[i], "recurring");
        const char *prompt = j_str(sorted[i], "prompt");
        buf_appendf(out, out_sz, &off, "- [%s] %s, at %s%s%s\n",
                    id ? id : "?", desc ? desc : "", run_at ? run_at : "?",
                    recur ? ", recurring " : "", recur ? recur : "");
        if (prompt && prompt[0])
            buf_appendf(out, out_sz, &off, "  prompt: %.*s\n",
                        utf8_prefix(prompt, 300), prompt);
    }
    free(sorted);
    cJSON_Delete(tasks);
    return out;
}

/* The task with this id, or NULL */
static cJSON *find_task(cJSON *tasks, const char *id) {
    cJSON *task;
    cJSON_ArrayForEach(task, tasks) {
        const char *tid = j_str(task, "id");
        if (tid && strcmp(tid, id) == 0)
            return task;
    }
    return NULL;
}

const char *sched_update(scheduler_t *s, const char *id, const char *run_at,
                         const char *desc, const char *prompt,
                         const char *interval, char *out, size_t out_sz) {
    if (!id || !id[0]) {
        snprintf(out, out_sz, "Error: task_id is required");
        return out;
    }

    char next[32];
    const char *err = NULL;
    pthread_mutex_lock(&s->lock);
    cJSON *tasks = load_tasks(s);
    cJSON *task = find_task(tasks, id);
    if (!tasks) {
        err = corrupt(s, out, out_sz);
    } else if (!task) {
        snprintf(out, out_sz, "Error: task %s not found.", id);
        err = out;
    } else {
        /* The past-time rule follows the interval the task will have */
        err = check_task(&run_at, interval ? interval : j_str(task, "recurring"),
                         next, out, out_sz);
    }
    if (!err) {
        if (run_at && run_at[0]) set_string(task, "run_at", run_at);
        if (desc && desc[0]) set_string(task, "description", desc);
        if (prompt) set_string(task, "prompt", prompt);
        if (interval) {
            cJSON_DeleteItemFromObject(task, "recurring");
            if (interval[0])
                cJSON_AddStringToObject(task, "recurring", interval);
            else
                cJSON_AddNullToObject(task, "recurring");
        }
        if (json_save_atomic(s->path, tasks, 1) != 0) {
            snprintf(out, out_sz, "Error: cannot save the task");
            err = out;
        }
    }
    cJSON_Delete(tasks);
    pthread_mutex_unlock(&s->lock);

    if (!err)
        snprintf(out, out_sz, "Task %s updated.", id);
    return out;
}

const char *sched_cancel(scheduler_t *s, const char *id,
                         char *out, size_t out_sz) {
    if (!id || !id[0]) {
        snprintf(out, out_sz, "Error: task_id is required");
        return out;
    }

    pthread_mutex_lock(&s->lock);
    cJSON *tasks = load_tasks(s);
    cJSON *task = find_task(tasks, id);
    if (!tasks)
        corrupt(s, out, out_sz);
    else if (!task)
        snprintf(out, out_sz, "Error: task %s not found.", id);
    else {
        cJSON_Delete(cJSON_DetachItemViaPointer(tasks, task));
        if (json_save_atomic(s->path, tasks, 1) != 0)
            snprintf(out, out_sz, "Error: cannot save the task list");
        else
            snprintf(out, out_sz, "Task %s cancelled.", id);
    }
    cJSON_Delete(tasks);
    pthread_mutex_unlock(&s->lock);
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
        time_t t = sched_parse_time(j_str(task, "run_at"));
        if (t > 0 && t <= now) {
            cJSON_AddItemToArray(due, cJSON_Duplicate(task, 1));
            count++;
        }
    }

    cJSON_Delete(tasks);
    *out = due;
    return count;
}

/* One-shot tasks go away; recurring ones move to their next run */
void sched_mark_done(scheduler_t *s, cJSON *ids) {
    pthread_mutex_lock(&s->lock);
    cJSON *tasks = load_tasks(s);
    time_t now = time(NULL);
    cJSON *id;
    cJSON_ArrayForEach(id, ids) {
        cJSON *task = find_task(tasks, cJSON_GetStringValue(id) ? id->valuestring : "");
        if (!task) continue;
        int secs = interval_seconds(j_str(task, "recurring"));
        if (secs <= 0) {
            cJSON_Delete(cJSON_DetachItemViaPointer(tasks, task));
            continue;
        }
        time_t next = sched_parse_time(j_str(task, "run_at"));
        while (next <= now) next += secs;
        char next_iso[32];
        format_time(next, 0, next_iso, sizeof(next_iso));
        set_string(task, "run_at", next_iso);
    }
    if (tasks)
        json_save_atomic(s->path, tasks, 1);
    cJSON_Delete(tasks);
    pthread_mutex_unlock(&s->lock);
}
