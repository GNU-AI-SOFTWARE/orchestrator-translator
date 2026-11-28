/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * storage_db.c — The data-base-translator backend.
 *
 * The orchestrator never speaks SQL: it writes one instruction
 * line to /db and reads one response line — exactly the frozen
 * contract of data-base-translator (its SPEC.md).  Node mode
 * keeps one open descriptor on the translator node (each write
 * installs a fresh response, the read serves it).  Child mode
 * runs the db-translator verification binary over pipes for the
 * integration tests: same protocol, same answers.
 *
 * Expected constraints are statuses (duplicate, invalid); a
 * transport failure (no response, broken pipe) is
 * ORC_ST_TRANSPORT — surfaced as EIO by the engine, per the
 * philosophy of the stack.
 */

#include "storage_db.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* One instruction line is bounded by the /db contract; the
 * response is a tiny JSON line. */
#define ORC_DB_LINE_MAX 4096
#define ORC_DB_RESP_MAX 512

int
orc_db_open (struct orc_db *db, const char *spec, const char *conninfo)
{
  memset (db, 0, sizeof *db);
  db->fd = -1;
  db->in_fd = -1;
  db->out_fd = -1;
  db->pid = -1;

  if (strncmp (spec, "db:", 3) == 0)
    {
      const char *path = spec + 3;

      db->mode = 0;
      if (*path == '\0')
        path = "/db";
      db->fd = open (path, O_RDWR);
      if (db->fd < 0)
        return -1;
      return 0;
    }
  if (strncmp (spec, "dbexec:", 7) == 0)
    {
      const char *bin = spec + 7;
      int in_pipe[2];
      int out_pipe[2];
      pid_t pid;

      db->mode = 1;
      if (*bin == '\0')
        {
          errno = EINVAL;
          return -1;
        }
      snprintf (db->bin, sizeof db->bin, "%s", bin);

      if (pipe (in_pipe) < 0)
        return -1;
      if (pipe (out_pipe) < 0)
        {
          close (in_pipe[0]);
          close (in_pipe[1]);
          return -1;
        }

      pid = fork ();
      if (pid < 0)
        {
          close (in_pipe[0]);
          close (in_pipe[1]);
          close (out_pipe[0]);
          close (out_pipe[1]);
          return -1;
        }
      if (pid == 0)
        {
          /* The child becomes the db-translator verification
           * binary, speaking the same line protocol on stdio. */
          dup2 (in_pipe[0], 0);
          dup2 (out_pipe[1], 1);
          close (in_pipe[0]);
          close (in_pipe[1]);
          close (out_pipe[0]);
          close (out_pipe[1]);
          if (conninfo != NULL && conninfo[0] != '\0')
            execl (bin, bin, "--conninfo", conninfo, (char *) NULL);
          else
            execl (bin, bin, (char *) NULL);
          _exit (127);
        }

      close (in_pipe[0]);
      close (out_pipe[1]);
      db->pid = pid;
      db->in_fd = in_pipe[1];
      db->out_fd = out_pipe[0];
      return 0;
    }

  errno = EINVAL;
  return -1;
}

void
orc_db_close (struct orc_db *db)
{
  if (db->mode == 0)
    {
      if (db->fd >= 0)
        close (db->fd);
    }
  else
    {
      if (db->in_fd >= 0)
        close (db->in_fd);
      if (db->out_fd >= 0)
        close (db->out_fd);
      if (db->pid > 0)
        {
          int status;

          waitpid (db->pid, &status, 0);
        }
    }
  db->fd = -1;
  db->in_fd = -1;
  db->out_fd = -1;
  db->pid = -1;
}

/* Write one full line (newline added), then read one response
 * line.  Returns 0 with the response in RESP, or -1 with errno
 * (transport). */
static int
db_roundtrip (struct orc_db *db, const char *line, char *resp,
              size_t resp_cap)
{
  size_t len = strlen (line);
  char wbuf[ORC_DB_LINE_MAX + 2];
  size_t total = 0;

  if (len + 1 > sizeof wbuf)
    {
      errno = EMSGSIZE;
      return -1;
    }
  memcpy (wbuf, line, len);
  wbuf[len] = '\n';
  len++;

  if (db->mode == 0)
    {
      ssize_t w = write (db->fd, wbuf, len);

      if (w != (ssize_t) len)
        return -1;
      /* The write installed the response at cursor 0 of this
       * open: read it fully. */
      for (;;)
        {
          ssize_t r = read (db->fd, resp + total, resp_cap - total);

          if (r < 0)
            return -1;
          if (r == 0)
            break;
          total += (size_t) r;
          if (total >= resp_cap)
            {
              errno = EMSGSIZE;
              return -1;
            }
        }
    }
  else
    {
      ssize_t w = write (db->in_fd, wbuf, len);

      if (w != (ssize_t) len)
        return -1;
      /* The child answers exactly one line per instruction.  Always
       * read at most resp_cap - 1 - total bytes: the NUL written at
       * resp[total] after each read must stay inside the buffer (the
       * old code wrote it at resp[total + r], one byte past the end
       * when a response exactly filled the buffer). */
      for (;;)
        {
          ssize_t r;

          if (total >= resp_cap - 1)
            {
              errno = EMSGSIZE;
              return -1;
            }
          r = read (db->out_fd, resp + total, resp_cap - 1 - total);
          if (r < 0)
            return -1;
          if (r == 0)
            {
              errno = EPIPE;                 /* the child died */
              return -1;
            }
          total += (size_t) r;
          resp[total] = '\0';
          {
            char *nl = strchr (resp, '\n');

            if (nl != NULL)
              {
                *nl = '\0';
                return 0;
              }
          }
        }
    }

  resp[total] = '\0';
  /* Trim a trailing newline in node mode. */
  {
    char *nl = strchr (resp, '\n');

    if (nl != NULL)
      *nl = '\0';
  }
  return 0;
}

/* Interpret one /db response line into a storage status. */
static enum orc_storage_status
interpret (const char *resp, long long *id)
{
  long long v = 0;

  if (id == NULL)
    {
      static long long sink;

      id = &sink;
    }
  *id = 0;
  if (strncmp (resp, "{\"ok\": true, \"id\":", 18) == 0)
    {
      v = strtoll (resp + 18, NULL, 10);
      *id = v;
      return ORC_ST_OK;
    }
  if (strncmp (resp, "{\"duplicate\":", 13) == 0)
    {
      v = strtoll (resp + 13, NULL, 10);
      *id = v;
      return ORC_ST_DUPLICATE;
    }
  if (strncmp (resp, "{\"invalid\":", 11) == 0)
    return ORC_ST_INVALID;
  if (strncmp (resp, "{\"empty\":", 9) == 0)
    return ORC_ST_EMPTY;
  return ORC_ST_TRANSPORT;
}

/* Append S to BUF as an escaped JSON string literal ("..."), bounded
 * by CAP.  Returns the new length, or 0 when it does not fit (the
 * caller refuses the write as ORC_ST_INVALID): a topology containing
 * a quote must never break the instruction line sent to /db. */
static size_t
put_json_str (char *buf, size_t len, size_t cap, const char *s)
{
  if (len + 1 >= cap)
    return 0;
  buf[len++] = '"';
  for (const unsigned char *p = (const unsigned char *) s;
       *p != '\0'; p++)
    {
      unsigned char c = *p;
      const char *esc = NULL;
      char tmp[8];

      switch (c)
        {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default:
          if (c < 0x20)
            {
              snprintf (tmp, sizeof tmp, "\\u%04x", (unsigned) c);
              esc = tmp;
            }
          break;
        }
      size_t n = esc != NULL ? strlen (esc) : 1;

      if (len + n + 1 >= cap)
        return 0;
      if (esc != NULL)
        memcpy (buf + len, esc, n);
      else
        buf[len] = (char) c;
      len += n;
    }
  if (len + 1 >= cap)
    return 0;
  buf[len++] = '"';
  return len;
}

/* Submit one instruction, return its status. */
static enum orc_storage_status
db_submit (struct orc_db *db, const char *line, long long *id)
{
  char resp[ORC_DB_RESP_MAX];

  if (db_roundtrip (db, line, resp, sizeof resp) < 0)
    {
      /* Only the transport fails as a POSIX error (EIO at the
       * engine level); the caller retries on the next write. */
      return ORC_ST_TRANSPORT;
    }
  return interpret (resp, id);
}
enum orc_storage_status
orc_db_write_run (struct orc_db *db, const struct orc_run_row *row,
                  long long *id)
{
  char line[ORC_DB_LINE_MAX];
  size_t n;
  int w;

  /* descriptor is verbatim JSON by construction; aggregate_strategy
   * is escaped like any string field. */
  w = snprintf (line, sizeof line,
                "{\"table\": \"runs\", \"row\": "
                "{\"descriptor\": %s, \"aggregate_strategy\": ",
                row->descriptor);
  if (w < 0 || (size_t) w >= sizeof line)
    return ORC_ST_INVALID;
  n = (size_t) w;
  n = put_json_str (line, n, sizeof line, row->aggregate_strategy);
  if (n == 0)
    return ORC_ST_INVALID;
  w = snprintf (line + n, sizeof line - n, "}}");
  if (w < 0 || (size_t) w >= sizeof line - n)
    return ORC_ST_INVALID;
  return db_submit (db, line, id);
}

enum orc_storage_status
orc_db_write_instance (struct orc_db *db, const struct orc_inst_row *row,
                       long long *id)
{
  char line[ORC_DB_LINE_MAX];
  size_t n;
  int w;

  w = snprintf (line, sizeof line,
                "{\"table\": \"run_instances\", \"row\": "
                "{\"run_id\": %lld, \"topology\": ",
                row->run_id);
  if (w < 0 || (size_t) w >= sizeof line)
    return ORC_ST_INVALID;
  n = (size_t) w;
  n = put_json_str (line, n, sizeof line, row->topology);
  if (n == 0)
    return ORC_ST_INVALID;
  /* input/output are JSON vectors rendered by orc_storage_json_vector
   * (or "null"): they are already valid JSON, kept verbatim. */
  w = snprintf (line + n, sizeof line - n,
                ", \"input\": %s, \"output\": %s, \"status\": ",
                row->input != NULL ? row->input : "null",
                row->output != NULL ? row->output : "null");
  if (w < 0 || (size_t) w >= sizeof line - n)
    return ORC_ST_INVALID;
  n += (size_t) w;
  n = put_json_str (line, n, sizeof line, row->status);
  if (n == 0)
    return ORC_ST_INVALID;
  w = snprintf (line + n, sizeof line - n, "}}");
  if (w < 0 || (size_t) w >= sizeof line - n)
    return ORC_ST_INVALID;
  return db_submit (db, line, id);
}

/* training_data rows carry fetched content: the /db phase-1 contract
 * bounds one instruction at 256 Kio, so the line buffer is sized
 * accordingly (static: the write path never allocates). */
#define ORC_DB_TRAIN_LINE_MAX (280u * 1024u)

enum orc_storage_status
orc_db_write_training (struct orc_db *db, const struct orc_training_row *row,
                       long long *id)
{
  static char line[ORC_DB_TRAIN_LINE_MAX];
  size_t n;
  int w;

  w = snprintf (line, sizeof line,
                "{\"table\": \"training_data\", \"row\": "
                "{\"source_url\": ");
  if (w < 0 || (size_t) w >= sizeof line)
    return ORC_ST_INVALID;
  n = (size_t) w;
  n = put_json_str (line, n, sizeof line, row->source_url);
  if (n == 0)
    return ORC_ST_INVALID;
  w = snprintf (line + n, sizeof line - n,
                ", \"http_status\": %ld, \"content\": ", row->http_status);
  if (w < 0 || (size_t) w >= sizeof line - n)
    return ORC_ST_INVALID;
  n += (size_t) w;
  n = put_json_str (line, n, sizeof line, row->content);
  if (n == 0)
    return ORC_ST_INVALID;
  w = snprintf (line + n, sizeof line - n, ", \"checksum\": ");
  if (w < 0 || (size_t) w >= sizeof line - n)
    return ORC_ST_INVALID;
  n += (size_t) w;
  n = put_json_str (line, n, sizeof line, row->checksum);
  if (n == 0)
    return ORC_ST_INVALID;
  w = snprintf (line + n, sizeof line - n, "}}");
  if (w < 0 || (size_t) w >= sizeof line - n)
    return ORC_ST_INVALID;
  return db_submit (db, line, id);
}

enum orc_storage_status
orc_db_write_incident (struct orc_db *db, const struct orc_incident_row *row,
                       long long *id)
{
  char line[ORC_DB_LINE_MAX];
  size_t n;
  int w;

  w = snprintf (line, sizeof line,
                "{\"table\": \"incidents\", \"row\": "
                "{\"run_id\": %lld, \"instance_id\": ",
                row->run_id);
  if (w < 0 || (size_t) w >= sizeof line)
    return ORC_ST_INVALID;
  n = (size_t) w;
  if (row->instance_id > 0)
    w = snprintf (line + n, sizeof line - n, "%lld", row->instance_id);
  else
    w = snprintf (line + n, sizeof line - n, "null");
  if (w < 0 || (size_t) w >= sizeof line - n)
    return ORC_ST_INVALID;
  n += (size_t) w;
  w = snprintf (line + n, sizeof line - n, ", \"kind\": ");
  if (w < 0 || (size_t) w >= sizeof line - n)
    return ORC_ST_INVALID;
  n += (size_t) w;
  n = put_json_str (line, n, sizeof line, row->kind);
  if (n == 0)
    return ORC_ST_INVALID;
  w = snprintf (line + n, sizeof line - n, "}}");
  if (w < 0 || (size_t) w >= sizeof line - n)
    return ORC_ST_INVALID;
  return db_submit (db, line, id);
}
