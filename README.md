# SyncText - Collaborative Text Editor

## What This Does

- Multiple people can run the editor on the same machine and edit their own copy of a document. The system automatically:
- Detects what you changed
- Sends your changes to others
- Receives changes from others
- Merges everything using Last-Writer-Wins conflict resolution
- Keeps all documents synchronized

## Requirements

- Linux (tested on Ubuntu 20.04+)
- gcc compiler
- POSIX message queues and pthreads (usually already installed)

## How to Build

one command:
```bash
make
```

This creates two executables:
- `editor` - the main program
- `clear_users` - utility to reset the user registry

If you want to rebuild from scratch:
```bash
make clean && make
```

## How to Run

### Basic Usage

Open 2-3 terminal windows in this directory.

Terminal 1:
```bash
./editor user_1
```

Terminal 2:
```bash
./editor user_2
```

Terminal 3 (optional):
```bash
./editor user_3
```

Each user gets their own file: `user_1_doc.txt`, `user_2_doc.txt`, etc.

### What You'll See

Each terminal shows:
```
Document: user_1_doc.txt
Last updated: 15:30:45
----------------------------------------
Line 0: int x = 10;
Line 1: int y = 20;
Line 2: int z = 30;
----------------------------------------
Active users: user_1, user_2
Monitoring for changes...
```

### Editing Files

Open the document in any text editor:
```bash
nano user_1_doc.txt
# or
vim user_1_doc.txt
# or use your favorite editor
```

Make changes and save. The editor automatically detects the changes and shows them.

After you make 5 changes, they get broadcast to other users.
When you receive 5 changes from others, they get merged into your file.

## How It Works

### Change Detection
- Program checks file every 200ms using stat()
- When file changes, reads it and compares with old version
- Finds exactly which lines and columns changed
- Creates Update objects with metadata (line, columns, timestamp, user_id)

### Broadcasting
- Local changes accumulate in a buffer
- After 5 changes, broadcast all of them to other users
- Uses POSIX message queues to send binary Update structs
- Each user has their own queue: `/queue_user_1`, `/queue_user_2`, etc.

### Receiving
- Listener thread continuously checks your message queue
- Receives Update structs from other users
- Stores them in a received buffer
- After 5 received, triggers merge

### Merging (CRDT)
- Combines local and received updates
- Detects conflicts (same line + overlapping columns)
- Resolves conflicts: newer timestamp wins, user_id breaks ties
- Applies winning updates to file
- Everyone converges to same final content

### Threading
- Main thread: monitors file, batches updates, broadcasts, performs merge
- Listener thread: receives updates from message queue
- Lock-free coordination using atomic variables
- No deadlocks possible

## Testing the System

### Test 1: Non-Conflicting Edits
1. Start user_1 and user_2
2. In user_1_doc.txt, edit line 0
3. In user_2_doc.txt, edit line 2
4. Make 5 total edits in each
5. Watch changes appear in both terminals

### Test 2: Conflicting Edits (The Interesting Part)
1. Start user_1 and user_2
2. Both edit line 0 at roughly the same time
3. Make 5 edits in each
4. System picks the most recent edit (Last-Writer-Wins)
5. Both users end up with same content

### Test 3: Three Users
1. Start user_1, user_2, user_3
2. Everyone edits different lines
3. Make changes quickly
4. All changes propagate to everyone
5. Documents stay synchronized

## Cleanup

If things get stuck or  want to start fresh:

```bash
# Remove build artifacts and test files
make clean

# Clear the user registry
./clear_users

# Manually remove message queues if needed
rm -f /dev/mqueue/queue_*
```

## Troubleshooting

### "mq_open: No such file or directory"
The message queue filesystem might not be set up. Try:
```bash
sudo mkdir -p /dev/mqueue
sudo mount -t mqueue none /dev/mqueue
```

### "User already registered"
Another instance is running or didn't clean up. Run:
```bash
./clear_users
```

### Changes not syncing
Make sure you're making at least 5 changes (that's when broadcast happens). Or check that both editors are running and showing each other in "Active users" list.

### File permission errors
Make sure the .txt files are writable and you're in the right directory.

## File Structure

```
editor.c           - Main program (file monitoring, thread management, merge coordination)
file_utils.c       - File I/O and change detection
user_registry.c    - Shared memory for active users
message_queue.c    - POSIX message queue operations
crdt_merge.c       - Conflict detection and resolution
display.c          - Terminal display
clear_users.c      - Utility to reset user registry
Makefile           - Build rules
```

