#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>

#define CFG "/etc/nobszram/nobszram.conf"
#define MAXLN 64
#define MAXVAL 16

typedef struct {
    char sz[MAXVAL];
    char alg[MAXVAL];
    int prio;
    int en;
} cfg_t;

static void die(const char *s) {
    fprintf(stderr, "nobszram: %s\n", s);
    _exit(1);
}

static char *trim(char *s) {
    char *e;
    while(isspace(*s)) s++;
    if(!*s) return s;
    e = s + strlen(s) - 1;
    while(e > s && isspace(*e)) e--;
    e[1] = 0;
    return s;
}

static long getmem(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if(!f) return -1;
    
    char ln[128];
    long m = -1;
    while(fgets(ln, sizeof(ln), f)) {
        if(sscanf(ln, "MemTotal: %ld kB", &m) == 1) break;
    }
    fclose(f);
    return m > 0 ? m : -1;
}

static int calcsz(const char *s, char *o, size_t osiz) {
    if(!s || !*s || !o || osiz < 2) return 0;
    
    int l = strlen(s);
    if(l >= MAXVAL) return 0;
    
    if(s[l-1] == '%') {
        for(int i = 0; i < l-1; i++)
            if(!isdigit(s[i])) return 0;
        
        int p = atoi(s);
        if(p <= 0 || p > 100) return 0;
        
        long m = getmem();
        if(m <= 0) return 0;
        
        long k = (m * p) / 100;
        if(k <= 0) return 0;
        
        int n = snprintf(o, osiz, "%ldK", k);
        return n > 0 && n < (int)osiz;
    }
    
    char *ep;
    long v = strtol(s, &ep, 10);
    if(v <= 0) return 0;
    if(!*ep || (strlen(ep) == 1 && strchr("KMGT", *ep))) {
        int n = snprintf(o, osiz, "%s", s);
        return n > 0 && n < (int)osiz;
    }
    return 0;
}

static int valsz(const char *s) {
    if(!s || !*s) return 0;
    int l = strlen(s);
    if(!l || l >= MAXVAL) return 0;
    
    if(s[l-1] == '%') {
        for(int i = 0; i < l-1; i++)
            if(!isdigit(s[i])) return 0;
        int v = atoi(s);
        return v > 0 && v <= 100;
    }
    
    char *ep;
    long v = strtol(s, &ep, 10);
    if(v <= 0) return 0;
    return !*ep || (strlen(ep) == 1 && strchr("KMGT", *ep));
}

static int valalg(const char *s) {
    if(!s || !*s) return 0;
    const char *va[] = {"lzo", "lz4", "lz4hc", "deflate", "842", "zstd", "lzo-rle", 0};
    for(int i = 0; va[i]; i++)
        if(!strcmp(s, va[i])) return 1;
    return 0;
}

static void initcfg(cfg_t *c) {
    if(!c) return;
    strcpy(c->sz, "25%");
    strcpy(c->alg, "zstd");
    c->prio = 100;
    c->en = 1;
}

static void parsecfg(cfg_t *c) {
    if(!c) return;
    FILE *f = fopen(CFG, "r");
    if(!f) return;
    
    char ln[MAXLN];
    while(fgets(ln, sizeof(ln), f)) {
        char *t = trim(ln);
        if(!*t || *t == '#') continue;
        
        char *eq = strchr(t, '=');
        if(!eq) continue;
        
        *eq = 0;
        char *k = trim(t), *v = trim(eq+1);
        if(!k || !v || strlen(v) >= MAXVAL) continue;
        
        if(!strcmp(k, "size") && valsz(v)) {
            strncpy(c->sz, v, MAXVAL-1);
            c->sz[MAXVAL-1] = 0;
        }
        else if(!strcmp(k, "algorithm") && valalg(v)) {
            strncpy(c->alg, v, MAXVAL-1);
            c->alg[MAXVAL-1] = 0;
        }
        else if(!strcmp(k, "priority")) {
            char *ep;
            errno = 0;
            long p = strtol(v, &ep, 10);
            if(!errno && !*ep && p >= -1 && p <= 32767) c->prio = p;
        }
        else if(!strcmp(k, "enabled"))
            c->en = !strcmp(v, "true") || !strcmp(v, "1");
    }
    fclose(f);
}

static int runcmd(char *const av[]) {
    if(!av || !av[0]) return -1;
    
    pid_t p = fork();
    if(p == -1) return -1;
    
    if(!p) {
        execvp(av[0], av);
        _exit(127);
    }
    
    int st;
    if(waitpid(p, &st, 0) == -1) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int haszram(void) {
    char *a[] = {"zramctl", "--noheadings", "--output", "NAME,MOUNTPOINT", 0};
    int pfd[2];
    if(pipe(pfd)) return 0;
    
    pid_t p = fork();
    if(p == -1) {
        close(pfd[0]);
        close(pfd[1]);
        return 0;
    }
    
    if(!p) {
        close(pfd[0]);
        dup2(pfd[1], 1);
        close(pfd[1]);
        execvp(a[0], a);
        _exit(127);
    }
    
    close(pfd[1]);
    char buf[256];
    ssize_t n = read(pfd[0], buf, sizeof(buf)-1);
    close(pfd[0]);
    
    int st;
    waitpid(p, &st, 0);
    
    if(n <= 0 || !WIFEXITED(st) || WEXITSTATUS(st)) return 0;
    
    buf[n] = 0;
    return strstr(buf, "[SWAP]") != 0;
}

static char *findzram(void) {
    char *a[] = {"zramctl", "--find", 0};
    int pfd[2];
    if(pipe(pfd)) return 0;
    
    pid_t p = fork();
    if(p == -1) {
        close(pfd[0]);
        close(pfd[1]);
        return 0;
    }
    
    if(!p) {
        close(pfd[0]);
        dup2(pfd[1], 1);
        close(pfd[1]);
        execvp(a[0], a);
        _exit(127);
    }
    
    close(pfd[1]);
    static char dev[16];
    ssize_t n = read(pfd[0], dev, sizeof(dev)-1);
    close(pfd[0]);
    
    int st;
    waitpid(p, &st, 0);
    
    if(n <= 0 || !WIFEXITED(st) || WEXITSTATUS(st)) return 0;
    
    dev[n] = 0;
    char *nl = strchr(dev, '\n');
    if(nl) *nl = 0;
    
    return strlen(dev) > 0 ? dev : 0;
}

static int setup(const cfg_t *c) {
    if(!c || !c->en) return 0;
    
    if(haszram()) return 1;
    
    char sb[32];
    if(!calcsz(c->sz, sb, sizeof(sb))) return -1;
    
    char *dev = findzram();
    if(!dev) return -1;
    
    char *a1[] = {"zramctl", "-s", sb, "-a", (char*)c->alg, dev, 0};
    if(runcmd(a1)) return -1;
    
    char *a2[] = {"mkswap", dev, 0};
    if(runcmd(a2)) return -1;
    
    char pb[8];
    int n = snprintf(pb, sizeof(pb), "%d", c->prio);
    if(n <= 0 || n >= (int)sizeof(pb)) return -1;
    
    char *a3[] = {"swapon", "--priority", pb, dev, 0};
    return !runcmd(a3);
}

static char *getzram(void) {
    char *a[] = {"zramctl", "--noheadings", "--output", "NAME,MOUNTPOINT", 0};
    int pfd[2];
    if(pipe(pfd)) return 0;
    
    pid_t p = fork();
    if(p == -1) {
        close(pfd[0]);
        close(pfd[1]);
        return 0;
    }
    
    if(!p) {
        close(pfd[0]);
        dup2(pfd[1], 1);
        close(pfd[1]);
        execvp(a[0], a);
        _exit(127);
    }
    
    close(pfd[1]);
    char buf[256];
    ssize_t n = read(pfd[0], buf, sizeof(buf)-1);
    close(pfd[0]);
    
    int st;
    waitpid(p, &st, 0);
    
    if(n <= 0 || !WIFEXITED(st) || WEXITSTATUS(st)) return 0;
    
    buf[n] = 0;
    char *line = strtok(buf, "\n");
    while(line) {
        if(strstr(line, "[SWAP]")) {
            static char dev[16];
            if(sscanf(line, "%15s", dev) == 1) return dev;
        }
        line = strtok(0, "\n");
    }
    return 0;
}

static int teardown(void) {
    char *dev = getzram();
    if(!dev) return 0;
    
    char *a1[] = {"swapoff", dev, 0};
    char *a2[] = {"zramctl", "--reset", dev, 0};
    runcmd(a1);
    runcmd(a2);
    return 0;
}

static int status(void) {
    char *a[] = {"zramctl", 0};
    return runcmd(a);
}

int main(int c, char **v) {
    if(c != 2 || !v || !v[1]) die("usage: nobszram start|stop|status");
    
    if(geteuid() && strcmp(v[1], "status")) die("root required");
    
    char *t[] = {"zramctl", "--version", 0};
    if(runcmd(t)) die("zramctl missing");
    
    if(!strcmp(v[1], "status")) return status();
    
    if(!strcmp(v[1], "start")) {
        cfg_t cfg;
        initcfg(&cfg);
        parsecfg(&cfg);
        return !setup(&cfg);
    }
    
    if(!strcmp(v[1], "stop")) return !teardown();
    
    die("invalid action");
}
