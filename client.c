#include "common.h"
#include <ctype.h>

char username[MAX_USERNAME_LENGTH];
int nm_sock = -1;

// Connect to Name Server
void connect_to_nm() {
    nm_sock = socket(AF_INET, SOCK_STREAM, 0);
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
    msg.operation = OP_REGISTER_CLIENT;
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    receive_message(nm_sock, &msg);
    
    if (msg.error_code != ERR_SUCCESS) {
        printf("Registration failed: %s\n", msg.data);
        exit(EXIT_FAILURE);
    }
    
    printf("Welcome, %s!\n", username);
}

// Command: VIEW [flags]
void cmd_view(char* flags) {
    Message msg = {0};
    msg.operation = OP_VIEW;
    strcpy(msg.username, username);
    msg.flags = 0;
    
    if (flags) {
        if (strstr(flags, "a")) msg.flags |= FLAG_ALL;
        if (strstr(flags, "l")) msg.flags |= FLAG_DETAILS;
    }
    
    send_message(nm_sock, &msg);
    receive_message(nm_sock, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("%s", msg.data);
    } else {
        printf("ERROR: %s\n", get_error_string(msg.error_code));
    }
}

// Command: READ <filename>
void cmd_read(char* filename) {
    Message msg = {0};
    msg.operation = OP_READ;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    // Get SS info from NM
    send_message(nm_sock, &msg);
    receive_message(nm_sock, &msg);
    
    if (msg.error_code != ERR_SUCCESS) {
        printf("ERROR: %s\n", get_error_string(msg.error_code));
        return;
    }
    
    // Connect to SS
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(msg.ss_port);
    inet_pton(AF_INET, msg.ss_ip, &ss_addr.sin_addr);
    
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        printf("ERROR: Could not connect to storage server\n");
        return;
    }
    
    Message ss_msg = {0};
    ss_msg.operation = OP_SS_READ;
    strcpy(ss_msg.filename, filename);
    
    send_message(ss_sock, &ss_msg);
    receive_message(ss_sock, &ss_msg);
    
    if (ss_msg.error_code == ERR_SUCCESS) {
        printf("\n=== File Content: %s ===\n%s\n", filename, ss_msg.data);
    } else {
        printf("ERROR: %s\n", get_error_string(ss_msg.error_code));
    }
    
    close(ss_sock);
}

// Command: CREATE <filename>
void cmd_create(char* filename) {
    Message msg = {0};
    msg.operation = OP_CREATE;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    send_message(nm_sock, &msg);
    receive_message(nm_sock, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("✓ File '%s' created successfully!\n", filename);
        printf("  Location: ./storage/%s\n", filename);
    } else {
        printf("ERROR: %s\n", get_error_string(msg.error_code));
    }
}

// Command: WRITE <filename> <sentence_number>
void cmd_write(char* filename, int sentence_num) {
    // Get SS info from NM
    Message msg = {0};
    msg.operation = OP_GET_SS_INFO;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    send_message(nm_sock, &msg);
    receive_message(nm_sock, &msg);
    
    if (msg.error_code != ERR_SUCCESS) {
        printf("ERROR: %s\n", get_error_string(msg.error_code));
        return;
    }
    
    // Connect to SS
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(msg.ss_port);
    inet_pton(AF_INET, msg.ss_ip, &ss_addr.sin_addr);
    
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        printf("ERROR: Could not connect to storage server\n");
        return;
    }
    
    // Lock sentence
    Message ss_msg = {0};
    ss_msg.operation = OP_SS_LOCK_SENTENCE;
    strcpy(ss_msg.filename, filename);
    strcpy(ss_msg.username, username);
    ss_msg.sentence_number = sentence_num;
    
    send_message(ss_sock, &ss_msg);
    receive_message(ss_sock, &ss_msg);
    
    if (ss_msg.error_code != ERR_SUCCESS) {
        printf("ERROR: %s\n", get_error_string(ss_msg.error_code));
        close(ss_sock);
        return;
    }
    
    printf("Sentence %d locked. Enter word modifications (word_index content):\n", sentence_num);
    printf("Type 'ETIRW' to finish and unlock.\n\n");
    
    char input[512];
    while (1) {
        printf("WRITE> ");
        if (fgets(input, sizeof(input), stdin) == NULL) break;
        input[strcspn(input, "\n")] = 0;
        
        if (strcmp(input, "ETIRW") == 0) {
            // Unlock sentence
            ss_msg.operation = OP_SS_UNLOCK_SENTENCE;
            ss_msg.sentence_number = sentence_num;
            strcpy(ss_msg.username, username);
            
            send_message(ss_sock, &ss_msg);
            receive_message(ss_sock, &ss_msg);
            
            printf("✓ Sentence unlocked. Changes saved.\n");
            break;
        }
        
        // Parse: word_index content
        int word_idx;
        char content[MAX_BUFFER_SIZE];
        if (sscanf(input, "%d %[^\n]", &word_idx, content) == 2) {
            ss_msg.operation = OP_SS_WRITE_SENTENCE;
            strcpy(ss_msg.filename, filename);
            strcpy(ss_msg.username, username);
            ss_msg.sentence_number = sentence_num;
            ss_msg.word_index = word_idx;
            strcpy(ss_msg.data, content);
            
            send_message(ss_sock, &ss_msg);
            receive_message(ss_sock, &ss_msg);
            
            if (ss_msg.error_code == ERR_SUCCESS) {
                printf("  ✓ Word %d updated\n", word_idx);
            } else {
                printf("  ERROR: %s\n", get_error_string(ss_msg.error_code));
            }
        } else {
            printf("  Invalid format. Use: <word_index> <content>\n");
        }
    }
    
    close(ss_sock);
}

// Command: DELETE <filename>
void cmd_delete(char* filename) {
    Message msg = {0};
    msg.operation = OP_DELETE;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    send_message(nm_sock, &msg);
    receive_message(nm_sock, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("✓ File '%s' deleted successfully!\n", filename);
    } else {
        printf("ERROR: %s\n", get_error_string(msg.error_code));
    }
}

// Command: INFO <filename>
void cmd_info(char* filename) {
    Message msg = {0};
    msg.operation = OP_INFO;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    // Get SS info from NM
    send_message(nm_sock, &msg);
    receive_message(nm_sock, &msg);
    
    if (msg.error_code != ERR_SUCCESS) {
        printf("ERROR: %s\n", get_error_string(msg.error_code));
        return;
    }
    
    // Connect to SS
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(msg.ss_port);
    inet_pton(AF_INET, msg.ss_ip, &ss_addr.sin_addr);
    
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        printf("ERROR: Could not connect to storage server\n");
        return;
    }
    
    Message ss_msg = {0};
    ss_msg.operation = OP_SS_INFO;
    strcpy(ss_msg.filename, filename);
    
    send_message(ss_sock, &ss_msg);
    receive_message(ss_sock, &ss_msg);
    
    if (ss_msg.error_code == ERR_SUCCESS) {
        printf("\n=== File Information ===\n%s\n", ss_msg.data);
    } else {
        printf("ERROR: %s\n", get_error_string(ss_msg.error_code));
    }
    
    close(ss_sock);
}

// Command: STREAM <filename>
// Replace the cmd_stream function in client.c with this fixed version:

void cmd_stream(char* filename) {
    Message msg = {0};
    msg.operation = OP_STREAM;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    // Get SS info from NM
    send_message(nm_sock, &msg);
    receive_message(nm_sock, &msg);
    
    if (msg.error_code != ERR_SUCCESS) {
        printf("ERROR: %s\n", get_error_string(msg.error_code));
        return;
    }
    
    // Connect directly to SS
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(msg.ss_port);
    inet_pton(AF_INET, msg.ss_ip, &ss_addr.sin_addr);
    
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        printf("ERROR: Could not connect to storage server\n");
        return;
    }
    
    Message ss_msg = {0};
    ss_msg.operation = OP_SS_STREAM;
    strcpy(ss_msg.filename, filename);
    
    send_message(ss_sock, &ss_msg);
    receive_message(ss_sock, &ss_msg);
    
    if (ss_msg.error_code != ERR_SUCCESS) {
        printf("ERROR: %s\n", get_error_string(ss_msg.error_code));
        close(ss_sock);
        return;
    }
    
    // Parse content into words and stream with delay
    printf("\n=== Streaming: %s ===\n\n", filename);
    
    // FIXED: Use dynamic allocation instead of fixed array
    char** words = NULL;
    int word_count = 0;
    int word_capacity = 100;
    
    words = malloc(word_capacity * sizeof(char*));
    if (!words) {
        printf("ERROR: Memory allocation failed\n");
        close(ss_sock);
        return;
    }
    
    char* content = ss_msg.data;
    char word_buffer[256];
    int word_idx = 0;
    int in_word = 0;
    
    for (int i = 0; content[i] != '\0'; i++) {
        if (content[i] != ' ' && content[i] != '\n' && content[i] != '\t') {
            if (!in_word) {
                word_idx = 0;
                in_word = 1;
            }
            if (word_idx < 255) {  // Safety check
                word_buffer[word_idx++] = content[i];
            }
        } else if (in_word) {
            word_buffer[word_idx] = '\0';
            
            // Grow array if needed
            if (word_count >= word_capacity) {
                word_capacity *= 2;
                char** new_words = realloc(words, word_capacity * sizeof(char*));
                if (!new_words) {
                    // Cleanup and exit
                    for (int j = 0; j < word_count; j++) {
                        free(words[j]);
                    }
                    free(words);
                    printf("ERROR: Memory allocation failed\n");
                    close(ss_sock);
                    return;
                }
                words = new_words;
            }
            
            // Store word
            words[word_count] = malloc(strlen(word_buffer) + 1);
            if (words[word_count]) {
                strcpy(words[word_count], word_buffer);
                word_count++;
            }
            
            in_word = 0;
        }
    }
    
    // Handle last word
    if (in_word) {
        word_buffer[word_idx] = '\0';
        if (word_count < word_capacity) {
            words[word_count] = malloc(strlen(word_buffer) + 1);
            if (words[word_count]) {
                strcpy(words[word_count], word_buffer);
                word_count++;
            }
        }
    }
    
    // Display words with 0.1 second delay
    for (int i = 0; i < word_count; i++) {
        printf("%s ", words[i]);
        fflush(stdout);
        usleep(100000); // 0.1 seconds = 100,000 microseconds
        free(words[i]);  // Free as we go
    }
    printf("\n\n=== Stream Complete ===\n");
    
    free(words);
    close(ss_sock);
}

// Command: LIST
void cmd_list() {
    Message msg = {0};
    msg.operation = OP_LIST_USERS;
    strcpy(msg.username, username);
    
    send_message(nm_sock, &msg);
    receive_message(nm_sock, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("%s", msg.data);
    } else {
        printf("ERROR: %s\n", get_error_string(msg.error_code));
    }
}

// Command: ADDACCESS -R/-W <filename> <username>
void cmd_addaccess(char access_type, char* filename, char* target_user) {
    Message msg = {0};
    msg.operation = OP_ADD_ACCESS;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    strcpy(msg.target_user, target_user);
    msg.access_type = (access_type == 'R') ? ACCESS_READ : ACCESS_WRITE;
    
    send_message(nm_sock, &msg);
    receive_message(nm_sock, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        const char* access_str = (access_type == 'R') ? "READ" : "WRITE";
        printf("✓ %s access granted to '%s' for file '%s'\n", 
               access_str, target_user, filename);
    } else {
        printf("ERROR: %s\n", get_error_string(msg.error_code));
    }
}

// Command: REMACCESS <filename> <username>
void cmd_remaccess(char* filename, char* target_user) {
    Message msg = {0};
    msg.operation = OP_REM_ACCESS;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    strcpy(msg.target_user, target_user);
    
    send_message(nm_sock, &msg);
    receive_message(nm_sock, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("✓ Access removed from '%s' for file '%s'\n", target_user, filename);
    } else {
        printf("ERROR: %s\n", get_error_string(msg.error_code));
    }
}

// Command: EXEC <filename>
void cmd_exec(char* filename) {
    Message msg = {0};
    msg.operation = OP_EXEC;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    send_message(nm_sock, &msg);
    receive_message(nm_sock, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("\n=== Execution Output ===\n%s\n", msg.data);
    } else {
        printf("ERROR: %s\n", get_error_string(msg.error_code));
    }
}

// Command: UNDO <filename>
void cmd_undo(char* filename) {
    Message msg = {0};
    msg.operation = OP_UNDO;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    // Get SS info from NM
    send_message(nm_sock, &msg);
    receive_message(nm_sock, &msg);
    
    if (msg.error_code != ERR_SUCCESS) {
        printf("ERROR: %s\n", get_error_string(msg.error_code));
        return;
    }
    
    // Connect to SS
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(msg.ss_port);
    inet_pton(AF_INET, msg.ss_ip, &ss_addr.sin_addr);
    
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        printf("ERROR: Could not connect to storage server\n");
        return;
    }
    
    Message ss_msg = {0};
    ss_msg.operation = OP_SS_UNDO;
    strcpy(ss_msg.filename, filename);
    
    send_message(ss_sock, &ss_msg);
    receive_message(ss_sock, &ss_msg);
    
    if (ss_msg.error_code == ERR_SUCCESS) {
        printf("✓ File '%s' restored to previous version\n", filename);
    } else {
        printf("ERROR: %s\n", get_error_string(ss_msg.error_code));
    }
    
    close(ss_sock);
}

// Print help
void print_help() {
    printf("\n=== Available Commands ===\n\n");
    printf("File Operations:\n");
    printf("  VIEW [-a] [-l]           - List files (use -a for all, -l for details)\n");
    printf("  READ <file>              - Read file content\n");
    printf("  CREATE <file>            - Create new file\n");
    printf("  WRITE <file> <sent_num>  - Edit sentence (word-level)\n");
    printf("  DELETE <file>            - Delete file (owner only)\n");
    printf("  INFO <file>              - Display file information\n");
    printf("  UNDO <file>              - Revert last change\n\n");
    
    printf("Advanced Operations:\n");
    printf("  STREAM <file>            - Stream file word-by-word\n");
    printf("  EXEC <file>              - Execute file as shell commands\n\n");
    
    printf("Access Control:\n");
    printf("  ADDACCESS -R <file> <user>  - Grant read access\n");
    printf("  ADDACCESS -W <file> <user>  - Grant write access\n");
    printf("  REMACCESS <file> <user>     - Remove access\n\n");
    
    printf("System:\n");
    printf("  LIST                     - List all registered users\n");
    printf("  HELP                     - Show this help\n");
    printf("  EXIT                     - Exit client\n\n");
    
    printf("Example - WRITE command:\n");
    printf("  > WRITE myfile.txt 0\n");
    printf("  WRITE> 0 Hello\n");
    printf("  WRITE> 1 World.\n");
    printf("  WRITE> ETIRW\n\n");
}

// Main function
int main() {
    printf("=================================\n");
    printf("  Network File System - Client\n");
    printf("=================================\n\n");
    printf("Enter your username: ");
    
    if (fgets(username, sizeof(username), stdin) == NULL) {
        printf("ERROR: Could not read username\n");
        return 1;
    }
    
    username[strcspn(username, "\n")] = 0;
    
    if (strlen(username) == 0) {
        printf("ERROR: Username cannot be empty\n");
        return 1;
    }
    
    connect_to_nm();
    print_help();
    
    char command[MAX_BUFFER_SIZE];
    while (1) {
        printf("\n%s> ", username);
        
        if (fgets(command, sizeof(command), stdin) == NULL) {
            break;
        }
        
        command[strcspn(command, "\n")] = 0;
        if (strlen(command) == 0) continue;
        
        // Parse command
        char cmd[64] = {0};
        char arg1[256] = {0};
        char arg2[256] = {0};
        char arg3[256] = {0};
        
        int argc = sscanf(command, "%s %s %s %s", cmd, arg1, arg2, arg3);
        
        // Convert to uppercase
        for (int i = 0; cmd[i]; i++) {
            cmd[i] = toupper(cmd[i]);
        }
        
        if (strcmp(cmd, "EXIT") == 0 || strcmp(cmd, "QUIT") == 0) {
            break;
        } 
        else if (strcmp(cmd, "HELP") == 0) {
            print_help();
        } 
        else if (strcmp(cmd, "VIEW") == 0) {
            cmd_view(argc >= 2 ? arg1 : NULL);
        } 
        else if (strcmp(cmd, "READ") == 0 && argc >= 2) {
            cmd_read(arg1);
        } 
        else if (strcmp(cmd, "CREATE") == 0 && argc >= 2) {
            cmd_create(arg1);
        } 
        else if (strcmp(cmd, "WRITE") == 0 && argc >= 3) {
            int sentence_num = atoi(arg2);
            cmd_write(arg1, sentence_num);
        } 
        else if (strcmp(cmd, "DELETE") == 0 && argc >= 2) {
            cmd_delete(arg1);
        } 
        else if (strcmp(cmd, "INFO") == 0 && argc >= 2) {
            cmd_info(arg1);
        } 
        else if (strcmp(cmd, "STREAM") == 0 && argc >= 2) {
            cmd_stream(arg1);
        } 
        else if (strcmp(cmd, "LIST") == 0) {
            cmd_list();
        } 
        else if (strcmp(cmd, "ADDACCESS") == 0 && argc >= 4) {
            if (arg1[0] == '-' && (arg1[1] == 'R' || arg1[1] == 'W')) {
                cmd_addaccess(arg1[1], arg2, arg3);
            } else {
                printf("ERROR: Use ADDACCESS -R/-W <filename> <username>\n");
            }
        } 
        else if (strcmp(cmd, "REMACCESS") == 0 && argc >= 3) {
            cmd_remaccess(arg1, arg2);
        } 
        else if (strcmp(cmd, "EXEC") == 0 && argc >= 2) {
            cmd_exec(arg1);
        } 
        else if (strcmp(cmd, "UNDO") == 0 && argc >= 2) {
            cmd_undo(arg1);
        } 
        else {
            printf("Invalid command. Type HELP for available commands.\n");
        }
    }
    
    if (nm_sock >= 0) {
        close(nm_sock);
    }
    
    printf("\nGoodbye, %s!\n", username);
    return 0;
}
