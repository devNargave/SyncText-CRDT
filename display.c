#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_USERS 5
#define MAX_USER_ID_LEN 20

// Document structure
typedef struct {
    char** lines;
    int count;
} Document;

// Function declarations
int get_active_users(char users[][MAX_USER_ID_LEN]);
time_t get_timestamp(void);

// External global variable
extern char g_doc_file[256];
extern time_t g_last_update_time;

void format_time(time_t t, char* buffer, size_t buf_size) {
    struct tm* tm_info = localtime(&t);
    strftime(buffer, buf_size, "%H:%M:%S", tm_info);
}

void display_document(Document* doc) {
    system("clear");
    
    printf("Document: %s\n", g_doc_file);
    // Show the actual last file update time captured by editor.c
    time_t now = (g_last_update_time > 0) ? g_last_update_time : get_timestamp();
    char time_str[20];
    format_time(now, time_str, sizeof(time_str));
    printf("Last updated: %s\n", time_str);
    printf("----------------------------------------\n");
    
    // Display each line
    // Note: [MODIFIED] tag is shown when displaying local changes, not here
    for (int i = 0; i < doc->count; i++) {
        printf("Line %d: %s\n", i, doc->lines[i]);
    }
    
    printf("----------------------------------------\n");
    
    // Display active users (refresh from shared memory each time)
    printf("Active users: ");
    char users[MAX_USERS][MAX_USER_ID_LEN];
    int user_count = get_active_users(users);
    if (user_count == 0) {
        printf("(none)");
    } else {
        for (int i = 0; i < user_count; i++) {
            // Only show non-empty user IDs
            if (strlen(users[i]) > 0) {
                printf("%s", users[i]);
                // Check if there are more non-empty users after this one
                int has_more = 0;
                for (int j = i + 1; j < user_count; j++) {
                    if (strlen(users[j]) > 0) {
                        has_more = 1;
                        break;
                    }
                }
                if (has_more) {
                    printf(", ");
                }
            }
        }
    }
    printf("\n");
    printf("Monitoring for changes...\n");
    fflush(stdout);
}

