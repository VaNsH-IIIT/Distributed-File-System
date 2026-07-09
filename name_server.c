#include "common.h"

// Hash table for O(1) file lookup
FileHashEntry* file_hash_table[FILE_HASH_TABLE_SIZE];
pthread_mutex_t hash_mutex = PTHREAD_MUTEX_INITIALIZER;

// Global data structures
StorageServerInfo storage_servers[MAX_STORAGE_SERVERS];
int ss_count = 0;
pthread_mutex_t ss_mutex = PTHREAD_MUTEX_INITIALIZER;

ClientInfo clients[MAX_CLIENTS];
int client_count = 0;
pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;

FileMetadata files[MAX_FILES_PER_SERVER * MAX_STORAGE_SERVERS];
int file_count = 0;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

// ============================================================================
// FIXED: find_file() - Now O(1) using hash table
// ============================================================================
int find_file(const char* filename) {
    pthread_mutex_lock(&hash_mutex);
    int file_index = hash_table_lookup(file_hash_table, filename);
    pthread_mutex_unlock(&hash_mutex);
    return file_index;
}

// Helper: Get available storage server
int get_available_ss() {
    pthread_mutex_lock(&ss_mutex);
    for (int i = 0; i < ss_count; i++) {
        if (storage_servers[i].is_active) {
            pthread_mutex_unlock(&ss_mutex);
            return i;
        }
    }
    pthread_mutex_unlock(&ss_mutex);
    return -1;
}

// Helper: Check access permissions
int check_access(FileMetadata* file, const char* username, int required_access) {
    // Owner always has full access
    if (strcmp(file->owner, username) == 0) {
        return 1;
    }

    // Check access list
    for (int i = 0; i < file->access_count; i++) {
        if (strcmp(file->access_list[i].username, username) == 0) {
            if (required_access == ACCESS_READ) {
                return (file->access_list[i].access_type >= ACCESS_READ);
            } else if (required_access == ACCESS_WRITE) {
                return (file->access_list[i].access_type >= ACCESS_WRITE);
            }
        }
    }
    return 0;
}

// Handle: Register Storage Server
void handle_register_ss(int client_sock, Message* msg) {
    pthread_mutex_lock(&ss_mutex);

    if (ss_count >= MAX_STORAGE_SERVERS) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Maximum storage servers reached");
        pthread_mutex_unlock(&ss_mutex);
        send_message(client_sock, msg);
        return;
    }

    char ip[INET_ADDRSTRLEN];
    int nm_port, client_port;
    sscanf(msg->data, "%s %d %d", ip, &nm_port, &client_port);

    StorageServerInfo* ss = &storage_servers[ss_count];
    ss->id = ss_count;
    strcpy(ss->ip, ip);
    ss->nm_port = nm_port;
    ss->client_port = client_port;
    ss->is_active = 1;
    ss->file_count = 0;

    msg->ss_index = ss_count;
    ss_count++;

    char log_msg[MAX_BUFFER_SIZE];
    snprintf(log_msg, sizeof(log_msg), "Storage Server %d registered: %s (NM:%d, Client:%d)", 
             ss->id, ss->ip, ss->nm_port, ss->client_port);
    log_message("NM", log_msg);

    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "Registration successful");
    pthread_mutex_unlock(&ss_mutex);
    send_message(client_sock, msg);
}

// Handle: Register Client
void handle_register_client(int client_sock, Message* msg) {
    pthread_mutex_lock(&client_mutex);

    if (client_count >= MAX_CLIENTS) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Maximum clients reached");
        pthread_mutex_unlock(&client_mutex);
        send_message(client_sock, msg);
        return;
    }

    ClientInfo* client = &clients[client_count];
    strcpy(client->username, msg->username);
    strcpy(client->ip, "127.0.0.1");
    client->is_active = 1;
    client_count++;

    char log_msg[MAX_BUFFER_SIZE];
    snprintf(log_msg, sizeof(log_msg), "Client registered: %s", client->username);
    log_message("NM", log_msg);

    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "Registration successful");
    pthread_mutex_unlock(&client_mutex);
    send_message(client_sock, msg);
}

// Handle: VIEW with flags
void handle_view(int client_sock, Message* msg) {
    pthread_mutex_lock(&file_mutex);

    char file_list[MAX_BUFFER_SIZE] = "\n=== Files ===\n\n";
    int show_all = (msg->flags & FLAG_ALL);
    int show_details = (msg->flags & FLAG_DETAILS);

    if (file_count == 0) {
        strcat(file_list, "No files available.\n");
    } else {
        for (int i = 0; i < file_count; i++) {
            // Check if user has access (unless -a flag)
            if (!show_all && !check_access(&files[i], msg->username, ACCESS_READ)) {
                continue;
            }

            if (show_details) {
                char entry[512];
                char atime_str[64], mtime_str[64];
                strftime(atime_str, sizeof(atime_str), "%Y-%m-%d %H:%M:%S", 
                        localtime(&files[i].last_accessed));
                strftime(mtime_str, sizeof(mtime_str), "%Y-%m-%d %H:%M:%S", 
                        localtime(&files[i].last_modified));

                snprintf(entry, sizeof(entry), 
                        "%d. %-20s Owner: %-12s Size: %6ld bytes  Words: %4d  Chars: %5d  "
                        "LastAccess: %s\n",
                        i + 1, files[i].filename, files[i].owner, 
                        files[i].file_size, files[i].word_count, files[i].char_count,
                        atime_str);
                strcat(file_list, entry);
            } else {
                char entry[512];
                snprintf(entry, sizeof(entry), "%d. %s (Owner: %s)\n", 
                        i + 1, files[i].filename, files[i].owner);
                strcat(file_list, entry);
            }
        }
    }

    strcpy(msg->data, file_list);
    msg->error_code = ERR_SUCCESS;
    pthread_mutex_unlock(&file_mutex);
    send_message(client_sock, msg);
}

// ============================================================================
// FIXED: handle_create() - Add to hash table after creating file
// ============================================================================
void handle_create(int client_sock, Message* msg) {
    pthread_mutex_lock(&file_mutex);

    // Check if file already exists (now O(1)!)
    if (find_file(msg->filename) != -1) {
        msg->error_code = ERR_FILE_EXISTS;
        strcpy(msg->data, "File already exists");
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        return;
    }

    // Get available storage server
    int ss_id = get_available_ss();
    if (ss_id == -1) {
        msg->error_code = ERR_NO_SS_AVAILABLE;
        strcpy(msg->data, "No storage server available");
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        return;
    }

    // Connect to storage server and create file
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(storage_servers[ss_id].nm_port);
    inet_pton(AF_INET, storage_servers[ss_id].ip, &ss_addr.sin_addr);

    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Could not connect to storage server");
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        close(ss_sock);
        return;
    }

    Message ss_msg = {0};
    ss_msg.operation = OP_SS_CREATE;
    strcpy(ss_msg.filename, msg->filename);

    send_message(ss_sock, &ss_msg);
    receive_message(ss_sock, &ss_msg);
    close(ss_sock);

    if (ss_msg.error_code != ERR_SUCCESS) {
        msg->error_code = ss_msg.error_code;
        strcpy(msg->data, ss_msg.data);
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        return;
    }

    // Add file metadata
    FileMetadata* file = &files[file_count];
    strcpy(file->filename, msg->filename);
    strcpy(file->owner, msg->username);
    file->ss_id = ss_id;
    file->created_time = time(NULL);
    file->last_accessed = time(NULL);
    file->last_modified = time(NULL);
    file->file_size = 0;
    file->word_count = 0;
    file->char_count = 0;
    file->sentence_count = 0;
    file->access_count = 0;

    // CRITICAL FIX: Add to hash table for O(1) lookup
    pthread_mutex_lock(&hash_mutex);
    if (hash_table_insert(file_hash_table, msg->filename, file_count) != 0) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Failed to add to hash table");
        pthread_mutex_unlock(&hash_mutex);
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        return;
    }
    pthread_mutex_unlock(&hash_mutex);

    file_count++;
    storage_servers[ss_id].file_count++;

    char log_msg[MAX_BUFFER_SIZE];
    snprintf(log_msg, sizeof(log_msg), "File created: %s by %s on SS %d", 
             file->filename, file->owner, file->ss_id);
    log_message("NM", log_msg);

    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "File created successfully");
    pthread_mutex_unlock(&file_mutex);
    send_message(client_sock, msg);
}

// Handle: GET_SS_INFO (for READ, WRITE, etc.)
void handle_get_ss_info(int client_sock, Message* msg) {
    pthread_mutex_lock(&file_mutex);

    int file_idx = find_file(msg->filename);  // Now O(1)!
    if (file_idx == -1) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->data, "File not found");
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        return;
    }

    // Check access based on operation
    if(msg->operation != OP_INFO){
        int required_access = (msg->operation == OP_READ || msg->operation == OP_STREAM) ? 
                            ACCESS_READ : ACCESS_WRITE;

        if (!check_access(&files[file_idx], msg->username, required_access)) {
            msg->error_code = ERR_ACCESS_DENIED;
            strcpy(msg->data, "Access denied");
            pthread_mutex_unlock(&file_mutex);
            send_message(client_sock, msg);
            return;
        }
    }

    int ss_id = files[file_idx].ss_id;
    pthread_mutex_lock(&ss_mutex);

    if (!storage_servers[ss_id].is_active) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Storage server not available");
        pthread_mutex_unlock(&ss_mutex);
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        return;
    }

    strcpy(msg->ss_ip, storage_servers[ss_id].ip);
    msg->ss_port = storage_servers[ss_id].client_port;
    msg->error_code = ERR_SUCCESS;

    // Update last accessed time
    files[file_idx].last_accessed = time(NULL);

    pthread_mutex_unlock(&ss_mutex);
    pthread_mutex_unlock(&file_mutex);
    send_message(client_sock, msg);
}

// ============================================================================
// FIXED: handle_delete() - Remove from hash table and update indices
// ============================================================================
void handle_delete(int client_sock, Message* msg) {
    pthread_mutex_lock(&file_mutex);

    int file_idx = find_file(msg->filename);  // Now O(1)!
    if (file_idx == -1) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->data, "File not found");
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        return;
    }

    // Only owner can delete
    if (strcmp(files[file_idx].owner, msg->username) != 0) {
        msg->error_code = ERR_ACCESS_DENIED;
        strcpy(msg->data, "Only owner can delete file");
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        return;
    }

    int ss_id = files[file_idx].ss_id;

    // Connect to storage server and delete file
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(storage_servers[ss_id].nm_port);
    inet_pton(AF_INET, storage_servers[ss_id].ip, &ss_addr.sin_addr);

    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Could not connect to storage server");
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        close(ss_sock);
        return;
    }

    Message ss_msg = {0};
    ss_msg.operation = OP_SS_DELETE;
    strcpy(ss_msg.filename, msg->filename);

    send_message(ss_sock, &ss_msg);
    receive_message(ss_sock, &ss_msg);
    close(ss_sock);

    if (ss_msg.error_code != ERR_SUCCESS) {
        msg->error_code = ss_msg.error_code;
        strcpy(msg->data, ss_msg.data);
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        return;
    }

    // CRITICAL FIX: Remove from hash table
    pthread_mutex_lock(&hash_mutex);
    hash_table_remove(file_hash_table, msg->filename);
    pthread_mutex_unlock(&hash_mutex);

    // Remove from files array and UPDATE HASH TABLE INDICES
    for (int i = file_idx; i < file_count - 1; i++) {
        files[i] = files[i + 1];

        // IMPORTANT: Update hash table index after shift
        pthread_mutex_lock(&hash_mutex);
        hash_table_remove(file_hash_table, files[i].filename);
        hash_table_insert(file_hash_table, files[i].filename, i);
        pthread_mutex_unlock(&hash_mutex);
    }
    file_count--;
    storage_servers[ss_id].file_count--;

    char log_msg[MAX_BUFFER_SIZE];
    snprintf(log_msg, sizeof(log_msg), "File deleted: %s by %s", 
             msg->filename, msg->username);
    log_message("NM", log_msg);

    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "File deleted successfully");
    pthread_mutex_unlock(&file_mutex);
    send_message(client_sock, msg);
}

// Handle: ADD_ACCESS
void handle_add_access(int client_sock, Message* msg) {
    pthread_mutex_lock(&file_mutex);

    int file_idx = find_file(msg->filename);  // Now O(1)!
    if (file_idx == -1) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->data, "File not found");
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        return;
    }

    // Only owner can modify access
    if (strcmp(files[file_idx].owner, msg->username) != 0) {
        msg->error_code = ERR_ACCESS_DENIED;
        strcpy(msg->data, "Only owner can modify access");
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        return;
    }

    FileMetadata* file = &files[file_idx];

    // Check if user already has access
    int existing_idx = -1;
    for (int i = 0; i < file->access_count; i++) {
        if (strcmp(file->access_list[i].username, msg->target_user) == 0) {
            existing_idx = i;
            break;
        }
    }

    if (existing_idx != -1) {
        // Update existing access
        file->access_list[existing_idx].access_type = msg->access_type;
    } else {
        // Add new access
        if (file->access_count < MAX_ACCESS_LIST) {
            strcpy(file->access_list[file->access_count].username, msg->target_user);
            file->access_list[file->access_count].access_type = msg->access_type;
            file->access_count++;
        } else {
            msg->error_code = ERR_SERVER_ERROR;
            strcpy(msg->data, "Access list full");
            pthread_mutex_unlock(&file_mutex);
            send_message(client_sock, msg);
            return;
        }
    }

    
    // **NEW: Update metadata file on storage server**
    int ss_id = file->ss_id;
    
    // Connect to storage server
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(storage_servers[ss_id].nm_port);
    inet_pton(AF_INET, storage_servers[ss_id].ip, &ss_addr.sin_addr);
    
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) == 0) {
        Message ss_msg = {0};
        ss_msg.operation = OP_SS_UPDATE_ACL;
        strcpy(ss_msg.filename, msg->filename);
        strcpy(ss_msg.username, file->owner);
        
        // Pack ACL info into data field
        char acl_data[MAX_BUFFER_SIZE] = {0};
        int offset = 0;
        offset += snprintf(acl_data + offset, MAX_BUFFER_SIZE - offset, 
                          "%d|%d|%d|%d|", 
                          file->word_count, file->char_count, 
                          file->sentence_count, file->access_count);
        
        for (int i = 0; i < file->access_count; i++) {
            offset += snprintf(acl_data + offset, MAX_BUFFER_SIZE - offset,
                             "%s:%d|", 
                             file->access_list[i].username,
                             file->access_list[i].access_type);
        }
        
        strcpy(ss_msg.data, acl_data);
        send_message(ss_sock, &ss_msg);
        receive_message(ss_sock, &ss_msg);
        close(ss_sock);
    }

    char log_msg[MAX_BUFFER_SIZE];
    const char* access_str = (msg->access_type == ACCESS_READ) ? "READ" : "WRITE";
    snprintf(log_msg, sizeof(log_msg), "Access granted: %s on %s for %s (%s)", 
             msg->target_user, msg->filename, msg->username, access_str);
    log_message("NM", log_msg);

    msg->error_code = ERR_SUCCESS;
    snprintf(msg->data, sizeof(msg->data), "%s access granted to %s", 
             access_str, msg->target_user);
    pthread_mutex_unlock(&file_mutex);
    send_message(client_sock, msg);
}

// Handle: REM_ACCESS
void handle_rem_access(int client_sock, Message* msg) {
    pthread_mutex_lock(&file_mutex);

    int file_idx = find_file(msg->filename);  // Now O(1)!
    if (file_idx == -1) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->data, "File not found");
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        return;
    }

    // Only owner can modify access
    if (strcmp(files[file_idx].owner, msg->username) != 0) {
        msg->error_code = ERR_ACCESS_DENIED;
        strcpy(msg->data, "Only owner can modify access");
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        return;
    }

    FileMetadata* file = &files[file_idx];

    // Find and remove access
    int found = 0;
    for (int i = 0; i < file->access_count; i++) {
        if (strcmp(file->access_list[i].username, msg->target_user) == 0) {
            // Shift remaining entries
            for (int j = i; j < file->access_count - 1; j++) {
                file->access_list[j] = file->access_list[j + 1];
            }
            file->access_count--;
            found = 1;
            break;
        }
    }
    
    if (!found) {
        msg->error_code = ERR_USER_NOT_FOUND;
        strcpy(msg->data, "User does not have access");
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        return;
    }
    
    // **NEW: Update metadata file on storage server**
    int ss_id = file->ss_id;
    
    // Connect to storage server (same code as handle_add_access)
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(storage_servers[ss_id].nm_port);
    inet_pton(AF_INET, storage_servers[ss_id].ip, &ss_addr.sin_addr);
    
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) == 0) {
        Message ss_msg = {0};
        ss_msg.operation = OP_SS_UPDATE_ACL;
        strcpy(ss_msg.filename, msg->filename);
        strcpy(ss_msg.username, file->owner);
        
        // Pack ACL info
        char acl_data[MAX_BUFFER_SIZE] = {0};
        int offset = 0;
        offset += snprintf(acl_data + offset, MAX_BUFFER_SIZE - offset, 
                          "%d|%d|%d|%d|", 
                          file->word_count, file->char_count, 
                          file->sentence_count, file->access_count);
        
        for (int i = 0; i < file->access_count; i++) {
            offset += snprintf(acl_data + offset, MAX_BUFFER_SIZE - offset,
                             "%s:%d|", 
                             file->access_list[i].username,
                             file->access_list[i].access_type);
        }
        
        strcpy(ss_msg.data, acl_data);
        send_message(ss_sock, &ss_msg);
        receive_message(ss_sock, &ss_msg);
        close(ss_sock);
    }

    char log_msg[MAX_BUFFER_SIZE];
    snprintf(log_msg, sizeof(log_msg), "Access removed: %s from %s by %s", 
             msg->target_user, msg->filename, msg->username);
    log_message("NM", log_msg);

    msg->error_code = ERR_SUCCESS;
    snprintf(msg->data, sizeof(msg->data), "Access removed from %s", msg->target_user);
    pthread_mutex_unlock(&file_mutex);
    send_message(client_sock, msg);
}

// Handle: LIST_USERS
void handle_list_users(int client_sock, Message* msg) {
    pthread_mutex_lock(&client_mutex);

    char user_list[MAX_BUFFER_SIZE] = "\n=== Registered Users ===\n\n";

    if (client_count == 0) {
        strcat(user_list, "No users registered.\n");
    } else {
        for (int i = 0; i < client_count; i++) {
            char entry[512];
            const char* status = clients[i].is_active ? "Active" : "Inactive";
            snprintf(entry, sizeof(entry), "%d. %s (%s)\n", 
                    i + 1, clients[i].username, status);
            strcat(user_list, entry);
        }
    }

    strcpy(msg->data, user_list);
    msg->error_code = ERR_SUCCESS;
    pthread_mutex_unlock(&client_mutex);
    send_message(client_sock, msg);
}

// Handle: EXEC
void handle_exec(int client_sock, Message* msg) {
    pthread_mutex_lock(&file_mutex);

    int file_idx = find_file(msg->filename);  // Now O(1)!
    if (file_idx == -1) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->data, "File not found");
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        return;
    }

    // Check READ access
    if (!check_access(&files[file_idx], msg->username, ACCESS_READ)) {
        msg->error_code = ERR_ACCESS_DENIED;
        strcpy(msg->data, "Access denied");
        pthread_mutex_unlock(&file_mutex);
        send_message(client_sock, msg);
        return;
    }

    int ss_id = files[file_idx].ss_id;
    pthread_mutex_unlock(&file_mutex);

    // Get file content from SS
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(storage_servers[ss_id].nm_port);
    inet_pton(AF_INET, storage_servers[ss_id].ip, &ss_addr.sin_addr);

    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->data, "Could not connect to storage server");
        send_message(client_sock, msg);
        close(ss_sock);
        return;
    }

    Message ss_msg = {0};
    ss_msg.operation = OP_SS_READ;
    strcpy(ss_msg.filename, msg->filename);

    send_message(ss_sock, &ss_msg);
    receive_message(ss_sock, &ss_msg);
    close(ss_sock);

    if (ss_msg.error_code != ERR_SUCCESS) {
        msg->error_code = ss_msg.error_code;
        strcpy(msg->data, ss_msg.data);
        send_message(client_sock, msg);
        return;
    }

    // Execute commands using popen
    char output[MAX_BUFFER_SIZE] = "";
    FILE* fp = popen(ss_msg.data, "r");
    if (fp) {
        char line[1024];
        while (fgets(line, sizeof(line), fp) != NULL) {
            if (strlen(output) + strlen(line) < MAX_BUFFER_SIZE - 1) {
                strcat(output, line);
            }
        }
        pclose(fp);
    } else {
        strcpy(output, "Failed to execute commands");
    }

    strcpy(msg->data, output);
    msg->error_code = ERR_SUCCESS;
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
            case OP_REGISTER_SS:
                handle_register_ss(client_sock, &msg);
                break;
            case OP_REGISTER_CLIENT:
                handle_register_client(client_sock, &msg);
                break;
            case OP_VIEW:
                handle_view(client_sock, &msg);
                break;
            case OP_CREATE:
                handle_create(client_sock, &msg);
                break;
            case OP_DELETE:
                handle_delete(client_sock, &msg);
                break;
            case OP_GET_SS_INFO:
            case OP_READ:
            case OP_STREAM:
            case OP_WRITE:
            case OP_INFO:
            case OP_UNDO:
                handle_get_ss_info(client_sock, &msg);
                break;
            case OP_ADD_ACCESS:
                handle_add_access(client_sock, &msg);
                break;
            case OP_REM_ACCESS:
                handle_rem_access(client_sock, &msg);
                break;
            case OP_LIST_USERS:
                handle_list_users(client_sock, &msg);
                break;
            case OP_EXEC:
                handle_exec(client_sock, &msg);
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

int main() {
    // CRITICAL FIX: Initialize hash table
    init_file_hash_table(file_hash_table);

    log_message("NM", "Name Server starting on port 8080");
    log_message("NM", "Hash table initialized for O(1) file lookup");

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        error_exit("Socket creation failed");
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(NM_PORT);

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        error_exit("Bind failed");
    }

    if (listen(server_sock, 20) < 0) {
        error_exit("Listen failed");
    }

    log_message("NM", "Name Server ready - All features enabled");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int* client_sock = malloc(sizeof(int));
        *client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_len);

        if (*client_sock < 0) {
            free(client_sock);
            continue;
        }

        pthread_t thread;
        pthread_create(&thread, NULL, handle_client, client_sock);
        pthread_detach(thread);
    }

    close(server_sock);
    return 0;
}