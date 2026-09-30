#include "terminal_guard.h"
#include <signal.h>
#include <stddef.h>
#include <termios.h>
#include <unistd.h>

static const int fatal_signals[] = {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE};
static struct sigaction previous[5];
static struct termios saved_termios;
static pid_t owner;
static size_t installed;
static volatile sig_atomic_t active;

static void restore_on_signal(int sig) {
  /* Forked tools must not restore the parent's live terminal. Use only
     async-signal-safe operations: no ncurses, stdio, allocation or Lua. */
  if (active && getpid() == owner) {
    static const char reset[] =
        "\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?1015l"
        "\033[?2004l\033[0m\033[?1049l\033[?25h\033>";
    tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    ssize_t ignored = write(STDOUT_FILENO, reset, sizeof(reset) - 1);
    (void)ignored;
  }
  /* Preserve signal exit status and OS crash reports; never resume execution. */
  struct sigaction action = {0};
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  sigaction(sig, &action, NULL);
  kill(getpid(), sig);
}

void terminal_guard_stop(void) {
  active = 0;
  while (installed > 0) {
    installed--;
    sigaction(fatal_signals[installed], &previous[installed], NULL);
  }
}

void terminal_guard_start(void) {
  if (active || tcgetattr(STDIN_FILENO, &saved_termios) != 0)
    return;
  owner = getpid();
  active = 1;
  struct sigaction action = {0};
  action.sa_handler = restore_on_signal;
  sigemptyset(&action.sa_mask);
  for (size_t i = 0; i < sizeof(fatal_signals) / sizeof(fatal_signals[0]); i++) {
    if (sigaction(fatal_signals[i], &action, &previous[i]) != 0) {
      terminal_guard_stop();
      return;
    }
    installed++;
  }
}
