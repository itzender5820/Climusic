#pragma once
/*  CLI.MUSIC.COM — proc_util.h
 *
 *  Minimal POSIX helper for spawning a shell command whose *process*
 *  (not just our read loop) can actually be killed on demand.
 *
 *  Why this exists:
 *  -----------------
 *  The old lyrics fetch used popen()/pclose(). The select()-with-timeout
 *  read loop around it correctly stopped *reading* within ~100ms of a
 *  cancel request, but pclose() itself blocks until the child process
 *  exits on its own. Since the child was `python3 -c "...syncedlyrics..."`
 *  doing a real network request, pclose() would sit there for however
 *  long the fetch/network call took (many seconds) — which is exactly
 *  the "switching songs gets stuck until the lyrics call finishes on
 *  its own" bug. Cancelling the read loop was never enough; the actual
 *  OS process was still alive and pclose() waited on it regardless.
 *
 *  spawn_killable() forks the command into its own process group so
 *  kill_child() can SIGTERM (then SIGKILL if needed) the whole group —
 *  including any grandchildren a shell pipeline spawns — and reap it
 *  without blocking the caller.
 */
#include <string>
#include <atomic>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <signal.h>
#include <fcntl.h>
#include <cstdio>

struct ChildProc {
    pid_t pid = -1;   // -1 = not running / already reaped
    int   fd  = -1;   // read end of the child's stdout
};

// Launch `cmd` via `/bin/sh -c`, with stdout piped back to us and stderr
// silenced (mirrors the old " 2>/dev/null" convention used throughout
// this codebase). The child (and anything it forks) lives in its own
// process group so it can be killed as a unit.
inline ChildProc spawn_killable(const std::string& cmd) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return {};
    // BUGFIX (audit A5): pipe() doesn't set FD_CLOEXEC. This app forks
    // from multiple threads concurrently (Lyrics' fetch thread,
    // StreamSearch, StreamPlayer's several spawn_killable calls) — without
    // CLOEXEC, a fork() happening on another thread between this pipe()
    // and our own exec() below would have ITS child inherit this pipe's
    // write end too. That write end staying open in an unrelated process
    // means our read-end here would never see EOF, even after our actual
    // child exits — a real hang. fcntl+FD_CLOEXEC (rather than pipe2,
    // which isn't available on macOS) closes this on our own exec() while
    // staying portable across every platform this project targets.
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return {};
    }

    if (pid == 0) {
        // ── child ──────────────────────────────────────────────────────
        setpgid(0, 0);                      // own process group
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            // BUGFIX (audit D4): if open() happened to return exactly fd
            // 2 (STDERR_FILENO) — e.g. because stdin was already closed
            // in some caller — dup2(2,2) is a documented no-op, and this
            // close() would then close stderr entirely instead of a
            // throwaway fd. Only close it when it's a genuinely separate fd.
            if (devnull != STDERR_FILENO) close(devnull);
        }
        close(pipefd[0]);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }

    // ── parent ────────────────────────────────────────────────────────
    setpgid(pid, pid);   // also set from here to close the fork/exec race
    close(pipefd[1]);
    ChildProc cp;
    cp.pid = pid;
    cp.fd  = pipefd[0];
    return cp;
}

// Terminate the child's whole process group and reap it. Safe to call
// even if the child already exited on its own. Never blocks longer than
// ~100ms before escalating to SIGKILL.
inline void kill_child(ChildProc& cp) {
    if (cp.fd >= 0) { close(cp.fd); cp.fd = -1; }
    if (cp.pid <= 0) return;

    killpg(cp.pid, SIGTERM);
    for (int i = 0; i < 5; ++i) {
        int status;
        pid_t r = waitpid(cp.pid, &status, WNOHANG);
        if (r == cp.pid) { cp.pid = -1; return; }
        usleep(20'000);   // 20ms
    }
    killpg(cp.pid, SIGKILL);
    int status;
    waitpid(cp.pid, &status, 0);
    cp.pid = -1;
}

// Reap a child that we believe already exited on its own (e.g. we read
// EOF from its pipe). Non-blocking-safe: waitpid(0) here is fine because
// EOF on the pipe means the process has already closed its stdout, which
// for a simple `sh -c "..."` command normally means it's finished or is
// about to be; we still block briefly to avoid a zombie.
inline void reap_child(ChildProc& cp) {
    if (cp.fd >= 0) { close(cp.fd); cp.fd = -1; }
    if (cp.pid <= 0) return;
    int status;
    waitpid(cp.pid, &status, 0);
    cp.pid = -1;
}

// Fire-and-forget: run `cmd` fully detached from us (double-fork so our
// process never has to reap it — init/subreaper does). Used for
// background downloads that should keep running even if the player
// exits or moves on immediately.
inline void spawn_detached(const std::string& cmd) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        // first child
        if (fork() > 0) _exit(0);   // first child exits immediately
        setsid();
        int devnull_in  = open("/dev/null", O_RDONLY);
        int devnull_out = open("/dev/null", O_WRONLY);
        if (devnull_in  >= 0) { dup2(devnull_in,  STDIN_FILENO);  close(devnull_in);  }
        if (devnull_out >= 0) {
            dup2(devnull_out, STDOUT_FILENO);
            dup2(devnull_out, STDERR_FILENO);
            close(devnull_out);   // BUGFIX (audit C3): was never closed — leaked 2 FDs per detached spawn
        }
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);   // reap the first (immediately-exiting) child only
}

// Run `cmd` and collect its stdout, checking `cancel` roughly every 100ms
// so a caller waiting on the calling thread's join() is never stuck for
// longer than that once cancel is requested — even if the subprocess is
// mid network-call. On cancel, the child's whole process group is killed
// (see kill_child()) rather than merely abandoned.
// Returns true if the command ran to completion (clean EOF) before cancel.
inline bool run_killable(const std::string& cmd, std::atomic<bool>& cancel,
                          std::string& out)
{
    ChildProc cp = spawn_killable(cmd);
    if (cp.pid < 0 || cp.fd < 0) return false;

    out.clear();
    out.reserve(4096);
    char buf[1024];
    bool got_eof = false;   // true only for a genuine n==0 EOF, not an error

    while (!cancel) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(cp.fd, &rfds);
        struct timeval tv{0, 100'000};  // 100 ms

        int ret = select(cp.fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ret > 0) {
            ssize_t n = read(cp.fd, buf, sizeof(buf));
            if (n < 0) {
                // BUGFIX (audit A4): a transient read() error (e.g. EINTR)
                // used to fall through to reap_child()'s *blocking*
                // waitpid() below exactly like a clean EOF would — but the
                // child (e.g. yt-dlp mid-download) can still be very much
                // alive, so that wait could block indefinitely, freezing
                // the calling thread. Only a real n==0 EOF means the
                // child actually closed its output.
                break;
            }
            if (n == 0) { got_eof = true; break; }
            out.append(buf, (size_t)n);
        } else if (ret < 0) {
            // select() error (e.g. EINTR) — same reasoning as above: not
            // evidence the child exited, so don't treat it like EOF.
            break;
        }
        // ret == 0: timeout -> loop again and re-check cancel
    }

    if (cancel || !got_eof) {
        kill_child(cp);   // don't risk a blocking wait on a possibly-still-alive child
        return false;
    }
    reap_child(cp);       // safe: got_eof means the child already closed its output
    return true;
}
