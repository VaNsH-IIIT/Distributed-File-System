#include "common.h"

#define STORAGEDIR ".storage"
#define UNDODIR ".storage/.undo"
#define TEMPDIR ".storage/.temp"

int ss_id = -1;
char ss_ip[INET_ADDRSTRLEN];
int ss_nm_port;
int ss_client_port;

pthread_mutex_t file_ops_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lock_mutex = PTHREAD_MUTEX_INITIALIZER;

// Sentence locks
SentenceLock sentence_locks[MAX_FILES_PER_SERVER * MAX_SENTENCES];
int lock_count = 0;

// Function declarations (add near the top after includes)
void calculate_file_stats(const char *content, int *wordcount, int *charcount, int *sentencecount);
void update_metadata_full(const char *filename, int wordcount, int charcount, int sentencecount);
void update_metadata(const char *filename, const char *update_type);
// Add near the top with other function declarations
void update_metadata_with_acl(const char *filename, int wordcount, 
                              int charcount, int sentencecount,
                              const char *owner,
                              AccessEntry *access_list, int access_count);

void handle_update_acl(int clientsock, Message *msg) {
    pthread_mutex_lock(&file_ops_mutex);
    
    // Parse ACL data
    int wordcount, charcount, sentencecount, access_count;
    AccessEntry access_list[MAX_ACCESS_LIST];
    
    char *data = msg->data;
    sscanf(data, "%d|%d|%d|%d|", &wordcount, &charcount, &sentencecount, &access_count);
    
    // Skip to ACL entries
    char *ptr = data;
    for (int i = 0; i < 4; i++) {
        ptr = strchr(ptr, '|');
        if (ptr) ptr++;
    }
    
    // Parse each ACL entry
    for (int i = 0; i < access_count && ptr; i++) {
        char username[MAX_USERNAME_LENGTH];
        int access_type;
        sscanf(ptr, "%[^:]:%d|", username, &access_type);
        strcpy(access_list[i].username, username);
        access_list[i].access_type = access_type;
        ptr = strchr(ptr, '|');
        if (ptr) ptr++;
    }
    
    // Update metadata file
    update_metadata_with_acl(msg->filename, wordcount, charcount, sentencecount,
                            msg->username, access_list, access_count);
    
    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "Metadata updated");
    pthread_mutex_unlock(&file_ops_mutex);
    send_message(clientsock, msg);
}


void ensure_directories() {
    struct stat st = {0};
    if (stat(STORAGEDIR, &st) == -1) {
        mkdir(STORAGEDIR, 0755);
        log_message("SS", "Created storage directory");
    }
    if (stat(UNDODIR, &st) == -1) {
        mkdir(UNDODIR, 0755);
        log_message("SS", "Created undo directory");
    }
    if (stat(TEMPDIR, &st) == -1) {
        mkdir(TEMPDIR, 0755);
        log_message("SS", "Created temp directory");
    }
}

void get_file_path(const char* filename, char* path) {
    snprintf(path, MAX_PATH_LENGTH, "%s/%s", STORAGEDIR, filename);
}

void get_undo_path(const char* filename, char* path) {
    snprintf(path, MAX_PATH_LENGTH, "%s/%s", UNDODIR, filename);
}

// Get temp file path
void get_temp_path(const char* filename, const char* username, char* path) {
    snprintf(path, MAX_PATH_LENGTH, "%s/%s_%s.tmp", TEMPDIR, filename, username);
}

// Check if sentence is locked
int is_sentence_locked(const char* filename, int sentence_num, const char* username) {
    pthread_mutex_lock(&lock_mutex);
    for (int i = 0; i < lock_count; i++) {
        if (strcmp(sentence_locks[i].filename, filename) == 0 &&
            sentence_locks[i].sentence_number == sentence_num) {
            int locked = (strcmp(sentence_locks[i].locked_by, username) != 0);
            pthread_mutex_unlock(&lock_mutex);
            return locked;
        }
    }
    pthread_mutex_unlock(&lock_mutex);
    return 0;
}

// Lock sentence for editing
int lock_sentence(const char* filename, int sentence_num, const char* username) {
    pthread_mutex_lock(&lock_mutex);
    
    // Check if already locked
    for (int i = 0; i < lock_count; i++) {
        if (strcmp(sentence_locks[i].filename, filename) == 0 &&
            sentence_locks[i].sentence_number == sentence_num) {
            if (strcmp(sentence_locks[i].locked_by, username) != 0) {
                pthread_mutex_unlock(&lock_mutex);
                return -1; // Locked by another user
            }
            pthread_mutex_unlock(&lock_mutex);
            return 0; // Already locked by this user
        }
    }
    
    // Add new lock
    if (lock_count < MAX_FILES_PER_SERVER * MAX_SENTENCES) {
        strcpy(sentence_locks[lock_count].filename, filename);
        sentence_locks[lock_count].sentence_number = sentence_num;
        strcpy(sentence_locks[lock_count].locked_by, username);
        sentence_locks[lock_count].lock_time = time(NULL);
        lock_count++;
    }
    
    pthread_mutex_unlock(&lock_mutex);
    return 0;
}

// Unlock sentence
void unlock_sentence(const char* filename, int sentence_num, const char* username) {
    pthread_mutex_lock(&lock_mutex);
    
    for (int i = 0; i < lock_count; i++) {
        if (strcmp(sentence_locks[i].filename, filename) == 0 &&
            sentence_locks[i].sentence_number == sentence_num &&
            strcmp(sentence_locks[i].locked_by, username) == 0) {
            // Remove lock
            for (int j = i; j < lock_count - 1; j++) {
                sentence_locks[j] = sentence_locks[j + 1];
            }
            lock_count--;
            break;
        }
    }
    
    pthread_mutex_unlock(&lock_mutex);
}

// Save file for undo
void save_for_undo(const char* filename) {
    char filepath[MAX_PATH_LENGTH];
    char undo_path[MAX_PATH_LENGTH];
    get_file_path(filename, filepath);
    get_undo_path(filename, undo_path);
    
    FILE* src = fopen(filepath, "r");
    if (!src) return;
    
    FILE* dst = fopen(undo_path, "w");
    if (!dst) {
        fclose(src);
        return;
    }
    
    char buffer[4096];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, bytes, dst);
    }
    
    fclose(src);
    fclose(dst);
}

// Calculate file statistics
// Calculate file statistics
void calculate_file_stats(const char* content, int* word_count, int* char_count, int* sentence_count) {
    *word_count = 0;
    *char_count = strlen(content);
    *sentence_count = 0;
    
    int in_word = 0;
    for (int i = 0; content[i] != '\0'; i++) {
        // Count sentences by delimiters
        if (is_sentence_delimiter(content[i])) {
            (*sentence_count)++;
        }
        
        // Count words
        if (content[i] != ' ' && content[i] != '\n' && content[i] != '\t') {
            if (!in_word) {
                (*word_count)++;
                in_word = 1;
            }
        } else {
            in_word = 0;
        }
    }
    
    // Edge case
    if (*sentence_count == 0 && *char_count > 0) {
        *sentence_count = 1;
    }
}

// Handle CREATE
void handle_create(int client_sock, Message* msg) {
    pthread_mutex_lock(&file_ops_mutex);
    
    char filepath[MAX_PATH_LENGTH];
    get_file_path(msg->filename, filepath);
    
    FILE* fp = fopen(filepath, "w");
    if (fp == NULL) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Could not create file");
        pthread_mutex_unlock(&file_ops_mutex);
        send_message(client_sock, msg);
        return;
    }
    
    fclose(fp);

    // Create metadata file with initial values
    update_metadata_full(msg->filename, 0, 0, 0);

    char log_msg[MAX_BUFFER_SIZE];
    snprintf(log_msg, sizeof(log_msg), "File created: %s", msg->filename);
    log_message("SS", log_msg);
    
    // Create metadata file
    char metapath[MAX_PATH_LENGTH];
    snprintf(metapath, sizeof(metapath), "%s.meta", filepath);
    
    FILE *metafp = fopen(metapath, "w");
    if (metafp) {
        time_t now = time(NULL);
        fprintf(metafp, "filename=%s\n", msg->filename);
        fprintf(metafp, "owner=%s\n", msg->username);
        fprintf(metafp, "created_time=%ld\n", now);
        fprintf(metafp, "last_accessed=%ld\n", now);
        fprintf(metafp, "last_modified=%ld\n", now);
        fprintf(metafp, "file_size=0\n");
        fprintf(metafp, "word_count=0\n");
        fprintf(metafp, "char_count=0\n");
        fprintf(metafp, "sentence_count=0\n");
        fclose(metafp);
    }

    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "File created successfully");
    pthread_mutex_unlock(&file_ops_mutex);
    send_message(client_sock, msg);
}

void update_metadata_with_acl(const char *filename, int wordcount, 
                              int charcount, int sentencecount,
                              const char *owner,
                              AccessEntry *access_list, int access_count) {
    char filepath[MAX_PATH_LENGTH];
    get_file_path(filename, filepath);
    
    char metapath[MAX_PATH_LENGTH + 10];
    snprintf(metapath, sizeof(metapath), "%s.meta", filepath);
    
    // Get file size
    struct stat st;
    stat(filepath, &st);
    
    // Write complete metadata
    FILE *fp = fopen(metapath, "w");
    if (!fp) return;
    
    time_t now = time(NULL);
    
    fprintf(fp, "filename=%s\n", filename);
    fprintf(fp, "owner=%s\n", owner);
    fprintf(fp, "created_time=%ld\n", now);
    fprintf(fp, "last_accessed=%ld\n", now);
    fprintf(fp, "last_modified=%ld\n", now);
    fprintf(fp, "file_size=%ld\n", st.st_size);
    fprintf(fp, "word_count=%d\n", wordcount);
    fprintf(fp, "char_count=%d\n", charcount);
    fprintf(fp, "sentence_count=%d\n", sentencecount);
    fprintf(fp, "access_count=%d\n", access_count);
    
    // Write access control list
    fprintf(fp, "access_list_start\n");
    for (int i = 0; i < access_count; i++) {
        const char *access_type = (access_list[i].access_type == ACCESS_READ) ? "READ" : "WRITE";
        fprintf(fp, "user=%s,access=%s\n", access_list[i].username, access_type);
    }
    fprintf(fp, "access_list_end\n");
    
    fclose(fp);
}


void update_metadata(const char *filename, const char *update_type) {
    char filepath[MAX_PATH_LENGTH];
    get_file_path(filename, filepath);
    
    char metapath[MAX_PATH_LENGTH];
    snprintf(metapath, sizeof(metapath), "%s.meta", filepath);
    
    // Read existing metadata
    FILE *fp = fopen(metapath, "r");
    if (!fp) return;
    
    char content[MAX_BUFFER_SIZE] = {0};
    char line[256];
    time_t now = time(NULL);
    
    while (fgets(line, sizeof(line), fp)) {
        if (strcmp(update_type, "access") == 0 && 
            strncmp(line, "last_accessed=", 14) == 0) {
            char temp[256];
            snprintf(temp, sizeof(temp), "last_accessed=%ld\n", now);
            strcat(content, temp);
        } else if (strcmp(update_type, "modify") == 0 && 
                   strncmp(line, "last_modified=", 14) == 0) {
            char temp[256];
            snprintf(temp, sizeof(temp), "last_modified=%ld\n", now);
            strcat(content, temp);
        } else {
            strcat(content, line);
        }
    }
    fclose(fp);
    
    // Write updated metadata
    fp = fopen(metapath, "w");
    if (fp) {
        fprintf(fp, "%s", content);
        fclose(fp);
    }
}

void update_metadata_full(const char *filename, int wordcount, 
                         int charcount, int sentencecount) {
    char filepath[MAX_PATH_LENGTH];
    get_file_path(filename, filepath);
    
    char metapath[MAX_PATH_LENGTH];
    snprintf(metapath, sizeof(metapath), "%s.meta", filepath);
    
    // Get file size
    struct stat st;
    stat(filepath, &st);
    
    // Read and update metadata
    FILE *fp = fopen(metapath, "r");
    if (!fp) return;
    
    char content[MAX_BUFFER_SIZE] = {0};
    char line[256];
    time_t now = time(NULL);
    
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "last_modified=", 14) == 0) {
            char temp[256];
            snprintf(temp, sizeof(temp), "last_modified=%ld\n", now);
            strcat(content, temp);
        } else if (strncmp(line, "file_size=", 10) == 0) {
            char temp[256];
            snprintf(temp, sizeof(temp), "file_size=%ld\n", st.st_size);
            strcat(content, temp);
        } else if (strncmp(line, "word_count=", 11) == 0) {
            char temp[256];
            snprintf(temp, sizeof(temp), "word_count=%d\n", wordcount);
            strcat(content, temp);
        } else if (strncmp(line, "char_count=", 11) == 0) {
            char temp[256];
            snprintf(temp, sizeof(temp), "char_count=%d\n", charcount);
            strcat(content, temp);
        } else if (strncmp(line, "sentence_count=", 15) == 0) {
            char temp[256];
            snprintf(temp, sizeof(temp), "sentence_count=%d\n", sentencecount);
            strcat(content, temp);
        } else {
            strcat(content, line);
        }
    }
    fclose(fp);
    
    // Write back
    fp = fopen(metapath, "w");
    if (fp) {
        fprintf(fp, "%s", content);
        fclose(fp);
    }
}


// Handle READ
void handle_read(int client_sock, Message* msg) {
    pthread_mutex_lock(&file_ops_mutex);
    
    char filepath[MAX_PATH_LENGTH];
    get_file_path(msg->filename, filepath);
    
    FILE* fp = fopen(filepath, "r");
    if (fp == NULL) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->data, "File not found");
        pthread_mutex_unlock(&file_ops_mutex);
        send_message(client_sock, msg);
        return;
    }
    
    char content[MAX_BUFFER_SIZE] = "";
    char line[1024];
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strlen(content) + strlen(line) < MAX_BUFFER_SIZE - 1) {
            strcat(content, line);
        }
    }
    
    fclose(fp);
    // Update access time in metadata
    update_metadata(msg->filename, "access");
    
    strcpy(msg->data, content);
    msg->error_code = ERR_SUCCESS;
    
    pthread_mutex_unlock(&file_ops_mutex);
    send_message(client_sock, msg);
}

// Handle: LOCK_SENTENCE
// Replace the handle_lock_sentence function in storage_server.c with this:

void handle_lock_sentence(int client_sock, Message* msg) {
    char filepath[MAX_PATH_LENGTH];
    get_file_path(msg->filename, filepath);
    
    // Read file content to count sentences
    FILE* fp = fopen(filepath, "r");
    char* file_content = NULL;
    size_t content_size = 0;
    
    if (fp) {
        fseek(fp, 0, SEEK_END);
        content_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        
        file_content = malloc(content_size + 1);
        if (file_content) {
            fread(file_content, 1, content_size, fp);
            file_content[content_size] = '\0';
        }
        fclose(fp);
    }
    
    if (!file_content) {
        file_content = malloc(1);
        file_content[0] = '\0';
    }
    
    // Split into sentences to count
    SentenceArray* sentences = split_into_sentences(file_content);
    free(file_content);
    
    if (!sentences) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Memory allocation failed");
        send_message(client_sock, msg);
        return;
    }
    
    // VALIDATE: Check if sentence index is in valid range
    if (msg->sentence_number < 0) {
        msg->error_code = ERR_INVALID_SENTENCE;
        strcpy(msg->data, "Sentence index cannot be negative");
        free_sentence_array(sentences);
        send_message(client_sock, msg);
        return;
    }

    // CRITICAL FIX FOR ISSUE 2:
    // Count COMPLETE sentences (those with delimiters)
    // Allow writing to the NEXT incomplete sentence only
    
    int complete_sentence_count = 0;
    int last_complete_index = -1;
    
    for (int i = 0; i < sentences->count; i++) {
        if (sentences->sentences[i].delimiter != '\0' && 
            sentences->sentences[i].word_count > 0) {
            complete_sentence_count++;
            last_complete_index = i;
        }
    }
    
    // Determine the writable range:
    // - Can edit any existing sentence [0, sentences->count - 1]
    // - Can only APPEND (write to next index) if the last sentence is complete
    
    if (msg->sentence_number < sentences->count) {
        // Editing existing sentence - always allowed
        // (Though you might want to add logic to prevent editing incomplete sentences)
    } else if (msg->sentence_number == sentences->count) {
        // Appending new sentence
        // Only allowed if file is empty OR last sentence is complete
        if (sentences->count > 0) {
            // Check if last sentence is complete
            Sentence* last_sent = &sentences->sentences[sentences->count - 1];
            if (last_sent->delimiter == '\0') {
                msg->error_code = ERR_INVALID_SENTENCE;
                snprintf(msg->data, MAX_BUFFER_SIZE, 
                        "Cannot write sentence %d: Previous sentence %d is incomplete (missing delimiter)",
                        msg->sentence_number, sentences->count - 1);
                free_sentence_array(sentences);
                send_message(client_sock, msg);
                return;
            }
        }
    } else {
        // Trying to skip sentences
        msg->error_code = ERR_INVALID_SENTENCE;
        snprintf(msg->data, MAX_BUFFER_SIZE, 
                "Sentence index out of range (file has %d sentence%s, can write up to index %d)",
                sentences->count, sentences->count == 1 ? "" : "s", sentences->count);
        free_sentence_array(sentences);
        send_message(client_sock, msg);
        return;
    }
    
    // Check if already locked
    int result = lock_sentence(msg->filename, msg->sentence_number, msg->username);
    
    if (result == -1) {
        msg->error_code = ERR_SENTENCE_LOCKED;
        strcpy(msg->data, "Sentence is locked by another user");
        free_sentence_array(sentences);
        send_message(client_sock, msg);
        return;
    }
    
    // Create temp file with existing sentence content (or empty for new sentence)
    char temppath[MAX_PATH_LENGTH];
    get_temp_path(msg->filename, msg->username, temppath);
    
    FILE* temp_fp = fopen(temppath, "w");
    if (temp_fp) {
        if (msg->sentence_number < sentences->count) {
            // Existing sentence - copy it
            Sentence* sent = &sentences->sentences[msg->sentence_number];
            for (int i = 0; i < sent->word_count; i++) {
                fprintf(temp_fp, "%s", sent->words[i]);
                if (i < sent->word_count - 1) {
                    fprintf(temp_fp, " ");
                }
            }
            if (sent->delimiter != '\0') {
                fprintf(temp_fp, "%c", sent->delimiter);
            }
        }
        // If appending new sentence, temp file starts empty
        fclose(temp_fp);
    }
    
    free_sentence_array(sentences);
    
    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "Sentence locked successfully");
    send_message(client_sock, msg);
}

void handle_write_sentence(int client_sock, Message* msg) {
    pthread_mutex_lock(&file_ops_mutex);

    // Check if sentence is locked by this user
    if (is_sentence_locked(msg->filename, msg->sentence_number, msg->username)) {
        msg->error_code = ERR_SENTENCE_LOCKED;
        strcpy(msg->data, "Sentence not locked by you");
        pthread_mutex_unlock(&file_ops_mutex);
        send_message(client_sock, msg);
        return;
    }

    char temppath[MAX_PATH_LENGTH];
    get_temp_path(msg->filename, msg->username, temppath);

    // Read current temp sentence
    FILE* temp_fp = fopen(temppath, "r");
    char* sentence = NULL;
    if (temp_fp) {
        fseek(temp_fp, 0, SEEK_END);
        size_t size = ftell(temp_fp);
        fseek(temp_fp, 0, SEEK_SET);
        sentence = malloc(size + 1);
        if (sentence) {
            fread(sentence, 1, size, temp_fp);
            sentence[size] = '\0';
        }
        fclose(temp_fp);
    }

    if (!sentence) {
        sentence = malloc(1);
        sentence[0] = '\0';
    }

    // Parse into words
    WordArray* words = parse_words(sentence);
    free(sentence);
    if (!words) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Memory allocation failed");
        pthread_mutex_unlock(&file_ops_mutex);
        send_message(client_sock, msg);
        return;
    }

    // VALIDATE: Word index must be in range [0, word_count]
    if (msg->word_index < 0) {
        msg->error_code = ERR_INVALID_WORD;
        strcpy(msg->data, "Word index cannot be negative");
        free_word_array(words);
        pthread_mutex_unlock(&file_ops_mutex);
        send_message(client_sock, msg);
        return;
    }

    if (msg->word_index > words->count) {
        msg->error_code = ERR_INVALID_WORD;
        snprintf(msg->data, MAX_BUFFER_SIZE,
                 "Word index out of range (sentence has %d word%s, max index: %d)",
                 words->count, words->count == 1 ? "" : "s", words->count);
        free_word_array(words);
        pthread_mutex_unlock(&file_ops_mutex);
        send_message(client_sock, msg);
        return;
    }

    // INSERT word at index
    if (insert_word(words, msg->word_index, msg->data) != 0) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Failed to insert word");
        free_word_array(words);
        pthread_mutex_unlock(&file_ops_mutex);
        send_message(client_sock, msg);
        return;
    }

    // Reconstruct sentence
    char* new_sentence = join_words(words);
    free_word_array(words);

    if (!new_sentence) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Failed to reconstruct sentence");
        pthread_mutex_unlock(&file_ops_mutex);
        send_message(client_sock, msg);
        return;
    }

    // ========================================================================
    // CRITICAL FIX: Check if new_sentence contains delimiters
    // If yes, we need to note this for the UNLOCK phase
    // For now, just store the modified sentence - delimiter splitting happens
    // during UNLOCK when all sentences are being reassembled
    // ========================================================================

    // Write back to temp file
    temp_fp = fopen(temppath, "w");
    if (!temp_fp) {
        free(new_sentence);
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Could not write to temp file");
        pthread_mutex_unlock(&file_ops_mutex);
        send_message(client_sock, msg);
        return;
    }

    fprintf(temp_fp, "%s", new_sentence);
    fclose(temp_fp);
    free(new_sentence);

    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "Word inserted successfully");
    pthread_mutex_unlock(&file_ops_mutex);
    send_message(client_sock, msg);
}

// ============================================================================
// CRITICAL FIX #3: handle_unlock_sentence() WITH DELIMITER RE-SPLITTING
// ============================================================================
// This is where the magic happens - after all word insertions, we:
// 1. Read the modified sentence from temp file
// 2. Re-parse it to detect new delimiters
// 3. Split into multiple sentences if delimiters found
// 4. Merge back into main file

void handle_unlock_sentence(int client_sock, Message* msg) {
    pthread_mutex_lock(&file_ops_mutex);

    char filepath[MAX_PATH_LENGTH];
    char temppath[MAX_PATH_LENGTH];
    get_file_path(msg->filename, filepath);
    get_temp_path(msg->filename, msg->username, temppath);

    // Save for undo BEFORE modifying
    save_for_undo(msg->filename);

    // Read main file
    FILE* fp = fopen(filepath, "r");
    char* file_content = NULL;
    if (fp) {
        fseek(fp, 0, SEEK_END);
        size_t size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        file_content = malloc(size + 1);
        if (file_content) {
            fread(file_content, 1, size, fp);
            file_content[size] = '\0';
        }
        fclose(fp);
    }

    if (!file_content) {
        file_content = malloc(1);
        file_content[0] = '\0';
    }

    // Split into sentences (using fixed split_into_sentences)
    SentenceArray* sentences = split_into_sentences(file_content);
    free(file_content);

    if (!sentences) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Memory allocation failed");
        pthread_mutex_unlock(&file_ops_mutex);
        send_message(client_sock, msg);
        return;
    }

    // Read modified sentence from temp
    FILE* temp_fp = fopen(temppath, "r");
    char* temp_sentence = NULL;

    if (temp_fp) {
        fseek(temp_fp, 0, SEEK_END);
        size_t size = ftell(temp_fp);
        fseek(temp_fp, 0, SEEK_SET);
        temp_sentence = malloc(size + 1);
        if (temp_sentence) {
            fread(temp_sentence, 1, size, temp_fp);
            temp_sentence[size] = '\0';
        }
        fclose(temp_fp);
        remove(temppath);
    }

    if (!temp_sentence) {
        temp_sentence = malloc(1);
        temp_sentence[0] = '\0';
    }

    // ========================================================================
    // CRITICAL FIX: RE-PARSE the modified sentence to detect new delimiters
    // ========================================================================
    SentenceArray* new_sentences = split_into_sentences(temp_sentence);
    free(temp_sentence);

    if (!new_sentences) {
        free_sentence_array(sentences);
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Failed to parse modified sentence");
        pthread_mutex_unlock(&file_ops_mutex);
        send_message(client_sock, msg);
        return;
    }

    // Now we need to replace sentence[msg->sentence_number] with new_sentences
    // This may involve:
    // 1. Replacing 1 sentence with 1 sentence (no delimiters added)
    // 2. Replacing 1 sentence with N sentences (delimiters were added)

    // Strategy: Remove old sentence, insert new sentences at same position

    // Extend sentences array if appending
    while (msg->sentence_number >= sentences->count) {
        add_sentence_with_delimiter(sentences, "", '\0');
    }

    // Free the old sentence at this index
    for (int i = 0; i < sentences->sentences[msg->sentence_number].word_count; i++) {
        free(sentences->sentences[msg->sentence_number].words[i]);
    }
    free(sentences->sentences[msg->sentence_number].words);

    // If new_sentences has exactly 1 sentence, simple replacement
    if (new_sentences->count == 1) {
    // CRITICAL FIX: Preserve original delimiter if new sentence doesn't have one
        char original_delimiter = sentences->sentences[msg->sentence_number].delimiter;
        sentences->sentences[msg->sentence_number] = new_sentences->sentences[0];
        
        // If the modified sentence lost its delimiter, restore it
        if (sentences->sentences[msg->sentence_number].delimiter == '\0' && original_delimiter != '\0') {
            sentences->sentences[msg->sentence_number].delimiter = original_delimiter;
        }
    } 
    // If multiple sentences, we need to insert additional sentences
    else if (new_sentences->count > 1) {
        // Replace first one
        sentences->sentences[msg->sentence_number] = new_sentences->sentences[0];

        // Insert the rest AFTER this position
        // Need to grow array and shift
        int insert_pos = msg->sentence_number + 1;
        int insert_count = new_sentences->count - 1;

        // Grow capacity if needed
        while (sentences->count + insert_count > sentences->capacity) {
            int new_capacity = sentences->capacity * 2;
            Sentence* new_arr = realloc(sentences->sentences, new_capacity * sizeof(Sentence));
            if (!new_arr) {
                // Error handling
                free_sentence_array(sentences);
                free_sentence_array(new_sentences);
                msg->error_code = ERR_SERVER_ERROR;
                strcpy(msg->data, "Failed to expand sentence array");
                pthread_mutex_unlock(&file_ops_mutex);
                send_message(client_sock, msg);
                return;
            }
            sentences->sentences = new_arr;
            sentences->capacity = new_capacity;
        }

        // Shift existing sentences forward
        for (int i = sentences->count - 1; i >= insert_pos; i--) {
            sentences->sentences[i + insert_count] = sentences->sentences[i];
        }

        // Insert new sentences
        for (int i = 0; i < insert_count; i++) {
            sentences->sentences[insert_pos + i] = new_sentences->sentences[i + 1];
        }

        sentences->count += insert_count;
    }

    // Free new_sentences struct (but NOT the sentence data - we transferred ownership)
    new_sentences->sentences[0].words = NULL;  // Already transferred
    for (int i = 1; i < new_sentences->count; i++) {
        new_sentences->sentences[i].words = NULL;  // Already transferred
    }
    free(new_sentences->sentences);
    free(new_sentences);

    // Join sentences (using FIXED join_sentences that preserves delimiters)
    char* updated_content = join_sentences(sentences);
    free_sentence_array(sentences);

    if (!updated_content) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Failed to join sentences");
        pthread_mutex_unlock(&file_ops_mutex);
        send_message(client_sock, msg);
        return;
    }

    // Write final content
    fp = fopen(filepath, "w");
    if (!fp) {
        free(updated_content);
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Could not write to file");
        pthread_mutex_unlock(&file_ops_mutex);
        send_message(client_sock, msg);
        return;
    }

    fprintf(fp, "%s", updated_content);
    fclose(fp);

    // **ADD THIS: Calculate file statistics from updated content**
    int wordcount = 0, charcount = 0, sentencecount = 0;
    calculate_file_stats(updated_content, &wordcount, &charcount, &sentencecount);
    
    free(updated_content);

    // Release lock
    unlock_sentence(msg->filename, msg->sentence_number, msg->username);

    // After successful write, update modification time and statistics
    update_metadata_full(msg->filename, wordcount, charcount, sentencecount);

    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "Sentence unlocked and saved");
    pthread_mutex_unlock(&file_ops_mutex);
    send_message(client_sock, msg);
}

// Handle UNDO
void handle_undo(int client_sock, Message* msg) {
    pthread_mutex_lock(&file_ops_mutex);
    
    char filepath[MAX_PATH_LENGTH];
    char undo_path[MAX_PATH_LENGTH];
    get_file_path(msg->filename, filepath);
    get_undo_path(msg->filename, undo_path);
    
    // Check if undo file exists
    FILE* undo_fp = fopen(undo_path, "r");
    if (!undo_fp) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->data, "No undo history available");
        pthread_mutex_unlock(&file_ops_mutex);
        send_message(client_sock, msg);
        return;
    }
    
    // Read undo content
    char undo_content[MAX_BUFFER_SIZE] = "";
    char line[1024];
    while (fgets(line, sizeof(line), undo_fp) != NULL) {
        strcat(undo_content, line);
    }
    fclose(undo_fp);
    
    // Write to main file
    FILE* fp = fopen(filepath, "w");
    if (!fp) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Could not restore file");
        pthread_mutex_unlock(&file_ops_mutex);
        send_message(client_sock, msg);
        return;
    }
    
    fprintf(fp, "%s", undo_content);
    fclose(fp);
    
    char log_msg[MAX_BUFFER_SIZE];
    snprintf(log_msg, sizeof(log_msg), "File restored from undo: %s", msg->filename);
    log_message("SS", log_msg);
    
    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "File restored successfully");
    pthread_mutex_unlock(&file_ops_mutex);
    send_message(client_sock, msg);
}

// Handle INFO
void handle_info(int client_sock, Message* msg) {
    pthread_mutex_lock(&file_ops_mutex);
    
    char filepath[MAX_PATH_LENGTH];
    get_file_path(msg->filename, filepath);
    
    // Get file stats
    struct stat st;
    if (stat(filepath, &st) != 0) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->data, "File not found");
        pthread_mutex_unlock(&file_ops_mutex);
        send_message(client_sock, msg);
        return;
    }
    
    // Read metadata file instead of recalculating
    char metapath[MAX_PATH_LENGTH];
    snprintf(metapath, sizeof(metapath), "%s.meta", filepath);
    
    int wordcount = 0, charcount = 0, sentencecount = 0;
    FILE *metafp = fopen(metapath, "r");
    if (metafp) {
        char line[256];
        while (fgets(line, sizeof(line), metafp)) {
            if (sscanf(line, "word_count=%d", &wordcount) == 1) continue;
            if (sscanf(line, "char_count=%d", &charcount) == 1) continue;
            if (sscanf(line, "sentence_count=%d", &sentencecount) == 1) continue;
        }
        fclose(metafp);
    }
    
    // Format info
    char info[MAX_BUFFER_SIZE];
    char atimestr[64], mtimestr[64];
    strftime(atimestr, sizeof(atimestr), "%Y-%m-%d %H:%M:%S", localtime(&st.st_atime));
    strftime(mtimestr, sizeof(mtimestr), "%Y-%m-%d %H:%M:%S", localtime(&st.st_mtime));
    
    snprintf(info, sizeof(info), 
             "File: %s\nSize: %ld bytes\nWords: %d\nCharacters: %d\nSentences: %d\n"
             "Last Accessed: %s\nLast Modified: %s",
             msg->filename, st.st_size, wordcount, charcount, sentencecount, 
             atimestr, mtimestr);
    
    strcpy(msg->data, info);
    msg->error_code = ERR_SUCCESS;
    pthread_mutex_unlock(&file_ops_mutex);
    send_message(client_sock, msg);
}

// Handle STREAM - Simple and safe version
void handle_stream(int client_sock, Message* msg) {
    pthread_mutex_lock(&file_ops_mutex);
    
    char filepath[MAX_PATH_LENGTH];
    get_file_path(msg->filename, filepath);
    
    FILE* fp = fopen(filepath, "r");
    if (!fp) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->data, "File not found");
        pthread_mutex_unlock(&file_ops_mutex);
        send_message(client_sock, msg);
        return;
    }
    
    // Read entire content safely
    char content[MAX_BUFFER_SIZE] = "";
    size_t total = 0;
    int ch;
    
    while ((ch = fgetc(fp)) != EOF && total < MAX_BUFFER_SIZE - 1) {
        content[total++] = (char)ch;
    }
    content[total] = '\0';
    
    fclose(fp);
    
    // Send content as-is (client handles word parsing)
    strcpy(msg->data, content);
    msg->error_code = ERR_SUCCESS;
    pthread_mutex_unlock(&file_ops_mutex);
    send_message(client_sock, msg);
}

// Handle DELETE
void handle_delete(int client_sock, Message* msg) {
    pthread_mutex_lock(&file_ops_mutex);
    
    char filepath[MAX_PATH_LENGTH];
    char undo_path[MAX_PATH_LENGTH];
    get_file_path(msg->filename, filepath);
    get_undo_path(msg->filename, undo_path);
    
    // Delete main file
    if (remove(filepath) != 0) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->data, "Could not delete file");
    } else {
        // Also delete undo file if exists
        remove(undo_path);
        
        char log_msg[MAX_BUFFER_SIZE];
        snprintf(log_msg, sizeof(log_msg), "File deleted: %s", msg->filename);
        log_message("SS", log_msg);
        
        msg->error_code = ERR_SUCCESS;
        strcpy(msg->data, "File deleted successfully");
    }
    
    pthread_mutex_unlock(&file_ops_mutex);
    send_message(client_sock, msg);
}

// Client handler
void* handle_client(void* arg) {
    int client_sock = *(int*)arg;
    free(arg);
    
    Message msg;
    while (1) {
        if (receive_message(client_sock, &msg) != 0) {
            break;
        }
        
        switch (msg.operation) {
            case OP_SS_CREATE:
                handle_create(client_sock, &msg);
                break;
            case OP_SS_READ:
                handle_read(client_sock, &msg);
                break;
            case OP_SS_DELETE:
                handle_delete(client_sock, &msg);
                break;
            case OP_SS_INFO:
                handle_info(client_sock, &msg);
                break;
            case OP_SS_STREAM:
                handle_stream(client_sock, &msg);
                break;
            case OP_SS_UNDO:
                handle_undo(client_sock, &msg);
                break;
            case OP_SS_LOCK_SENTENCE:
                handle_lock_sentence(client_sock, &msg);
                break;
            case OP_SS_WRITE_SENTENCE:
                handle_write_sentence(client_sock, &msg);
                break;
            case OP_SS_UNLOCK_SENTENCE:
                handle_unlock_sentence(client_sock, &msg);
                break;
            case OP_SS_UPDATE_ACL:
                handle_update_acl(client_sock, &msg);
                break;
            default:
                msg.error_code = ERR_INVALID_OPERATION;
                strcpy(msg.data, "Invalid operation");
                send_message(client_sock, &msg);
                break;
        }
    }
    
    close(client_sock);
    return NULL;
}

// Register with Name Server
void register_with_nm() {
    log_message("SS", "Registering with Name Server...");
    
    int nm_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (nm_sock < 0) {
        error_exit("Socket creation failed");
    }
    
    struct sockaddr_in nm_addr;
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(NM_PORT);
    inet_pton(AF_INET, "127.0.0.1", &nm_addr.sin_addr);
    
    if (connect(nm_sock, (struct sockaddr*)&nm_addr, sizeof(nm_addr)) < 0) {
        error_exit("Could not connect to Name Server");
    }
    
    Message msg = {0};
    msg.operation = OP_REGISTER_SS;
    snprintf(msg.data, sizeof(msg.data), "%s %d %d", ss_ip, ss_nm_port, ss_client_port);
    
    send_message(nm_sock, &msg);
    receive_message(nm_sock, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        ss_id = msg.ss_index;
        log_message("SS", "Successfully registered with Name Server");
    } else {
        error_exit("Registration failed");
    }
    
    close(nm_sock);
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <ip> <nm_port> <client_port>\n", argv[0]);
        fprintf(stderr, "Example: %s 127.0.0.1 9000 10000\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    strcpy(ss_ip, argv[1]);
    ss_nm_port = atoi(argv[2]);
    ss_client_port = atoi(argv[3]);
    
    char start_msg[MAX_BUFFER_SIZE];
    snprintf(start_msg, sizeof(start_msg), 
             "Storage Server starting (IP: %s, NM Port: %d, Client Port: %d)", 
             ss_ip, ss_nm_port, ss_client_port);
    log_message("SS", start_msg);
    
    ensure_directories();
    register_with_nm();
    
    // Create socket for NM connections
    int nm_server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (nm_server_sock < 0) {
        error_exit("Socket creation failed");
    }
    
    int opt = 1;
    setsockopt(nm_server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in nm_server_addr;
    nm_server_addr.sin_family = AF_INET;
    nm_server_addr.sin_addr.s_addr = INADDR_ANY;
    nm_server_addr.sin_port = htons(ss_nm_port);
    
    if (bind(nm_server_sock, (struct sockaddr*)&nm_server_addr, sizeof(nm_server_addr)) < 0) {
        error_exit("Bind failed for NM port");
    }
    
    if (listen(nm_server_sock, 10) < 0) {
        error_exit("Listen failed");
    }
    
    // Create socket for client connections
    int client_server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (client_server_sock < 0) {
        error_exit("Socket creation failed");
    }
    
    setsockopt(client_server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in client_server_addr;
    client_server_addr.sin_family = AF_INET;
    client_server_addr.sin_addr.s_addr = INADDR_ANY;
    client_server_addr.sin_port = htons(ss_client_port);
    
    if (bind(client_server_sock, (struct sockaddr*)&client_server_addr, sizeof(client_server_addr)) < 0) {
        error_exit("Bind failed for client port");
    }
    
    if (listen(client_server_sock, 10) < 0) {
        error_exit("Listen failed");
    }
    
    log_message("SS", "Storage Server ready - All features enabled");
    
    // Main server loop
    while (1) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(nm_server_sock, &read_fds);
        FD_SET(client_server_sock, &read_fds);
        
        int max_fd = (nm_server_sock > client_server_sock) ? nm_server_sock : client_server_sock;
        
        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            continue;
        }
        
        if (FD_ISSET(nm_server_sock, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int* client_sock = malloc(sizeof(int));
            *client_sock = accept(nm_server_sock, (struct sockaddr*)&client_addr, &addr_len);
            
            if (*client_sock >= 0) {
                pthread_t thread;
                pthread_create(&thread, NULL, handle_client, client_sock);
                pthread_detach(thread);
            } else {
                free(client_sock);
            }
        }
        
        if (FD_ISSET(client_server_sock, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int* client_sock = malloc(sizeof(int));
            *client_sock = accept(client_server_sock, (struct sockaddr*)&client_addr, &addr_len);
            
            if (*client_sock >= 0) {
                pthread_t thread;
                pthread_create(&thread, NULL, handle_client, client_sock);
                pthread_detach(thread);
            } else {
                free(client_sock);
            }
        }
    }
    
    close(nm_server_sock);
    close(client_server_sock);
    return 0;
}
