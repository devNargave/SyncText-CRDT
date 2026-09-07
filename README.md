# SyncText — Collaborative Text Editor

A lightweight, terminal-based collaborative text editor for Linux. Multiple users on the same machine can edit their own copy of a document while SyncText automatically detects local changes, propagates them to other users, and merges incoming changes using a Last-Writer-Wins (LWW) CRDT strategy — keeping every copy converged on the same final content.

## Table of Contents

- [Features](#features)
- [Architecture](#architecture)
- [Requirements](#requirements)
- [Installation](#installation)
- [Usage](#usage)
- [How It Works](#how-it-works)
- [Testing](#testing)
- [Troubleshooting](#troubleshooting)
- [Project Structure](#project-structure)

## Features

- **Automatic change detection** — polls the document for changes and diffs against the previous version to identify exactly which lines and columns changed.
- **Peer-to-peer synchronization** — changes are broadcast to and received from other active users via POSIX message queues.
- **Conflict resolution** — concurrent edits to overlapping regions are resolved deterministically using timestamp-based Last-Writer-Wins semantics, with user ID as a tiebreaker.
- **Multi-user support** — supports two or more concurrent editors, each maintaining an independently synchronized copy of the document.
- **Lock-free coordination** — uses atomic variables to coordinate between the monitoring and listener threads without risk of deadlock.

## Architecture

Each running instance of the editor consists of two threads:

| Thread | Responsibility |
|---|---|
| Main | Monitors the local file, batches outgoing changes, broadcasts updates, and performs merges |
| Listener | Continuously polls the user's message queue for incoming updates from peers |

Updates are exchanged as binary `Update` structs (containing line, column range, timestamp, and user ID) over per-user POSIX message queues (`/queue_user_1`, `/queue_user_2`, etc.).

## Requirements

- Linux (tested on Ubuntu 20.04+)
- GCC compiler
- POSIX message queues and pthreads (included in most standard Linux installations)

## Installation

Build the project with:

```bash
make
```

This produces two executables:

| Executable | Purpose |
|---|---|
| `editor` | Main collaborative editor program |
| `clear_users` | Utility to reset the shared user registry |

To rebuild from a clean state:

```bash
make clean && make
```

## Usage

### Starting an editing session

Open two or three terminal windows in the project directory, and start one editor instance per terminal:

```bash
# Terminal 1
./editor user_1

# Terminal 2
./editor user_2

# Terminal 3 (optional)
./editor user_3
```

Each user operates on their own file (`user_1_doc.txt`, `user_2_doc.txt`, etc.), and the terminal displays a live view of the document:

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

### Editing a document

Open a user's document file in any text editor and save your changes:

```bash
nano user_1_doc.txt
# or
vim user_1_doc.txt
```

The running editor process automatically detects saved changes and displays them in the terminal.

- Every 5 local changes are broadcast to other active users.
- Every 5 changes received from peers trigger a merge into the local file.

## How It Works

### 1. Change Detection

The file is polled every 200ms using `stat()`. When a modification is detected, the new content is compared against the prior version to determine exactly which lines and column ranges changed. Each change is recorded as an `Update` struct with line, column, timestamp, and user ID metadata.

### 2. Broadcasting

Local changes accumulate in a buffer. Once 5 changes have been recorded, they are broadcast as binary `Update` structs to all other active users via POSIX message queues.

### 3. Receiving

A dedicated listener thread continuously polls the local message queue and stores incoming `Update` structs in a receive buffer. Once 5 updates have been received, a merge is triggered.

### 4. Merging (CRDT)

Local and received updates are combined. Conflicts — defined as updates to the same line with overlapping columns — are resolved by:

1. Comparing timestamps; the more recent update wins.
2. Using user ID as a tiebreaker in the event of identical timestamps.

The winning updates are applied to the file, ensuring all users eventually converge on identical content.

## Testing

### Test 1: Non-Conflicting Edits

1. Start `user_1` and `user_2`.
2. Edit line 0 in `user_1_doc.txt` and line 2 in `user_2_doc.txt`.
3. Make 5 total edits in each file.
4. Confirm changes appear in both terminals.

### Test 2: Conflicting Edits

1. Start `user_1` and `user_2`.
2. Have both users edit line 0 at roughly the same time.
3. Make 5 edits in each file.
4. Confirm the system resolves the conflict via Last-Writer-Wins and both users converge on the same content.

### Test 3: Three Concurrent Users

1. Start `user_1`, `user_2`, and `user_3`.
2. Have each user edit a different line.
3. Make changes in quick succession.
4. Confirm all changes propagate and all documents remain synchronized.

## Troubleshooting

**`mq_open: No such file or directory`**
The message queue filesystem is not mounted. Set it up with:

```bash
sudo mkdir -p /dev/mqueue
sudo mount -t mqueue none /dev/mqueue
```

**`User already registered`**
A previous instance did not exit cleanly. Reset the user registry:

```bash
./clear_users
```

**Changes not syncing**
Confirm at least 5 changes have been made (broadcasts are batched in groups of 5), and verify both editors are running and appear in each other's "Active users" list.

**File permission errors**
Verify the `.txt` document files are writable and that you're running commands from the correct directory.

### Manual Cleanup

If the system becomes stuck or you want to start fresh:

```bash
# Remove build artifacts and test files
make clean

# Clear the user registry
./clear_users

# Manually remove message queues if needed
rm -f /dev/mqueue/queue_*
```

## Project Structure

```
editor.c           Main program: file monitoring, thread management, merge coordination
file_utils.c        File I/O and change detection
user_registry.c      Shared memory for active users
message_queue.c      POSIX message queue operations
crdt_merge.c         Conflict detection and resolution
display.c            Terminal display
clear_users.c        Utility to reset user registry
Makefile             Build rules
```
