#include <dlfcn.h>
#include <stdio.h>

int main(void) {
    void *handle = dlopen("/lib/libtest.so", RTLD_LAZY);
    if (!handle) {
        printf("dlopen failed: %s\n", dlerror());
        return 1;
    }

    dlerror();
    int (*fn)(void) = (int (*)(void))dlsym(handle, "test_function");
    const char *err = dlerror();
    if (err || !fn) {
        printf("dlsym failed: %s\n", err ? err : "null symbol");
        dlclose(handle);
        return 2;
    }

    int value = fn();
    printf("test_function() = %d\n", value);

    if (dlclose(handle) != 0) {
        printf("dlclose failed: %s\n", dlerror());
        return 3;
    }

    return value == 1337 ? 0 : 4;
}
