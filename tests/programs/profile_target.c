#include <stdio.h>

int compute(int x) {
    int s = 0;
    for (int i = 0; i < x; i++) {
        s += i * 3;
        s ^= s >> 2;
    }
    return s;
}

int main(void) {
    printf("%d\n", compute(10));
    return 0;
}
