#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#define MAX_USERS 5
#define MAX_USER_ID_LEN 20
#define MAX_QUEUE_NAME_LEN 50

// Structure to store user information in shared memory
typedef struct {
    char user_ids[MAX_USERS][MAX_USER_ID_LEN];
    char queue_names[MAX_USERS][MAX_QUEUE_NAME_LEN];
    int count;
} UserRegistry;

// Function declaration
void clear_all_users(void);

// Simple utility program to clear all active users from the registry
int main(void) {
    printf("Clearing all active users from registry...\n");
    clear_all_users();
    return 0;
}

