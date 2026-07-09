#include "common.h"

void error_exit(const char* msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

void get_timestamp(char* buffer, size_t size) {
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

void log_message(const char* component, const char* message) {
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));
    printf("[%s][%s] %s\n", timestamp, component, message);
    fflush(stdout);
}

int send_message(int sock, Message* msg) {
    size_t total_sent = 0;
    size_t bytes_left = sizeof(Message);
    char* ptr = (char*)msg;
    while (total_sent < sizeof(Message)) {
        ssize_t sent = send(sock, ptr + total_sent, bytes_left, 0);
        if (sent <= 0) return -1;
        total_sent += (size_t)sent;
        bytes_left -= (size_t)sent;
    }
    return 0;
}

int receive_message(int sock, Message* msg) {
    size_t total_received = 0;
    size_t bytes_left = sizeof(Message);
    char* ptr = (char*)msg;
    while (total_received < sizeof(Message)) {
        ssize_t received = recv(sock, ptr + total_received, bytes_left, 0);
        if (received <= 0) return -1;
        total_received += (size_t)received;
        bytes_left -= (size_t)received;
    }
    return 0;
}

const char* get_error_string(int error_code) {
    switch (error_code) {
        case ERR_SUCCESS: return "Success";
        case ERR_FILE_NOT_FOUND: return "File not found";
        case ERR_FILE_EXISTS: return "File already exists";
        case ERR_INVALID_OPERATION: return "Invalid operation";
        case ERR_SERVER_ERROR: return "Server error";
        case ERR_NO_SS_AVAILABLE: return "No storage server available";
        case ERR_ACCESS_DENIED: return "Access denied";
        case ERR_SENTENCE_LOCKED: return "Sentence is locked by another user";
        case ERR_INVALID_SENTENCE: return "Invalid sentence number out of range";
        case ERR_INVALID_WORD: return "Invalid word index";
        case ERR_USER_NOT_FOUND: return "User not found";
        default: return "Unknown error";
    }
}

int is_sentence_delimiter(char c) {
    return (c == '.' || c == '!' || c == '?');
}

// ============================================================================
// HASH TABLE FUNCTIONS FOR O(1) FILE LOOKUP
// ============================================================================

// djb2 hash function
unsigned int hash_filename(const char* filename) {
    unsigned int hash = 5381;
    int c;
    while ((c = *filename++)) {
        hash = ((hash << 5) + hash) + c;  // hash * 33 + c
    }
    return hash % FILE_HASH_TABLE_SIZE;
}

void init_file_hash_table(FileHashEntry** table) {
    for (int i = 0; i < FILE_HASH_TABLE_SIZE; i++) {
        table[i] = NULL;
    }
}

int hash_table_insert(FileHashEntry** table, const char* filename, int file_index) {
    unsigned int index = hash_filename(filename);
    FileHashEntry* new_entry = malloc(sizeof(FileHashEntry));
    if (!new_entry) return -1;

    strncpy(new_entry->filename, filename, MAX_FILENAME_LENGTH - 1);
    new_entry->filename[MAX_FILENAME_LENGTH - 1] = '\0';
    new_entry->file_index = file_index;
    new_entry->next = table[index];
    table[index] = new_entry;
    return 0;
}

int hash_table_lookup(FileHashEntry** table, const char* filename) {
    unsigned int index = hash_filename(filename);
    FileHashEntry* entry = table[index];

    while (entry != NULL) {
        if (strcmp(entry->filename, filename) == 0) {
            return entry->file_index;
        }
        entry = entry->next;
    }
    return -1;  // Not found
}

int hash_table_remove(FileHashEntry** table, const char* filename) {
    unsigned int index = hash_filename(filename);
    FileHashEntry* entry = table[index];
    FileHashEntry* prev = NULL;

    while (entry != NULL) {
        if (strcmp(entry->filename, filename) == 0) {
            if (prev == NULL) {
                table[index] = entry->next;
            } else {
                prev->next = entry->next;
            }
            free(entry);
            return 0;
        }
        prev = entry;
        entry = entry->next;
    }
    return -1;  // Not found
}

// ============================================================================
// SENTENCE ARRAY FUNCTIONS WITH DELIMITER SUPPORT
// ============================================================================

SentenceArray* create_sentence_array() {
    SentenceArray* arr = malloc(sizeof(SentenceArray));
    if (!arr) return NULL;

    arr->sentences = malloc(INITIAL_SENTENCES_CAPACITY * sizeof(Sentence));
    if (!arr->sentences) {
        free(arr);
        return NULL;
    }

    arr->count = 0;
    arr->capacity = INITIAL_SENTENCES_CAPACITY;
    return arr;
}

void free_sentence_array(SentenceArray* arr) {
    if (!arr) return;

    for (int i = 0; i < arr->count; i++) {
        for (int j = 0; j < arr->sentences[i].word_count; j++) {
            free(arr->sentences[i].words[j]);
        }
        free(arr->sentences[i].words);
    }
    free(arr->sentences);
    free(arr);
}

// CRITICAL FIX: Add sentence with delimiter preservation
int add_sentence_with_delimiter(SentenceArray* arr, const char* sentence, char delimiter) {
    if (arr->count >= arr->capacity) {
        int new_capacity = arr->capacity * 2;
        Sentence* new_sentences = realloc(arr->sentences, new_capacity * sizeof(Sentence));
        if (!new_sentences) return -1;
        arr->sentences = new_sentences;
        arr->capacity = new_capacity;
    }

    // Parse words from sentence
    WordArray* words = parse_words(sentence);
    if (!words) return -1;

    // Store words and delimiter
    arr->sentences[arr->count].words = words->words;
    arr->sentences[arr->count].word_count = words->count;
    arr->sentences[arr->count].delimiter = delimiter;

    // Free WordArray struct but NOT the words (we transferred ownership)
    words->words = NULL;
    free_word_array(words);

    arr->count++;
    return 0;
}

// ============================================================================
// WORD ARRAY FUNCTIONS
// ============================================================================

WordArray* create_word_array() {
    WordArray* arr = malloc(sizeof(WordArray));
    if (!arr) return NULL;

    arr->words = malloc(INITIAL_WORDS_CAPACITY * sizeof(char*));
    if (!arr->words) {
        free(arr);
        return NULL;
    }

    arr->count = 0;
    arr->capacity = INITIAL_WORDS_CAPACITY;
    return arr;
}

void free_word_array(WordArray* arr) {
    if (!arr) return;

    if (arr->words) {
        for (int i = 0; i < arr->count; i++) {
            free(arr->words[i]);
        }
        free(arr->words);
    }
    free(arr);
}

int insert_word(WordArray* arr, int index, const char* word) {
    if (!arr || !word) return -1;

    // Grow capacity if needed
    if (arr->count + 1 >= arr->capacity) {
        int new_capacity = arr->capacity * 2;
        if (new_capacity < arr->count + 2) {
            new_capacity = arr->count + 2;
        }

        char** new_words = realloc(arr->words, new_capacity * sizeof(char*));
        if (!new_words) return -1;
        arr->words = new_words;
        arr->capacity = new_capacity;
    }

    // If index is beyond count, fill with empty strings
    for (int i = arr->count; i < index; i++) {
        arr->words[i] = malloc(1);
        if (!arr->words[i]) return -1;
        arr->words[i][0] = '\0';
        arr->count++;
    }

    // Shift existing words forward if inserting in middle
    if (index < arr->count) {
        for (int i = arr->count; i > index; i--) {
            arr->words[i] = arr->words[i - 1];
        }
    }

    // Allocate and insert new word
    arr->words[index] = malloc(strlen(word) + 1);
    if (!arr->words[index]) return -1;
    strcpy(arr->words[index], word);

    // Increment count if we actually added a new word
    if (index >= arr->count) {
        arr->count = index + 1;
    } else {
        arr->count++;
    }

    return 0;
}

// ============================================================================
// CRITICAL FIX: SENTENCE PARSING WITH DELIMITER SPLITTING
// ============================================================================

SentenceArray* split_into_sentences(const char* content) {
    SentenceArray* arr = create_sentence_array();
    if (!arr) return NULL;

    char* buffer = malloc(strlen(content) + 1);
    if (!buffer) {
        free_sentence_array(arr);
        return NULL;
    }

    int buf_idx = 0;
    for (int i = 0; content[i] != '\0'; i++) {
        buffer[buf_idx++] = content[i];

        // CRITICAL FIX: Split on delimiter immediately
        if (is_sentence_delimiter(content[i])) {
            buffer[buf_idx] = '\0';

            // Trim leading spaces
            int start = 0;
            while (buffer[start] == ' ' || buffer[start] == '\n' || buffer[start] == '\t') start++;

            if (strlen(buffer + start) > 0) {
                // Add sentence WITH its delimiter
                char delimiter = buffer[buf_idx - 1];  // Save delimiter
                buffer[buf_idx - 1] = '\0';  // Remove delimiter from text temporarily

                if (add_sentence_with_delimiter(arr, buffer + start, delimiter) != 0) {
                    free(buffer);
                    free_sentence_array(arr);
                    return NULL;
                }
            }
            buf_idx = 0;
        }
    }

    // Handle remaining content (sentence without delimiter at end)
    if (buf_idx > 0) {
        buffer[buf_idx] = '\0';
        int start = 0;
        while (buffer[start] == ' ' || buffer[start] == '\n' || buffer[start] == '\t') start++;

        if (strlen(buffer + start) > 0) {
            add_sentence_with_delimiter(arr, buffer + start, '\0');
        }
    }

    free(buffer);
    return arr;
}

// CRITICAL FIX: Join sentences WITH delimiter preservation
char* join_sentences(SentenceArray* arr) {
    size_t total_len = 0;

    // Calculate total length including delimiters
    for (int i = 0; i < arr->count; i++) {
        for (int j = 0; j < arr->sentences[i].word_count; j++) {
            total_len += strlen(arr->sentences[i].words[j]);
            if (j < arr->sentences[i].word_count - 1) total_len += 1;  // Space between words
        }

        // Add delimiter if present
        if (arr->sentences[i].delimiter != '\0') {
            total_len += 1;  // The delimiter itself
        }

        // Add space before next sentence
        if (i < arr->count - 1) total_len += 1;
    }

    total_len += 1;  // Null terminator

    char* output = malloc(total_len);
    if (!output) return NULL;
    output[0] = '\0';

    for (int i = 0; i < arr->count; i++) {
        // Add words
        for (int j = 0; j < arr->sentences[i].word_count; j++) {
            if (j > 0) strcat(output, " ");
            strcat(output, arr->sentences[i].words[j]);
        }

        // CRITICAL FIX: Append delimiter immediately after last word (NO SPACE)
        if (arr->sentences[i].delimiter != '\0') {
            char delim_str[2] = {arr->sentences[i].delimiter, '\0'};
            strcat(output, delim_str);
        }

        // Add space before next sentence
        if (i < arr->count - 1) {
            strcat(output, " ");
        }
    }

    return output;
}

// ============================================================================
// WORD PARSING FUNCTIONS
// ============================================================================

WordArray* parse_words(const char* sentence) {
    WordArray* arr = create_word_array();
    if (!arr) return NULL;
    
    char* buffer = malloc(strlen(sentence) + 1);
    if (!buffer) {
        free_word_array(arr);
        return NULL;
    }
    
    int buf_idx = 0;
    for (int i = 0; sentence[i] != '\0'; i++) {
        // CRITICAL FIX: Skip delimiters - they're stored separately in Sentence struct
        if (is_sentence_delimiter(sentence[i])) {
            continue; // Don't include delimiter as part of word
        }
        
        if (sentence[i] != ' ' && sentence[i] != '\n' && sentence[i] != '\t') {
            buffer[buf_idx++] = sentence[i];
        } else if (buf_idx > 0) {
            buffer[buf_idx] = '\0';
            if (insert_word(arr, arr->count, buffer) != 0) {
                free(buffer);
                free_word_array(arr);
                return NULL;
            }
            buf_idx = 0;
        }
    }
    
    // Handle last word
    if (buf_idx > 0) {
        buffer[buf_idx] = '\0';
        insert_word(arr, arr->count, buffer);
    }
    
    free(buffer);
    return arr;
}

char* join_words(WordArray* arr) {
    size_t total_len = 0;
    for (int i = 0; i < arr->count; i++) {
        total_len += strlen(arr->words[i]);
        if (i < arr->count - 1) total_len += 1;
    }

    total_len += 1;
    char* output = malloc(total_len);
    if (!output) return NULL;
    output[0] = '\0';

    for (int i = 0; i < arr->count; i++) {
        if (strlen(arr->words[i]) > 0) {
            if (i > 0) strcat(output, " ");
            strcat(output, arr->words[i]);
        }
    }

    return output;
}