/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * netfs-hooks.c — Hurd netfs server of /orchestrate (phase A1b).
 *
 * The mount point is a directory with two file views served fresh
 * from the engine on every read (netfs reads carry the offset, so
 * there is no shared cursor):
 *
 *   /orchestrate/status   trio type 2 (live state of the run)
 *   /orchestrate/result   trio type 3 (last aggregate)
 *
 * Commands keep the phase 1 line protocol on the mount point
 * itself: a write on the root node assembles complete lines and
 * hands each one to the engine; reads on the root node still serve
 * the response slot (backward compatible with the trivfs phase).
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <dirent.h>
#include <unistd.h>

#include "contract.h"
#include "orchestrate.h"

#if ON_HURD == 1

#include <mach/port.h>
typedef mach_port_t *processor_name_array_t;

#include <hurd/netfs.h>
#include <hurd/iohelp.h>
#include <hurd/fsys.h>

/* ---------------------------------------------------------------------
 *  libnetfs identity (required symbols)
 * ------------------------------------------------------------------- */

char *netfs_server_name = "orchestrator-translator";
char *netfs_server_version = VERSION;
int netfs_maxsymlinks = 0;

/* ---------------------------------------------------------------------
 *  Nodes
 * ------------------------------------------------------------------- */

enum orc_kind { ORC_DIR = 0, ORC_STATUS, ORC_RESULT };

struct netnode
  {
    struct node *node;
    enum orc_kind kind;
    struct netnode *parent;
    struct netnode *sibling;       /* next child of parent */
    char wbuf[ORC_MAX_LINE];        /* root only: line assembly */
    size_t wlen;
  };

static struct netnode orc_root;
static struct netnode orc_status_nd;
static struct netnode orc_result_nd;

/* Create the fixed tree once, before netfs_startup. */
void
orc_netfs_init_tree (void)
{
  memset (&orc_root, 0, sizeof orc_root);
  memset (&orc_status_nd, 0, sizeof orc_status_nd);
  memset (&orc_result_nd, 0, sizeof orc_result_nd);
  orc_root.kind = ORC_DIR;
  orc_status_nd.kind = ORC_STATUS;
  orc_status_nd.parent = &orc_root;
  orc_status_nd.sibling = &orc_result_nd;
  orc_result_nd.kind = ORC_RESULT;
  orc_result_nd.parent = &orc_root;

  netfs_root_node = netfs_make_node (&orc_root);
  orc_root.node = netfs_root_node;
  orc_root.node->nn_stat.st_mode = S_IFDIR | 0777;
  orc_root.node->nn_stat.st_nlink = 3;
  orc_root.node->nn_translated = S_IFDIR | 0777;

  orc_status_nd.node = netfs_make_node (&orc_status_nd);
  orc_result_nd.node = netfs_make_node (&orc_result_nd);
  orc_status_nd.node->nn_stat.st_mode = S_IFREG | 0444;
  orc_result_nd.node->nn_stat.st_mode = S_IFREG | 0444;
  orc_status_nd.node->nn_translated = S_IFREG | 0444;
  orc_result_nd.node->nn_translated = S_IFREG | 0444;
}

static const char *
orc_node_name (const struct netnode *n)
{
  return n->kind == ORC_STATUS ? "status" : "result";
}

static struct netnode *
orc_child_by_name (struct netnode *dir, const char *name)
{
  for (struct netnode *c = dir->sibling == NULL && dir == &orc_root
                          ? &orc_status_nd : NULL; c; c = c->sibling)
    if (strcmp (name, orc_node_name (c)) == 0)
      return c;
  return NULL;
}

/* ---------------------------------------------------------------------
 *  libnetfs callbacks
 * ------------------------------------------------------------------- */

error_t
netfs_attempt_lookup (struct iouser *user, struct node *dir,
                      const char *name, struct node **np)
{
  struct netnode *dn = dir->nn;

  (void) user;
  *np = NULL;

  if (dn->kind != ORC_DIR)
    {
      pthread_mutex_unlock (&dir->lock);
      return ENOTDIR;
    }

  if (strcmp (name, ".") == 0 || strcmp (name, "..") == 0)
    {
      netfs_nref (dir);
      *np = dir;
      return 0;
    }

  struct netnode *cn = orc_child_by_name (dn, name);

  if (cn != NULL)
    {
      struct node *hit = cn->node;

      netfs_nref (hit);
      pthread_mutex_unlock (&dir->lock);
      pthread_mutex_lock (&hit->lock);
      *np = hit;
      return 0;
    }

  pthread_mutex_unlock (&dir->lock);
  return ENOENT;
}

#define DIRENTS_BUF (8 * 1024)
#define DIRENT_ALIGN 4
#define DIRENT_NAME_OFFS offsetof (struct dirent, d_name)
#define DIRENT_LEN(nl)                                                     \
    ((DIRENT_NAME_OFFS + (nl) + 1 + (DIRENT_ALIGN - 1))                    \
     & ~((size_t) (DIRENT_ALIGN - 1)))

error_t
netfs_get_dirents (struct iouser *cred, struct node *dir, int entry,
                   int nentries, char **data,
                   mach_msg_type_number_t *datacnt, vm_size_t bufsize,
                   int *amt)
{
  static const char *const names[] = { ".", "..", "status", "result" };
  const int nnames = 4;

  (void) cred;
  (void) bufsize;
  (void) dir;

  if (entry >= nnames)
    {
      *amt = 0;
      return 0;
    }

  char *buf = mmap (NULL, DIRENTS_BUF, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

  if (buf == MAP_FAILED)
    return ENOMEM;

  size_t off = 0;
  int count = 0;

  for (int i = entry; i < nnames && count < nentries; i++, count++)
    {
      size_t nl = strlen (names[i]);
      size_t dl = DIRENT_LEN (nl);

      if (off + dl > DIRENTS_BUF)
        break;
      struct dirent *de = (struct dirent *) (buf + off);

      de->d_fileno = (ino_t) (i + 2);
      de->d_reclen = (unsigned short) dl;
      de->d_type = i < 2 ? DT_DIR : DT_REG;
      de->d_namlen = (unsigned char) nl;
      memcpy (de->d_name, names[i], nl + 1);
      off += dl;
    }

  *data = buf;
  *datacnt = off;
  *amt = count;
  return 0;
}

error_t
netfs_validate_stat (struct node *np, struct iouser *cred)
{
  struct netnode *nn = np->nn;

  (void) cred;
  if (nn->kind == ORC_DIR)
    {
      np->nn_stat.st_mode = S_IFDIR | 0777;
      np->nn_stat.st_nlink = 3;
      np->nn_stat.st_size = 0;
    }
  else
    {
      char tmp[ORC_MAX_RESPONSE];
      size_t n = nn->kind == ORC_STATUS
                 ? orc_engine_build_status (tmp, sizeof tmp)
                 : orc_engine_build_result (tmp, sizeof tmp);

      np->nn_stat.st_mode = S_IFREG | 0444;
      np->nn_stat.st_nlink = 1;
      np->nn_stat.st_size = n;
    }
  np->nn_stat.st_blocks = 0;
  return 0;
}

error_t
netfs_attempt_read (struct iouser *cred, struct node *np,
                    loff_t offset, size_t *len, void *data)
{
  struct netnode *nn = np->nn;

  (void) cred;

  if (nn->kind == ORC_DIR)
    {
      /* Phase 1 compatibility: reads on the mount point serve the
       * response slot. */
      size_t n = orc_engine_response_len ();
      const char *slot = orc_engine_response ();

      if (offset < 0 || (size_t) offset >= n)
        {
          *len = 0;
          return 0;
        }
      size_t avail = n - (size_t) offset;

      if (*len > avail)
        *len = avail;
      memcpy (data, slot + offset, *len);
      return 0;
    }

  char tmp[ORC_MAX_RESPONSE];
  size_t n = nn->kind == ORC_STATUS
             ? orc_engine_build_status (tmp, sizeof tmp)
             : orc_engine_build_result (tmp, sizeof tmp);

  if (offset < 0 || (size_t) offset >= n)
    {
      *len = 0;
      return 0;
    }
  size_t avail = n - (size_t) offset;

  if (*len > avail)
    *len = avail;
  memcpy (data, tmp + offset, *len);
  return 0;
}

/* Commands are written to the mount point itself (phase 1 contract):
 * complete lines are handed to the engine, one response per line. */
error_t
netfs_attempt_write (struct iouser *cred, struct node *np,
                     loff_t offset, size_t *len, const void *data)
{
  struct netnode *nn = np->nn;

  (void) cred;
  (void) offset;

  if (nn->kind != ORC_DIR)
    return EROFS;

  const char *p = data;
  size_t remaining = *len;

  while (remaining > 0)
    {
      const char *nl = memchr (p, '\n', remaining);
      size_t take = nl != NULL ? (size_t) (nl - p) + 1 : remaining;

      if (nn->wlen + take >= sizeof nn->wbuf)
        {
          nn->wlen = 0;
          return EMSGSIZE;
        }
      memcpy (nn->wbuf + nn->wlen, p, take);
      nn->wlen += take;
      p += take;
      remaining -= take;

      if (nl != NULL)
        {
          nn->wbuf[nn->wlen] = '\0';
          if (orc_engine_process_line (nn->wbuf, nn->wlen) < 0)
            {
              nn->wlen = 0;
              return EIO;
            }
          nn->wlen = 0;
        }
    }
  return 0;
}

/* ---------------------------------------------------------------------
 *  Remaining callbacks: the tree is fixed and read-only except the
 *  command channel on the root.
 * ------------------------------------------------------------------- */

void
netfs_node_norefs (struct node *np)
{
  (void) np;
}

void
netfs_try_dropping_softrefs (struct node *np)
{
  (void) np;
}

error_t
netfs_check_open_permissions (struct iouser *user, struct node *np,
                              int flags, int newnode)
{
  (void) user;
  (void) np;
  (void) flags;
  (void) newnode;
  return 0;
}

error_t
netfs_report_access (struct iouser *cred, struct node *np, int *types)
{
  (void) cred;
  (void) np;
  *types = O_READ | O_WRITE;
  return 0;
}

struct iouser *
netfs_make_user (uid_t *uids, int nuids, uid_t *gids, int ngids)
{
  struct iouser *user = NULL;

  if (iohelp_create_complex_iouser (&user, uids, nuids, gids, ngids) != 0)
    return NULL;
  return user;
}

error_t netfs_attempt_set_size (struct iouser *c, struct node *n, loff_t s)
{ (void) c; (void) n; (void) s; return EROFS; }
error_t netfs_attempt_chown (struct iouser *c, struct node *n, uid_t u, gid_t g)
{ (void) c; (void) n; (void) u; (void) g; return EROFS; }
error_t netfs_attempt_chauthor (struct iouser *c, struct node *n, uid_t a)
{ (void) c; (void) n; (void) a; return EROFS; }
error_t netfs_attempt_chmod (struct iouser *c, struct node *n, mode_t m)
{ (void) c; (void) n; (void) m; return EROFS; }
error_t netfs_attempt_chflags (struct iouser *c, struct node *n, int f)
{ (void) c; (void) n; (void) f; return EROFS; }
error_t netfs_attempt_utimes (struct iouser *c, struct node *n,
                             struct timespec *a, struct timespec *m)
{ (void) c; (void) n; (void) a; (void) m; return EROFS; }
error_t netfs_attempt_unlink (struct iouser *u, struct node *d, const char *n)
{ (void) u; (void) d; (void) n; return EROFS; }
error_t netfs_attempt_rmdir (struct iouser *u, struct node *d, const char *n)
{ (void) u; (void) d; (void) n; return EROFS; }
error_t netfs_attempt_mkdir (struct iouser *u, struct node *d, const char *n,
                            mode_t m)
{ (void) u; (void) d; (void) n; (void) m; return EROFS; }
error_t netfs_attempt_rename (struct iouser *u, struct node *fd,
                             const char *fn, struct node *td, const char *tn,
                             int ex)
{ (void) u; (void) fd; (void) fn; (void) td; (void) tn; (void) ex; return EROFS; }
error_t netfs_attempt_link (struct iouser *u, struct node *d, struct node *f,
                           const char *n, int ex)
{ (void) u; (void) d; (void) f; (void) n; (void) ex; return EROFS; }
error_t netfs_attempt_mkfile (struct iouser *u, struct node *d,
                             mode_t m, struct node **np)
{ *np = NULL; pthread_mutex_unlock (&d->lock); (void) u; (void) m; return EROFS; }
error_t netfs_attempt_create_file (struct iouser *u, struct node *d,
                                  const char *n, mode_t m, struct node **np)
{ (void) u; (void) d; (void) n; (void) m; (void) np; return EROFS; }
error_t netfs_attempt_mksymlink (struct iouser *c, struct node *n, const char *t)
{ (void) c; (void) n; (void) t; return EROFS; }
error_t netfs_attempt_mkdev (struct iouser *c, struct node *n, mode_t t, dev_t d)
{ (void) c; (void) n; (void) t; (void) d; return EROFS; }
error_t netfs_attempt_readlink (struct iouser *u, struct node *n, char *b)
{ (void) u; (void) n; (void) b; return EINVAL; }
error_t netfs_attempt_statfs (struct iouser *c, struct node *n, fsys_statfsbuf_t *st)
{ (void) c; (void) n; memset (st, 0, sizeof *st); return 0; }
error_t netfs_attempt_sync (struct iouser *c, struct node *n, int w)
{ (void) c; (void) n; (void) w; return 0; }
error_t netfs_attempt_syncfs (struct iouser *c, int w)
{ (void) c; (void) w; return 0; }

#endif /* ON_HURD */
