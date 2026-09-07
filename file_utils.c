#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>

#define MAX_LINE_LENGTH 1024
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

// External global variable
extern char g_user_id[MAX_USER_ID_LEN];

time_t get_timestamp(void) {
    return time(NULL);
}

void init_document(const char* filename) {
    FILE* check = fopen(filename, "r");
    if (check != NULL) {
        fclose(check);
        return;
    }
    
    FILE* file = fopen(filename, "w");
    if (file != NULL) {
        fprintf(file, "int x = 10;\n");
        fprintf(file, "int y = 20;\n");
        fprintf(file, "int z = 30;\n");
        fclose(file);
    }
}

Document* create_document(int initial_size) {
    Document* doc = (Document*)malloc(sizeof(Document));
    doc->count = 0;
    doc->lines = (char**)malloc(initial_size * sizeof(char*));
    return doc;
}

void add_line_to_document(Document* doc, const char* line) {
    doc->lines[doc->count] = (char*)malloc(strlen(line) + 1);
    strcpy(doc->lines[doc->count], line);
    doc->count++;
}

void free_document(Document* doc) {
    if (doc == NULL) return;
    for (int i = 0; i < doc->count; i++) {
        free(doc->lines[i]);
    }
    free(doc->lines);
    free(doc);
}

Document* read_file_lines(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (file == NULL) {
        return create_document(10);
    }
    
    Document* doc = create_document(100);
    char line[MAX_LINE_LENGTH];
    
    while (fgets(line, MAX_LINE_LENGTH, file) != NULL) {
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
        }
        add_line_to_document(doc, line);
    }
    
    fclose(file);
    return doc;
}

void write_file_lines(const char* filename, Document* doc) {
    FILE* file = fopen(filename, "w");
    if (file != NULL) {
        for (int i = 0; i < doc->count; i++) {
            fprintf(file, "%s\n", doc->lines[i]);
        }
        fflush(file);
        fsync(fileno(file));
        fclose(file);
    }
}

int detect_line_changes(const char* old_line, const char* new_line, int line_num, Update* updates) {
    if (strcmp(old_line, new_line) == 0) {
        return 0;
    }
    
    int old_len = strlen(old_line);
    int new_len = strlen(new_line);
    
    // Find the start of the change (first differing character)
    int start_col = 0;
    while (start_col < old_len && start_col < new_len && old_line[start_col] == new_line[start_col]) {
        start_col++;
    }
    
    // Find the end of the change (last differing character)
    int old_end = old_len;
    int new_end = new_len;
    while (old_end > start_col && new_end > start_col && old_line[old_end - 1] == new_line[new_end - 1]) {
        old_end--;
        new_end--;
    }
    
    // Extract the changed portions
    Update* update = &updates[0];
    update->line_num = line_num;
    update->col_start = start_col;
    update->col_end = old_end;
    
    // For CRDT correctness: store full lines
    // This ensures proper conflict resolution and convergence
    strncpy(update->old_content, old_line, sizeof(update->old_content) - 1);
    update->old_content[sizeof(update->old_content) - 1] = '\0';
    
    strncpy(update->new_content, new_line, sizeof(update->new_content) - 1);
    update->new_content[sizeof(update->new_content) - 1] = '\0';
    
    // Still calculate change lengths for operation type determination
    int old_change_len = old_end - start_col;
    int new_change_len = new_end - start_col;
    
    if (old_change_len == 0 && new_change_len > 0) {
        update->op_type = 0; // INSERT
    } else if (old_change_len > 0 && new_change_len == 0) {
        update->op_type = 1; // DELETE
    } else {
        update->op_type = 2; // REPLACE
    }
    
    update->timestamp = get_timestamp();
    strncpy(update->user_id, g_user_id, MAX_USER_ID_LEN - 1);
    update->user_id[MAX_USER_ID_LEN - 1] = '\0';
    
    return 1;
}

// Function to detect all changes in a document by comparing old and new versions
int detect_changes(Document* old_doc, Document* new_doc, Update* updates) {
    int update_count = 0;
    
    // Find maximum number of lines
    int max_lines = (old_doc->count > new_doc->count) ? old_doc->count : new_doc->count;
    
    // Compare each line
    for (int i = 0; i < max_lines && update_count < MAX_UPDATES; i++) {
        const char* old_line = (i < old_doc->count) ? old_doc->lines[i] : "";
        const char* new_line = (i < new_doc->count) ? new_doc->lines[i] : "";
        
        // If line changed, detect the changes
        if (strcmp(old_line, new_line) != 0) {
            int found = detect_line_changes(old_line, new_line, i, &updates[update_count]);
            update_count += found;
        }
    }
    
    return update_count;
}

