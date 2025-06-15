#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>

#define CFILE "/etc/nobszram/nobszram.conf"
#define MLN 64
#define MVAL 16

typedef struct {
    char sz[MVAL], alg[MVAL];
    int prio, en;
} cf_t;

static void die(char *s) {
    fprintf(stderr, "nobszram: %s\n", s);
    _exit(1);
}

static char *trim(char *s) {
    char *e = s + strlen(s) - 1;
    while (isspace(*s)) s++;
    if (!*s) return s;
    while (e > s && isspace(*e)) e--;
    e[1] = 0;
    return s;
}

static long getmem() {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char ln[128];
    long m = -1;
    while (fgets(ln, sizeof(ln), f))
        if (sscanf(ln, "MemTotal: %ld kB", &m) == 1) break;
    fclose(f);
    return m > 0 ? m : -1;
}

static int calcsz(char *s, char *o, size_t osiz) {
    if (!s || !*s || !o || osiz < 2) return 0;
    int l = strlen(s);
    if (l >= MVAL) return 0;
    if (s[l-1] == '%') {
        for (int i = 0; i < l-1; i++)
            if (!isdigit(s[i])) return 0;
        int p = atoi(s);
        if (p <= 0 || p > 100) return 0;
        long m = getmem();
        if (m <= 0) return 0;
        long k = (m * p) / 100;
        if (k <= 0) return 0;
        int n = snprintf(o, osiz, "%ldK", k);
        return n > 0 && n < (int)osiz;
    }
    char *ep;
    long v = strtol(s, &ep, 10);
    if (v <= 0) return 0;
    if (!*ep || (strlen(ep) == 1 && strchr("KMGT", *ep))) {
        int n = snprintf(o, osiz, "%s", s);
        return n > 0 && n < (int)osiz;
    }
    return 0;
}

static int valsz(char *s) {
    if (!s || !*s) return 0;
    int l = strlen(s);
    if (!l || l >= MVAL) return 0;
    if (s[l-1] == '%') {
        for (int i = 0; i < l-1; i++)
            if (!isdigit(s[i])) return 0;
        int v = atoi(s);
        return v > 0 && v <= 100;
    }
    char *ep;
    long v = strtol(s, &ep, 10);
    return v > 0 && (!*ep || (strlen(ep) == 1 && strchr("KMGT", *ep)));
}

static int valalg(char *s) {
    const char *a[] = {"lzo", "lz4", "lz4hc", "deflate", "842", "zstd", "lzo-rle", 0};
    for (int i = 0; a[i]; i++) if (!strcmp(s, a[i])) return 1;
    return 0;
}

static void initcf(cf_t *c) {
    strcpy(c->sz, "25%");
    strcpy(c->alg, "zstd");
    c->prio = 100;
    c->en = 1;
}

static void parsecf(cf_t *c) {
    FILE *f = fopen(CFILE, "r");
    if (!f) return;
    char ln[MLN];
    while (fgets(ln, sizeof(ln), f)) {
        char *t = trim(ln);
        if (!*t || *t == '#') continue;
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(t), *v = trim(eq+1);
        if (!k || !v || strlen(v) >= MVAL) continue;
        if (!strcmp(k, "size") && valsz(v)) strncpy(c->sz, v, MVAL-1);
        else if (!strcmp(k, "algorithm") && valalg(v)) strncpy(c->alg, v, MVAL-1);
        else if (!strcmp(k, "priority")) {
            char *ep;
            errno = 0;
            long p = strtol(v, &ep, 10);
            if (!errno && !*ep && p >= -1 && p <= 32767) c->prio = p;
        }
        else if (!strcmp(k, "enabled")) c->en = !strcmp(v, "true") || !strcmp(v, "1");
    }
    fclose(f);
}

static int runcmd(char **av) {
    pid_t p = fork();
    if (p == -1) return -1;
    if (!p) { execvp(av[0], av); _exit(127); }
    int st;
    if (waitpid(p, &st, 0) == -1) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int haszram() {
    char *a[] = {"zramctl", "-n", "-o", "NAME,MOUNTPOINT", 0};
    int pfd[2];
    if (pipe(pfd)) return 0;
    pid_t p = fork();
    if (p == -1) { close(pfd[0]); close(pfd[1]); return 0; }
    if (!p) {
        close(pfd[0]); dup2(pfd[1], 1); close(pfd[1]);
        execvp(a[0], a); _exit(127);
    }
    close(pfd[1]);
    char b[256];
    ssize_t n = read(pfd[0], b, sizeof(b)-1);
    close(pfd[0]);
    int st;
    waitpid(p, &st, 0);
    if (n <= 0 || !WIFEXITED(st) || WEXITSTATUS(st)) return 0;
    b[n] = 0;
    return !!strstr(b, "[SWAP]");
}

static char *findzram() {
    char *a[] = {"zramctl", "-f", 0};
    int pfd[2];
    if (pipe(pfd)) return 0;
    pid_t p = fork();
    if (p == -1) { close(pfd[0]); close(pfd[1]); return 0; }
    if (!p) {
        close(pfd[0]); dup2(pfd[1], 1); close(pfd[1]);
        execvp(a[0], a); _exit(127);
    }
    close(pfd[1]);
    static char dev[16];
    ssize_t n = read(pfd[0], dev, sizeof(dev)-1);
    close(pfd[0]);
    int st;
    waitpid(p, &st, 0);
    if (n <= 0 || !WIFEXITED(st) || WEXITSTATUS(st)) return 0;
    dev[n] = 0;
    char *nl = strchr(dev, '\n');
    if (nl) *nl = 0;
    return dev[0] ? dev : 0;
}

static int setup(cf_t *c) {
    if (!c || !c->en) return 0;
    if (haszram()) return 1;
    char sb[32];
    if (!calcsz(c->sz, sb, sizeof(sb))) return -1;
    char *dev = findzram();
    if (!dev) return -1;
    char *a1[] = {"zramctl", "-s", sb, "-a", c->alg, dev, 0};
    if (runcmd(a1)) return -1;
    char *a2[] = {"mkswap", dev, 0};
    if (runcmd(a2)) return -1;
    char pb[8];
    int n = snprintf(pb, sizeof(pb), "%d", c->prio);
    if (n <= 0 || n >= (int)sizeof(pb)) return -1;
    char *a3[] = {"swapon", "--priority", pb, dev, 0};
    return !runcmd(a3);
}

static char *getzram() {
    char *a[] = {"zramctl", "-n", "-o", "NAME,MOUNTPOINT", 0};
    int pfd[2];
    if (pipe(pfd)) return 0;
    pid_t p = fork();
    if (p == -1) { close(pfd[0]); close(pfd[1]); return 0; }
    if (!p) {
        close(pfd[0]); dup2(pfd[1], 1); close(pfd[1]);
        execvp(a[0], a); _exit(127);
    }
    close(pfd[1]);
    char b[256];
    ssize_t n = read(pfd[0], b, sizeof(b)-1);
    close(pfd[0]);
    int st;
    waitpid(p, &st, 0);
    if (n <= 0 || !WIFEXITED(st) || WEXITSTATUS(st)) return 0;
    b[n] = 0;
    char *line = strtok(b, "\n");
    static char dev[16];
    while (line) {
        if (strstr(line, "[SWAP]") && sscanf(line, "%15s", dev) == 1)
            return dev;
        line = strtok(0, "\n");
    }
    return 0;
}

static int teardown() {
    char *dev = getzram();
    if (!dev) return 0;
    char *a1[] = {"swapoff", dev, 0}, *a2[] = {"zramctl", "--reset", dev, 0};
    runcmd(a1); runcmd(a2);
    return 0;
}

static int status() {
    char *a[] = {"zramctl", 0};
    return runcmd(a);
}

int main(int ac, char **av) {
    if (ac != 2 || !av[1]) die("usage: nobszram start|stop|status");
    if (geteuid() && strcmp(av[1], "status")) die("root required");
    char *t[] = {"zramctl", "-V", 0};
    if (runcmd(t)) die("zramctl missing");
    if (!strcmp(av[1], "status")) return status();
    if (!strcmp(av[1], "start")) {
        cf_t c;
        initcf(&c);
        parsecf(&c);
        return !setup(&c);
    }
    if (!strcmp(av[1], "stop")) return !teardown();
    die("invalid action");
}
