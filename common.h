#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>

// Configuration constants
#define MAX_BUFFER_SIZE 16384
#define MAX_PATH_LENGTH 512
#define MAX_FILENAME_LENGTH 256
#define MAX_USERNAME_LENGTH 64
#define MAX_CLIENTS 100
#define MAX_STORAGE_SERVERS 10
#define MAX_FILES_PER_SERVER 1000
#define MAX_ACCESS_LIST 50
#define MAX_SENTENCES 1000
#define MAX_WORDS_PER_SENTENCE 500

// Network ports
#define NM_PORT 8080
#define DEFAULT_SS_NM_PORT 9000
#define DEFAULT_SS_CLIENT_PORT 10000

// View flags
#define FLAG_ALL 1
#define FLAG_DETAILS 2

// Operation codes - Client to NM
#define OP_REGISTER_SS 1
#define OP_REGISTER_CLIENT 2
#define OP_VIEW 3
#define OP_READ 4
#define OP_CREATE 5
#define OP_WRITE 6
#define OP_DELETE 7
#define OP_INFO 8
#define OP_STREAM 9
#define OP_LIST_USERS 10
#define OP_ADD_ACCESS 11
#define OP_REM_ACCESS 12
#define OP_EXEC 13
#define OP_UNDO 14
#define OP_GET_SS_INFO 20

// Storage Server operations
#define OP_SS_CREATE 30
#define OP_SS_READ 31
#define OP_SS_WRITE 32
#define OP_SS_DELETE 33
#define OP_SS_INFO 34
#define OP_SS_STREAM 35
#define OP_SS_UNDO 36
#define OP_SS_LOCK_SENTENCE 37
#define OP_SS_UNLOCK_SENTENCE 38
#define OP_SS_WRITE_SENTENCE 39
#define OP_SS_UPDATE_ACL 40

// Error codes
#define ERR_SUCCESS 0
#define ERR_FILE_NOT_FOUND 1
#define ERR_FILE_EXISTS 2
#define ERR_INVALID_OPERATION 3
#define ERR_SERVER_ERROR 4
#define ERR_NO_SS_AVAILABLE 5
#define ERR_ACCESS_DENIED 6
#define ERR_SENTENCE_LOCKED 7
#define ERR_INVALID_SENTENCE 8
#define ERR_INVALID_WORD 9
#define ERR_USER_NOT_FOUND 10

// Access permissions
#define ACCESS_NONE 0
#define ACCESS_READ 1
#define ACCESS_WRITE 2

#define INITIAL_SENTENCES_CAPACITY 50
#define INITIAL_WORDS_CAPACITY 100
#define TEMP_DIR ".storage/.temp"

// Hash table size for efficient search (Prime number)
#define FILE_HASH_TABLE_SIZE 10007

// Message structure
typedef struct {
    int operation;
    int error_code;
    char username[MAX_USERNAME_LENGTH];
    char filename[MAX_FILENAME_LENGTH];
    char data[MAX_BUFFER_SIZE];
    char ss_ip[INET_ADDRSTRLEN];
    int ss_port;
    int ss_index;
    int sentence_number;
    int word_index;
    int access_type;
    char target_user[MAX_USERNAME_LENGTH];
    int flags;
} Message;

// Storage Server Info
typedef struct {
    int id;
    char ip[INET_ADDRSTRLEN];
    int nm_port;
    int client_port;
    int is_active;
    int file_count;
} StorageServerInfo;

// Client Info
typedef struct {
    char username[MAX_USERNAME_LENGTH];
    char ip[INET_ADDRSTRLEN];
    int is_active;
} ClientInfo;

// Access control entry
typedef struct {
    char username[MAX_USERNAME_LENGTH];
    int access_type;
} AccessEntry;

// File Metadata
typedef struct {
    char filename[MAX_FILENAME_LENGTH];
    char owner[MAX_USERNAME_LENGTH];
    int ss_id;
    time_t created_time;
    time_t last_accessed;
    time_t last_modified;
    size_t file_size;
    int word_count;
    int char_count;
    int sentence_count;
    AccessEntry access_list[MAX_ACCESS_LIST];
    int access_count;
} FileMetadata;

// Sentence lock info
typedef struct {
    char filename[MAX_FILENAME_LENGTH];
    int sentence_number;
    char locked_by[MAX_USERNAME_LENGTH];
    time_t lock_time;
} SentenceLock;

// CRITICAL FIX: Enhanced sentence structure with delimiter field
typedef struct {
    char** words;
    int word_count;
    char delimiter;  // NEW: Stores '.', '!', '?', or '\0'
} Sentence;

// Dynamic sentence array
typedef struct {
    Sentence* sentences;  // CHANGED: Now array of Sentence structs (not char**)
    int count;
    int capacity;
} SentenceArray;

// Dynamic word array
typedef struct {
    char** words;
    int count;
    int capacity;
} WordArray;

// Hash table for O(1) file lookup
typedef struct FileHashEntry {
    char filename[MAX_FILENAME_LENGTH];
    int file_index;
    struct FileHashEntry* next;
} FileHashEntry;

// Function prototypes
void error_exit(const char* msg);
void log_message(const char* component, const char* message);
int send_message(int sock, Message* msg);
int receive_message(int sock, Message* msg);
void get_timestamp(char* buffer, size_t size);
const char* get_error_string(int error_code);

// Dynamic array functions
int is_sentence_delimiter(char c);
SentenceArray* create_sentence_array();
void free_sentence_array(SentenceArray* arr);
int add_sentence_with_delimiter(SentenceArray* arr, const char* sentence, char delimiter);
WordArray* create_word_array();
void free_word_array(WordArray* arr);
int insert_word(WordArray* arr, int index, const char* word);
SentenceArray* split_into_sentences(const char* content);
char* join_sentences(SentenceArray* arr);
WordArray* parse_words(const char* sentence);
char* join_words(WordArray* arr);

// Hash table functions for Name Server
unsigned int hash_filename(const char* filename);
void init_file_hash_table(FileHashEntry** table);
int hash_table_insert(FileHashEntry** table, const char* filename, int file_index);
int hash_table_lookup(FileHashEntry** table, const char* filename);
int hash_table_remove(FileHashEntry** table, const char* filename);

#endif