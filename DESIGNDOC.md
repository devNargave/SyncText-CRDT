# DESIGNDOC — SyncText 

This document explains the design of the SyncText collaborative editor in the format requested by the assignment. The goal is to show how the system works, why certain choices were made, and how correctness was verified.



## a) System Architecture

### High-level design overview
- Multiple editor processes run on the same Linux machine.
- Each process maintains a local file: `<user_id>_doc.txt`.
- Edits are detected locally, batched, and broadcast to others.
- Updates from others are merged with local changes using CRDT (Last-Writer-Wins).
- No locks are used; only atomic variables coordinate threads.

#### What happens during an edit
- When a user types and saves, the process is intentionally simple and predictable:
- The main thread notices the file’s modification time changed during its next 200ms poll.
- It reads the whole file, computes deltas at a line and sub-line range level, and appends `Update` objects to a local buffer.
- Until there are 5 local updates, nothing is sent; this batching keeps IPC traffic low and respects the assignment constraint.
- Once the threshold is reached, the editor sends the buffered updates via POSIX message queues to every other active user it discovers in the shared registry.
- On the receiving side, a listener thread accumulates incoming updates. As soon as 5 updates are received, the main thread performs a merge that includes both received and any unbroadcast local updates. The LWW policy deterministically chooses winners, applies them, writes the file, and refreshes the display.
- Because all editors follow the same rules, everyone converges to the same state after each merge round.



### Components and Responsibilities
- `editor.c` — Main loop, file monitoring, batching, merge orchestration, display refresh.
- `message_queue.c` — POSIX message queue creation and binary send/receive of `Update`.
- `user_registry.c` — Shared memory registry of active users and their queue names.
- `file_utils.c` — File I/O and change detection (builds `Update` objects).
- `crdt_merge.c` — Conflict detection and LWW merge logic, apply updates to document.
- `display.c` — Terminal output: file view, active users, timestamps.

#### How the components collaborate
- The editor orchestrates timing and thresholds, calling into utilities but keeping the policy at the top level.
- The registry provides discovery. It is read often (to list active users) and written rarely (on join/leave).
- The message queue layer abstracts binary messaging and retry-once behavior to avoid blocking producers.
- The CRDT merge layer encapsulates correctness: conflict detection, total ordering via timestamp then `user_id`, and idempotent application (replace whole line).


### Key Data Structures
```c
typedef struct {
  int line_num;
  int col_start, col_end;
  char old_content[256];
  char new_content[256];
  time_t timestamp;
  char user_id[20];
  int op_type; // 0=insert, 1=delete, 2=replace
} Update;

typedef struct {
  char user_ids[5][20];
  char queue_names[5][50];
  int count;
} UserRegistry;
```

## b) Implementation Details

### File Monitoring & Change Detection
- Poll the local file with `stat()` every ~200ms.
- When `st_mtime` changes, read new content and compare with previous version.
- For each changed line:
  - Forward scan to find first differing character.
  - Backward scan to find last differing character.
  - Create `Update` with `line_num`, `col_start`, `col_end`, full `old_content` and `new_content`, `timestamp`, `user_id`, `op_type`.
- Store updates in `g_local_updates[]` and increment `g_update_count`.

Worked example:
- Before: `int x = 10;`  → After: `int x = 100;`
- First difference at col 8; backward scan trims the trailing `;`.
- Update has `line=0`, `col_start=8`, `col_end=10`, `op_type=REPLACE`, and both full line strings.

Why this approach
- Line-oriented documents are common, and line-wise scanning drastically simplifies range identification without complex diff algorithms.
- Comparing entire lines but recording the changed column span achieves a good middle ground between coarse (whole-file) and fine-grained (character-by-character) diffs.
- Storing the full pre/post line strings keeps application idempotent and tolerates reordering.

Edge considerations
- Insertions at end-of-line present as a small range at the tail; deletions present as a range where `new_content` is shorter.
- Full-line deletion is encoded as `REPLACE` with empty `new_content`, which the applier treats as setting the line to empty.

### Batching & Broadcasting (N = 5)
- After `g_update_count >= 5`, broadcast the buffered local updates to all other users.
- Uses POSIX message queues; each user has `/queue_<sanitized_user_id>`.
- Message size is `sizeof(Update)`; send the struct as binary to avoid parsing issues.
- Non-blocking `mq_send` with a single retry after a short delay (~10ms) if needed.
- After sending, clear the local buffer and reset `g_update_count`.

Why batching matters
- Batching amortizes IPC overhead and avoids “message storms” during active typing.
- The retry-once policy provides light backpressure resistance without risking a deadlock.
- Keeping messages binary and fixed-size ensures constant-time enqueues and predictable queue depth usage.

### Listener Thread
- Runs in parallel with the main thread.
- Uses `mq_timedreceive()` with a small timeout to avoid blocking.
- Pushes incoming `Update` structs into `g_received_updates[]`.
- When `g_received_count >= 5`, sets an atomic flag prompting the main thread to merge.

Non-blocking rationale
- Timed receive plus short timeouts keeps the listener responsive to shutdown signals and avoids starving the main loop.
- The single-writer rule per buffer eliminates the need for mutexes while ensuring memory safety.

### Shared Memory (User Registry)
- Structure: a compact array-based registry with bounded capacity (`MAX_USERS=5`), holding `user_id` and corresponding POSIX queue name.
- Purpose: discovery of peers to broadcast to; updated on process start/exit.
- Access pattern: writes are rare (join/leave), reads are frequent (list active users). Simple array copy avoids locks.
- Robustness: a small utility (`clear_users`) can reset the registry if state becomes inconsistent.

### CRDT Merge (LWW)
- When merging:
  1. Copy received updates and include current local updates (so local work participates in conflict resolution).
  2. Detect conflicts: same `line_num` and overlapping columns.
     - Overlap test: `!(u1.col_end < u2.col_start || u2.col_end < u1.col_start)`.
  3. Resolve conflicts with Last-Writer-Wins:
     - Newer `timestamp` wins.
     - If timestamps tie, lexicographically smaller `user_id` wins.
  4. Apply winners by replacing the entire line with `new_content`.
  5. Write merged lines to file and refresh display.
- Clear only the received buffer after merge (local buffer preserved until broadcast).

```c
static inline int overlap(int a0, int a1, int b0, int b1) {
  return !(a1 < b0 || b1 < a0);
}

static int newer(const Update* a, const Update* b) {
  if (a->timestamp != b->timestamp) return a->timestamp > b->timestamp;
  return strcmp(a->user_id, b->user_id) < 0; // tie-breaker
}

static const Update* lww_pick(const Update* u1, const Update* u2) {
  return newer(u1, u2) ? u1 : u2;
}
```

Why LWW is acceptable here
- The assignment emphasizes convergence and determinism over intent preservation. LWW delivers both with low complexity.
- Replacing whole lines avoids partial interleavings and makes convergence easy to reason about across processes.
- Ties broken by `user_id` remove non-determinism from identical timestamps.

### Threading & Lock-Free Coordination
- Two threads per process:
  - Main thread: file monitoring, batching, broadcasting, and merging.
  - Listener thread: receives updates and fills `g_received_updates[]`.
- No mutexes. Single-writer rule per buffer and atomic variables (`g_running`, `g_update_count`, `g_merge_flag`).

Memory-ordering notes
- Atomics gate state transitions (e.g., thresholds, shutdown) while buffers adhere to a single-writer discipline.
- Display reads are eventually consistent; the system favors progress and convergence over instantaneous views.

## c) Design Decisions

1) Polling with `stat()` instead of inotify
- Easy to implement and portable for the assignment.
- The overhead is small in this context.

2) Batch size N = 5 (broadcast & merge)
- Matches the assignment requirement exactly.
- Reduces message overhead while remaining responsive enough.

3) Binary serialization for message queues
- Avoids delimiter issues and parsing errors.
- Faster and simpler than text.

4) Replace entire line on apply
- Ensures deterministic convergence across users.
- Simplifies conflict resolution application.

5) Lock-free via atomics
- Satisfies “no locks” requirement and avoids deadlocks.

### How we ensured lock-free operation
- Single-writer discipline: main thread exclusively writes local buffers; listener exclusively writes received buffers.
- Atomics only for counters/flags (`g_running`, `g_update_count`, `g_merge_flag`) to coordinate thresholds and shutdown.
- Non-blocking MQ operations with short timeouts prevent deadlocks and keep threads responsive.
- Idempotent apply (whole-line replace) reduces shared-state complexity during merges.

## d) Challenges and Solutions

1) Conflict detection edge case
- Issue: Adjacent ranges were flagged as conflicts.
- Fix: Use `<` (strict) in overlap check, so only true overlaps conflict.

Details
- Context: When two users edited disjoint but adjacent spans on the same line, the initial overlap predicate treated them as conflicting, causing unnecessary LWW resolution.
- Symptom: One user’s adjacent change was occasionally discarded despite no real overlap.
- Root cause: The overlap predicate used `<=` semantics, collapsing adjacency into overlap.
- Validation: Unit-like tests on synthetic spans, plus multi-user manual tests with adjacent edits, confirmed both updates are retained.
- Residual risk: If editors disagree on column indices due to different encodings, adjacency could be misclassified; using consistent UTF-8 indexing mitigates this.

2) Text-based message format broke on special characters
- Issue: Pipe-delimited parsing failed when content contained `|`.
- Fix: Switched to binary send/receive of `Update` structs.

Details
- Context: Early prototype serialized updates as `user|line|start|end|content...`.
- Symptom: Lines containing delimiters, newlines, or multibyte characters corrupted parsing.
- Root cause: Escaping and delimiter handling were incomplete; newline-in-content is especially tricky.
- Validation: Fuzz-like tests with random ASCII and UTF-8 payloads showed no corruption using fixed-size binary structs.
- Residual risk: Struct packing/ABI differences across compilers; mitigated by building with the same toolchain and using explicit field sizes.

3) Incorrect batching/merge timing
- Issue: Broadcast/merge triggered too early (not at N=5).
- Fix: Enforced N=5 thresholds for both; broadcast happens before clearing local buffer.

Details
- Context: Initial logic mixed counters for local and received updates, causing premature triggers.
- Symptom: Peers received fewer than 5 updates; merges executed without full batches.
- Root cause: Counter reuse and off-by-one checks in the threshold comparisons.
- Validation: Instrumented logs around thresholds; repeated tests confirmed exact-at-5 behavior.
- Residual risk: If updates are dropped by MQ due to full queues, peers may merge with fewer net-new operations; this is acceptable under non-blocking semantics.

4) Local edits lost during merges
- Issue: Clearing local buffer during merge erased pending edits.
- Fix: Preserve local buffer/counter across merges; include local edits in conflict resolution.

Details
- Context: Merge originally assumed only remote updates mattered; local work-in-progress was inadvertently discarded.
- Symptom: A user could lose unbroadcast edits after a remote-triggered merge.
- Root cause: Clearing the local buffer as part of the merge step.
- Validation: Regression tests where local changes existed during remote merges; verified that post-fix both sets participate and converge.
- Residual risk: A local change may still lose to a later timestamp from remote (by design under LWW), but it will not silently disappear.

5) Misleading “Last updated” time
- Issue: Displayed wall-clock refresh time, not file change time.
- Fix: Track and display actual file modification time.

Details
- Context: UI originally reported the display redraw time, which could occur without any file change.
- Symptom: Users believed new merges happened when only the UI refreshed.
- Root cause: Using the display loop’s clock rather than the file’s mtime for the status line.
- Validation: Cross-checking stat-reported mtime with edit and merge events; the timestamp now only changes on real writes.
- Residual risk: mtime granularity on some filesystems can be coarse; this only affects display fidelity, not correctness.
