#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <mqueue.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <signal.h>
#include <errno.h>

#define MAX_LINE_LENGTH 1024
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

// User registry in shared memory
typedef struct {
    char user_ids[MAX_USERS][MAX_USER_ID_LEN];
    char queue_names[MAX_USERS][MAX_QUEUE_NAME_LEN];
    int count;
} UserRegistry;

// Document structure
typedef struct {
    char** lines;
    int count;
} Document;

// Function declarations
void init_document(const char* filename);
Document* read_file_lines(const char* filename);
void free_document(Document* doc);
void write_file_lines(const char* filename, Document* doc);
Document* create_document(int initial_size);
void add_line_to_document(Document* doc, const char* line);
int detect_line_changes(const char* old_line, const char* new_line, int line_num, Update* updates);
int detect_changes(Document* old_doc, Document* new_doc, Update* updates);
void register_user(const char* user_id);
void deregister_user(const char* user_id);
int get_active_users(char users[][MAX_USER_ID_LEN]);
void sanitize_queue_name(const char* user_id, char* output);
mqd_t create_message_queue(const char* user_id);
void serialize_update(const Update* update, char* buffer, int bufsize);
void deserialize_update(const char* data, Update* update);
void broadcast_updates(Update* updates, int count);
int do_conflict(const Update* u1, const Update* u2);
Update resolve_conflict(const Update* u1, const Update* u2);
int merge_updates(Update* local, int local_count, Update* received, int received_count, Update* merged);
void apply_update(Document* doc, const Update* update);
void display_document(Document* doc);
void format_time(time_t t, char* buffer, size_t buf_size);
time_t get_timestamp(void);
void* listener_thread(void* arg);

// Global variables
char g_user_id[MAX_USER_ID_LEN];
char g_doc_file[256];
mqd_t g_my_queue = (mqd_t)-1;
Update g_local_updates[MAX_UPDATES];
int g_local_count = 0;
Update g_received_updates[MAX_UPDATES];
int g_received_count = 0;
atomic_bool g_running = ATOMIC_VAR_INIT(true);
atomic_int g_update_count = ATOMIC_VAR_INIT(0);
atomic_bool g_merge_flag = ATOMIC_VAR_INIT(false);
pthread_t g_listener_thread;
Document* g_current_doc = NULL;
int g_cleanup_done = 0;
time_t g_last_update_time = 0;

// Cleanup on exit
void cleanup_resources(void) {
    // Prevent double cleanup
    if (g_cleanup_done) {
        return;
    }
    g_cleanup_done = 1;
    
    // Broadcast any remaining updates before exiting
    if (g_local_count > 0) {
        Update to_broadcast[MAX_UPDATES];
        int broadcast_count = g_local_count;
        for (int i = 0; i < g_local_count; i++) {
            to_broadcast[i] = g_local_updates[i];
        }
        broadcast_updates(to_broadcast, broadcast_count);
        g_local_count = 0;
    }
    
    // Stop listener thread
    atomic_store(&g_running, false);
    
    if (g_listener_thread != 0) {
        usleep(100000);
        pthread_join(g_listener_thread, NULL);
    }
    
    // Close message queue
    if (g_my_queue != (mqd_t)-1) {
        mq_close(g_my_queue);
        g_my_queue = (mqd_t)-1;
    }
    
    // Unlink message queue
    char safe_id_cleanup[MAX_USER_ID_LEN];
    sanitize_queue_name(g_user_id, safe_id_cleanup);
    char queue_name[MAX_QUEUE_NAME_LEN];
    snprintf(queue_name, MAX_QUEUE_NAME_LEN, "/queue_%s", safe_id_cleanup);
    mq_unlink(queue_name);
    
    // Deregister user from shared memory
    deregister_user(g_user_id);
    
    // Free document
    if (g_current_doc != NULL) {
        free_document(g_current_doc);
        g_current_doc = NULL;
    }
}

// Signal handler
void signal_handler(int sig) {
    (void)sig;
    atomic_store(&g_running, false);
    cleanup_resources();
    _exit(0);
}

// Listener thread
void* listener_thread(void* arg __attribute__((unused))) {
    struct mq_attr attr;
    mq_getattr(g_my_queue, &attr);
    size_t buffer_size = attr.mq_msgsize;
    char* buffer = (char*)malloc(buffer_size);
    
    while (atomic_load(&g_running)) {
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_nsec += 100000000;
        if (timeout.tv_nsec >= 1000000000) {
            timeout.tv_sec += 1;
            timeout.tv_nsec -= 1000000000;
        }
        
        ssize_t bytes_read = mq_timedreceive(g_my_queue, buffer, buffer_size, NULL, &timeout);
        
        if (bytes_read > 0) {
            // Receive binary Update struct directly (no deserialization needed)
            if (bytes_read == sizeof(Update) && g_received_count < MAX_UPDATES) {
                // Copy the binary data directly to the Update struct
                memcpy(&g_received_updates[g_received_count], buffer, sizeof(Update));
                
                g_received_count++;
                
                atomic_store(&g_merge_flag, true);
                
            }
        } else if (bytes_read == -1 && errno != ETIMEDOUT) {
            // Error occurred (but not timeout)
            // Continue anyway
        }
        
        // Small sleep to avoid busy waiting
        // Reduced sleep time to receive updates faster
        usleep(100000);  // 100 milliseconds - check 10 times per second
    }
    
    free(buffer);
    return NULL;
}

// Main function
int main(int argc, char* argv[]) {
    // Check command line arguments
    if (argc != 2) {
        fprintf(stderr, "Usage: ./editor <user_id>\n");
        return 1;
    }
    
    // Get user ID from command line
    strncpy(g_user_id, argv[1], MAX_USER_ID_LEN - 1);
    g_user_id[MAX_USER_ID_LEN - 1] = '\0';
    
    // Create document filename
    snprintf(g_doc_file, sizeof(g_doc_file), "%s_doc.txt", g_user_id);
    
    // Initialize the document file with default content
    init_document(g_doc_file);
    
    // Register this user in shared memory
    register_user(g_user_id);
    printf("Registered as %s\n", g_user_id);
    
    // Create message queue for this user
    g_my_queue = create_message_queue(g_user_id);
    if (g_my_queue == (mqd_t)-1) {
        fprintf(stderr, "Failed to create message queue\n");
        return 1;
    }
    
    // Get sanitized queue name for display
    char safe_id_display[MAX_USER_ID_LEN];
    sanitize_queue_name(g_user_id, safe_id_display);
    printf("Message queue created: /queue_%s\n", safe_id_display);
    
    // Set up signal handlers for cleanup on termination
    signal(SIGINT, signal_handler);   // Ctrl+C
    signal(SIGTERM, signal_handler);  // Termination signal
    
    // Register cleanup function to be called on normal exit
    atexit(cleanup_resources);
    
    // Start the listener thread
    g_listener_thread = 0;
    if (pthread_create(&g_listener_thread, NULL, listener_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create listener thread\n");
        deregister_user(g_user_id);
        if (g_my_queue != (mqd_t)-1) {
            mq_close(g_my_queue);
        }
        return 1;
    }
    
    // Main monitoring loop
    // This thread monitors the local file for changes
    g_current_doc = read_file_lines(g_doc_file);
    Document* old_doc = g_current_doc;
    struct stat file_stat;
    time_t last_mtime = 0;
    time_t last_display_refresh = 0;
    
    // Set stdout to unbuffered for immediate output
    setbuf(stdout, NULL);
    
    // Initialize last update time from file mtime before first display
    if (stat(g_doc_file, &file_stat) == 0) {
        last_mtime = file_stat.st_mtime;
        g_last_update_time = last_mtime;
    }
    // Display initial document
    display_document(old_doc);
    last_display_refresh = time(NULL);
    
    while (atomic_load(&g_running)) {
        // Check if file was modified using stat()
        if (stat(g_doc_file, &file_stat) == 0) {
            // If modification time changed, file was edited
            if (file_stat.st_mtime != last_mtime) {
                last_mtime = file_stat.st_mtime;
                g_last_update_time = last_mtime;
                
                // Read the new file content
                Document* new_doc = read_file_lines(g_doc_file);
                
                // Detect what changed
                Update changes[MAX_UPDATES];
                int change_count = detect_changes(old_doc, new_doc, changes);
                
                if (change_count > 0) {
                    // Free old document and update
                    if (old_doc != g_current_doc) {
                        free_document(old_doc);
                    }
                    old_doc = new_doc;
                    g_current_doc = new_doc;
                    
                    // Add changes to local updates buffer first
                    for (int i = 0; i < change_count && g_local_count < MAX_UPDATES; i++) {
                        g_local_updates[g_local_count++] = changes[i];
                        atomic_fetch_add(&g_update_count, 1);
                    }
                    
                    // Display updated document with [MODIFIED] tag for changed lines
                    // First, display document with modification markers
                    system("clear");
                    printf("Document: %s\n", g_doc_file);
                    char time_str_display[20];
                    format_time(get_timestamp(), time_str_display, sizeof(time_str_display));
                    printf("Last updated: %s\n", time_str_display);
                    printf("----------------------------------------\n");
                    
                    // Track which lines were modified
                    int modified_lines[MAX_UPDATES] = {0};
                    for (int i = 0; i < change_count; i++) {
                        modified_lines[changes[i].line_num] = 1;
                    }
                    
                    // Display each line with [MODIFIED] tag if changed
                    for (int i = 0; i < old_doc->count; i++) {
                        if (modified_lines[i]) {
                            printf("Line %d: %s [MODIFIED]\n", i, old_doc->lines[i]);
                        } else {
                            printf("Line %d: %s\n", i, old_doc->lines[i]);
                        }
                    }
                    printf("----------------------------------------\n");
                    
                    // Display active users
                    printf("Active users: ");
                    char users[MAX_USERS][MAX_USER_ID_LEN];
                    int user_count = get_active_users(users);
                    if (user_count == 0) {
                        printf("(none)");
                    } else {
                        for (int i = 0; i < user_count; i++) {
                            if (strlen(users[i]) > 0) {
                                printf("%s", users[i]);
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
                    
                    // Show change messages after display (so they persist)
                    for (int i = 0; i < change_count; i++) {
                        char time_str[20];
                        format_time(changes[i].timestamp, time_str, sizeof(time_str));
                        
                        // Format exactly as in assignment example
                        printf("Change detected: Line %d, col %d-%d, \"%s\" → \"%s\", timestamp: %s\n",
                               changes[i].line_num, changes[i].col_start, changes[i].col_end,
                               changes[i].old_content, changes[i].new_content, time_str);
                    }
                    fflush(stdout);
                    
                    // Broadcast after accumulating 5 operations
                    if (atomic_load(&g_update_count) >= 5) {
                        Update to_broadcast[MAX_UPDATES];
                        int broadcast_count = g_local_count;
                        for (int i = 0; i < g_local_count; i++) {
                            to_broadcast[i] = g_local_updates[i];
                        }
                        
                        // Send updates to all other users BEFORE clearing
                        broadcast_updates(to_broadcast, broadcast_count);
                        
                        // Now clear buffers and counters
                        g_local_count = 0;
                        atomic_store(&g_update_count, 0);
                        
                        printf("Broadcasted %d updates to other users\n", broadcast_count);
                        fflush(stdout);
                    }
                } else {
                    free_document(new_doc);
                }
            }
        }
        
        int should_merge = 0;
        
        // Merge only when received buffer reaches 5 updates (per assignment requirement)
        // This follows the "N=5 operations" rule from the assignment
        if (g_received_count >= 5) {
            should_merge = 1;
        }
        
        // Perform merge if needed
        if (should_merge) {
            // Get all updates to merge
            Update received[MAX_UPDATES];
            int received_count = g_received_count;
            for (int i = 0; i < g_received_count; i++) {
                received[i] = g_received_updates[i];
            }
            
            Update local[MAX_UPDATES];
            int local_count = g_local_count;
            for (int i = 0; i < g_local_count; i++) {
                local[i] = g_local_updates[i];
            }
            
            // IMPORTANT: Add local updates to received buffer for conflict resolution
            // This ensures local edits that haven't been broadcast yet compete in conflict resolution
            for (int i = 0; i < local_count && received_count < MAX_UPDATES; i++) {
                received[received_count++] = local[i];
            }
            
            // Now clear buffers AND reset update counter
            // CRITICAL: Must reset g_update_count because we cleared g_local_count
            // IMPORTANT: Do NOT clear local buffer or reset g_update_count here.
            // We only clear received updates so that local updates continue to
            // accumulate toward the N=5 broadcast threshold even across merges.
            g_received_count = 0;
            atomic_store(&g_merge_flag, false);
            
            // Merge using CRDT algorithm
            // Note: We already added local updates to received buffer, so pass empty local array
            Update empty_local[1];
            Update merged[MAX_UPDATES];
            int merged_count = merge_updates(empty_local, 0, received, received_count, merged);
            
            // Read current file state to apply updates
            Document* current_doc = read_file_lines(g_doc_file);
            
            // Apply all merged updates to the document
            for (int i = 0; i < merged_count; i++) {
                apply_update(current_doc, &merged[i]);
            }
            
            // Write merged content back to file
            write_file_lines(g_doc_file, current_doc);
            
            // Update file modification time tracking to prevent detecting this as a user change
            struct stat file_stat_after;
            if (stat(g_doc_file, &file_stat_after) == 0) {
                last_mtime = file_stat_after.st_mtime;
                g_last_update_time = last_mtime;
            }
            
            // Free old document if it's not the same as g_current_doc
            // This prevents double-free when old_doc and g_current_doc point to the same memory
            if (old_doc != NULL && old_doc != g_current_doc) {
                free_document(old_doc);
            }
            // If old_doc == g_current_doc, we need to free g_current_doc before reassigning
            else if (old_doc == g_current_doc && g_current_doc != NULL) {
                free_document(g_current_doc);
            }
            
            // Update pointers
            old_doc = current_doc;
            g_current_doc = current_doc;
            
            // Display updated document with merged changes from other users
            display_document(current_doc);
            
            // Show received updates after display - match assignment example format
            if (received_count > 0) {
                for (int i = 0; i < received_count; i++) {
                    // Format exactly as in assignment: "Received update from user_1: Line 0 modified"
                    printf("Received update from %s: Line %d modified\n",
                           received[i].user_id, received[i].line_num);
                }
                fflush(stdout);
            }
            
            int had_conflict = 0;
            if (local_count > 0 && received_count > 0) {
                for (int i = 0; i < local_count; i++) {
                    for (int j = 0; j < received_count; j++) {
                        if (do_conflict(&local[i], &received[j])) {
                            had_conflict = 1;
                            break;
                        }
                    }
                    if (had_conflict) break;
                }
            }
            
            if (had_conflict) {
                printf("Conflict detected and resolved using LWW\n");
            }
            
            printf("All updates merged successfully\n");
            fflush(stdout);

        }
        
        time_t now = time(NULL);
        if (now - last_display_refresh >= 2) {
            display_document(old_doc);
            last_display_refresh = now;
        }
        
        usleep(200000);
    }
    
    return 0;
}
