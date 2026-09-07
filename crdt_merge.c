#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_USER_ID_LEN 20
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

// Document structure
typedef struct {
    char** lines;
    int count;
} Document;

int do_conflict(const Update* u1, const Update* u2) {
    if (u1->line_num != u2->line_num) {
        return 0;  // false
    }
    
    int u1_start = u1->col_start;
    int u1_end = u1->col_end;
    int u2_start = u2->col_start;
    int u2_end = u2->col_end;
    
    // Check if column ranges overlap
    // Two ranges overlap if NOT (one ends before the other starts)
    if (u1_end < u2_start || u2_end < u1_start) {
        return 0;  // No overlap, no conflict
    }
    return 1;  // Overlap detected, conflict exists
}

Update resolve_conflict(const Update* u1, const Update* u2) {
    if (u1->timestamp > u2->timestamp) {
        return *u1;
    } else if (u2->timestamp > u1->timestamp) {
        return *u2;
    } else {
        if (strcmp(u1->user_id, u2->user_id) < 0) {
            return *u1;
        } else {
            return *u2;
        }
    }
}

int merge_updates(Update* local, int local_count, Update* received, int received_count, Update* merged) {
    Update all_updates[MAX_UPDATES * 2];
    int all_count = 0;
    
    // Copy local updates
    for (int i = 0; i < local_count && all_count < MAX_UPDATES * 2; i++) {
        all_updates[all_count++] = local[i];
    }
    
    // Copy received updates
    for (int i = 0; i < received_count && all_count < MAX_UPDATES * 2; i++) {
        all_updates[all_count++] = received[i];
    }
    
    int merged_count = 0;
    int used[MAX_UPDATES * 2] = {0};  // Track which updates are used
    
    // Process each update
    for (int i = 0; i < all_count; i++) {
        if (used[i]) continue;  // Skip if already processed
        
        Update current = all_updates[i];
        int should_add = 1;  // Track if current should be added to merged
        
        // Check for conflicts with other updates
        for (int j = i + 1; j < all_count; j++) {
            if (used[j]) continue;
            
            // If conflict found, resolve it
            if (do_conflict(&current, &all_updates[j])) {
                Update winner = resolve_conflict(&current, &all_updates[j]);
                
                // Check who won by comparing timestamps and user_ids
                if (winner.timestamp == current.timestamp && 
                    strcmp(winner.user_id, current.user_id) == 0) {
                    // current won, all_updates[j] lost
                    used[j] = 1;  // Mark j (loser) as used
                } else {
                    // all_updates[j] won, current lost
                    // Don't mark j as used - it might conflict with other updates
                    // Mark i as used and skip adding current
                    used[i] = 1;
                    should_add = 0;
                    break;  // Stop checking conflicts for this update
                }
            }
        }
        
        // Add the winning update (or non-conflicting update)
        if (should_add && !used[i]) {
            merged[merged_count++] = current;
            used[i] = 1;
        }
    }
    
    return merged_count;
}

// Function to apply an update to the document lines
void apply_update(Document* doc, const Update* update) {
    // Make sure we have enough lines
    if (update->line_num < 0) {
        return;
    }
    
    // Extend document if needed
    while (doc->count <= update->line_num) {
        doc->lines = (char**)realloc(doc->lines, (doc->count + 1) * sizeof(char*));
        doc->lines[doc->count] = (char*)malloc(1);
        doc->lines[doc->count][0] = '\0';
        doc->count++;
    }
    
    char* line = doc->lines[update->line_num];
    
    // Make sure line is long enough
    int current_len = strlen(line);
    if (update->col_start > current_len) {
        line = (char*)realloc(line, update->col_start + 1);
        memset(line + current_len, ' ', update->col_start - current_len);
        line[update->col_start] = '\0';
        doc->lines[update->line_num] = line;
    }
    
    // For CRDT correctness with full-line storage:
    // After conflict resolution via merge_updates, simply replace the entire line
    // This ensures all users converge to the same state
    free(line);
    doc->lines[update->line_num] = strdup(update->new_content);
}
