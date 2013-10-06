/*
The MIT License (MIT)

Copyright (c) 2013 Frank Hunleth

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/reboot.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define ERLANG_ROOT_DIR "/usr/lib/erlang"
#define RELEASE_ROOT_DIR "/srv/erlang"
#define RELEASE_RELEASES_DIR  RELEASE_ROOT_DIR "/releases"

static char erts_dir[PATH_MAX];
static char release_dir[PATH_MAX];
static char root_dir[PATH_MAX];
static char boot_path[PATH_MAX];
static char sys_config[PATH_MAX];

static void info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void fatal(const char *fmt, ...)
{
    fprintf(stderr, "\n\nFATAL ERROR:\n");

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\nCANNOT CONTINUE.\n");

    sleep(9999);
}

static int readsysfs(const char *path, char *buffer, int maxlen)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) 
        return 0;

    int count = read(fd, buffer, maxlen - 1);
    
    close(fd);
    if (count <= 0)
        return 0;

    // Trim trailing \n
    count--;
    buffer[count] = 0;
    return count;
}

static void fix_ctty()
{
    // Set up a controlling terminal for Erlang so that
    // it's possible to get to shell management mode.
    // See http://www.busybox.net/FAQ.html#job_control
    setsid();

    char ttypath[32];
    strcpy(ttypath, "/dev/");
    readsysfs("/sys/class/tty/console/active", &ttypath[5], sizeof(ttypath) - 5);
   
    int fd = open(ttypath, O_RDWR);
    if (fd > 0) {
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
    } else {
        fprintf(stderr, "Error setting controlling terminal: %s\n", ttypath);
    }
}

static int starts_with(const char *str, const char *what)
{
    return strstr(str, what) == str;
}

static int erts_filter(const struct dirent *d)
{
    return starts_with(d->d_name, "erts-");
}

static void find_erts_directory()
{
    struct dirent **namelist;
    int n = scandir(ERLANG_ROOT_DIR,
                    &namelist,
                    erts_filter,
                    NULL);
    if (n < 0)
        fatal("scandir failed: %s\n", strerror(errno));
    else if (n == 0)
        fatal("erts not found. Check that erlang was installed to %s\n", ERLANG_ROOT_DIR);
    else if (n > 1)
        fatal("Found multiple erts directories. Clean up the installation.\n");

    sprintf(erts_dir, "%s/%s", ERLANG_ROOT_DIR, namelist[0]->d_name);

    free(namelist[0]);
    free(namelist);
}

static int file_exists(const char *path)
{
    struct stat sb;
    return stat(path, &sb) == 0;
}

static int release_filter(const struct dirent *d)
{
    // Releases are of the form NAME-VERSION
    return strchr(d->d_name, '-') != NULL;
}

static void find_sys_config()
{
    sprintf(sys_config, "%s/sys.config", release_dir);
    if (!file_exists(sys_config))
        *sys_config = '\0';
}

static void find_boot_path()
{
    *boot_path = '\0';

    const char *release_name_start = strrchr(release_dir, '/');
    const char *release_name_end = strrchr(release_dir, '-');
    if (!release_name_start ||
            !release_name_end ||
            release_name_end <= release_name_start)
        return;

    char release_name[256];
    release_name_start++; // Skip pass leading '/'
    memcpy(release_name, release_name_start, release_name_end - release_name_start);
    release_name[release_name_end - release_name_start] = '\0';

    sprintf(boot_path, "%s/%s", release_dir, release_name);
}

static void find_release()
{
    struct dirent **namelist;
    int n = scandir(RELEASE_RELEASES_DIR,
                    &namelist,
                    release_filter,
                    NULL);
    if (n <= 0) {
        info("No release found in %s.\n", RELEASE_RELEASES_DIR);
        *release_dir = '\0';
        *sys_config = '\0';
        *boot_path = '\0';

        strcpy(root_dir, ERLANG_ROOT_DIR);
    } else if (n == 1) {
        sprintf(release_dir, "%s/%s", RELEASE_RELEASES_DIR, namelist[0]->d_name);
        strcpy(root_dir, RELEASE_ROOT_DIR);
        free(namelist[0]);
        free(namelist);

        find_sys_config();
        find_boot_path();
    } else {
        fatal("Multiple releases found. Not sure which to run.\n");
    }
}

static void trim_whitespace(char *s)
{
    char *left = s;
    while (*left != 0 && isspace(*left))
        left++;
    char *right = s + strlen(s) - 1;
    while (right >= left && isspace(*right))
        right--;

    int len = right - left + 1;
    if (len)
        memmove(s, left, len);
    s[len] = 0;
}

static void load_erlinit()
{
    char line[128];
    int lineno = 0;

    // Load up Erlang environment overrides
    FILE *fp = fopen("/etc/erlinit.conf", "r");
    if (!fp)
        return;

    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        trim_whitespace(line);

        // Skip comments and blank lines
        if (*line == 0 || *line == '#')
            continue;

        // Simple format check
        if (!strchr(line, '=')) {
            info("erlinit.conf[%d]: syntax error in '%s'\n", lineno, line);
            continue;
        }

        // All values currently populate the environment
        putenv(strdup(line));
    }

    fclose(fp);
}

static void setup_environment()
{
    // Set up the environment for running erlang.
    putenv("HOME=/");

    // PATH appears to only be needed for user convenience when runninf os:cmd/1
    // It may be possible to remove in the future.
    putenv("PATH=/usr/sbin:/usr/bin:/sbin:/bin");
    putenv("TERM=vt100");

    // Erlang environment

    // ROOTDIR points to the release unless it wasn't found.
    char envvar[PATH_MAX];
    sprintf(envvar, "ROOTDIR=%s", root_dir);
    putenv(strdup(envvar));

    // BINDIR points to the erts bin directory.
    sprintf(envvar, "BINDIR=%s/bin", erts_dir);
    putenv(strdup(envvar));

    putenv("EMU=beam");
    putenv("PROGNAME=erl");
}

static void forkexec(const char *path, ...)
{
    va_list ap;
#define MAX_ARGS 32
    char *argv[MAX_ARGS];
    int i;

    va_start(ap, path);
    argv[0] = path;
    for (i = 1; i < MAX_ARGS - 1; i++)
    {
        argv[i] = va_arg(ap, char *);
        if (argv[i] == NULL)
            break;
    }
    argv[i] = 0;
    va_end(ap);

    pid_t pid = fork();
    if (pid == 0) {
        execv(path, argv);
        exit(127);
    } else {
        waitpid(pid, 0, 0);
    }
}

static void child()
{
    // Locate everything needed to configure the environment
    // and pass to erlexec.
    find_erts_directory();
    find_release();

    // Set up the environment for running erlang.
    setup_environment();
    load_erlinit();

    // Mount the virtual file systems.
    mount("", "/proc", "proc", 0, NULL);
    mount("", "/sys", "sysfs", 0, NULL);

    // Bring up the loopback interface (needed if erlang is a node)
    forkexec("/sbin/ip", "link", "set", "lo", "up", NULL);
    forkexec("/sbin/ip", "addr", "add", "127.0.0.1", "dev", "lo", NULL);

    // Fix the terminal settings so that CTRL keys work.
    fix_ctty();

    chdir(root_dir);

    // Start Erlang up
    char erlexec_path[PATH_MAX];
    sprintf(erlexec_path, "%s/bin/erlexec", erts_dir);

    char *erlargv[32];
    int arg = 0;
//#define USE_STRACE
#ifdef USE_STRACE
    erlargv[arg++] = "strace";
    erlargv[arg++] = "-f";
    erlargv[arg++] = erlexec_path;
#else
    erlargv[arg++] = "erlexec";
#endif
    if (*sys_config) {
        erlargv[arg++] = "-config";
        erlargv[arg++] = sys_config;
    }
    if (*boot_path) {
        erlargv[arg++] = "-boot";
        erlargv[arg++] = boot_path;
    }

    erlargv[arg] = NULL;

#if 0
    // Dump the environment and commandline
    extern char **environ;
    char** env = environ;
    while (*env != 0)
        printf("Env: '%s'\n", *env++);

    int i;
    for (i = 0; i < arg; i++)
        printf("Arg: '%s'\n", erlargv[i]);
#endif

#ifdef USE_STRACE
    execvp("/usr/bin/strace", erlargv);
#else
    execvp(erlexec_path, erlargv);
#endif

    // execvpe is not supposed to return
    fatal("execvp failed to run %s: %s", erlexec_path, strerror(errno));
}

int main(int argc, char *argv[])
{
    info("Loading runtime...\n");

    pid_t pid = fork();
    if (pid == 0) {
        child();
        exit(1);
    }

    // If Erlang exists, then something went wrong, so handle it.
    if (waitpid(pid, 0, 0) < 0)
        info("Unexpected error from waitpid(): %s\n", strerror(errno));

    fatal("Unexpected exit. Hanging to make debugging easier...\n");

    // When erlang exits on purpose (or on accident), reboot
    reboot(LINUX_REBOOT_CMD_RESTART);

    // If we can't reboot, oops the kernel.
    return 0;
}