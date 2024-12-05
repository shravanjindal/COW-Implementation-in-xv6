#include "types.h"
#include "stat.h"
#include "user.h"

int main() {
    int free_pages = getNumFreePages();
    printf(1, "Free pages: %d\n", free_pages);
    exit();
}
