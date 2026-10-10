// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// headless_daemon.c
// Daemon mode: the shell on a TCP socket, for AI agents and test harnesses.
//
// Scope and threat model: a development tool.  The listener binds
// 127.0.0.1 only and has no authentication -- any local process (any user on
// the host) that can connect runs any shell statement as the daemon's user:
// host file reads and writes (files.cp, checkpoint.save to any path), image
// creation, quit.  Run it only on a host whose local users you trust.
//
// While a client is served, stdout AND stderr are that client's: every
// printf -- the job's output, LOG lines, a core warning, a debug print added
// later -- reaches the socket.  Nothing is filtered or kept back, so do not
// print anything there that the client may not see.

#include "platform.h"

#include "headless.h"

#include "debug.h"
#include "scheduler.h"
#include "system.h"
#include "job/job.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_listen_fd = -1;
static int g_client_fd = -1;
static bool g_client_lost = false; // a write to the client failed: stop serving it
static int g_saved_stdout = -1;
static int g_saved_stderr = -1;

// ============================================================================
// PID file: /tmp/gs-headless-<port>.pid, locked for the daemon's lifetime
// ============================================================================
//
// The daemon holds an exclusive flock() on its PID file until it exits, so a
// held lock means "a live daemon wrote this PID" and the PID cannot have been
// recycled; a file nobody locks is stale.  `--kill` relies on that instead of
// kill(pid, 0), which raced against PID reuse.  The file is opened with
// O_NOFOLLOW and never truncated before it is locked and checked to be our
// own regular file, so a planted symlink or another user's file in /tmp is
// refused rather than written through.

static char g_pid_path[256];
static int g_pid_fd = -1;

// Build the PID file path for the given port
static void pid_file_path(int port, char *buf, size_t len) {
    snprintf(buf, len, "/tmp/gs-headless-%d.pid", port);
}

// Create, lock and fill the PID file.  Failure is reported, not fatal: the
// daemon serves without a PID file.
static void write_pid_file(int port) {
    pid_file_path(port, g_pid_path, sizeof(g_pid_path));
    int fd = open(g_pid_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    struct stat st;
    const char *why = NULL;
    if (fd < 0)
        why = strerror(errno);
    else if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid())
        why = "not a regular file owned by this user";
    else if (flock(fd, LOCK_EX | LOCK_NB) != 0)
        why = "locked by another daemon";
    if (why) {
        fprintf(stderr, "daemon: no PID file %s: %s\n", g_pid_path, why);
        if (fd >= 0)
            close(fd);
        g_pid_path[0] = '\0';
        return;
    }
    // Ours and locked: replace the contents with our PID
    char line[32];
    int n = snprintf(line, sizeof(line), "%d\n", (int)getpid());
    if (ftruncate(fd, 0) != 0 || pwrite(fd, line, (size_t)n, 0) != n)
        fprintf(stderr, "daemon: cannot write PID file %s: %s\n", g_pid_path, strerror(errno));
    g_pid_fd = fd; // kept open: the lock lives as long as the descriptor
}

// Remove the PID file on exit (only one this process created and locked)
static void remove_pid_file(void) {
    if (g_pid_fd < 0)
        return;
    unlink(g_pid_path);
    close(g_pid_fd);
    g_pid_fd = -1;
}

// Whether the lock on `fd` is free (the daemon that held it has exited)
static bool pid_lock_free(int fd) {
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
        return false;
    flock(fd, LOCK_UN);
    return true;
}

// Wait up to `ms` for the lock on `fd` to come free
static bool pid_lock_wait(int fd, int ms) {
    for (int waited = 0; waited < ms; waited += 100) {
        if (pid_lock_free(fd))
            return true;
        usleep(100000); // 100 ms
    }
    return pid_lock_free(fd);
}

// Stop the daemon running on `port`, if any: SIGTERM, up to 5 s for it to
// finish (a checkpoint in flight takes a while), then SIGKILL.  Returns 1
// when a daemon was stopped.
static int kill_existing_daemon(int port) {
    char path[256];
    pid_file_path(port, path, sizeof(path));
    int fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return 0; // no PID file, nothing to kill
    if (pid_lock_free(fd)) {
        close(fd); // nobody holds it: stale (the next daemon reuses it)
        return 0;
    }
    // Locked: the holder is alive and wrote its PID before locking returned
    char line[32] = {0};
    ssize_t n = pread(fd, line, sizeof(line) - 1, 0);
    int pid = n > 0 ? atoi(line) : 0;
    if (pid <= 0 || pid == (int)getpid()) {
        close(fd);
        return 0;
    }
    fprintf(stderr, "Killing existing daemon (PID %d) on port %d\n", pid, port);
    kill(pid, SIGTERM);
    if (!pid_lock_wait(fd, 5000)) {
        fprintf(stderr, "Daemon (PID %d) did not exit within 5 s; sending SIGKILL\n", pid);
        kill(pid, SIGKILL);
        pid_lock_wait(fd, 1000);
    }
    close(fd);
    return 1;
}

// ============================================================================
// The client's streams
// ============================================================================

// Restore stdout/stderr to their original destinations
static void daemon_restore_output(void) {
    fflush(stdout);
    fflush(stderr);
    if (g_saved_stdout >= 0) {
        if (dup2(g_saved_stdout, STDOUT_FILENO) < 0)
            perror("daemon: restoring stdout");
        close(g_saved_stdout);
        g_saved_stdout = -1;
    }
    if (g_saved_stderr >= 0) {
        if (dup2(g_saved_stderr, STDERR_FILENO) < 0)
            perror("daemon: restoring stderr");
        close(g_saved_stderr);
        g_saved_stderr = -1;
    }
}

// Point stdout/stderr at the client socket so everything printed goes to the
// agent (see the file header: all of it).  fd 1 and 2 are replaced under the
// FILE*s, which keep their descriptors (fileno(stdout) is still 1, now the
// socket); whatever they had buffered for the terminal is flushed first.
// Returns false (streams unchanged) when the descriptors cannot be swapped.
static bool daemon_redirect_output(int client_fd) {
    fflush(stdout);
    fflush(stderr);
    g_saved_stdout = dup(STDOUT_FILENO);
    g_saved_stderr = dup(STDERR_FILENO);
    if (g_saved_stdout < 0 || g_saved_stderr < 0 || dup2(client_fd, STDOUT_FILENO) < 0 ||
        dup2(client_fd, STDERR_FILENO) < 0) {
        int err = errno;
        daemon_restore_output();
        fprintf(stderr, "daemon: cannot redirect output to the client: %s\n", strerror(err));
        return false;
    }
    // A previous client that vanished left the streams' error flags set;
    // daemon_client_gone reads them for this client.
    clearerr(stdout);
    clearerr(stderr);
    return true;
}

// Create and bind the listening socket for daemon mode
static int daemon_create_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("daemon: socket");
        return -1;
    }

    // Allow port reuse for quick restarts
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // bind to 127.0.0.1 only
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Error: port %d already in use. Use --port=N to specify a different port.\n", port);
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0) {
        perror("daemon: listen");
        close(fd);
        return -1;
    }

    return fd;
}

// Whether the daemon client is gone.  EOF is not that: a half-closing client
// (nc -N, nc -q, Python's shutdown(SHUT_WR)) sends a FIN and still reads the
// output, and a FIN cannot tell a half-close from a close -- the old check
// peeked recv() == 0 and cancelled such a client's own scheduler.run.  Only
// a failed write can tell, and it shows up as POLLERR/POLLHUP on the socket
// once a write has drawn the peer's RST (the once-a-second heartbeat writes,
// so a closed client is noticed within ~1-2 s), or as a failed fflush.
static bool daemon_client_gone(int client_fd) {
    if (client_fd < 0)
        return false;
    struct pollfd pfd = {.fd = client_fd, .events = 0, .revents = 0};
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
        return true;
    return ferror(stdout) != 0;
}

// While a run is in flight the daemon is inside one client's dispatch, so a
// second connection waits in the listen backlog until that dispatch returns.
// For a bare `scheduler.run` that is a short wait; for a `scheduler.run`
// inside a shell `while` loop it is forever, and the daemon looks wedged with
// only kill -9 as a way out.  Answer such a connection out of band instead:
// the run is stoppable from it, and anything else is refused with a note
// rather than executed, since the shell is busy with the first client.
static void daemon_serve_control_connection(void) {
    struct pollfd pfd = {.fd = g_listen_fd, .events = POLLIN, .revents = 0};
    if (g_listen_fd < 0 || poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN))
        return;
    int fd = accept(g_listen_fd, NULL, NULL);
    if (fd < 0)
        return;

    // One line, and only what is already there: a control client that sends
    // nothing must not stall the run it came to stop.
    char line[256];
    ssize_t n = recv(fd, line, sizeof(line) - 1, MSG_DONTWAIT);
    if (n < 0)
        n = 0;
    line[n] = '\0';
    for (char *p = line; *p; p++)
        if (*p == '\n' || *p == '\r')
            *p = '\0';
    char *cmd = line;
    while (*cmd == ' ')
        cmd++;

    const char *reply;
    if (strcmp(cmd, "stop") == 0 || strcmp(cmd, "scheduler.stop") == 0 || strcmp(cmd, "shell.interrupt") == 0 ||
        strcmp(cmd, "shell.interrupt()") == 0) {
        // The two halves of the terminal's Ctrl-C, for the daemon's client:
        // cancel its statement in flight (which ends the run that statement
        // started), and stop a run an earlier statement left going.
        job_cancel_client(HL_CLIENT_DAEMON);
        job_glue_stop_modes(HL_CLIENT_DAEMON);
        reply = "# run stopped by control connection\n";
    } else if (strcmp(cmd, "quit") == 0) {
        g_quit_requested = 1;
        reply = "# quit requested\n";
    } else {
        reply = "# busy: a run is in flight on another connection; "
                "this connection accepts only stop / shell.interrupt / quit\n";
    }
    (void)!write(fd, reply, strlen(reply));
    // Drain before closing, as daemon_handle_client does: closing with input
    // unread -- or still in flight, since the recv above takes only what had
    // arrived -- sends an RST, and the client loses the reply to "connection
    // reset by peer".  Bounded (~100 ms) so a silent client cannot stall the
    // run; a half-closing client ends it at once with its FIN.
    shutdown(fd, SHUT_WR);
    for (int i = 0; i < 10; i++) {
        struct pollfd dp = {.fd = fd, .events = POLLIN, .revents = 0};
        int r = poll(&dp, 1, 10);
        if (r < 0 && errno != EINTR)
            break;
        if (r > 0 && recv(fd, line, sizeof(line), 0) <= 0)
            break; // the client's FIN (or an error): nothing more can arrive
    }
    close(fd);
}

void daemon_watch_statement(uint32_t client, bool running) {
    if (g_client_fd < 0 || g_client_lost)
        return;
    if (daemon_client_gone(g_client_fd)) {
        // Nobody left to read the result: this client's run and script end
        // here; nothing else is touched.
        g_client_lost = true;
        job_cancel_client(client);
        job_glue_stop_modes(client);
    } else if (running) {
        // A second connection during a run is a control connection (stop /
        // quit); once the run is over, the next connection is the next
        // client and waits its turn in the backlog.
        daemon_serve_control_connection();
    }
}

// ============================================================================
// Serving a client
// ============================================================================
//
// The daemon used to guess where a request ended BEFORE running anything --
// read until a newline plus 1 ms of silence, then run the lot -- and every
// one of its defects was that guess: a 2 KB cap, a fragment dispatched
// half-read, a second send lost, a block silently dropped.  Now it feeds the
// statement assembler (headless.h) line by line.

// Run one complete daemon statement, pump the run it starts, and report the
// PC as a status line.  The statement's own outcome reaches the client as
// its output (an error's lines on stderr, which is the socket; `@end error`
// framed); the daemon's exit status does not carry it.
static void daemon_run_statement(char *stmt) {
    hl_run_statement(HL_CLIENT_DAEMON, stmt);
    if (!g_client_lost && system_is_initialized() && debug_prompt_enabled()) {
        char disasm_buf[160];
        debugger_disasm_pc(disasm_buf, sizeof(disasm_buf));
        if (disasm_buf[0] != '\0')
            printf("%s\n", disasm_buf);
    }
    fflush(stdout);
}

// How long a client may stay silent, after its last statement has finished
// and its output is flushed, before the daemon hangs up.  Not a latency: a
// statement runs the moment it is complete.  `echo cmd | nc -w 2` keeps
// working (the close lands well inside nc's timeout); `nc -N` just closes
// sooner.
#define DAEMON_IDLE_CLOSE_MS 500

// Serve one client: run its statements as they complete, while reading on.
// Supports any number of newline-delimited statements, in any number of
// sends, of any size up to STMT_MAX.
static void daemon_handle_client(int client_fd) {
    if (!daemon_redirect_output(client_fd)) {
        close(client_fd);
        return;
    }
    g_client_fd = client_fd;
    g_client_lost = false;

    stmt_asm_t stmt = {0};
    char *pending = NULL; // bytes of a line not yet ended by a newline
    size_t pending_len = 0, pending_cap = 0;
    bool skipping = false; // dropping the rest of a line past STMT_MAX
    bool eof = false;
    double idle_since = host_time_ms();
    char chunk[65536];

    while (g_running && !g_quit_requested && !g_client_lost) {
        // Run every statement the input already completes.
        char *nl;
        while (g_running && !g_quit_requested && !g_client_lost && pending_len &&
               (nl = memchr(pending, '\n', pending_len)) != NULL) {
            size_t line_len = (size_t)(nl - pending);
            if (!skipping) {
                int r = stmt_feed_line(&stmt, pending, line_len);
                if (r < 0)
                    printf("error: statement too long (over %u bytes); discarded\n", STMT_MAX);
                else if (r > 0) {
                    daemon_run_statement(stmt.buf);
                    stmt_reset(&stmt);
                }
            }
            skipping = false;
            memmove(pending, nl + 1, pending_len - line_len - 1);
            pending_len -= line_len + 1;
            idle_since = host_time_ms();
        }
        if (!g_running || g_quit_requested || g_client_lost)
            break;

        if (eof) {
            // The last line may lack its newline; it is still a line.
            if (pending_len && !skipping) {
                int r = stmt_feed_line(&stmt, pending, pending_len);
                pending_len = 0;
                if (r < 0)
                    printf("error: statement too long (over %u bytes); discarded\n", STMT_MAX);
                else if (r > 0) {
                    daemon_run_statement(stmt.buf);
                    stmt_reset(&stmt);
                }
            }
            break;
        }

        // Wait for more, until the client has been idle long enough.
        double idle = host_time_ms() - idle_since;
        if (idle >= DAEMON_IDLE_CLOSE_MS)
            break;
        // The machine runs on between statements: a frame, then a look
        // at the socket, instead of one long poll.
        struct pollfd pfd = {.fd = client_fd, .events = POLLIN, .revents = 0};
        bool busy = hl_pump_once();
        int ready = poll(&pfd, 1, busy ? 0 : 5);
        if (ready < 0 && errno != EINTR)
            break;
        if (ready <= 0)
            continue;
        ssize_t n = recv(client_fd, chunk, sizeof(chunk), 0);
        if (n == 0) {
            eof = true; // no more input -- not "client gone" (see daemon_client_gone)
            continue;
        }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            g_client_lost = true;
            break;
        }
        idle_since = host_time_ms();
        if (pending_len + (size_t)n > STMT_MAX) {
            // A line longer than any statement may be: drop it up to its end.
            printf("error: statement too long (over %u bytes); discarded\n", STMT_MAX);
            stmt_reset(&stmt);
            char *end = memchr(chunk, '\n', (size_t)n);
            pending_len = 0;
            if (!end) {
                skipping = true;
                continue;
            }
            skipping = true;
            size_t rest = (size_t)n - (size_t)(end - chunk);
            memmove(chunk, end, rest);
            n = (ssize_t)rest;
        }
        if (pending_len + (size_t)n > pending_cap) {
            size_t cap = pending_cap ? pending_cap : 65536;
            while (cap < pending_len + (size_t)n)
                cap *= 2;
            char *grown = realloc(pending, cap);
            if (!grown) {
                printf("error: out of memory reading the request\n");
                break;
            }
            pending = grown;
            pending_cap = cap;
        }
        memcpy(pending + pending_len, chunk, (size_t)n);
        pending_len += (size_t)n;
    }

    // An open block at the end of input is reported, never dropped.
    if (!g_client_lost && (stmt.len || (pending_len && !skipping)))
        printf("error: incomplete block at end of input; not run\n");
    stmt_free(&stmt);
    free(pending);

    daemon_restore_output();
    // Drain what the client still sends before closing: closing a socket with
    // unread input sends an RST, which can destroy output the client has not
    // read yet.
    shutdown(client_fd, SHUT_WR);
    for (int i = 0; i < 50; i++) {
        struct pollfd pfd = {.fd = client_fd, .events = POLLIN, .revents = 0};
        if (poll(&pfd, 1, 10) <= 0 || recv(client_fd, chunk, sizeof(chunk), 0) <= 0)
            break;
    }
    close(client_fd);
    g_client_fd = -1;
}

// Accept connections and handle them one at a time
static void daemon_loop(int port) {
    fprintf(stderr, "Daemon listening on 127.0.0.1:%d -- unauthenticated: any local process can run any\n", port);
    fprintf(stderr, "shell statement (host file reads and writes included) as this user\n");
    fprintf(stderr, "Send commands with: echo \"command\" | nc localhost %d\n", port);

    // Emit READY signal so callers can block-read instead of sleeping
    printf("READY\n");
    fflush(stdout);

    while (g_running && !g_quit_requested) {
        // A frame if the machine runs (a `scheduler.run` left it going),
        // then the listener: no long wait while there is work.
        bool busy = hl_pump_once();
        struct pollfd pfd = {.fd = g_listen_fd, .events = POLLIN, .revents = 0};
        int ready = poll(&pfd, 1, busy ? 0 : 10);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("daemon: poll");
            break;
        }
        if (ready == 0)
            continue; // timeout, check g_running again

        // Accept client connection
        int client_fd = accept(g_listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("daemon: accept");
            continue;
        }

        daemon_handle_client(client_fd);
    }

    close(g_listen_fd);
    g_listen_fd = -1;
}

int daemon_run(int port, bool kill_existing) {
    if (kill_existing)
        kill_existing_daemon(port);

    g_listen_fd = daemon_create_listener(port);
    if (g_listen_fd < 0) {
        fprintf(stderr, "Error: Failed to create daemon listener on port %d\n", port);
        return 1;
    }

    // Write PID file and register cleanup
    write_pid_file(port);
    atexit(remove_pid_file);
    fprintf(stderr, "Daemon PID: %d\n", getpid());

    daemon_loop(port);
    return 0;
}
