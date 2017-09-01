/* nbdkit
 * Copyright (C) 2013-2016 Red Hat Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * * Neither the name of Red Hat nor the names of its contributors may be
 * used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY RED HAT AND CONTRIBUTORS ''AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL RED HAT OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <limits.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#include <pthread.h>

#include <dlfcn.h>

#include "nbdkit-plugin.h"
#include "internal.h"

#define FIRST_SOCKET_ACTIVATION_FD 3 /* defined by systemd ABI */

static char *make_random_fifo (void);
static void open_plugin_so (const char *filename);
static void start_serving (void);
static void set_up_signals (void);
static void run_command (void);
static void change_user (void);
static void write_pidfile (void);
static void fork_into_background (void);
static uid_t parseuser (const char *);
static gid_t parsegroup (const char *);
static unsigned int get_socket_activation (void);

int exit_with_parent;           /* --exit-with-parent */
const char *exportname;         /* -e */
int foreground;                 /* -f */
const char *ipaddr;             /* -i */
int newstyle;                   /* -n */
char *pidfile;                  /* -P */
const char *port;               /* -p */
int readonly;                   /* -r */
char *run;                      /* --run */
int listen_stdin;               /* -s */
char *unixsocket;               /* -U */
const char *user, *group;       /* -u & -g */
int verbose;                    /* -v */
unsigned int socket_activation  /* $LISTEN_FDS and $LISTEN_PID set */;

volatile int quit;

static char *random_fifo_dir = NULL;
static char *random_fifo = NULL;

enum { HELP_OPTION = CHAR_MAX + 1 };

static const char *short_options = "e:fg:i:nop:P:rsu:U:vV";
static const struct option long_options[] = {
  { "help",       0, NULL, HELP_OPTION },
  { "dump-config",0, NULL, 0 },
  { "dump-plugin",0, NULL, 0 },
  { "exit-with-parent", 0, NULL, 0 },
  { "export",     1, NULL, 'e' },
  { "export-name",1, NULL, 'e' },
  { "exportname", 1, NULL, 'e' },
  { "foreground", 0, NULL, 'f' },
  { "no-fork",    0, NULL, 'f' },
  { "group",      1, NULL, 'g' },
  { "ip-addr",    1, NULL, 'i' },
  { "ipaddr",     1, NULL, 'i' },
  { "new-style",  0, NULL, 'n' },
  { "newstyle",   0, NULL, 'n' },
  { "old-style",  0, NULL, 'o' },
  { "oldstyle",   0, NULL, 'o' },
  { "pid-file",   1, NULL, 'P' },
  { "pidfile",    1, NULL, 'P' },
  { "port",       1, NULL, 'p' },
  { "read-only",  0, NULL, 'r' },
  { "readonly",   0, NULL, 'r' },
  { "run",        1, NULL, 0 },
  { "single",     0, NULL, 's' },
  { "stdin",      0, NULL, 's' },
  { "unix",       1, NULL, 'U' },
  { "user",       1, NULL, 'u' },
  { "verbose",    0, NULL, 'v' },
  { "version",    0, NULL, 'V' },
  { NULL },
};

static void
usage (void)
{
  printf ("nbdkit [--dump-config] [--dump-plugin]\n"
          "       [-e EXPORTNAME] [--exit-with-parent] [-f]\n"
          "       [-g GROUP] [-i IPADDR]\n"
          "       [--newstyle] [--oldstyle] [-P PIDFILE] [-p PORT] [-r]\n"
          "       [--run CMD] [-s] [-U SOCKET] [-u USER] [-v] [-V]\n"
          "       PLUGIN [key=value [key=value [...]]]\n"
          "\n"
          "Please read the nbdkit(1) manual page for full usage.\n");
}

static void
display_version (void)
{
  printf ("%s %s\n", PACKAGE_NAME, PACKAGE_VERSION);
}

static void
dump_config (void)
{
  printf ("%s=%s\n", "bindir", bindir);
  printf ("%s=%s\n", "libdir", libdir);
  printf ("%s=%s\n", "mandir", mandir);
  printf ("%s=%s\n", "name", PACKAGE_NAME);
  printf ("%s=%s\n", "plugindir", plugindir);
  printf ("%s=%s\n", "sbindir", sbindir);
  printf ("%s=%s\n", "sysconfdir", sysconfdir);
  printf ("%s=%s\n", "version", PACKAGE_VERSION);
}

int
main (int argc, char *argv[])
{
  int c;
  int option_index;
  int help = 0, version = 0, dump_plugin = 0;

  tls_init ();

  /* Returns 0 if no socket activation, or the number of FDs. */
  socket_activation = get_socket_activation ();

  for (;;) {
    c = getopt_long (argc, argv, short_options, long_options, &option_index);
    if (c == -1)
      break;

    switch (c) {
    case 0:                     /* options which are long only */
      if (strcmp (long_options[option_index].name, "dump-config") == 0) {
        dump_config ();
        exit (EXIT_SUCCESS);
      }
      else if (strcmp (long_options[option_index].name, "dump-plugin") == 0) {
        dump_plugin = 1;
      }
      else if (strcmp (long_options[option_index].name, "exit-with-parent") == 0) {
#ifdef PR_SET_PDEATHSIG
        exit_with_parent = 1;
        foreground = 1;
#else
        fprintf (stderr, "%s: --exit-with-parent is not implemented for this operating system\n",
                 program_name);
        exit (EXIT_FAILURE);
#endif
      }
      else if (strcmp (long_options[option_index].name, "run") == 0) {
        if (socket_activation) {
          fprintf (stderr, "%s: cannot use socket activation with --run flag\n",
                   program_name);
          exit (EXIT_FAILURE);
        }
        run = optarg;
        foreground = 1;
      }
      else {
        fprintf (stderr, "%s: unknown long option: %s (%d)\n",
                 program_name, long_options[option_index].name, option_index);
        exit (EXIT_FAILURE);
      }
      break;

    case 'e':
      exportname = optarg;
      newstyle = 1;
      break;

    case 'f':
      foreground = 1;
      break;

    case 'g':
      group = optarg;
      break;

    case 'i':
      if (socket_activation) {
        fprintf (stderr, "%s: cannot use socket activation with -i flag\n",
                 program_name);
        exit (EXIT_FAILURE);
      }
      ipaddr = optarg;
      break;

    case 'n':
      newstyle = 1;
      break;

    case 'o':
      newstyle = 0;
      break;

    case 'P':
      pidfile = nbdkit_absolute_path (optarg);
      if (pidfile == NULL)
        exit (EXIT_FAILURE);
      break;

    case 'p':
      if (socket_activation) {
        fprintf (stderr, "%s: cannot use socket activation with -p flag\n",
                 program_name);
        exit (EXIT_FAILURE);
      }
      port = optarg;
      break;

    case 'r':
      readonly = 1;
      break;

    case 's':
      if (socket_activation) {
        fprintf (stderr, "%s: cannot use socket activation with -s flag\n",
                 program_name);
        exit (EXIT_FAILURE);
      }
      listen_stdin = 1;
      break;

    case 'U':
      if (socket_activation) {
        fprintf (stderr, "%s: cannot use socket activation with -U flag\n",
                 program_name);
        exit (EXIT_FAILURE);
      }
      if (strcmp (optarg, "-") == 0)
        unixsocket = make_random_fifo ();
      else
        unixsocket = nbdkit_absolute_path (optarg);
      if (unixsocket == NULL)
        exit (EXIT_FAILURE);
      break;

    case 'u':
      user = optarg;
      break;

    case 'v':
      verbose = 1;
      break;

    case 'V':
      version = 1;
      break;

    case HELP_OPTION:
      help = 1;
      break;

    default:
      usage ();
      exit (EXIT_FAILURE);
    }
  }

  /* No extra parameters. */
  if (optind >= argc) {
    if (help) {
      usage ();
      exit (EXIT_SUCCESS);
    }
    if (version) {
      display_version ();
      exit (EXIT_SUCCESS);
    }
    if (dump_plugin) {
      /* Incorrect use of --dump-plugin. */
      fprintf (stderr,
               "%s: use 'nbdkit plugin --dump-plugin' or\n"
               "'nbdkit /path/to/plugin.so --dump-plugin'\n",
               program_name);
      exit (EXIT_FAILURE);
    }

    /* Otherwise this is an error. */
    fprintf (stderr,
             "%s: no plugins given on the command line.\nRead nbdkit(1) for documentation.\n",
             program_name);
    exit (EXIT_FAILURE);
  }

  /* Oldstyle protocol + exportname not allowed. */
  if (newstyle == 0 && exportname != NULL) {
    fprintf (stderr,
             "%s: cannot use oldstyle protocol (-o) and exportname (-e)\n",
             program_name);
    exit (EXIT_FAILURE);
  }

  /* If exportname was not set on the command line, use "". */
  if (exportname == NULL)
    exportname = "";

  /* Implement --exit-with-parent early in case plugin initialization
   * takes a long time and the parent exits during that time.
   */
#ifdef PR_SET_PDEATHSIG
  if (exit_with_parent) {
    if (prctl (PR_SET_PDEATHSIG, SIGTERM) == -1) {
      perror ("prctl: PR_SET_PDEATHSIG");
      exit (EXIT_FAILURE);
    }
  }
#endif

  /* Remaining command line arguments define the plugins and plugin
   * configuration.  If --help or --version was specified, we still
   * partially parse these in order that we can display the per-plugin
   * help/version information.
   */
  while (optind < argc) {
    const char *filename = argv[optind];
    char *p;

    open_plugin_so (filename);

    if (help) {
      usage ();
      printf ("\n%s:\n\n", filename);
      plugin_usage ();
      exit (EXIT_SUCCESS);
    }

    if (version) {
      const char *v;

      display_version ();
      printf ("%s", plugin_name ());
      if ((v = plugin_version ()) != NULL)
        printf (" %s", v);
      printf ("\n");
      exit (EXIT_SUCCESS);
    }

    if (dump_plugin) {
      plugin_dump_fields ();
      exit (EXIT_SUCCESS);
    }

    /* Find key=value configuration parameters for this plugin. */
    ++optind;
    while (optind < argc && (p = strchr (argv[optind], '=')) != NULL) {
      if (help || version)
        continue;

      *p = '\0';
      plugin_config (argv[optind], p+1);

      ++optind;
    }

    plugin_config_complete ();

    /* If we supported export names, then we'd continue in the loop
     * here, but at the moment only one plugin may be used per server
     * so exit if there are any more.
     */
    ++optind;
    if (optind < argc) {
      fprintf (stderr, "%s: this server only supports a single plugin\n",
               program_name);
      exit (EXIT_FAILURE);
    }
  }

  start_serving ();

  plugin_cleanup ();

  free (unixsocket);
  free (pidfile);

  if (random_fifo) {
    unlink (random_fifo);
    free (random_fifo);
  }

  if (random_fifo_dir) {
    rmdir (random_fifo_dir);
    free (random_fifo_dir);
  }

  exit (EXIT_SUCCESS);
}

/* Implementation of '-U -' */
static char *
make_random_fifo (void)
{
  char template[] = "/tmp/nbdkitXXXXXX";
  char *unixsocket;

  if (mkdtemp (template) == NULL) {
    perror ("mkdtemp");
    return NULL;
  }

  random_fifo_dir = strdup (template);
  if (random_fifo_dir == NULL) {
    perror ("strdup");
    return NULL;
  }

  if (asprintf (&random_fifo, "%s/socket", template) == -1) {
    perror ("asprintf");
    return NULL;
  }

  unixsocket = strdup (random_fifo);
  if (unixsocket == NULL) {
    perror ("strdup");
    return NULL;
  }

  return unixsocket;
}

static void
open_plugin_so (const char *name)
{
  char *filename = (char *) name;
  int free_filename = 0;
  void *dl;
  struct nbdkit_plugin *(*plugin_init) (void);
  char *error;

  if (strchr (name, '.') == NULL && strchr (name, '/') == NULL) {
    /* Short names are rewritten relative to libdir. */
    if (asprintf (&filename, "%s/nbdkit-%s-plugin.so", plugindir, name) == -1) {
      perror ("asprintf");
      exit (EXIT_FAILURE);
    }
    free_filename = 1;
  }

  dl = dlopen (filename, RTLD_NOW|RTLD_GLOBAL);
  if (dl == NULL) {
    fprintf (stderr, "%s: %s: %s\n", program_name, filename, dlerror ());
    exit (EXIT_FAILURE);
  }

  /* Initialize the plugin.  See dlopen(3) to understand C weirdness. */
  dlerror ();
  *(void **) (&plugin_init) = dlsym (dl, "plugin_init");
  if ((error = dlerror ()) != NULL) {
    fprintf (stderr, "%s: %s: %s\n", program_name, name, error);
    exit (EXIT_FAILURE);
  }
  if (!plugin_init) {
    fprintf (stderr, "%s: %s: invalid plugin_init\n", program_name, name);
    exit (EXIT_FAILURE);
  }

  /* Register the plugin. */
  plugin_register (filename, dl, plugin_init);

  if (free_filename)
    free (filename);
}

static void
start_serving (void)
{
  int *socks;
  size_t nr_socks;
  size_t i;

  /* If the user has mixed up -p/-U/-s options, then give an error.
   *
   * XXX Actually the server could easily be extended to handle both
   * TCP/IP and Unix sockets, or even multiple TCP/IP ports.
   */
  if ((port && unixsocket) || (port && listen_stdin) ||
      (unixsocket && listen_stdin) || (listen_stdin && run)) {
    fprintf (stderr,
             "%s: -p, -U and -s options cannot appear at the same time\n",
             program_name);
    exit (EXIT_FAILURE);
  }

  set_up_signals ();

  /* Socket activation -- we are handling connections on pre-opened
   * file descriptors [FIRST_SOCKET_ACTIVATION_FD ..
   * FIRST_SOCKET_ACTIVATION_FD+nr_socks-1].
   */
  if (socket_activation) {
    nr_socks = socket_activation;
    debug ("using socket activation, nr_socks = %zu", nr_socks);
    socks = malloc (sizeof (int) * nr_socks);
    if (socks == NULL) {
      perror ("malloc");
      exit (EXIT_FAILURE);
    }
    for (i = 0; i < nr_socks; ++i)
      socks[i] = FIRST_SOCKET_ACTIVATION_FD + i;
    change_user ();
    write_pidfile ();
    accept_incoming_connections (socks, nr_socks);
    free_listening_sockets (socks, nr_socks); /* also closes them */
    return;
  }

  /* Handling a single connection on stdin/stdout. */
  if (listen_stdin) {
    change_user ();
    write_pidfile ();
    tls_new_server_thread ();
    if (handle_single_connection (0, 1) == -1)
      exit (EXIT_FAILURE);
    return;
  }

  /* Handling multiple connections on TCP/IP or a Unix domain socket. */
  if (unixsocket)
    socks = bind_unix_socket (&nr_socks);
  else
    socks = bind_tcpip_socket (&nr_socks);

  run_command ();
  change_user ();
  fork_into_background ();
  write_pidfile ();
  accept_incoming_connections (socks, nr_socks);

  free_listening_sockets (socks, nr_socks);
}

static void
handle_quit (int sig)
{
  quit = 1;
}

static void
set_up_signals (void)
{
  struct sigaction sa;

  memset (&sa, 0, sizeof sa);
  sa.sa_flags = SA_RESTART;
  sa.sa_handler = handle_quit;
  sigaction (SIGINT, &sa, NULL);
  sigaction (SIGQUIT, &sa, NULL);
  sigaction (SIGTERM, &sa, NULL);
  sigaction (SIGHUP, &sa, NULL);

  memset (&sa, 0, sizeof sa);
  sa.sa_flags = SA_RESTART;
  sa.sa_handler = SIG_IGN;
  sigaction (SIGPIPE, &sa, NULL);
}

static void
change_user (void)
{
  if (group) {
    gid_t gid = parsegroup (group);

    if (setgid (gid) == -1) {
      perror ("setgid");
      exit (EXIT_FAILURE);
    }

    /* Kill supplemental groups from parent process. */
    if (setgroups (1, &gid) == -1) {
      perror ("setgroups");
      exit (EXIT_FAILURE);
    }

    debug ("changed group to %s", group);
  }

  if (user) {
    uid_t uid = parseuser (user);

    if (setuid (uid) == -1) {
      perror ("setuid");
      exit (EXIT_FAILURE);
    }

    debug ("changed user to %s", user);
  }
}

static void
write_pidfile (void)
{
  int fd;
  pid_t pid;
  char pidstr[64];
  size_t len;

  if (!pidfile)
    return;

  pid = getpid ();
  snprintf (pidstr, sizeof pidstr, "%d\n", pid);
  len = strlen (pidstr);

  fd = open (pidfile, O_WRONLY|O_TRUNC|O_CREAT|O_CLOEXEC|O_NOCTTY, 0644);
  if (fd == -1) {
    perror (pidfile);
    exit (EXIT_FAILURE);
  }

  if (write (fd, pidstr, len) < len ||
      close (fd) == -1) {
    perror (pidfile);
    exit (EXIT_FAILURE);
  }

  debug ("written pidfile %s", pidfile);
}

static void
fork_into_background (void)
{
  pid_t pid;

  if (foreground)
    return;

  pid = fork ();
  if (pid == -1) {
    perror ("fork");
    exit (EXIT_FAILURE);
  }

  if (pid > 0)                  /* Parent process exits. */
    exit (EXIT_SUCCESS);

  chdir ("/");

  /* Close stdin/stdout and redirect to /dev/null. */
  close (0);
  close (1);
  open ("/dev/null", O_RDONLY);
  open ("/dev/null", O_WRONLY);

  /* If not verbose, set stderr to the same as stdout as well. */
  if (!verbose)
    dup2 (1, 2);

  debug ("forked into background (new pid = %d)", getpid ());
}

static void
run_command (void)
{
  char *url;
  char *cmd;
  int r;
  pid_t pid;

  if (!run)
    return;

  /* Construct an nbd "URL".  Unfortunately guestfish and qemu take
   * different syntax, so try to guess which one we need.
   */
  if (strstr (run, "guestfish")) {
    if (port)
      r = asprintf (&url, "nbd://localhost:%s", port);
    else if (unixsocket)
      /* XXX escaping? */
      r = asprintf (&url, "nbd://?socket=%s", unixsocket);
    else
      abort ();
  }
  else /* qemu */ {
    if (port)
      r = asprintf (&url, "nbd:localhost:%s", port);
    else if (unixsocket)
      r = asprintf (&url, "nbd:unix:%s", unixsocket);
    else
      abort ();
  }
  if (r == -1) {
    perror ("asprintf");
    exit (EXIT_FAILURE);
  }

  /* Construct the final command including shell variables. */
  /* XXX Escaping again. */
  r = asprintf (&cmd,
                "nbd='%s'\n"
                "port='%s'\n"
                "unixsocket='%s'\n"
                "%s",
                url, port ? port : "", unixsocket ? unixsocket : "", run);
  if (r == -1) {
    perror ("asprintf");
    exit (EXIT_FAILURE);
  }

  free (url);

  /* Fork.  Captive nbdkit runs as the child process. */
  pid = fork ();
  if (pid == -1) {
    perror ("fork");
    exit (EXIT_FAILURE);
  }

  if (pid > 0) {              /* Parent process is the run command. */
    r = system (cmd);
    if (WIFEXITED (r))
      r = WEXITSTATUS (r);
    else if (WIFSIGNALED (r)) {
      fprintf (stderr, "%s: external command was killed by signal %d\n",
               program_name, WTERMSIG (r));
      r = 1;
    }
    else if (WIFSTOPPED (r)) {
      fprintf (stderr, "%s: external command was stopped by signal %d\n",
               program_name, WSTOPSIG (r));
      r = 1;
    }

    kill (pid, SIGTERM);        /* Kill captive nbdkit. */

    _exit (r);
  }

  free (cmd);

  debug ("forked into background (new pid = %d)", getpid ());
}

static uid_t
parseuser (const char *id)
{
  struct passwd *pwd;
  int saved_errno;

  errno = 0;
  pwd = getpwnam (id);

  if (NULL == pwd) {
    int val;

    saved_errno = errno;

    if (sscanf (id, "%d", &val) == 1)
      return val;

    fprintf (stderr, "%s: -u option: %s is not a valid user name or uid",
             program_name, id);
    if (saved_errno != 0)
      fprintf (stderr, " (getpwnam error: %s)", strerror (saved_errno));
    fprintf (stderr, "\n");
    exit (EXIT_FAILURE);
  }

  return pwd->pw_uid;
}

static gid_t
parsegroup (const char *id)
{
  struct group *grp;
  int saved_errno;

  errno = 0;
  grp = getgrnam (id);

  if (NULL == grp) {
    int val;

    saved_errno = errno;

    if (sscanf (id, "%d", &val) == 1)
      return val;

    fprintf (stderr, "%s: -g option: %s is not a valid group name or gid",
             program_name, id);
    if (saved_errno != 0)
      fprintf (stderr, " (getgrnam error: %s)", strerror (saved_errno));
    fprintf (stderr, "\n");
    exit (EXIT_FAILURE);
  }

  return grp->gr_gid;
}

/* Returns 0 if no socket activation, or the number of FDs.
 * See also virGetListenFDs in libvirt.org:src/util/virutil.c
 */
static unsigned int
get_socket_activation (void)
{
  const char *s;
  unsigned int pid;
  unsigned int nr_fds;
  unsigned int i;
  int fd;

  s = getenv ("LISTEN_PID");
  if (s == NULL)
    return 0;
  if (sscanf (s, "%u", &pid) != 1) {
    fprintf (stderr, "%s: malformed %s environment variable (ignored)\n",
             program_name, "LISTEN_PID");
    return 0;
  }
  if (pid != getpid ()) {
    fprintf (stderr, "%s: %s was not for us (ignored)\n",
             program_name, "LISTEN_PID");
    return 0;
  }

  s = getenv ("LISTEN_FDS");
  if (s == NULL)
    return 0;
  if (sscanf (s, "%u", &nr_fds) != 1) {
    fprintf (stderr, "%s: malformed %s environment variable (ignored)\n",
             program_name, "LISTEN_FDS");
    return 0;
  }

  /* So these are not passed to any child processes we might start. */
  unsetenv ("LISTEN_FDS");
  unsetenv ("LISTEN_PID");

  /* So the file descriptors don't leak into child processes. */
  for (i = 0; i < nr_fds; ++i) {
    fd = FIRST_SOCKET_ACTIVATION_FD + i;
    if (fcntl (fd, F_SETFD, FD_CLOEXEC) == -1) {
      /* If we cannot set FD_CLOEXEC then it probably means the file
       * descriptor is invalid, so socket activation has gone wrong
       * and we should exit.
       */
      fprintf (stderr, "%s: socket activation: "
               "invalid file descriptor fd = %d: %m\n",
               program_name, fd);
      exit (EXIT_FAILURE);
    }
  }

  return nr_fds;
}
