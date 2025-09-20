#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

// Declare the syscall
// extern int getNumFreePages(void);

int main(void) {
    // Step 1: Record the initial number of free pages
    int initial_free_pages = getNumFreePages();
    printf(1, "Initial free pages: %d\n", initial_free_pages);

    // Step 2: Allocate memory in the parent process
    char *parent_memory = (char*) malloc(4096);  // 4KB memory allocation
    if (parent_memory == 0) {
        printf(1, "Memory allocation failed\n");
        exit();
    }

    // Step 3: Record the number of free pages after allocation
    int free_pages_after_alloc = getNumFreePages();
    printf(1, "Free pages after allocation: %d\n", free_pages_after_alloc);

    // Step 4: Fork the process
    int pid = fork();
    if (pid < 0) {
        printf(1, "Fork failed\n");
        exit();
    }

    // Step 5: Check free pages after fork
    int free_pages_after_fork = getNumFreePages();
    printf(1, "Free pages after fork: %d\n", free_pages_after_fork);

    // Step 6: Modify memory in parent or child
    if (pid == 0) {
        // Child process: Modify memory (should trigger COW)
        parent_memory[0] = 'C';
        printf(1, "Child process modified memory.\n");
        int free_pages_after_modification = getNumFreePages();
        printf(1, "Free pages after modification: %d\n", free_pages_after_modification);
        // Step 8: Compare results and verify COW behavior
        if (free_pages_after_modification < free_pages_after_fork) {
            printf(1, "COW mechanism working! A new page was allocated after write.\n");
        } else {
            printf(1, "Something went wrong, no new page allocated after write.\n");
        }
        // Wait for the child process to avoid a zombie process
        exit();
    } else {
        // Parent process: Modify memory (should trigger COW)
        parent_memory[0] = 'P';
        printf(1, "Parent process modified memory.\n");
        // Step 7: Check free pages after modification
        int free_pages_after_modification = getNumFreePages();
        printf(1, "Free pages after modification: %d\n", free_pages_after_modification);

        // Step 8: Compare results and verify COW behavior
        if (free_pages_after_modification < free_pages_after_fork) {
            printf(1, "COW mechanism working! A new page was allocated after write.\n");
        } else {
            printf(1, "Something went wrong, no new page allocated after write.\n");
        }
        // Wait for the child process to avoid a zombie process
        wait();
    }

    

    exit();
}
