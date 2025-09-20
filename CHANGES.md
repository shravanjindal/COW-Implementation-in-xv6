# COW (Copy-On-Write) Implementation Guide for xv6

## Overview

Copy-On-Write (COW) is an optimization technique that delays the copying of pages during fork() until a write operation actually occurs. Instead of immediately copying all pages when forking, both parent and child processes share the same physical pages marked as read-only. When either process attempts to write to a shared page, a page fault occurs, and only then is the page copied.

## 1. Memory Management Infrastructure Changes

### 1.1 Reference Counting System (`kalloc.c`)

**Added Global Variables:**
```c
int free_pages_count = 0;
#define MAX_PAGES (PHYSTOP / PGSIZE)
int ref_count[MAX_PAGES];
```

**Purpose:** Track how many processes reference each physical page.

**Key Functions Added:**

#### `incref(char *pa)` - Increment Reference Count
```c
void incref(char *pa) {
    if(kmem.use_lock) acquire(&kmem.lock);
    int index = ((uint)pa)/PGSIZE;
    if (index >= 0 && index < MAX_PAGES) {
        ref_count[index]++;
    }
    if(kmem.use_lock) release(&kmem.lock);
}
```
- **What it does:** Increases the reference count for a physical page
- **When used:** Called when a page is shared between processes (during fork)
- **Thread safety:** Uses the kernel memory lock for atomic operations

#### `decref(char *pa)` - Decrement Reference Count
```c
void decref(char *pa) {
    if(kmem.use_lock) acquire(&kmem.lock);
    int index = ((uint)pa)/PGSIZE;
    if (index >= 0 && index < MAX_PAGES) {
        if (ref_count[index] > 0) {
            ref_count[index]--;
        }
    }
    if(kmem.use_lock) release(&kmem.lock);
}
```
- **What it does:** Decreases the reference count for a physical page
- **When used:** Called when a process no longer needs a page (during page copy or process termination)

#### `get_ref_count(char *pa)` - Get Reference Count
- **Purpose:** Returns the current reference count for a page
- **Usage:** Helps determine if a page can be safely freed or if it needs special COW handling

### 1.2 Modified Memory Allocation (`kalloc.c`)

**Changes to `kalloc()`:**
```c
if(r){
    ref_count[((uint)(char*)V2P(r))/PGSIZE]++;
}
```
- **What changed:** When allocating a new page, its reference count is set to 1
- **Why:** Ensures proper tracking of page ownership from the moment of allocation

**Changes to `kfree()`:**
```c
decref((char *)V2P(v));
if (get_ref_count((char *)V2P(v)) > 0) {
    return; // Page is still in use
}
```
- **What changed:** Before freeing a page, check if other processes still reference it
- **Why:** Prevents freeing pages that are still shared between processes

## 2. Page Table Entry Modifications

### 2.1 New PTE Flag (`mmu.h`)

**Added:**
```c
#define PTE_COW         0x800   // Page using COW implementation
```

**Purpose:** 
- Marks pages that are shared between processes via COW
- Uses bit 11 of the page table entry (available for OS use)
- Distinguishes COW pages from regular read-only pages

## 3. Fork Implementation Changes

### 3.1 Modified `copyuvm()` Function (`vm.c`)

**Original Behavior:** Fork copied all pages immediately
**New COW Behavior:** Fork shares pages and marks them for COW

**Key Changes:**
```c
// Increment the reference count for the shared page
incref((char *)pa);

// Mark the page as read-only in both parent and child
*pte &= ~PTE_W;        // Remove write permission
*pte |= PTE_COW;       // Mark as COW page

// Map the same physical page in child's page table
if(mappages(d, (void*)i, PGSIZE, pa, flags=((*pte & ~PTE_W) | PTE_COW)) < 0) {
    freevm(d);
    return 0;
}
```

**Step-by-step Process:**
1. **Share Physical Pages:** Instead of allocating new pages, both processes reference the same physical memory
2. **Remove Write Permissions:** Clear the PTE_W bit to make pages read-only
3. **Set COW Flag:** Add PTE_COW bit to identify these as COW pages
4. **Update Reference Count:** Increment ref_count so the page won't be freed prematurely
5. **Flush TLB:** Ensure the CPU sees the updated page table entries

**Memory Savings:** This change dramatically reduces memory usage during fork, as no pages are copied initially.

## 4. Page Fault Handling System

### 4.1 Trap Handler Integration (`trap.c`)

**Added Case:**
```c
case T_PGFLT:
    handle_page_fault(tf);
    break;
```

**Purpose:** When a page fault occurs (attempt to write to read-only COW page), the kernel now handles it specially instead of killing the process.

### 4.2 Page Fault Handler (`vm.c`)

**Main Handler Function:**
```c
void handle_page_fault(struct trapframe *tf) {
    uint faulting_addr = rcr2(); // Get the faulting address from CR2 register
    
    // Walk the page table to find the PTE
    pte_t *pte = walkpgdir(myproc()->pgdir, (void*)faulting_addr, 0);
    
    if(pte == 0 || !(*pte & PTE_P)) {
        // Invalid address - kill process
        cprintf("Page fault: invalid address 0x%x\n", faulting_addr);
        kill(myproc()->pid);
        return;
    }
    
    if(*pte & PTE_COW) {
        // Handle COW page fault
        if(handle_cow_page(faulting_addr, pte) < 0) {
            cprintf("Failed to handle COW page fault\n");
            return;
        }
    } else {
        // Regular page fault - kill process
        cprintf("Page fault: unauthorized write to address 0x%x\n", faulting_addr);
        kill(myproc()->pid);
        return;
    }
}
```

**Process Flow:**
1. **Get Fault Address:** Read from CR2 register which contains the virtual address that caused the fault
2. **Validate Address:** Check if the address is mapped in the page table
3. **Check COW Status:** Determine if this is a COW page that needs handling
4. **Handle or Kill:** Either handle the COW fault or kill the process for invalid access

### 4.3 COW Page Handler

**Core COW Logic:**
```c
int handle_cow_page(uint faulting_addr, pte_t *pte) {
    uint pa = PTE_ADDR(*pte);  // Get current physical address
    char *mem;
    
    // Allocate a new page
    mem = kalloc();
    if(mem == 0) {
        cprintf("Out of memory\n");
        return -1;
    }
    
    // Copy content from shared page to new private page
    memmove(mem, (char*)P2V(pa), PGSIZE);
    
    // Map new page as writable in current process
    uint va = PGROUNDDOWN(faulting_addr);
    if(mappages(myproc()->pgdir, (void*)va, PGSIZE, V2P(mem), PTE_W | PTE_U) < 0) {
        kfree(mem);
        return -1;
    }
    
    // Handle reference counting
    if (get_ref_count((char *)pa) == 1) {
        // Only one reference left - make original writable again
        *pte |= PTE_W;
        *pte &= ~PTE_COW;
        lcr3(V2P(myproc()->pgdir));  // Flush TLB
    } else {
        // Multiple references - decrement count for old page
        decref((char *)pa);
    }
    
    // Set reference count for new page
    incref((char *)V2P(mem));
    return 0;
}
```

**Optimization Detail:**
The implementation includes a smart optimization: if only one process remains referencing a COW page, instead of copying, it simply makes the original page writable again. This saves both memory and copying time.

## 5. System Call Integration

### 5.1 Free Pages Monitoring System

**New System Call Added:**
- **syscall.h:** `#define SYS_getNumFreePages 22`
- **syscall.c:** Added `sys_getNumFreePages` to syscall table
- **sysproc.c:** Implementation returns `free_pages_count`
- **user.h & usys.S:** User-space interface

**Purpose:** Allows monitoring memory usage to verify COW is working correctly.

### 5.2 Free Pages Tracking (`kalloc.c`)

**In `kfree()`:**
```c
free_pages_count++;
```

**In `kalloc()`:**
```c
if(r){
    free_pages_count--;
}
```

**Purpose:** Maintains an accurate count of available pages for testing and monitoring.

## 6. Testing Infrastructure

### 6.1 Test Program (`testcow.c`)

**Test Strategy:**
1. **Measure Initial State:** Record free pages before any operations
2. **Allocate Memory:** Allocate memory in parent process
3. **Fork Process:** Create child process (should not decrease free pages significantly)
4. **Trigger COW:** Have both parent and child write to shared memory
5. **Verify Behavior:** Confirm that writes trigger page copying (decrease in free pages)

**Key Test Points:**
```c
// Before fork - pages are shared
int free_pages_after_fork = getNumFreePages();

// After write - new page allocated
parent_memory[0] = 'P';  // This triggers COW
int free_pages_after_modification = getNumFreePages();

// Verification
if (free_pages_after_modification < free_pages_after_fork) {
    printf(1, "COW mechanism working! A new page was allocated after write.\n");
}
```

### 6.2 Free Pages Utility (`getnumfreepages.c`)

Simple utility to check current free page count from command line.

## 7. How It All Works Together

### 7.1 Fork Process Flow

1. **Process calls fork():**
   - `copyuvm()` is called to set up child's address space
   - Instead of copying pages, physical pages are shared
   - Both parent and child page tables point to same physical memory
   - All shared pages marked read-only with PTE_COW flag
   - Reference counts incremented for all shared pages

2. **Both processes run normally:**
   - Read operations work normally (pages are present and readable)
   - Write operations trigger page faults (pages are read-only)

### 7.2 Write Operation Flow

1. **Process attempts to write to COW page:**
   - CPU generates page fault (write to read-only page)
   - Kernel trap handler catches T_PGFLT

2. **Page fault analysis:**
   - `handle_page_fault()` gets the faulting address
   - Walks page table to find the PTE
   - Checks if PTE has PTE_COW flag set

3. **COW page handling:**
   - Allocates new physical page
   - Copies content from shared page to new page
   - Maps new page as writable in current process's page table
   - Updates reference counts appropriately
   - If original page has only one reference left, makes it writable again

4. **Process continues:**
   - Write operation completes successfully
   - Process now has its own private copy of the page

## 8. Benefits and Performance

### 8.1 Memory Efficiency
- **Before COW:** Fork immediately doubled memory usage
- **After COW:** Fork uses minimal additional memory initially
- **Real copying:** Only happens when pages are actually modified

### 8.2 Performance Benefits
- **Faster fork():** No immediate copying means faster process creation
- **Lazy copying:** Only copy pages that are actually modified
- **Cache efficiency:** Shared pages remain in CPU caches longer

### 8.3 Reference Counting Optimization
The implementation includes a clever optimization: when a COW page's reference count drops to 1, instead of copying the page, the remaining process gets write access to the original page. This saves both memory allocation and copying time.

## 9. Edge Cases and Error Handling

### 9.1 Memory Exhaustion
- If `kalloc()` fails during COW page copy, the process is terminated
- Reference counts ensure no memory leaks during failures

### 9.2 Invalid Memory Access
- Regular page faults (non-COW) still kill the process
- Proper validation ensures only legitimate COW faults are handled

### 9.3 Process Termination
- When a process exits, `kfree()` properly decrements reference counts
- Pages are only returned to free list when no processes reference them

## 10. Summary

This COW implementation transforms xv6's fork mechanism from an expensive immediate-copy operation to an efficient lazy-copy system. The key innovations include:

1. **Reference counting system** to track page sharing
2. **Special COW page table entries** to mark shared pages
3. **Page fault handler** to perform copy-on-write when needed
4. **Optimization for single-reference pages** to avoid unnecessary copying

The result is a more memory-efficient and faster fork operation that only performs copying when absolutely necessary, while maintaining the illusion that each process has its own private memory space.