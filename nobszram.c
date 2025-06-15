#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <errno.h>
#include <ctype.h>
#include <syslog.h>
#include <pwd.h>
#include <limits.h>

#define CF "/etc/nobszram/nobszram.conf"
#define ML 64
#define MV 16
#define MX 100

#define ZC "/usr/bin/zramctl"
#define SO "/usr/sbin/swapon"
#define SF "/usr/sbin/swapoff"
#define MS "/usr/sbin/mkswap"

extern char **environ;

typedef struct {
    char sz[MV], alg[MV];
    int prio, en;
} cf_t;

static void die(const char *s) {
    openlog("nobszram", LOG_PID, LOG_DAEMON);
    syslog(LOG_ERR, "%s", s);
    closelog();
    _exit(1);
}

static char *trim(char *s) {
    if (!s) return NULL;
    char *e = s + strlen(s) - 1;
    while (isspace(*s)) s++;
    if (!*s) return s;
    while (e > s && isspace(*e)) e--;
    e[1] = 0;
    return s;
}

static int vp(const char *p) {
    return p && access(p, X_OK) == 0;
}

static int vd(const char *d) {
    if (!d || strlen(d) >= 16) return 0;
    if (strncmp(d, "/dev/zram", 9) != 0) return 0;
    const char *n = d + 9;
    if (!*n) return 0;
    for (const char *c = n; *c; c++)
        if (!isdigit(*c)) return 0;
    return 1;
}

static int hs(const char *s) {
    return s && !strpbrk(s, ";&|`$(){}[]<>\"'\\*?");
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

static int cs(const char *s, char *o, size_t sz) {
    if (!s || !*s || !o || sz < 2) return 0;
    size_t l = strlen(s);
    if (l >= MV || !hs(s)) return 0;
    
    if (s[l-1] == '%') {
        for (size_t i = 0; i < l-1; i++)
            if (!isdigit(s[i])) return 0;
        int p = atoi(s);
        if (p <= 0 || p > 100) return 0;
        long m = getmem();
        if (m <= 0) return 0;
        long k = (m * p) / 100;
        if (k <= 0 || k > LONG_MAX/1024) return 0;
        int n = snprintf(o, sz, "%ldK", k);
        return n > 0 && n < (int)sz;
    }
    
    char *ep;
    errno = 0;
    long v = strtol(s, &ep, 10);
    if (errno || v <= 0 || v > LONG_MAX/1024) return 0;
    if (!*ep || (strlen(ep) == 1 && strchr("KMGT", *ep))) {
        int n = snprintf(o, sz, "%s", s);
        return n > 0 && n < (int)sz;
    }
    return 0;
}

static int vs(const char *s) {
    if (!s || !*s || !hs(s)) return 0;
    size_t l = strlen(s);
    if (!l || l >= MV) return 0;
    
    if (s[l-1] == '%') {
        for (size_t i = 0; i < l-1; i++)
            if (!isdigit(s[i])) return 0;
        int v = atoi(s);
        return v > 0 && v <= 100;
    }
    
    char *ep;
    errno = 0;
    long v = strtol(s, &ep, 10);
    if (errno || v <= 0) return 0;
    return !*ep || (strlen(ep) == 1 && strchr("KMGT", *ep));
}

static int va(const char *s) {
    if (!s || !hs(s) || strlen(s) >= MV) return 0;
    const char *a[] = {"lzo", "lz4", "lz4hc", "deflate", "842", "zstd", "lzo-rle", NULL};
    for (int i = 0; a[i]; i++) 
        if (!strcmp(s, a[i])) return 1;
    return 0;
}

static int vc(const char *p) {
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    if (st.st_uid != 0 || st.st_gid != 0) return 0;
    return !(st.st_mode & (S_IWGRP | S_IWOTH | S_IRGRP | S_IROTH));
}

static void ic(cf_t *c) {
    strncpy(c->sz, "25%", MV-1);
    c->sz[MV-1] = 0;
    strncpy(c->alg, "zstd", MV-1);
    c->alg[MV-1] = 0;
    c->prio = 100;
    c->en = 1;
}

static void pc(cf_t *c) {
    if (!vc(CF)) return;
    
    FILE *f = fopen(CF, "r");
    if (!f) return;
    
    char ln[ML];
    int lc = 0;
    
    while (fgets(ln, sizeof(ln), f) && lc++ < MX) {
        char *t = trim(ln);
        if (!t || !*t || *t == '#') continue;
        
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        
        char *k = trim(t), *v = trim(eq+1);
        if (!k || !v || strlen(v) >= MV || !hs(k) || !hs(v)) continue;
        
        if (!strcmp(k, "size") && vs(v)) {
            strncpy(c->sz, v, MV-1);
            c->sz[MV-1] = 0;
        }
        else if (!strcmp(k, "algorithm") && va(v)) {
            strncpy(c->alg, v, MV-1);
            c->alg[MV-1] = 0;
        }
        else if (!strcmp(k, "priority")) {
            char *ep;
            errno = 0;
            long p = strtol(v, &ep, 10);
            if (!errno && !*ep && p >= -1 && p <= 32767) c->prio = (int)p;
        }
        else if (!strcmp(k, "enabled")) {
            c->en = !strcmp(v, "true") || !strcmp(v, "1");
        }
    }
    fclose(f);
}

static int se(const char *prog, char *const av[]) {
    if (!vp(prog)) return -1;
    
    pid_t p = fork();
    if (p == -1) return -1;
    
    if (p == 0) {
        char *env[] = {NULL};
        environ = env;
        execv(prog, av);
        _exit(127);
    }
    
    int st;
    if (waitpid(p, &st, 0) == -1) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int hz() {
    char *av[] = {(char*)ZC, "-n", "-o", "NAME,MOUNTPOINT", NULL};
    int pfd[2];
    if (pipe(pfd)) return 0;
    
    pid_t p = fork();
    if (p == -1) {
        close(pfd[0]); 
        close(pfd[1]); 
        return 0;
    }
    
    if (p == 0) {
        close(pfd[0]); 
        dup2(pfd[1], 1); 
        close(pfd[1]);
        char *env[] = {NULL};
        environ = env;
        execv(ZC, av);
        _exit(127);
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

static char *fz() {
    char *av[] = {(char*)ZC, "-f", NULL};
    int pfd[2];
    if (pipe(pfd)) return NULL;
    
    pid_t p = fork();
    if (p == -1) {
        close(pfd[0]); 
        close(pfd[1]); 
        return NULL;
    }
    
    if (p == 0) {
        close(pfd[0]); 
        dup2(pfd[1], 1); 
        close(pfd[1]);
        char *env[] = {NULL};
        environ = env;
        execv(ZC, av);
        _exit(127);
    }
    
    close(pfd[1]);
    static char dev[16];
    ssize_t n = read(pfd[0], dev, sizeof(dev)-1);
    close(pfd[0]);
    
    int st;
    waitpid(p, &st, 0);
    if (n <= 0 || !WIFEXITED(st) || WEXITSTATUS(st)) return NULL;
    
    dev[n] = 0;
    char *nl = strchr(dev, '\n');
    if (nl) *nl = 0;
    
    return vd(dev) ? dev : NULL;
}

static int setup(cf_t *c) {
    if (!c || !c->en) return 0;
    if (hz()) return 1;
    
    char sb[32];
    if (!cs(c->sz, sb, sizeof(sb))) return -1;
    
    char *dev = fz();
    if (!dev || !vd(dev)) return -1;
    
    char *a1[] = {(char*)ZC, "-s", sb, "-a", c->alg, dev, NULL};
    if (se(ZC, a1)) return -1;
    
    char *a2[] = {(char*)MS, dev, NULL};
    if (se(MS, a2)) return -1;
    
    char pb[16];
    int n = snprintf(pb, sizeof(pb), "%d", c->prio);
    if (n <= 0 || n >= (int)sizeof(pb)) return -1;
    
    char *a3[] = {(char*)SO, "--priority", pb, dev, NULL};
    return !se(SO, a3);
}

static char *gz() {
    char *av[] = {(char*)ZC, "-n", "-o", "NAME,MOUNTPOINT", NULL};
    int pfd[2];
    if (pipe(pfd)) return NULL;
    
    pid_t p = fork();
    if (p == -1) {
        close(pfd[0]); 
        close(pfd[1]); 
        return NULL;
    }
    
    if (p == 0) {
        close(pfd[0]); 
        dup2(pfd[1], 1); 
        close(pfd[1]);
        char *env[] = {NULL};
        environ = env;
        execv(ZC, av);
        _exit(127);
    }
    
    close(pfd[1]);
    char b[256];
    ssize_t n = read(pfd[0], b, sizeof(b)-1);
    close(pfd[0]);
    
    int st;
    waitpid(p, &st, 0);
    if (n <= 0 || !WIFEXITED(st) || WEXITSTATUS(st)) return NULL;
    
    b[n] = 0;
    char *line = strtok(b, "\n");
    static char dev[16];
    
    while (line) {
        if (strstr(line, "[SWAP]") && sscanf(line, "%15s", dev) == 1) {
            return vd(dev) ? dev : NULL;
        }
        line = strtok(NULL, "\n");
    }
    return NULL;
}

static int teardown() {
    char *dev = gz();
    if (!dev || !vd(dev)) return 0;
    
    char *a1[] = {(char*)SF, dev, NULL};
    char *a2[] = {(char*)ZC, "--reset", dev, NULL};
    
    se(SF, a1);
    se(ZC, a2);
    return 0;
}

static int status() {
    char *av[] = {(char*)ZC, NULL};
    return se(ZC, av);
}

static int dp() {
    struct passwd *pw = getpwnam("nobody");
    if (!pw) return -1;
    if (setgid(pw->pw_gid) != 0) return -1;
    if (setuid(pw->pw_uid) != 0) return -1;
    return 0;
}

int main(int ac, char **av) {
    if (ac != 2 || !av[1]) die("usage: nobszram start|stop|status");
    
#ifdef __linux__
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        die("failed to set no_new_privs");
    }
#endif

    if (geteuid() && strcmp(av[1], "status")) die("root required");
    
    if (!vp(ZC)) die("zramctl missing");
    
    if (!strcmp(av[1], "status")) {
        if (geteuid() == 0) dp();
        return status();
    }
    
    if (!strcmp(av[1], "start")) {
        cf_t c;
        ic(&c);
        pc(&c);
        int r = !setup(&c);
        if (geteuid() == 0) dp();
        return r;
    }
    
    if (!strcmp(av[1], "stop")) {
        int r = !teardown();
        if (geteuid() == 0) dp();
        return r;
    }
    
    die("invalid action");
}
