/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * test_engine.c — End-to-end unit test of the engine: the real
 * scheduler and aggregator, the dir: instance transport (a file
 * that behaves like /llm<N> is enough, PLAN.md section 7) and
 * the mem storage backend (tests only).  The assertions read the
 * in-memory twin of /db: same rows, same ids, as the production
 * backend writes them.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "contract.h"
#include "orchestrate.h"
#include "storage.h"

static int checks_run = 0;
static int checks_failed = 0;

#define CHECK(cond)                                                     \
  do                                                                    \
    {                                                                   \
      checks_run++;                                                     \
      if (!(cond))                                                      \
        {                                                               \
          checks_failed++;                                              \
          printf ("test_engine: FAIL: %s (%s:%d)\n", #cond,            \
                  __FILE__, __LINE__);                                  \
        }                                                               \
    }                                                                   \
  while (0)

#define CHECK_STR(a, b)                                                 \
  do                                                                    \
    {                                                                   \
      checks_run++;                                                     \
      if ((a) == NULL || strstr ((a), (b)) == NULL)                     \
        {                                                               \
          checks_failed++;                                              \
          printf ("test_engine: FAIL: %s contains \"%s\""               \
                  " (got \"%s\", %s:%d)\n",                             \
                  #a, (b), (a) ? (a) : "(null)",                        \
                  __FILE__, __LINE__);                                  \
        }                                                               \
    }                                                                   \
  while (0)

/* The instance simulator: three instances that answer, one that
 * stays silent (a "failed" instance, exactly what the phase 2
 * supervisor will detect in production). */
static void
seed_instance (const char *dir, int id, const char *outputs)
{
  char path[512];
  FILE *f;

  snprintf (path, sizeof path, "%s/llm%d.out", dir, id);
  f = fopen (path, "w");
  if (f == NULL)
    exit (2);
  fprintf (f, "Output:\n%s\n", outputs);
  fclose (f);
}

static void
process (const char *line)
{
  if (orc_engine_process_line (line, strlen (line)) < 0)
    {
      printf ("test_engine: FAIL: transport failure on %s\n", line);
      checks_failed++;
    }
}

/* Submit a run descriptor and wait for the asynchronous run to
 * finish (phase 2: the write answers "submitted", the worker
 * thread publishes the final state).  Leaves the RESULT slot in
 * the engine, like a reader of /orchestrate would see. */
static void
process_run (const char *line)
{
  int tries;

  process (line);
  CHECK_STR (orc_engine_response (), "\"state\": \"submitted\"");
  for (tries = 0; tries < 500; tries++)
    {
      process ("{\"command\": \"result\"}");
      if (strstr (orc_engine_response (), "\"state\": \"done\"")
          || strstr (orc_engine_response (), "\"state\": \"failed\""))
        return;
      usleep (10000);
    }
  printf ("test_engine: FAIL: run did not finish in 5s\n");
  checks_failed++;
}

int
main (void)
{
  struct orc_config cfg = { 0 };
  char command[1024];
  char dirtemplate[] = "inst-sim-XXXXXX";
  char *dir;

  /* The test owns its scratch directory of simulated instances,
   * created and removed here — nothing leaks into the tree. */
  dir = mkdtemp (dirtemplate);
  if (dir == NULL)
    {
      printf ("test_engine: FAIL: mkdtemp\n");
      return 2;
    }

  /* The test instances: two agree, one disagrees, one silent. */
  seed_instance (dir, 1, "[0]: 0.9\n[1]: 0.8");
  seed_instance (dir, 2, "[0]: 0.8\n[1]: 0.9");
  seed_instance (dir, 3, "[0]: 0.2\n[1]: 0.1");
  {
    char path[512];
    FILE *f;

    snprintf (path, sizeof path, "%s/llm4.out", dir);
    f = fopen (path, "w");                /* no output line at all */
    if (f == NULL)
      exit (2);
    fprintf (f, "Output:\n(nothing)\n");
    fclose (f);
  }

  cfg.storage = "mem";
  cfg.mount = true;
  /* The dir: transport of the test instances, from the argument. */
  {
    static char spec[512];

    snprintf (spec, sizeof spec, "dir:%s", dir);
    cfg.instio = spec;
  }
  if (orc_engine_init (&cfg) < 0)
    {
      printf ("test_engine: FAIL: engine init\n");
      return 2;
    }

  /* 1. The idle status is the initial response. */
  CHECK_STR (orc_engine_response (), "\"state\": \"idle\"");
  CHECK_STR (orc_engine_response (), "\"instances\": []");

  /* 2. A full run: 4 instances, topologies cycled, majority. */
  snprintf (command, sizeof command,
            "{\"instances\": 4, \"topologies\": [\"2,4,1\", \"2,8,1\"],"
            " \"input\": [0.5, 0.3], \"aggregate\": \"majority\"}");
  process_run (command);
  CHECK_STR (orc_engine_response (), "\"run_id\": 1");
  CHECK_STR (orc_engine_response (), "\"aggregate_strategy\": \"majority\"");
  CHECK_STR (orc_engine_response (), "\"output\": \"[1,1]\"");
  /* Two of the three answering instances fully agree with the
   * aggregate, one does not: 2/3. */
  CHECK_STR (orc_engine_response (), "\"confidence\": 0.667");
  CHECK_STR (orc_engine_response (), "\"state\": \"done\"");

  /* 3. The status of the run (trio type 2). */
  process ("{\"command\": \"status\"}");
  CHECK_STR (orc_engine_response (), "\"run_id\": 1");
  CHECK_STR (orc_engine_response (), "\"state\": \"done\"");
  CHECK_STR (orc_engine_response (), "\"id\": 1");
  CHECK_STR (orc_engine_response (), "\"state\": \"failed\"");
  CHECK_STR (orc_engine_response (), "\"topology\": \"2,8,1\"");

  /* 4. The mem twin of /db holds the rows, as collected —
   * durability from the first execution. */
  {
    const struct orc_mem_state *mem =
      orc_storage_mem_state (orc_engine_storage ());

    CHECK (mem != NULL);
    CHECK (mem->n_runs == 1);
    CHECK (mem->n_instances == 4);
    CHECK_STR (mem->runs[0].aggregate_strategy, "majority");
    CHECK_STR (mem->runs[0].descriptor, "\"instances\": 4");
    CHECK_STR (mem->instances[0].input, "[0.5,0.3]");
    CHECK_STR (mem->instances[0].output, "[0.9,0.8]");
    CHECK_STR (mem->instances[0].status, "ok");
    CHECK_STR (mem->instances[3].status, "failed");
    /* The instance that stayed silent persisted no output. */
    CHECK (strcmp (mem->instances[3].output, "null") == 0);
  }

  /* 5. Invalid commands are answers, never crashes. */
  process ("{\"instances\": 0}");
  CHECK_STR (orc_engine_response (), "\"invalid\": \"instances\"");
  process ("{\"select\": 1}");
  /* Like /db: the offending key IS the detail of the answer. */
  CHECK_STR (orc_engine_response (), "\"invalid\": \"select\"");

  /* 6. The result is still servable. */
  process ("{\"command\": \"result\"}");
  CHECK_STR (orc_engine_response (), "\"output\": \"[1,1]\"");

  /* 7. Phase A3: history aggregates run_instances per topology. */
  process ("{\"command\": \"history\"}");
  CHECK_STR (orc_engine_response (), "\"command\": \"history\"");
  CHECK_STR (orc_engine_response (), "\"topology\": \"2,4,1\"");
  CHECK_STR (orc_engine_response (), "\"total\": 2");
  CHECK_STR (orc_engine_response (), "\"topology\": \"2,8,1\"");

  /* 8. Phase A3: replay of run 1 re-executes from the archive and
   * matches the historical aggregate (deterministic simulator). */
  process ("{\"command\": \"replay\", \"run_id\": 1}");
  CHECK_STR (orc_engine_response (), "\"command\": \"replay\"");
  CHECK_STR (orc_engine_response (), "\"run_id\": 1");
  CHECK_STR (orc_engine_response (), "\"match\": true");
  CHECK_STR (orc_engine_response (), "\"replayed\": [1,1]");
  CHECK_STR (orc_engine_response (), "\"historical\": [1,1]");

  /* 9. Replay of an unknown run is a readable answer. */
  process ("{\"command\": \"replay\", \"run_id\": 999}");
  CHECK_STR (orc_engine_response (), "\"error\": \"not_found\"");

  /* 10. Phase A4: instance rows carry scores, weights were learned. */
  {
    const struct orc_mem_state *mem =
      orc_storage_mem_state (orc_engine_storage ());

    CHECK (mem->instances[0].has_score == true);
    CHECK (mem->instances[0].score > 0.9);      /* agrees with [1,1] */
    CHECK (mem->instances[3].has_score == false); /* failed: no score */
    CHECK (mem->n_aggregator >= 2);              /* 2,4,1 and 2,8,1 */
  }

  /* 11. Phase A4: watch is a pure-read state view. */
  process ("{\"command\": \"watch\"}");
  CHECK_STR (orc_engine_response (), "\"command\": \"watch\"");
  CHECK_STR (orc_engine_response (), "\"run\": {");
  CHECK_STR (orc_engine_response (), "\"incidents\": [");

  /* 12. Phase A4: "topologies": "auto" picks the learned best. */
  snprintf (command, sizeof command,
            "{\"instances\": 2, \"topologies\": \"auto\","
            " \"input\": [0.5, 0.3], \"aggregate\": \"majority\"}");
  process_run (command);
  CHECK_STR (orc_engine_response (), "\"state\": \"done\"");
  process ("{\"command\": \"status\"}");
  /* the learned weights rank 2,4,1 first (both runs used it most) */
  CHECK_STR (orc_engine_response (), "\"topology\": \"2,4,1\"");

  orc_engine_shutdown ();
  /* Leave the tree exactly as we found it (distcheck checks). */
  for (int i = 1; i <= 4; i++)
    {
      char p[512];

      snprintf (p, sizeof p, "%s/llm%d.in", dir, i);
      unlink (p);
      snprintf (p, sizeof p, "%s/llm%d.out", dir, i);
      unlink (p);
    }
  rmdir (dir);

  printf ("test_engine: %d checks, %d failures\n",
          checks_run, checks_failed);
  return checks_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
