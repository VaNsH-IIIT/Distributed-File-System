# Docs++ - Distributed File System

A simplified shared document system similar to Google Docs, implemented in C with support for concurrency and access control.

---

## 🚀 Features

### 🧩 Core Functionalities (150 Marks)
- ✅ View files with flags (`-a`, `-l`, `-al`)
- ✅ Read file content  
- ✅ Create new files  
- ✅ Write to files with **sentence-level locking**  
- ✅ Undo last changes  
- ✅ Get file metadata and information  
- ✅ Delete files  
- ✅ Stream content **word-by-word**  
- ✅ List all users  
- ✅ Access control (add/remove read/write permissions)  
- ✅ Execute file content as shell commands  

### ⚙️ System Requirements (40 Marks)
- ✅ Data persistence across server restarts  
- ✅ Access control enforcement  
- ✅ Comprehensive logging system  
- ✅ Error handling with clear error codes  
- ✅ Efficient search using **Trie data structure** (O(m) complexity)  
- ✅ **LRU caching** for recent file lookups  

---

## 🏗️ Architecture

- **Name Server (NM)** – Central coordinator managing file metadata and routing  
- **Storage Servers (SS)** – Handle actual file storage and operations  
- **Clients** – User interface for file operations  

---

## 🔧 Compilation

```bash
make
```

This will compile three executables:
- `name_server`
- `storage_server`
- `client`

---

## ▶️ Running the System

### 1. Start Name Server
```bash
./name_server
```
The Name Server will start on **port 8080**.

### 2. Start Storage Servers
```bash
# Terminal 2
./storage_server 127.0.0.1 9001 10001

# Terminal 3
./storage_server 127.0.0.1 9002 10002
```

### 3. Start Client(s)
```bash
# Terminal 4
./client
```
Enter your **username** when prompted, then use the available commands.

---

## 💻 Available Commands

| Command | Description | Example |
|----------|--------------|----------|
| `VIEW` | List accessible files | `VIEW` |
| `VIEW -a` | List all files | `VIEW -a` |
| `VIEW -l` | List files with details | `VIEW -l` |
| `VIEW -al` | List all files with details | `VIEW -al` |
| `READ <filename>` | Read file content | `READ test.txt` |
| `CREATE <filename>` | Create new file | `CREATE newfile.txt` |
| `WRITE <filename> <sent_idx>` | Write to a specific sentence index | `WRITE test.txt 0` |
| `DELETE <filename>` | Delete file | `DELETE oldfile.txt` |
| `INFO <filename>` | Get file information | `INFO test.txt` |
| `STREAM <filename>` | Stream file word-by-word | `STREAM test.txt` |
| `LIST` | List all users | `LIST` |
| `ADDACCESS -R <file> <user>` | Add read access | `ADDACCESS -R test.txt user2` |
| `ADDACCESS -W <file> <user>` | Add write access | `ADDACCESS -W test.txt user2` |
| `REMACCESS <file> <user>` | Remove access | `REMACCESS test.txt user2` |
| `EXEC <filename>` | Execute file as shell script | `EXEC script.txt` |
| `UNDO <filename>` | Undo last change | `UNDO test.txt` |
| `HELP` | Show help | `HELP` |
| `EXIT` | Exit client | `EXIT` |

---

## 🧪 Testing

Run the automated test suite:
```bash
chmod +x test.sh
./test.sh
```

The test script will:
1. Compile the project  
2. Start all necessary servers  
3. Run **15+ automated tests**  
4. Display results with **pass/fail** status  
5. Clean up processes and files  

---

## 🧠 Key Implementation Details

### 🔍 Efficient Search
- **Trie data structure** for O(m) file lookups (m = filename length)  
- **LRU cache** (100-entry capacity) for recent searches  
- **Hash-based fallback** for edge cases  

### 🧵 Concurrency
- **Multi-threading** using `pthreads` for handling multiple clients  
- **Mutex locks** for protecting shared data structures  
- **Sentence-level locking** for concurrent file editing  

### 🔐 Access Control
- **Owner** always has read/write access  
- **Read access** allows viewing content  
- **Write access** includes read permissions  
- Access lists stored in file metadata  

### 💾 Data Persistence
- Files stored in `./storage/`  
- Undo history in `./storage/.undo/`  
- Metadata maintained by **Name Server**  

### ⚠️ Error Handling

| Error Code | Meaning |
|-------------|----------|
| `ERR_FILE_NOT_FOUND` | File doesn't exist |
| `ERR_ACCESS_DENIED` | Insufficient permissions |
| `ERR_FILE_EXISTS` | File already exists |
| `ERR_SENTENCE_LOCKED` | Sentence being edited by another user |
| `ERR_INDEX_OUT_OF_RANGE` | Invalid sentence/word index |
| `ERR_SS_NOT_AVAILABLE` | Storage server unavailable |

---

## 🌐 Network Architecture

```
Client(s) <--TCP--> Name Server <--TCP--> Storage Server(s)
   |                                          |
   +--------------- Direct TCP ---------------+
          (for READ, WRITE, STREAM ops)
```

---

## 📂 Project Structure

```
docs_plus_plus/
├── common.h              # Shared data structures and constants
├── common.c              # Common utility functions
├── name_server.c         # Name Server implementation
├── storage_server.c      # Storage Server implementation
├── client.c              # Client implementation
├── Makefile              # Build configuration
├── test.sh               # Automated testing script
└── README.md             # Project documentation
```

---

## ⚠️ Limitations

- Single undo per file (no multi-level undo)  
- No folder hierarchy (bonus feature not implemented)  
- No fault tolerance or replication (bonus feature)  
- Name Server failure means system failure  
- Maximum: 100 clients, 10 storage servers  

---

## 🚧 Future Enhancements

- Hierarchical folder structure  
- Multi-level undo with checkpoints  
- Fault tolerance with replication  
- Request-access mechanism  
- Name Server redundancy  

---

## 👥 Authors

Implemented as part of the **Operating Systems and Networks (OSN)** course project at IIIT Hyderabad.

---

## 📜 License

Educational use only.

---

## 🧭 Usage Instructions

### Compile everything
```bash
make
```

### Run in separate terminals
```bash
# Terminal 1
./name_server

# Terminal 2
./storage_server 127.0.0.1 9001 10001

# Terminal 3
./storage_server 127.0.0.1 9002 10002

# Terminal 4
./client
```

### Run tests
```bash
chmod +x test.sh
./test.sh
```
