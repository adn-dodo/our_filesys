#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include "namespace.h"
#include "userfs.h"

void test_path_validation() {
    printf("Running Path Validation Tests...\n");
    
    // Valid paths
    assert(validate_path("/") == 0);
    assert(validate_path("/docs") == 0);
    assert(validate_path("/docs/course/notes.txt") == 0);

    // Invalid paths
    assert(validate_path("docs/course") == -EINVAL); // Missing absolute slash
    assert(validate_path("/docs/../course") == -EINVAL); // Contains '..'
    
    printf("Path Validation: PASS\n\n");
}

void test_acceptance_sequence() {
    printf("Running Acceptance Sequence...\n");

    // Acceptance test from TASK.md[cite: 1]
    int res;

    res = ufs_mkdir("/docs");
    printf("mkdir /docs -> %d\n", res);

    res = ufs_mkdir("/docs/course");
    printf("mkdir /docs/course -> %d\n", res);

    res = ufs_create("/docs/course/notes.txt");
    printf("create /docs/course/notes.txt -> %d\n", res);

    // List directory
    struct ufs_dirent entries[10];
    res = ufs_listdir("/docs/course", entries, 10);
    printf("list /docs/course -> %d entries found\n", res);

    // Stat
    struct ufs_stat st;
    res = ufs_stat("/docs/course/notes.txt", &st);
    printf("stat /docs/course/notes.txt -> res: %d, type: %d, size: %zu\n", res, st.type, st.size);

    // Teardown
    res = ufs_unlink("/docs/course/notes.txt");
    printf("unlink /docs/course/notes.txt -> %d\n", res);

    res = ufs_rmdir("/docs/course");
    printf("rmdir /docs/course -> %d\n", res);

    res = ufs_rmdir("/docs");
    printf("rmdir /docs -> %d\n", res);
    
    printf("Acceptance Sequence: COMPLETE\n");
}

int main() {
    printf("=== Starting Namespace Tests ===\n\n");
    
    test_path_validation();
    test_acceptance_sequence();
    
    printf("\n=== All Tests Finished ===\n");
    return 0;
}
