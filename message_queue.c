#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mqueue.h>
#include <time.h>
#include <fcntl.h>
#include <ctype.h>

#define MAX_USERS 5
#define MAX_USER_ID_LEN 20
#define MAX_QUEUE_NAME_LEN 50
#define MAX_UPDATES 100

// Update structure
typedef struct {
    int line_num;
    int col_start;
    int col_end;
    char old_content[256];
    char new_content[256];
    time_t timestamp;
    char user_id[MAX_USER_ID_LEN];
    int op_type;
} Update;

// Function declarations
int get_active_users(char users[][MAX_USER_ID_LEN]);

// External global variable
extern char g_user_id[MAX_USER_ID_LEN];

void sanitize_queue_name(const char* user_id, char* output) {
    int i;
    for (i = 0; user_id[i] != '\0' && i < MAX_USER_ID_LEN - 1; i++) {
        char c = user_id[i];
        if (isalnum(c) || c == '_') {
            output[i] = c;
        } else {
            output[i] = '_';
        }
    }
    output[i] = '\0';
}

mqd_t create_message_queue(const char* user_id) {
    char safe_id[MAX_USER_ID_LEN];
    sanitize_queue_name(user_id, safe_id);
    
    char queue_name[MAX_QUEUE_NAME_LEN];
    snprintf(queue_name, MAX_QUEUE_NAME_LEN, "/queue_%s", safe_id);
    
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;       
    attr.mq_msgsize = sizeof(Update);  // Use binary size instead of 1024
    attr.mq_curmsgs = 0;
    
    mq_unlink(queue_name);  
    
    mqd_t mq = mq_open(queue_name, O_CREAT | O_RDWR, 0666, &attr);
    if (mq == (mqd_t)-1) {
        perror("mq_open");
        fprintf(stderr, "Failed to create queue: %s\n", queue_name);
        fprintf(stderr, "Queue name length: %zu\n", strlen(queue_name));
    }
    return mq;
}

void serialize_update(const Update* update, char* buffer, int bufsize) {
    snprintf(buffer, bufsize, "%d|%d|%d|%s|%s|%ld|%s|%d",
             update->line_num,
             update->col_start,
             update->col_end,
             update->old_content,
             update->new_content,
             (long)update->timestamp,
             update->user_id,
             update->op_type);
}

void deserialize_update(const char* data, Update* update) {
    const char* ptr = data;
    int field = 0;
    char temp[256];
    
    while (*ptr && field < 8) {
        const char* delim = strchr(ptr, '|');
        int len = delim ? (int)(delim - ptr) : (int)strlen(ptr);
        
        if (len > 0 && len < 256) {
            strncpy(temp, ptr, len);
            temp[len] = '\0';
        } else if (len == 0) {
            temp[0] = '\0';
        } else {
            temp[255] = '\0';
        }
        
        switch(field) {
            case 0: update->line_num = atoi(temp); break;
            case 1: update->col_start = atoi(temp); break;
            case 2: update->col_end = atoi(temp); break;
            case 3: 
                strncpy(update->old_content, temp, 255);
                update->old_content[255] = '\0';
                break;
            case 4:
                strncpy(update->new_content, temp, 255);
                update->new_content[255] = '\0';
                break;
            case 5: update->timestamp = (time_t)atol(temp); break;
            case 6:
                strncpy(update->user_id, temp, MAX_USER_ID_LEN - 1);
                update->user_id[MAX_USER_ID_LEN - 1] = '\0';
                break;
            case 7: update->op_type = atoi(temp); break;
        }
        
        field++;
        
        if (delim) {
            ptr = delim + 1;
        } else {
            break;
        }
    }
}

void broadcast_updates(Update* updates, int count) {
    char users[MAX_USERS][MAX_USER_ID_LEN];
    int user_count = get_active_users(users);
    
    for (int i = 0; i < count; i++) {
        // Send to each user (except ourselves)
        for (int j = 0; j < user_count; j++) {
            if (strcmp(users[j], g_user_id) != 0) {
                // Open their message queue (sanitize the name)
                char safe_user[MAX_USER_ID_LEN];
                sanitize_queue_name(users[j], safe_user);
                
                char queue_name[MAX_QUEUE_NAME_LEN];
                snprintf(queue_name, MAX_QUEUE_NAME_LEN, "/queue_%s", safe_user);
                
                // Open in non-blocking mode to avoid hanging if queue is full
                mqd_t mq = mq_open(queue_name, O_WRONLY | O_NONBLOCK);
                if (mq != (mqd_t)-1) {
                    // Send the message as binary struct (more reliable than text)
                    int send_result = mq_send(mq, (char*)&updates[i], sizeof(Update), 0);
                    if (send_result == -1) {
                        // If send fails, try to reopen and send again
                        mq_close(mq);
                        // Small delay and retry once
                        usleep(10000);  // 10ms
                        mq = mq_open(queue_name, O_WRONLY | O_NONBLOCK);
                        if (mq != (mqd_t)-1) {
                            mq_send(mq, (char*)&updates[i], sizeof(Update), 0);
                            mq_close(mq);
                        }
                    } else {
                        mq_close(mq);
                    }
                }
            }
        }
    }
}

