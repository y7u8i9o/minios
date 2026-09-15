/* Start audiod only for the Lua integration test, and reap both processes
 * even when a Lua assertion fails. No init or persistent home is needed. */
#include <audio/audio.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>

static int reap(pid_t pid)
{
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return 1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}

static int run_lua(const char *mode)
{
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        char *const args[] = { "lua", "/etc/tests/audio.lua", (char *)mode, NULL };
        execv("/bin/lua", args);
        _exit(127);
    }
    return reap(pid);
}

int main(void)
{
    if (run_lua("no-server")) return 1;
    pid_t daemon = fork();
    if (daemon < 0) return 1;
    if (daemon == 0) {
        char *const args[] = { "audiod", NULL };
        execv("/bin/audiod", args);
        _exit(127);
    }
    struct audio_connection *connection = NULL;
    for (int i = 0; i < 50 && !connection; i++) {
        connection = audio_connect();
        if (!connection) usleep(20000);
    }
    int failed = connection ? 0 : 1;
    audio_disconnect(connection);
    if (!failed) failed = run_lua("server");
    kill(daemon, SIGTERM);
    failed |= reap(daemon);
    printf("luaaudiotest: %d failures\n", failed);
    return failed;
}
