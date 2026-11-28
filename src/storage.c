/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * storage.c — Backend dispatch and the in-memory backend.
 *
 * The in-memory backend is the test twin of /db: same statuses,
 * same sequential ids, same rows — so the unit tests of the
 * scheduler and the engine assert against exactly what the
 * production backend writes (PLAN.md section 4).
 */

#include "storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage_db.h"

struct orc_storage
  {
    int mode;                       /* 0 = mem, 1 = db */
    struct orc_mem_state mem;
    struct orc_db db;               /* db mode handle */
  };

int
orc_storage_open (struct orc_storage **out, const char *spec,
                  const char *conninfo)
{
  struct orc_storage *st = calloc (1, sizeof *st);

  if (st == NULL)
    return -1;
  st->mode = 0;

  if (spec != NULL && strcmp (spec, "mem") == 0)
    {
      /* in memory: nothing to prepare */
    }
  else
    {
      if (orc_db_open (&st->db, spec != NULL ? spec : "db:/db",
                       conninfo) < 0)
        {
          int e = errno;

          free (st);
          errno = e;
          return -1;
        }
      st->mode = 1;
    }

  *out = st;
  return 0;
}

void
orc_storage_close (struct orc_storage *st)
{
  if (st == NULL)
    return;
  if (st->mode == 1)
    orc_db_close (&st->db);
  free (st);
}

/* --- In-memory backend --------------------------------------------- */

static enum orc_storage_status
mem_write_run (struct orc_storage *st, const struct orc_run_row *row,
               long long *id)
{
  struct orc_mem_state *m = &st->mem;

  if (m->n_runs >= 128)
    return ORC_ST_INVALID;
  snprintf (m->runs[m->n_runs].descriptor,
            sizeof m->runs[0].descriptor, "%s", row->descriptor);
  snprintf (m->runs[m->n_runs].aggregate_strategy,
            sizeof m->runs[0].aggregate_strategy, "%s",
            row->aggregate_strategy);
  m->n_runs++;
  m->last_run_id++;
  *id = m->last_run_id;
  return ORC_ST_OK;
}

static enum orc_storage_status
mem_write_instance (struct orc_storage *st, const struct orc_inst_row *row,
                    long long *id)
{
  struct orc_mem_state *m = &st->mem;

  if (m->n_instances >= 512)
    return ORC_ST_INVALID;
  m->instances[m->n_instances].run_id = row->run_id;
  snprintf (m->instances[m->n_instances].topology,
            sizeof m->instances[0].topology, "%s", row->topology);
  snprintf (m->instances[m->n_instances].input,
            sizeof m->instances[0].input, "%s",
            row->input != NULL ? row->input : "null");
  snprintf (m->instances[m->n_instances].output,
            sizeof m->instances[0].output, "%s",
            row->output != NULL ? row->output : "null");
  snprintf (m->instances[m->n_instances].status,
            sizeof m->instances[0].status, "%s", row->status);
  m->n_instances++;
  m->instances[m->n_instances - 1].run_id = row->run_id;
  *id = m->n_instances;
  return ORC_ST_OK;
}

static enum orc_storage_status
mem_write_incident (struct orc_storage *st, const struct orc_incident_row *row,
                    long long *id)
{
  struct orc_mem_state *m = &st->mem;

  if (m->n_incidents >= 128)
    return ORC_ST_INVALID;
  m->incidents[m->n_incidents].run_id = row->run_id;
  m->incidents[m->n_incidents].instance_id = row->instance_id;
  snprintf (m->incidents[m->n_incidents].kind,
            sizeof m->incidents[0].kind, "%s", row->kind);
  m->n_incidents++;
  if (id != NULL)
    *id = m->n_incidents;
  return ORC_ST_OK;
}

static enum orc_storage_status
mem_write_training (struct orc_storage *st, const struct orc_training_row *row,
                    long long *id)
{
  struct orc_mem_state *m = &st->mem;

  if (m->n_training >= 256)
    return ORC_ST_INVALID;
  snprintf (m->training[m->n_training].source_url,
            sizeof m->training[0].source_url, "%s", row->source_url);
  m->training[m->n_training].http_status = row->http_status;
  snprintf (m->training[m->n_training].checksum,
            sizeof m->training[0].checksum, "%s", row->checksum);
  m->n_training++;
  if (id != NULL)
    *id = m->n_training;
  return ORC_ST_OK;
}

/* --- In-memory select (unit tests; mirrors the db wire shape) ----- */

static enum orc_storage_status
mem_select (struct orc_storage *st, const char *table,
            const char *where_col, const char *where_val, long limit,
            char *out, size_t cap, long *count)
{
  struct orc_mem_state *m = &st->mem;
  size_t len = 0;
  long n = 0;

  if (strcmp (table, "runs") == 0)
    {
      for (int i = 0; i < m->n_runs && n < limit; i++)
        {
          char idbuf[24];

          snprintf (idbuf, sizeof idbuf, "%lld", m->last_run_id - m->n_runs + 1 + i);
          if (where_col != NULL && strcmp (where_col, "id") == 0
              && strcmp (where_val, idbuf) != 0)
            continue;
          {
            int w = snprintf (out + len, cap - len,
                              "%s{\"id\": %s, \"descriptor\": %s, "
                              "\"aggregate_strategy\": \"%s\"}",
                              n > 0 ? "," : "", idbuf,
                              m->runs[i].descriptor,
                              m->runs[i].aggregate_strategy);

            if (w < 0 || (size_t) w >= cap - len)
              break;
            len += (size_t) w;
            n++;
          }
        }
    }
  else if (strcmp (table, "run_instances") == 0)
    {
      for (int i = 0; i < m->n_instances && n < limit; i++)
        {
          char idbuf[24];
          char ridbuf[24];

          snprintf (idbuf, sizeof idbuf, "%d", i + 1);
          snprintf (ridbuf, sizeof ridbuf, "%lld", m->instances[i].run_id);
          if (where_col != NULL
              && ((strcmp (where_col, "id") == 0
                   && strcmp (where_val, idbuf) != 0)
                  || (strcmp (where_col, "run_id") == 0
                      && strcmp (where_val, ridbuf) != 0)))
            continue;
          {
            int w = snprintf (out + len, cap - len,
                              "%s{\"id\": %s, \"run_id\": %lld, "
                              "\"topology\": \"%s\", \"input\": %s, "
                              "\"output\": %s, \"status\": \"%s\"}",
                              n > 0 ? "," : "", idbuf,
                              m->instances[i].run_id,
                              m->instances[i].topology,
                              m->instances[i].input,
                              m->instances[i].output[0] != '\0'
                                ? m->instances[i].output : "null",
                              m->instances[i].status);

            if (w < 0 || (size_t) w >= cap - len)
              break;
            len += (size_t) w;
            n++;
          }
        }
    }
  else
    return ORC_ST_INVALID;

  if (len < cap)
    out[len] = '\0';
  *count = n;
  return n > 0 ? ORC_ST_OK : ORC_ST_EMPTY;
}

/* --- Dispatch -------------------------------------------------------- */

enum orc_storage_status
orc_storage_write_run (struct orc_storage *st,
                      const struct orc_run_row *row, long long *id)
{
  if (st->mode == 1)
    return orc_db_write_run (&st->db, row, id);
  return mem_write_run (st, row, id);
}

enum orc_storage_status
orc_storage_write_instance (struct orc_storage *st,
                            const struct orc_inst_row *row,
                            long long *id)
{
  if (st->mode == 1)
    return orc_db_write_instance (&st->db, row, id);
  return mem_write_instance (st, row, id);
}

enum orc_storage_status
orc_storage_write_incident (struct orc_storage *st,
                            const struct orc_incident_row *row,
                            long long *id)
{
  if (st->mode == 1)
    return orc_db_write_incident (&st->db, row, id);
  return mem_write_incident (st, row, id);
}

enum orc_storage_status
orc_storage_write_training (struct orc_storage *st,
                            const struct orc_training_row *row,
                            long long *id)
{
  if (st->mode == 1)
    return orc_db_write_training (&st->db, row, id);
  return mem_write_training (st, row, id);
}

enum orc_storage_status
orc_storage_select (struct orc_storage *st, const char *table,
                    const char *where_col, const char *where_val,
                    long limit, char *out, size_t cap, long *count)
{
  if (st->mode == 1)
    return orc_db_select (&st->db, table, where_col, where_val, limit,
                          out, cap, count);
  return mem_select (st, table, where_col, where_val, limit, out, cap,
                     count);
}

const struct orc_mem_state *
orc_storage_mem_state (struct orc_storage *st)
{
  return st->mode == 0 ? &st->mem : NULL;
}

size_t
orc_storage_json_vector (const double *v, int n, char *buf, size_t cap)
{
  size_t len = 0;

  if (cap < 2)
    return 0;
  buf[len++] = '[';
  for (int i = 0; i < n; i++)
    {
      int w = snprintf (buf + len, cap - len, i > 0 ? ",%.6g" : "%.6g",
                        v[i]);

      if (w < 0 || (size_t) w >= cap - len)
        return 0;
      len += (size_t) w;
    }
  if (len + 2 > cap)
    return 0;
  buf[len++] = ']';
  buf[len] = '\0';
  return len;
}
