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
    const char *va[] = {"lzo", "lz4", "zstd", "lzo-rle", 0};
    for(int i = 0; va[i]; i++)
        if(!strcmp(s, va[i])) return 1;
    return 0;
}

static void initcfg(cfg_t *c) {
    strcpy(c->sz, "25%");
    strcpy(c->alg, "zstd");
    c->prio = 100;
    c->en = 1;
}

static void parsecfg(cfg_t *c) {
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
        if(strlen(v) >= MAXVAL) continue;
        
        if(!strcmp(k, "size") && valsz(v)) strcpy(c->sz, v);
        else if(!strcmp(k, "algorithm") && valalg(v)) strcpy(c->alg, v);
        else if(!strcmp(k, "priority")) {
            long p = strtol(v, 0, 10);
            if(p >= -1 && p <= 32767) c->prio = p;
        }
        else if(!strcmp(k, "enabled"))
            c->en = !strcmp(v, "true") || !strcmp(v, "1");
    }
    fclose(f);
}

static int runcmd(char *const av[]) {
    pid_t p = fork();
    if(p == -1) return -1;
    
    if(!p) {
        execvp(av[0], av);
        _exit(127);
    }
    
    int st;
    return waitpid(p, &st, 0) == -1 ? -1 : (WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}

static int setup(const cfg_t *c) {
    if(!c->en) return 0;
    
    char *a1[] = {"zramctl", "--find", "--size", (char*)c->sz, 0};
    if(runcmd(a1)) return -1;
    
    char *a2[] = {"zramctl", "--algorithm", (char*)c->alg, "/dev/zram0", 0};
    if(runcmd(a2)) return -1;
    
    char *a3[] = {"mkswap", "/dev/zram0", 0};
    if(runcmd(a3)) return -1;
    
    char p[8];
    snprintf(p, sizeof(p), "%d", c->prio);
    char *a4[] = {"swapon", "--priority", p, "/dev/zram0", 0};
    return !runcmd(a4);
}

static int teardown(void) {
    char *a1[] = {"swapoff", "/dev/zram0", 0};
    char *a2[] = {"zramctl", "--reset", "/dev/zram0", 0};
    runcmd(a1);
    runcmd(a2);
    return 0;
}

static int status(void) {
    char *a[] = {"zramctl", 0};
    return runcmd(a);
}

int main(int c, char **v) {
    if(c != 2) die("usage: nobszram start|stop|status");
    
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
