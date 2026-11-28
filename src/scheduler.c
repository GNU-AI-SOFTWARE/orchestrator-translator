/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * scheduler.c — Starting and collecting the instances of one run.
 *
 * For each instance: mount (node mode) or open (dir mode), write
 * the topology line, write the input line, read the state text,
 * pick the output vector out of it, persist the row — then move
 * to the next instance.  A failed instance is a status ("failed"),
 * never a crash of the run: the run survives with fewer answers,
 * which is exactly what the aggregator is for (PLAN.md section
 * 5, phase 2 generalizes this to mid-run losses).
 */

#include "scheduler.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "neuron.h"

/* One attempt at collecting the output of instance ID: mount or
 * open, write topology, write input, read the state text.  Returns
 * true and fills OUT/N_OUT when an output vector was collected. */
static bool
try_instance (struct orc_instio *io, int id, const char *topology,
              const char *input_line, char *text, size_t text_cap,
              double *out, int *n_out)
{
  struct orc_inst inst;
  long len;

  if (orc_inst_begin (io, id, &inst) < 0)
    return false;
  if (orc_inst_write (&inst, topology) < 0)
    {
      orc_inst_end (&inst);
      return false;
    }
  if (orc_inst_write (&inst, input_line) < 0)
    {
      orc_inst_end (&inst);
      return false;
    }
  len = orc_inst_read (&inst, text, text_cap);
  orc_inst_end (&inst);
  if (len < 0)
    return false;
  text[len < (long) text_cap ? len : (long) text_cap - 1] = '\0';
  *n_out = orc_neuron_parse_outputs (text, (size_t) len, out,
                                     ORC_MAX_OUTPUT);
  return *n_out > 0;
}

int
orc_scheduler_run (struct orc_instio *io, struct orc_storage *st,
                   const struct orc_descriptor *desc,
                   long long run_id, struct orc_sched_result *res)
{
  char input_line[ORC_MAX_INPUT * 16 + 2];
  char text[ORC_INST_TEXT_MAX];
  double out[ORC_MAX_OUTPUT];

  memset (res, 0, sizeof *res);
  res->n = desc->instances;

  if (orc_neuron_input_line (desc->input, desc->n_input, input_line,
                             sizeof input_line) == 0)
    {
      errno = EINVAL;                       /* unreachable: bounded */
      return -1;
    }

  for (int i = 0; i < desc->instances; i++)
    {
      struct orc_inst_out *o = &res->inst[i];
      const char *topology =
        desc->topologies[i % desc->n_topologies];
      int n_out = -1;
      bool ok = false;
      const char *incident_kind = NULL;

      strcpy (o->topology, topology);

      /* Supervisor (PLAN phase 2): one restart per failed instance,
       * each failure persisted in the incidents table. */
      ok = try_instance (io, i + 1, topology, input_line, text,
                         sizeof text, out, &n_out);
      if (!ok)
        {
          struct orc_incident_row inc;

          inc.run_id = run_id;
          inc.instance_id = 0;
          inc.kind = "crash";
          orc_storage_write_incident (st, &inc, NULL);
          ok = try_instance (io, i + 1, topology, input_line, text,
                             sizeof text, out, &n_out);
          if (!ok)
            incident_kind = "restart";
        }
      if (ok)
        {
          o->n_out = n_out;
          memcpy (o->out, out, (size_t) n_out * sizeof *out);
          res->n_ok++;
        }

      o->ok = ok;
      {
        struct orc_inst_row row;
        char input_json[1024];
        char output_json[ORC_MAX_OUTPUT * 16 + 4];
        long long id = 0;

        row.run_id = run_id;
        row.topology = o->topology;
        /* A failed render must never leave the stack buffer
         * uninitialized: fall back to JSON null. */
        if (orc_storage_json_vector (desc->input, desc->n_input, input_json,
                                     sizeof input_json) == 0)
          strcpy (input_json, "null");
        row.input = input_json;
        if (ok)
          {
            if (orc_storage_json_vector (o->out, o->n_out, output_json,
                                         sizeof output_json) == 0)
              strcpy (output_json, "null");
            row.output = output_json;
          }
        else
          row.output = NULL;
        row.status = ok ? "ok" : "failed";

        /* Durability from the first execution: the row is written
         * as soon as the instance is collected.  A transport
         * failure of /db fails the run as EIO — the caller keeps
         * what was already persisted. */
        if (orc_storage_write_instance (st, &row, &id) == ORC_ST_TRANSPORT)
          {
            errno = EIO;
            return -1;
          }
        /* An INVALID/EMPTY answer of the base is a status: the
         * instance row is simply not there, the run continues. */
        if (!ok)
          {
            struct orc_incident_row inc;

            inc.run_id = run_id;
            inc.instance_id = id;
            inc.kind = incident_kind != NULL ? incident_kind : "crash";
            orc_storage_write_incident (st, &inc, NULL);
          }
      }
    }

  return 0;
}
