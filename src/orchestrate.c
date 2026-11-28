/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * orchestrate.c — The command engine of /orchestrate.
 *
 * Commands (SPEC.md section 5): run a task descriptor (bare or
 * under "run"), ask for the status (trio type 2), ask for the
 * result (trio type 3).  Since phase 2 (PLAN.md section 5) a run
 * is asynchronous: the write returns immediately with
 * {"run_id": N, "state": "submitted"} and the worker thread
 * executes the run; status/result reflect the live state
 * ("running", then "done"/"failed").
 *
 * The state is one static block, pre-allocated once: the write
 * path never allocates (the discipline of the stack).  A mutex
 * serializes the command thread (trivfs loop, REPL) and the
 * worker thread of the current run.
 */

#include "orchestrate.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "aggregator.h"
#include "scheduler.h"
#include "sha256.h"
#include "tokenize.h"

/* The engine state: configuration handles, the last run, and
 * the response slot served by reads. */
static struct
  {
    struct orc_instio io;
    struct orc_storage *storage;
    bool valid;

    /* Last run */
    bool has_run;
    long long run_id;
    enum { RUN_IDLE = 0, RUN_RUNNING, RUN_DONE, RUN_FAILED } run_state;
    struct orc_descriptor pending;  /* descriptor of the running run */
    pthread_mutex_t lock;
    int n_instances;
    struct
      {
        char topology[ORC_MAX_TOPOLOGY + 1];
        bool ok;
      } inst[ORC_MAX_INSTANCES];
    char strategy[16];
    struct orc_aggregate agg;       /* valid when run_state == RUN_DONE */
    int n_ok;

    /* Response slot */
    char slot[ORC_MAX_RESPONSE];
    size_t slot_len;
  } eng;

static char eng_scratch[ORC_SCRATCH];

/* Escape IN into OUT as the body of a JSON string (no surrounding
 * quotes), bounded by CAP and always NUL-terminated.  Topologies are
 * free-form strings from the task descriptor: emitting them raw in
 * the status JSON would break the line as soon as one contains a
 * quote.  ORC_MAX_TOPOLOGY * 6 + 1 covers the worst case. */
static void
json_escape (const char *in, char *out, size_t cap)
{
  size_t n = 0;

  for (const unsigned char *p = (const unsigned char *) in;
       *p != '\0' && n + 7 < cap; p++)
    {
      unsigned char c = *p;

      switch (c)
        {
        case '"':  out[n++] = '\\'; out[n++] = '"';  break;
        case '\\': out[n++] = '\\'; out[n++] = '\\'; break;
        case '\n': out[n++] = '\\'; out[n++] = 'n';  break;
        case '\r': out[n++] = '\\'; out[n++] = 'r';  break;
        case '\t': out[n++] = '\\'; out[n++] = 't';  break;
        default:
          if (c < 0x20)
            n += (size_t) snprintf (out + n, cap - n, "\\u%04x", c);
          else
            out[n++] = (char) c;
          break;
        }
    }
  out[n] = '\0';
}

/* ---------------------------------------------------------------------
 *  Response slot
 * ------------------------------------------------------------------- */

static void
slot_set (size_t len)
{
  eng.slot_len = len;
}

/* ---------------------------------------------------------------------
 *  Status and result (the trio JSON, SPEC.md section 7)
 * ------------------------------------------------------------------- */

static void
build_status (void)
{
  size_t n = 0;
  int w;

  if (!eng.has_run)
    {
      w = snprintf (eng.slot, sizeof eng.slot,
                    "{\"run_id\": 0, \"state\": \"idle\","
                    " \"instances\": []}");
      eng.slot_len = w > 0 && (size_t) w < sizeof eng.slot
                       ? (size_t) w : 0;
      return;
    }

  const char *state = eng.run_state == RUN_DONE ? "done"
                      : eng.run_state == RUN_RUNNING ? "running" : "failed";

  w = snprintf (eng.slot, sizeof eng.slot,
                "{\"run_id\": %lld, \"state\": \"%s\", \"instances\": [",
                eng.run_id, state);
  if (w < 0 || (size_t) w >= sizeof eng.slot)
    {
      eng.slot_len = 0;
      return;
    }
  n = (size_t) w;

  for (int i = 0; i < eng.n_instances; i++)
    {
      char esc[ORC_MAX_TOPOLOGY * 6 + 1];

      json_escape (eng.inst[i].topology, esc, sizeof esc);
      if (i > 0 && n + 1 < sizeof eng.slot)
        eng.slot[n++] = ',';
      w = snprintf (eng.slot + n, sizeof eng.slot - n,
                    "{\"id\": %d, \"topology\": \"%s\","
                    " \"state\": \"%s\"}",
                    i + 1, esc,
                    eng.inst[i].ok ? "done" : "failed");
      if (w < 0 || (size_t) w >= sizeof eng.slot - n)
        {
          eng.slot_len = 0;
          return;
        }
      n += (size_t) w;
    }
  w = snprintf (eng.slot + n, sizeof eng.slot - n, "]}");
  if (w < 0 || (size_t) w >= sizeof eng.slot - n)
    {
      eng.slot_len = 0;
      return;
    }
  eng.slot_len = n + (size_t) w;
}

static void
build_result (void)
{
  /* The aggregated vector, serialized as a string (the trio type
   * 3 keeps "output" a string; the richer synthesis belongs to
   * the interface). */
  char outbuf[ORC_MAX_OUTPUT * 16 + 4];
  int w;

  if (!eng.has_run || eng.run_state != RUN_DONE)
    {
      if (eng.has_run)
        {
          /* A running or failed run has no aggregate yet: an
           * honest state, not an empty result. */
          w = snprintf (eng.slot, sizeof eng.slot,
                        "{\"run_id\": %lld, \"state\": \"%s\","
                        " \"aggregate_strategy\": \"%s\","
                        " \"output\": \"\", \"confidence\": 0}",
                        eng.run_id,
                        eng.run_state == RUN_RUNNING ? "running" : "failed",
                        eng.strategy);
        }
      else
        w = snprintf (eng.slot, sizeof eng.slot, "{\"empty\": true}");
      eng.slot_len = w > 0 && (size_t) w < sizeof eng.slot
                       ? (size_t) w : 0;
      return;
    }

  if (orc_storage_json_vector (eng.agg.out, eng.agg.n, outbuf,
                               sizeof outbuf) == 0)
    {
      eng.slot_len = 0;
      return;
    }
  w = snprintf (eng.slot, sizeof eng.slot,
                "{\"run_id\": %lld, \"state\": \"done\","
                " \"aggregate_strategy\": \"%s\","
                " \"output\": \"%s\", \"confidence\": %.3f}",
                eng.run_id, eng.strategy, outbuf, eng.agg.confidence);
  eng.slot_len = w > 0 && (size_t) w < sizeof eng.slot
                   ? (size_t) w : 0;
}

/* ---------------------------------------------------------------------
 *  One run (asynchronous since phase 2)
 * ------------------------------------------------------------------- */

/* Worker thread of the current run: execute the descriptor over
 * the instances (the supervisor of the scheduler persists each
 * instance row and each incident), aggregate, then publish the
 * final state and the result slot. */
static void *
run_worker (void *arg)
{
  struct orc_descriptor desc = *(struct orc_descriptor *) arg;
  struct orc_sched_result sched;
  struct orc_aggregate agg;
  long long run_id;

  pthread_mutex_lock (&eng.lock);
  run_id = eng.run_id;
  pthread_mutex_unlock (&eng.lock);

  if (orc_scheduler_run (&eng.io, eng.storage, &desc, run_id, &sched) < 0)
    {
      pthread_mutex_lock (&eng.lock);
      eng.run_state = RUN_FAILED;
      build_result ();
      pthread_mutex_unlock (&eng.lock);
      return NULL;
    }

  pthread_mutex_lock (&eng.lock);
  eng.n_instances = sched.n;
  eng.n_ok = sched.n_ok;
  for (int i = 0; i < sched.n && i < ORC_MAX_INSTANCES; i++)
    {
      strcpy (eng.inst[i].topology, sched.inst[i].topology);
      eng.inst[i].ok = sched.inst[i].ok;
    }
  if (orc_aggregate (desc.aggregate, &sched, &agg) < 0)
    eng.run_state = RUN_FAILED;             /* no instance answered */
  else
    {
      eng.run_state = RUN_DONE;
      eng.agg = agg;
    }
  build_result ();
  pthread_mutex_unlock (&eng.lock);
  return NULL;
}

/* Submit one descriptor: persist the run first (durability from
 * the first execution), then hand it to a worker thread and
 * answer immediately.  Returns 0, or -1 with errno == EIO
 * (transport only). */
static int
submit_run (const struct orc_descriptor *desc)
{
  struct orc_run_row run_row;
  long long run_id = 0;
  pthread_t tid;
  int w;

  pthread_mutex_lock (&eng.lock);
  if (eng.has_run && eng.run_state == RUN_RUNNING)
    {
      /* One run at a time for now; the descriptor queue of
       * PLAN phase 2 is the next increment. */
      w = snprintf (eng.slot, sizeof eng.slot,
                    "{\"busy\": true, \"run_id\": %lld, \"state\": \"running\"}",
                    eng.run_id);
      eng.slot_len = w > 0 && (size_t) w < sizeof eng.slot
                       ? (size_t) w : 0;
      pthread_mutex_unlock (&eng.lock);
      return 0;
    }

  run_row.descriptor = desc->raw;
  run_row.aggregate_strategy = desc->aggregate;
  {
    enum orc_storage_status st =
      orc_storage_write_run (eng.storage, &run_row, &run_id);

    if (st == ORC_ST_TRANSPORT)
      {
        pthread_mutex_unlock (&eng.lock);
        errno = EIO;                        /* transport only */
        return -1;
      }
    if (st != ORC_ST_OK)
      {
        /* The base refused the row (a constraint is a status):
         * the run is not started, the caller learns why in the
         * response line — no EIO, no partial state. */
        slot_set (orc_response_invalid (eng.slot, sizeof eng.slot,
                                        ORC_INVALID_VALUE, "run"));
        pthread_mutex_unlock (&eng.lock);
        return 0;
      }
  }

  eng.has_run = true;
  eng.run_id = run_id;
  eng.run_state = RUN_RUNNING;
  eng.n_instances = desc->instances;
  eng.n_ok = 0;
  strcpy (eng.strategy, desc->aggregate);
  for (int i = 0; i < desc->instances && i < ORC_MAX_INSTANCES; i++)
    {
      strcpy (eng.inst[i].topology,
              desc->topologies[i % desc->n_topologies]);
      eng.inst[i].ok = false;
    }
  eng.pending = *desc;

  w = snprintf (eng.slot, sizeof eng.slot,
                "{\"run_id\": %lld, \"state\": \"submitted\"}", run_id);
  eng.slot_len = w > 0 && (size_t) w < sizeof eng.slot ? (size_t) w : 0;

  if (pthread_create (&tid, NULL, run_worker, &eng.pending) == 0)
    pthread_detach (tid);
  else
    {
      /* No thread available: run inline so the contract still
       * holds (the response becomes the final result). */
      pthread_mutex_unlock (&eng.lock);
      run_worker (&eng.pending);
      return 0;
    }
  pthread_mutex_unlock (&eng.lock);
  return 0;
}

/* ---------------------------------------------------------------------
 *  Phase 3: fetch — acquire a URL through httpfs, archive the content
 *  in training_data (before any use), tokenize it into an input
 *  vector.
 * ------------------------------------------------------------------- */

static struct orc_config eng_cfg;

#define FETCH_MOUNT   "/page"
#define FETCH_MAX_BODY (200u * 1024u)

/* shell_quote — S into DST as a single-quoted /bin/sh word. */
static void
shell_quote (char *dst, size_t dstsz, const char *s)
{
  size_t n = 0;

  if (dstsz == 0)
    return;
  dst[n++] = '\'';
  for (const char *p = s; *p != '\0' && n + 3 < dstsz; p++)
    {
      if (*p == '\'')
        {
          dst[n++] = '\'';
          dst[n++] = '\\';
          dst[n++] = '\'';
          dst[n++] = '\'';
        }
      else
        dst[n++] = *p;
    }
  dst[n++] = '\'';
  dst[n] = '\0';
}

/* find_httpfs — the httpfs translator binary for the fetch mount. */
static const char *
find_httpfs (void)
{
  const char *env;

  if (eng_cfg.httpfs != NULL && eng_cfg.httpfs[0] != '\0')
    return eng_cfg.httpfs;
  env = getenv ("HTTPFS_BIN");
  if (env != NULL && env[0] != '\0' && access (env, X_OK) == 0)
    return env;
  if (access ("/root/src/httpfs-translator/src/httpfs", X_OK) == 0)
    return "/root/src/httpfs-translator/src/httpfs";
  if (access ("/usr/local/bin/httpfs", X_OK) == 0)
    return "/usr/local/bin/httpfs";
  return "httpfs";
}

/* read_small_file — whole content of PATH into BUF; returns the
 * length, or -1. */
static long
read_small_file (const char *path, char *buf, size_t cap)
{
  int fd = open (path, O_RDONLY);
  ssize_t r;

  if (fd < 0)
    return -1;
  r = read (fd, buf, cap - 1);
  close (fd);
  if (r < 0)
    return -1;
  buf[r] = '\0';
  return (long) r;
}

/* fetch_url — mount httpfs on FETCH_MOUNT for URL, read the HTTP
 * status and the body (bounded), archive the content in
 * training_data, and tokenize it into VEC.  Returns 0 and fills
 * STATUS/ID/BLEN/VEC/NVEC, or -1 (transport/mount). */
static int
fetch_url (const char *url, long *status, long long *id, size_t *blen,
           double *vec, int *nvec)
{
  char q[2200], b[2200], cmd[4800];
  static char body[FETCH_MAX_BODY + 1];
  char stbuf[64];
  long slen;
  char sum[65];
  struct orc_training_row row;
  char *nl;

  shell_quote (q, sizeof q, url);
  shell_quote (b, sizeof b, find_httpfs ());
  snprintf (cmd, sizeof cmd,
            "settrans -g " FETCH_MOUNT " 2>/dev/null; "
            "mkdir -p " FETCH_MOUNT "; "
            "settrans -a " FETCH_MOUNT " %s %s 2>/dev/null", b, q);
  if (system (cmd) != 0)
    return -1;

  slen = read_small_file (FETCH_MOUNT "/status", stbuf, sizeof stbuf);
  if (slen <= 0)
    {
      system ("settrans -g " FETCH_MOUNT " 2>/dev/null");
      errno = EIO;
      return -1;
    }
  *status = strtol (stbuf, &nl, 10);

  *blen = 0;
  if (*status >= 200 && *status < 300)
    {
      long r = read_small_file (FETCH_MOUNT "/content", body,
                                sizeof body);

      if (r > 0)
        *blen = (size_t) r;
    }

  /* Archive BEFORE any use (PLAN phase 3), even a non-200 (the 404
   * trace belongs to training_data too). */
  orc_sha256_hex (body, *blen, sum);
  row.source_url = url;
  row.http_status = *status;
  row.content = body;
  row.checksum = sum;
  {
    enum orc_storage_status st = orc_storage_write_training (eng.storage,
                                                            &row, id);

    if (st == ORC_ST_TRANSPORT)
      {
        system ("settrans -g " FETCH_MOUNT " 2>/dev/null");
        errno = EIO;
        return -1;
      }
  }

  *nvec = orc_tokenize (body, *blen, vec, ORC_MAX_INPUT);
  system ("settrans -g " FETCH_MOUNT " 2>/dev/null");
  return 0;
}

/* do_fetch — {"command":"fetch","url":...}: archive + tokenize, and
 * answer with the vector so the caller can run on it. */
static void
do_fetch (const char *url)
{
  long status = 0;
  long long id = 0;
  size_t blen = 0;
  double vec[ORC_MAX_INPUT];
  int nvec = 0;
  char esc[2048 * 2];
  char vecbuf[ORC_MAX_INPUT * 16 + 4];
  int w;

  if (fetch_url (url, &status, &id, &blen, vec, &nvec) < 0)
    {
      slot_set (orc_response_invalid (eng.slot, sizeof eng.slot,
                                      ORC_INVALID_VALUE, "fetch"));
      return;
    }
  json_escape (url, esc, sizeof esc);
  if (orc_storage_json_vector (vec, nvec, vecbuf, sizeof vecbuf) == 0)
    strcpy (vecbuf, "[]");
  w = snprintf (eng.slot, sizeof eng.slot,
                "{\"url\": \"%s\", \"http_status\": %ld, \"id\": %lld,"
                " \"bytes\": %zu, \"vector\": %s}",
                esc, status, id, blen, vecbuf);
  eng.slot_len = w > 0 && (size_t) w < sizeof eng.slot ? (size_t) w : 0;
}

/* do_fetch_run — fetch_run: archive the URL, then submit a run whose
 * input vector is the tokenized content. */
static int
do_fetch_run (const struct orc_request *req)
{
  long status = 0;
  long long id = 0;
  size_t blen = 0;
  double vec[ORC_MAX_INPUT];
  int nvec = 0;
  struct orc_descriptor desc = req->desc;
  int rc;

  if (fetch_url (req->url, &status, &id, &blen, vec, &nvec) < 0)
    return -1;                              /* EIO: transport */
  desc.n_input = nvec;
  memcpy (desc.input, vec, (size_t) nvec * sizeof *vec);
  /* The persisted descriptor keeps the fetched input: rebuild the
   * raw line cheaply is not possible, so persist the original run
   * object — the vector is reproducible from the archived content. */
  rc = submit_run (&desc);
  return rc;
}

/* ---------------------------------------------------------------------
 *  Init and command processing
 * ------------------------------------------------------------------- */

int
orc_engine_init (const struct orc_config *cfg)
{
  struct orc_config defaults = { 0 };

  if (cfg == NULL)
    cfg = &defaults;
  eng_cfg = *cfg;

  if (orc_instio_open (&eng.io, cfg->instio, cfg->neuron_bin,
                       cfg->mount) < 0)
    return -1;
  if (orc_storage_open (&eng.storage, cfg->storage, cfg->conninfo) < 0)
    return -1;
  eng.valid = true;
  eng.has_run = false;
  eng.run_state = RUN_IDLE;
  pthread_mutex_init (&eng.lock, NULL);

  build_status ();                           /* the idle slot */
  return 0;
}

void
orc_engine_shutdown (void)
{
  if (!eng.valid)
    return;
  orc_storage_close (eng.storage);
  eng.storage = NULL;
  eng.valid = false;
}

int
orc_engine_process_line (const char *line, size_t len)
{
  struct orc_request req;
  int rc;

  while (len > 0 && line[len - 1] == '\r')
    len--;
  if (len >= ORC_MAX_LINE)
    {
      pthread_mutex_lock (&eng.lock);
      slot_set (orc_response_invalid (eng.slot, sizeof eng.slot,
                                       ORC_INVALID_TOOLONG, NULL));
      pthread_mutex_unlock (&eng.lock);
      return 0;
    }

  orc_parse (line, len, eng_scratch, sizeof eng_scratch, &req);

  switch (req.command)
    {
    case ORC_CMD_INVALID:
      pthread_mutex_lock (&eng.lock);
      slot_set (orc_response_invalid (eng.slot, sizeof eng.slot,
                                       req.invalid,
                                       req.invalid_detail));
      pthread_mutex_unlock (&eng.lock);
      return 0;

    case ORC_CMD_STATUS:
      pthread_mutex_lock (&eng.lock);
      build_status ();
      pthread_mutex_unlock (&eng.lock);
      return 0;

    case ORC_CMD_RESULT:
      pthread_mutex_lock (&eng.lock);
      if (!eng.has_run)
        slot_set (orc_response_empty (eng.slot, sizeof eng.slot));
      else
        build_result ();
      pthread_mutex_unlock (&eng.lock);
      return 0;

    case ORC_CMD_RUN:
      /* submit_run takes the lock itself (it spawns the worker). */
      rc = submit_run (&req.desc);
      return rc;

    case ORC_CMD_FETCH:
      pthread_mutex_lock (&eng.lock);
      do_fetch (req.url);
      pthread_mutex_unlock (&eng.lock);
      return 0;

    case ORC_CMD_FETCH_RUN:
      /* fetch once, then submit with the tokenized vector. */
      return do_fetch_run (&req);

    default:
      errno = EINVAL;
      return -1;
    }
}

const char *
orc_engine_response (void)
{
  return eng.slot;
}

size_t
orc_engine_response_len (void)
{
  size_t n;

  pthread_mutex_lock (&eng.lock);
  n = eng.slot_len;
  pthread_mutex_unlock (&eng.lock);
  return n;
}

struct orc_storage *
orc_engine_storage (void)
{
  return eng.storage;
}
