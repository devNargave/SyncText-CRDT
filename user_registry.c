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

// User registry structure
typedef struct {
    char user_ids[MAX_USERS][MAX_USER_ID_LEN];
    char queue_names[MAX_USERS][MAX_QUEUE_NAME_LEN];
    int count;
} UserRegistry;

void register_user(const char* user_id) {
    key_t key = ftok("/tmp", 1234);
    
    int shmid = shmget(key, sizeof(UserRegistry), IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget");
        return;
    }
    
    UserRegistry* registry = (UserRegistry*)shmat(shmid, NULL, 0);
    if (registry == (void*)-1) {
        perror("shmat");
        return;
    }
    
    int exists = 0;
    for (int i = 0; i < registry->count; i++) {
        if (strcmp(registry->user_ids[i], user_id) == 0) {
            exists = 1;
            break;
        }
    }
    
    if (!exists && registry->count < MAX_USERS) {
        int idx = registry->count;
        strncpy(registry->user_ids[idx], user_id, MAX_USER_ID_LEN - 1);
        registry->user_ids[idx][MAX_USER_ID_LEN - 1] = '\0';
        
        char safe_id[MAX_USER_ID_LEN];
        strncpy(safe_id, user_id, MAX_USER_ID_LEN - 1);
        safe_id[MAX_USER_ID_LEN - 1] = '\0';
        
        for (int i = 0; safe_id[i] != '\0'; i++) {
            char c = safe_id[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
                  (c >= '0' && c <= '9') || c == '_')) {
                safe_id[i] = '_';
            }
        }
        
        char queue_name[MAX_QUEUE_NAME_LEN];
        snprintf(queue_name, MAX_QUEUE_NAME_LEN, "/queue_%s", safe_id);
        strncpy(registry->queue_names[idx], queue_name, MAX_QUEUE_NAME_LEN - 1);
        registry->queue_names[idx][MAX_QUEUE_NAME_LEN - 1] = '\0';
        
        registry->count++;
    }
    
    shmdt(registry);
}

int get_active_users(char users[][MAX_USER_ID_LEN]) {
    int count = 0;
    
    key_t key = ftok("/tmp", 1234);
    int shmid = shmget(key, sizeof(UserRegistry), IPC_CREAT | 0666);
    if (shmid == -1) {
        return 0;  
    }
    
    UserRegistry* registry = (UserRegistry*)shmat(shmid, NULL, 0);
    if (registry == (void*)-1) {
        return 0;
    }
    
    for (int i = 0; i < registry->count && i < MAX_USERS; i++) {
        strncpy(users[count], registry->user_ids[i], MAX_USER_ID_LEN - 1);
        users[count][MAX_USER_ID_LEN - 1] = '\0';
        count++;
    }
    
    shmdt(registry);
    return count;
}

void deregister_user(const char* user_id) {
    key_t key = ftok("/tmp", 1234);
    int shmid = shmget(key, sizeof(UserRegistry), IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget in deregister_user");
        return;
    }
    
    UserRegistry* registry = (UserRegistry*)shmat(shmid, NULL, 0);
    if (registry == (void*)-1) {
        perror("shmat in deregister_user");
        return;
    }
    
    int found_index = -1;
    for (int i = 0; i < registry->count; i++) {
        if (strcmp(registry->user_ids[i], user_id) == 0) {
            found_index = i;
            break;
        }
    }
    
    if (found_index != -1) {
        for (int i = found_index; i < registry->count - 1; i++) {
            strncpy(registry->user_ids[i], registry->user_ids[i + 1], MAX_USER_ID_LEN - 1);
            registry->user_ids[i][MAX_USER_ID_LEN - 1] = '\0';
            strncpy(registry->queue_names[i], registry->queue_names[i + 1], MAX_QUEUE_NAME_LEN - 1);
            registry->queue_names[i][MAX_QUEUE_NAME_LEN - 1] = '\0';
        }
        
        registry->user_ids[registry->count - 1][0] = '\0';
        registry->queue_names[registry->count - 1][0] = '\0';
        
        registry->count--;
    }
    
    shmdt(registry);
}

void clear_all_users(void) {
    key_t key = ftok("/tmp", 1234);
    int shmid = shmget(key, sizeof(UserRegistry), IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget in clear_all_users");
        return;
    }
    
    // Attach to shared memory
    UserRegistry* registry = (UserRegistry*)shmat(shmid, NULL, 0);
    if (registry == (void*)-1) {
        perror("shmat in clear_all_users");
        return;
    }
    
    // Clear all user entries
    for (int i = 0; i < MAX_USERS; i++) {
        registry->user_ids[i][0] = '\0';
        registry->queue_names[i][0] = '\0';
    }
    
    // Reset count to 0
    registry->count = 0;
    
    // Detach from shared memory
    shmdt(registry);
    
    printf("All active users cleared from registry\n");
}

