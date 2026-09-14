# P2P Distributed File Sharing System — AOS Assignment 3

A tracker-coordinated, peer-to-peer file sharing system in C++17. Two tracker
servers keep replicated metadata (users, groups, file/piece hashes, peer
locations); actual file bytes move directly between clients, piece by piece,
each piece SHA1-verified on arrival.

## 1. Compilation

Requires g++ with C++17 and pthreads (tested with g++ 13, Ubuntu 24.04). No
external libraries are used — SHA1, framing, etc. are all implemented from
scratch, per the assignment's constraints.

```bash
make            # builds tracker/tracker and client/client
make clean      # removes both binaries
```

or build each independently: `cd tracker && make`, `cd client && make`.

## 2. Running

```bash
# Trackers (tracker_no is 1-based; tracker_info.txt lists "<ip> <port>" per tracker)
./tracker/tracker tracker_info.txt 1
./tracker/tracker tracker_info.txt 2

# Clients (<IP>:<PORT> is THIS client's own peer-server address — see §7)
./client/client 127.0.0.1:6001 tracker_info.txt
./client/client 127.0.0.1:6002 tracker_info.txt
```

`tracker_info.txt` at the repo root is a whitespace-separated list of
`<ip> <port>` pairs, one tracker each — grading uses exactly two, per the
spec. Type `quit` at a tracker's console to shut it down; type `exit` at a
client to quit it (this also logs it out, which stops it seeding).

### 2.1 Client CLI commands — what you actually type

These are typed at the client's `>` prompt, exactly as in the spec. **You
never type your own user_id** — the client remembers who you're logged in
as (from `login`) and fills it in wherever the tracker needs it, for every
command below. This is the one thing worth double-checking if a command
comes back `ERR Unknown command or wrong number of arguments`: the fix is
almost always "drop the user_id you added," not "add one."

(§7.1 further down documents the *internal* tracker wire protocol, which
*does* carry `user_id` explicitly on the wire — that's what the client
sends on your behalf after reading the command below; it is not what you
type.)

| Type this | Example | Notes |
|---|---|---|
| `create_user <user_id> <password>` | `create_user alice pw123` | |
| `login <user_id> <password>` | `login alice pw123` | your peer address (given as `<IP>:<PORT>` on the command line) is sent along automatically |
| `logout` | `logout` | no args |
| `create_group <group_id>` | `create_group cs6006` | you become the owner |
| `join_group <group_id>` | `join_group cs6006` | sends a request; owner must `accept_request` |
| `leave_group <group_id>` | `leave_group cs6006` | owner cannot leave their own group |
| `list_groups` | `list_groups` | no args; works even if not logged in |
| `list_requests <group_id>` | `list_requests cs6006` | owner only |
| `accept_request <group_id> <user_id>` | `accept_request cs6006 bob` | owner only — the *other* user's id, not yours |
| `upload_file <group_id> <file_path>` | `upload_file cs6006 ./notes.pdf` | must already be a group member |
| `list_files <group_id>` | `list_files cs6006` | must be a group member |
| `download_file <group_id> <file_name> <destination_path>` | `download_file cs6006 notes.pdf ./dl/notes.pdf` | `file_name` is the *basename* the uploader shared it as, not a path; runs in the background — check `show_downloads` |
| `show_downloads` | `show_downloads` | no args |
| `stop_share <group_id> <file_name>` | `stop_share cs6006 notes.pdf` | |
| `exit` | `exit` | quits the client; logs out first if logged in |

## 3. Architecture overview

```
                     ┌──────────────┐   tracker<->tracker   ┌──────────────┐
   clients  <──TCP──>│  Tracker 1   │<══ sync mesh + HB ═══>│  Tracker 2   │<──TCP──> clients
                     └──────────────┘  (full-mesh replica)  └──────────────┘
        │                                                                      │
        └───────────────────────── TCP, direct piece transfer ─────────────────┘
                                  (client <-> client)
```

- **Trackers** hold all metadata: user accounts, group membership/requests,
  per-file piece hashes, and which peers currently have which pieces. They
  never see file bytes.
- **Clients** do the actual file I/O: split/hash files into 512KB pieces on
  upload, request pieces from other clients' peer servers on download, and
  run their own peer server to seed pieces back out.
- Every socket in the system (tracker↔client, tracker↔tracker, client↔client)
  uses the same length-prefixed framing (`common/netio.h`), so partial
  reads/writes are handled once, centrally, instead of per call site.

### Code layout

```
common/            shared, dependency-free building blocks
  sha1.h             streaming SHA1 (RFC 3174), used for piece + whole-file hashes
  netio.h            [4-byte length][payload] framing over TCP
  protocol.h         shared constants (piece size, ports, timeouts)
  utils.h            tokenizing, tracker_info.txt parsing, bitmap<->hex, logging
tracker/
  tracker.cpp        state, replication/election mesh, client-facing command handling
client/
  client.cpp         CLI loop, translates spec commands to wire commands
  tracker_client.h   connection to "whichever tracker is current", failover + redirect
  fileops.h          piece splitting/hashing, local "what do I have" file registry
  peer_server.h      serves GET_PIECE requests, tit-for-tat upload admission (§6.2)
  reciprocity.h      "who has sent me pieces" ledger backing tit-for-tat
  downloader.h       multi-peer rarest-first download manager, resume (§6.1), show_downloads state
```

Both binaries are single translation units (`tracker.cpp` / `client.cpp`)
including these headers, so `g++ file.cpp -o binary` is all the Makefiles do
— matching the simple build spec's example commands imply.

## 4. Tracker replication & failover (§7 of the spec)

**This is deliberately generalized to N trackers**, not hardcoded to two —
`tracker_info.txt` is just a list, and the election/replication code loops
over it. Grading uses exactly two entries (as the spec requires); scaling up
later is adding a line to the file and starting another tracker process (see
§4.4) — no code changes.

### 4.1 Election

Trackers form a full mesh (one TCP "sync" link per pair, on `client_port +
100`) and exchange a 1-second heartbeat over each link. A tracker computes
its role independently and continuously:

> **PRIMARY** = the tracker with the lowest configured index among those it
> currently sees as alive; everyone else is **SECONDARY**.

This is a simple static-priority rule: easy to reason about, and it means a
low-index tracker that comes back online reclaims PRIMARY automatically. A
link is declared dead if no frame (heartbeat or otherwise) arrives for 4
seconds, or immediately if the TCP connection itself errors/closes (which is
what actually happens when a tracker process is killed — the kernel tears
down its sockets, so failover in practice is detected in about a second, not
the full 4-second timeout).

Only the PRIMARY accepts state-**mutating** commands (create_user, login,
create_group, ...). A SECONDARY replies `ERR NOT_PRIMARY <ip> <port>`
instead of guessing — and clients act on that immediately (§4.3). Read-only
queries (list_groups, list_files, list_requests, get_peers) are answered by
*any* tracker straight from its own replicated state — this is what makes
"clients get accurate info regardless of which tracker they connect to"
(spec §7) hold without forwarding every read through the primary.

### 4.2 Replication

Every mutation the PRIMARY accepts is applied locally and immediately pushed
to each connected peer as one `OP <command line>` message — cheap and
low-latency, since the command line already carries the resolved actor's
`user_id` explicitly (no per-connection session state needs replicating).

To recover anything a peer *missed* (a link that was down for a while),
every tracker keeps a monotonically increasing `stateVersion` counter,
bumped once per accepted/applied mutation. Whenever a link (re)connects, both
sides immediately exchange a full state snapshot tagged with their current
version; whichever side is behind **wholesale-adopts** the other's snapshot.

This is intentionally simpler than log-replay-with-deduplication: because
only the current PRIMARY ever originates new state, "higher version" always
means "strictly more complete history" — there's no independent history on a
secondary that a snapshot adoption could clobber, even across a primary
handoff (a returning ex-primary has a *lower* version than whoever kept
serving clients while it was down, so *it* is the one that gets overwritten
on reconnect, not the other way around). See `tracker.cpp`'s top comment and
`mergeSnapshotIfNewer` for the detailed reasoning.

### 4.3 What a client sees during a failover

`TrackerSession` (client/tracker_client.h) keeps exactly one active tracker
connection. On a dead socket *or* an `ERR NOT_PRIMARY` redirect, it
transparently: reconnects (to the next reachable tracker, or to the redirect
target), silently replays `LOGIN` with cached credentials if it had a
session, and retries the original command — all inside one call, invisible
to the user. This is what lets `create_group`/`upload_file`/etc. keep
working across a tracker crash with zero user action; verified in testing
(§9) by killing the primary mid-session and watching the next command
succeed against the survivor.

### 4.4 Adding a tracker later

Add its `<ip> <port>` to `tracker_info.txt`, then **restart the tracker
fleet** (existing processes + the new one) pointed at the updated file —
each tracker only reads the file once at startup, so a *running* tracker
won't discover a new peer without being restarted itself. Because state
converges automatically via the snapshot mechanism above, this is a brief,
coordinated restart with no data loss, not a hot/zero-downtime add — that
would need a config-reload mechanism (e.g. SIGHUP re-reading the file) that
wasn't implemented, given it's not something the spec's "exactly two
trackers, at least one always up" requirement actually calls for. Verified
with a 3-tracker fleet in testing (§9): full mesh forms, election and
replication behave identically with N=3.

## 5. File pieces & integrity (§3.1, §6)

Files are split into 512KB pieces (`PIECE_SIZE` in `common/protocol.h`), the
last piece however many bytes remain. `computeFileMeta` (client/fileops.h)
streams the file through one 512KB buffer, hashing each piece **and** the
whole file in a single pass with two SHA1 contexts — so hashing a 1GB file
never holds more than one piece in memory. On download, every piece is
verified the moment it arrives (before it's written to disk or counted as
"have"); a bad piece is silently discarded and re-requested from a different
peer (see `fetchPiece` in downloader.h). The completed file gets one more
whole-file hash check before being renamed from `<dest>.part` into place.

SHA1 itself (`common/sha1.h`) is implemented from scratch (RFC 3174,
streaming update/finalize) rather than linked against a crypto library, to
avoid any "external library" ambiguity and keep the build to a bare `g++
file.cpp` — verified against the standard test vectors (`""`, `"abc"`, and
one million `'a'`s) during development.

## 6. Piece selection & multi-peer download (§6, §3.3)

`download_file` first does a synchronous `GET_PEERS` query (so a bad
group/file name reports an error immediately), then hands the transfer to a
background thread — `show_downloads` reports progress while the CLI prompt
stays responsive, and multiple `download_file` calls for different files run
independently and concurrently, each itself pulling from multiple peers at
once (spec §3.3).

Piece selection is **rarest-first**: at the start of a download, piece
rarity is computed from the seeder bitmaps the tracker returned, and pieces
are queued rarest-first. A small worker pool (up to 4 threads) drains that
queue; for each piece, a worker builds the list of currently-viable peers
(have that piece, haven't already failed it this pass) and picks one via a
shared round-robin counter rather than always taking the first match — with
more than one full-file seeder this is what actually spreads the six/sixty/
six-hundred pieces across them instead of every worker piling onto the same
one peer. A piece that fails against every currently-known peer is retried
once after a single fresh `GET_PEERS` (in case a new seeder appeared); if it
still can't be obtained, the download is reported failed with exactly which
pieces are missing rather than silently hanging.

Rarest-first was chosen (over e.g. purely sequential) because it's the
standard answer to "avoid unnecessary duplicate downloads" and "efficient
utilization of available peers" (§6): it spreads demand across the whole
swarm instead of every downloader converging on the same popular piece.

**Seeing the multi-peer behavior happen**: every piece that arrives prints a
line naming exactly who it came from —

```
[Download] [g1] big.bin piece 2/6 <- alice (127.0.0.1:16031)  [1/6 done]
[Download] [g1] big.bin piece 1/6 <- bob (127.0.0.1:16032)    [2/6 done]
```

— and `show_downloads` tallies it per peer for both in-progress and
completed downloads (`DownloadStatus::piecesFromPeer` /
`peerBreakdown()` in downloader.h):

```
[D] [g1] big.bin 4/6 pieces (from: alice:2, bob:2)
[C] [g1] big.bin
      pieces came from: alice:3, bob:3
```

(The `[C] [group_id] filename` line itself is kept byte-for-byte per the
spec's required completion format §8 — the breakdown is an extra line right
after it, not appended to it.)

### 6.1 Resuming an interrupted download

If `download_file` is re-issued for the same `(group, file, destination)`
after a previous attempt was cut short (client killed, crashed, network
died), it doesn't start over. Before fetching anything, `resumeFromExistingPart`
(downloader.h) checks whatever bytes are already sitting in `<dest>.part`
piece-by-piece against the expected hash and marks anything that verifies as
already `have` — only the genuinely missing pieces go into the fetch queue.
Verified pieces are also registered immediately (before the rest of the
download even starts), so this client can re-seed them to others right away
instead of waiting for the whole file to finish again.

This is safe by construction rather than by tracking any extra "was this a
clean download" bookkeeping: every recovered piece is independently
re-hashed against the *current* tracker-provided hash before being trusted,
so a `.part` file that's actually leftover garbage, or belongs to some
different content that happened to reuse the same path, simply verifies
zero pieces and the download proceeds exactly like a fresh one. Recovered
pieces show up in `show_downloads`'s per-source breakdown as `(resumed):N`,
alongside whichever live peers supplied the rest — see §9 for a test run
that pre-seeds half a file and confirms only the missing half is re-fetched.

### 6.2 Tit-for-tat upload admission

Every client is also asked to serve pieces to others, and right now nothing
stops a peer who never uploads anything back from consuming just as much of
a seeder's capacity as one who does — the classic BitTorrent free-rider
problem (see `bittorrentecon.pdf`). `peer_server.h` addresses this with a
small two-tier admission scheme rather than serving every request
unconditionally:

- `GENERAL_UPLOAD_SLOTS` (3) concurrent piece transfers are open to *any*
  requester — a brand-new peer we've never dealt with can always compete for
  one, so nobody is locked out from the start (a simplified version of
  BitTorrent's "optimistic unchoke").
- Once those are full, `RECIPROCATOR_BONUS_SLOTS` (2) more open up, but only
  to requesters who have themselves sent *this* client at least one piece
  before (`client/reciprocity.h` — a small in-memory, per-process ledger of
  "who has given me pieces", checked via `isReciprocator`).

A request that finds every eligible slot full gets `ERR BUSY` back
immediately (no queueing, no blocking) — which the requester's download
manager treats exactly like any other failed attempt: it just retries a
different peer. `GET_PIECE` now carries the requester's `user_id` (§7.2) so
the serving side has someone to look up.

This was checked two ways beyond the functional tests in §9: a standalone
unit test (`acquireUploadSlot`/`releaseUploadSlot` in isolation, not
through any socket) asserting slot admission never exceeds either cap and
always returns to zero after release, including under 50 threads × 200
acquire/release cycles each; and the same stress run under ThreadSanitizer,
which reported no data races on the atomic slot counters.

## 7. Network protocol

Every socket uses the same framing (`common/netio.h`):
`[4-byte big-endian length][payload bytes]`. A frame's payload is opaque —
the same primitive carries a text command line or a raw 512KB piece; sends
and receives loop until the whole frame is written/read (or the socket
fails), so partial transmissions never leak into application logic.

### 7.1 Tracker ↔ client

**This is the internal wire protocol, not what you type** — see §2.1 for
the actual CLI commands. One command per frame: `<COMMAND> <args...>`.
Mutating commands always carry the acting `user_id` explicitly as an
argument, which the client fills in from its own session rather than the
user typing it — this is also exactly the string a tracker forwards to its
peers for replication, so no separate wire format is needed for that.
Response frames start with `OK`/`ERR` followed by a message (which may be
multi-line for list-style responses).

| Command | Args | Notes |
|---|---|---|
| `CREATE_USER` | user_id password | |
| `LOGIN` | user_id password peer_ip peer_port | records where to reach this user's peer server |
| `LOGOUT` | user_id | also removes this user from every file's seeder list |
| `CREATE_GROUP` / `JOIN_GROUP` / `LEAVE_GROUP` | user_id group_id | |
| `LIST_GROUPS` | — | `group_id owner member_count` per line |
| `LIST_REQUESTS` | user_id group_id | owner-only |
| `ACCEPT_REQUEST` | user_id group_id target_id | owner-only |
| `REGISTER_FILE` | user_id group_id file_name size num_pieces file_hash csv_piece_hashes | uploader becomes the first full seeder |
| `LIST_FILES` | user_id group_id | member-only; `name size num_pieces seeder_count` per line |
| `GET_PEERS` | user_id group_id file_name | member-only; size/hashes + `user_id ip port bitmap_hex` per seeder |
| `UPDATE_SEED` | user_id group_id file_name bitmap_hex | OR-merged into the seeder's bitmap |
| `STOP_SHARE` | user_id group_id file_name | |

Everything except `LIST_GROUPS`, `LIST_REQUESTS`, `LIST_FILES` and
`GET_PEERS` mutates replicated global state, so those are PRIMARY-only; a
SECONDARY answers `ERR NOT_PRIMARY <ip> <port>` instead. The four read-only
queries are answered by whichever tracker gets asked (see §4.1) — they
still require the caller to be logged in (and, for `LIST_FILES`/
`GET_PEERS`, a member of the group), checked against the replicated
`isLoggedin`/membership state rather than any per-connection session.

### 7.2 Client ↔ client (peer protocol)

```
request:   GET_PIECE <requester_user_id> <group_id> <file_name> <piece_index>
response:  OK <piece_index> <length> <sha1hex>     (a second frame with <length> raw bytes follows)
       or: ERR <reason>                            (includes "ERR BUSY ..." - see §6.2 - not just "not available")
```

`requester_user_id` exists purely so the serving side can credit the
requester for tit-for-tat (§6.2); it isn't otherwise authenticated by the
peer being asked.

### 7.3 Tracker ↔ tracker (sync mesh)

Each frame is `<TYPE>\n<body>`: `HELLO <idx>` (identify on connect),
`SNAPSHOT <version>\n<records>` (full-state catch-up, see §4.2),
`OP <command line>` (single live-propagated mutation), `HB` (heartbeat).

## 8. Data structures & rationale

- **Tracker**: `unordered_map<user_id, User>`, `unordered_map<group_id,
  Group>`, `unordered_map<group_id, unordered_map<file_name, FileInfo>>` — a
  single `stateMutex` guards all three. Given the assignment's tested scale
  (a handful of users/groups/files, ≤3 clients), one coarse lock kept the
  replication logic (which needs a consistent, atomic view of "all of
  users+groups+files" for snapshotting) simple and obviously correct, rather
  than chasing fine-grained-locking bugs for a performance win nothing in
  the spec asks for.
- `FileInfo.seeders` is `unordered_map<user_id, vector<bool>>` — a per-user
  piece bitmap, OR-merged on `UPDATE_SEED` so replay/redelivery is naturally
  idempotent (a property leaned on throughout — see §4.2).
- **Client**: `LocalFileRecord` (client/fileops.h) is the one struct shared
  between the CLI, the peer server, and the download manager for "what
  bytes do I actually have for this (group, file)" — each record has its
  own mutex so a download thread writing pieces doesn't race the peer
  server reading them for another client's request.
- Piece bitmaps travel over the wire as hex (4 bits/char) rather than, say,
  a list of indices, since it's compact, fixed-format, and trivial to
  OR-merge or intersect.

## 9. Testing performed

- **SHA1 correctness**: standalone check against the standard test vectors
  (empty string, `"abc"`, one million `'a'`s) and a streaming/chunk-boundary
  case, all matching.
- **End-to-end flow**: two trackers + two clients — create_user, login,
  create_group, upload_file (a 1.3MB / 3-piece file), join_group,
  list_requests, accept_request, list_files, download_file, show_downloads.
  Downloaded file's SHA1 matched the original exactly; the required `[C]
  [group_id] filename` line appeared on completion.
- **Tracker failover**: killed the primary tracker mid-session (`kill -9`,
  simulating a crash) between two client commands. The secondary promoted
  itself to primary within about a second (visible in its log), and the
  client's very next command succeeded transparently — no error surfaced,
  no manual reconnect needed.
- **N=3 tracker mesh**: restarted the fleet with a third tracker added to
  `tracker_info.txt`; confirmed all three pairwise links form, and election
  picks the lowest-index tracker as primary, exactly as with two.
- **Multi-seeder distribution**: three clients, one file — alice uploads,
  bob downloads from alice (becoming a second full seeder), then carol
  downloads with both alice and bob available. Carol's per-piece log showed
  a genuine 3/3 split across both seeders (interleaved, not batched), and
  `show_downloads` reported the same breakdown; this is what caught and
  fixed a real bug where piece assignment always picked the same first
  matching peer instead of spreading load (§6).
- **Resumable download** (§6.1): pre-seeded a `.part` file with exactly the
  first half of a 6-piece file's bytes (bypassing the app so the test is
  deterministic rather than racing a real kill against a fast loopback
  transfer), then issued `download_file` for that destination. Log
  confirmed "resuming ... 3/6 piece(s) already verified", only pieces 4-6
  were fetched from the network, and the final file's SHA1 matched the
  original exactly.
- **Tit-for-tat admission** (§6.2): a standalone unit test exercising
  `acquireUploadSlot`/`releaseUploadSlot` directly (general-slot filling,
  bonus-slot-for-reciprocators-only, exhaustion of both, and a 50-thread ×
  200-iteration stress run) passed, both in a plain build and under
  ThreadSanitizer (no data races reported). Worth noting honestly: the
  stress test's *first* version had a wrong assertion — it read the
  internal gate counter mid-flight as if it equaled "current holders",
  which transiently overshoots by design before self-correcting (see the
  comment in the test) — and failed on that basis, not on an actual app
  bug. Fixed the measurement (a dedicated holder counter, incremented only
  on a true admission) rather than loosening the assertion, and it now
  passes cleanly.
- **60 concurrent clients**: 60 separate client processes hitting one
  tracker pair at once — each creating its own user, logging in, creating
  a group, listing all groups, and logging out, all launched in the same
  instant. All 60 succeeded with zero errors, and every single one's
  `list_groups` showed the exact same 60 groups — proof the tracker's
  single `stateMutex` (§8) serializes concurrent writes correctly with no
  lost or torn updates, not just that it doesn't crash. See §12 for what
  this does and doesn't say about capacity.
- **8 concurrent downloaders, 1 seeder**: 8 clients joined a group and
  issued `download_file` for the same 20MB/39-piece file from a single
  seeder at once — 3 more than that seeder's 5 upload-slot cap (§6.2). All
  8 completed successfully with correct hashes. Notable honest result: this
  didn't actually observe the slot cap reject anything (`ERR BUSY` never
  fired) — loopback piece transfers are fast enough that 8 independent
  processes' requests didn't reliably stack up 6+ deep at the same instant,
  even retried at larger file sizes. The admission logic itself is
  separately verified correct under real forced contention by the §6.2 unit
  test + ThreadSanitizer run; this run instead demonstrates that exceeding
  the design capacity degrades gracefully (retry) rather than failing.
- Not yet exercised in this pass: a 1GB file end-to-end (piece-level logic
  is size-independent and was verified up to 20MB; §5 covers why memory
  usage stays flat regardless of file size), and a mid-download peer
  disconnect (the retry-against-a-different-peer path is implemented per
  §6 but wasn't forced with an actual killed seeder). Recommended before
  final submission: run both explicitly, plus `valgrind --leak-check=full`
  given the spec's explicit penalty for leaks.

## 10. Assumptions & known limitations

- `user_id`, `group_id`, and `file_name` may not contain whitespace or
  commas (enforced by `isValidId`) — they're used as tokens in the wire
  protocol and as CSV-style fields in the tracker's snapshot format.
  `upload_file` uses the uploaded file's basename as `file_name`.
- A client's authenticated identity for mutating commands is whatever
  `user_id` it puts in the command — enforced by the tracker only insofar
  as that user must currently be `LOGIN`-ed (a global, replicated flag).
  There's no per-connection secret binding a socket to "the user who
  actually typed the password there" beyond that; the CLI itself is the
  only thing constructing these commands, so this isn't reachable from the
  documented command set. Out of scope given the assignment doesn't specify
  a threat model beyond the listed commands.
- A client begins seeding a file to others once it's *fully* downloaded and
  verified (one `UPDATE_SEED` at completion), not incrementally as pieces
  arrive — keeps tracker chatter down given the tested scale (≤3 clients).
  Extending to periodic partial-bitmap updates during a download would be a
  straightforward addition (`UPDATE_SEED` already supports arbitrary
  partial bitmaps; only the download manager would need to call it more
  often).
- If a seeder disconnects uncleanly (crash, not `logout`/`stop_share`), the
  tracker won't know until that user next logs out or is queried; a
  downloader that picks a now-dead peer simply times out (5s socket
  timeout) and retries a different one — correct, just not instant.
- Passwords are stored in memory as given, unhashed — acceptable for an
  in-memory, single-course-assignment system with no stated security
  requirement beyond basic login; would need hashing for anything real.
- Peer addresses are plain `ip:port` as given on the command line — for
  multi-machine testing, pass a real reachable IP, not `localhost` (every
  machine's `localhost` means itself).
- The reciprocity ledger (§6.2) is per-process, in-memory state — it starts
  empty on every client restart, isn't shared across a client's multiple
  simultaneous downloads/uploads beyond the one shared map, and doesn't
  persist or replicate anywhere (deliberately: it's a local trust signal
  between peers, not tracker state). At the assignment's tested scale (≤3
  clients) `GENERAL_UPLOAD_SLOTS` alone comfortably covers every request in
  practice, so the bonus-slot path was verified with a direct unit test
  (§9) rather than by actually saturating a live client's upload capacity
  over the network — the admission logic itself is exercised for real on
  every piece transfer either way, just never past the point of contention
  in the end-to-end runs.
- A resumed download (§6.1) only recognizes pieces that are still bit-exact
  on disk from the *same* destination path; it has no separate manifest, so
  if the `.part` file is deleted or edited outside the app there's nothing
  to detect that beyond re-verification naturally finding those pieces
  invalid (safe, just falls back to re-fetching them).

## 11. Capacity & concurrency limits

Concrete answers, not just "it depends" — what's a hard limit in the code
versus what's bounded only by the OS, with the actual numbers measured on
this dev machine (§9) as a reference point.

**How many clients can be connected to a tracker at once?**
No cap in the code. Every accepted connection gets its own thread
(`thread(handleClient, fd).detach()` in tracker.cpp) and there's no limit on
how many can be accepted. Measured: **60 simultaneous clients** against one
tracker pair, all succeeding with fully consistent state (§9). Beyond that
it's purely OS resource limits — mainly file descriptors (one per client
connection): a common Linux default of `ulimit -n 1024` would cap a tracker
process at roughly ~1000 concurrent client connections before `accept()`
starts transiently failing (which the code just retries, not crashes on);
this dev machine's configured limit is 1,048,576, so on a typical grading
machine expect somewhere in the hundreds to low thousands, not 1024 exactly.
None of this is a designed ceiling — raising `ulimit -n`/`-u` raises it
further with no code changes. The spec's own testing scale is 3 clients, 2
trackers, which this has enormous headroom over.

**How many clients can download at once?**
Also no code-level cap on simultaneous *downloads* — each `download_file`
spawns its own detached worker thread, so a client can have as many
downloads running as `download_file` calls it's issued (bounded only by the
same OS thread/fd limits as above). Each individual download itself uses up
to `min(4, number_of_seeders_for_that_file)` concurrent peer connections
(§6) — more workers than available seeders wouldn't help.

What *is* a deliberate, tunable cap: how many pieces **one seeder** will
serve out at the same instant — `GENERAL_UPLOAD_SLOTS` (3) +
`RECIPROCATOR_BONUS_SLOTS` (2) = **5 concurrent piece transfers per
seeder** (§6.2, `common/protocol.h`). A 6th simultaneous requester gets
`ERR BUSY` and its download manager retries elsewhere automatically — this
isn't a hard failure, just throttling. With multiple seeders for the same
file, total serving capacity scales as 5 × number of seeders. Measured: 8
clients downloading the same 20MB file from a single seeder simultaneously
all completed correctly (§9) — exceeding the 5-slot design capacity degrades
to "some requests retry," not "some requests fail."

**How much data is supported?**
Pieces are a fixed 512KB (spec-mandated, `PIECE_SIZE`). The spec's stated
target is files "up to 1GB", which this is built to handle efficiently with
no hardcoded ceiling: hashing (§5) and piece serving/fetching (§6) never
hold more than one 512KB piece in memory regardless of total file size, so
memory usage is flat whether the file is 1KB or 1GB — verified end-to-end up
to 20MB so far (§9), a full 1GB run is still recommended before final
submission. There is one real hard ceiling, from the wire protocol rather
than memory: a file's complete piece-hash list travels in a single frame
(`REGISTER_FILE`/`GET_PEERS`, §7.1), and frames are capped at
`MAX_FRAME_SIZE` = 16MB (`common/netio.h`, guarding against a corrupted
length prefix). At ~41 bytes per piece-hash entry, that caps a single file
at roughly **200GB** — a 1GB file uses under 0.04% of that budget, so it's
nowhere near a concern at the spec's required scale; stated here just so
"how big can a file be" has a precise answer instead of an unstated one.

## 12. Commands implemented

Every command from the spec's §4 is implemented, with the exact argument
lists in §2.1 (plus `quit` at the tracker console and `exit` at the
client).
