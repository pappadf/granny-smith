// Shell command stubs for unit tests
// Provides no-op implementations of shell command registration and dispatch.

int register_cmd(const char *name, const char *category, const char *synopsis,
                 int (*fn)(int, char**)) {
    (void)name; (void)category; (void)synopsis; (void)fn;
    return 0;
}

int register_cmd2(const char *name, const char *category, const char *synopsis,
                  int (*fn)(int, char**)) {
    (void)name; (void)category; (void)synopsis; (void)fn;
    return 0;
}

int register_cmd3(const char *name, int (*fn)(int, char**)) {
    (void)name; (void)fn;
    return 0;
}

int unregister_cmd(const char *name) {
    (void)name;
    return 0;
}

int shell_init(void) {
    return 0;
}

int shell_dispatch(char *line) {
    (void)line;
    return 0;
}
