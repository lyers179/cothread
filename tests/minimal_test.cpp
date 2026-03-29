#include <cstdio>

int main() {
    printf("=== Minimal Test - First printf ===\n");
    fflush(stdout);
    
    // Just test printf, no bthread includes
    printf("=== Minimal Test - Second printf ===\n");
    fflush(stdout);
    
    return 0;
}
