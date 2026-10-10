# uConnect app protocol

This is the wire contract between uConnect apps: the Qt desktop app in this
repository and the separately maintained Flutter mobile app. An app that
follows this document can talk to any other, whatever language it is
written in.

Revision: **storage 1.3, sync 1.0, screen/camera 1.0, display 1.0** (2026-10). Changes are listed at the end.

The words MUST, SHOULD and MAY are used as in RFC 2119.

## 1. Transport

Devices join a *protected topic* through the uconnect library
(<https://github.com/AbdoWise-z/uconnect>), which gives each pair of devices
an encrypted session. Every message below is one uconnect message on that
session. Messages between two devices arrive complete, in order, and at
most once. Nothing here is sent to the rendezvous server.

A sender's queue towards a peer can be full, and a message that cannot be
queued is not sent. An offerer MUST retry its replies (`fs.list.r`,
`fs.read.r`, `fs.done`, `fs.err`, `fs.write.*`) for a while rather than
drop them. A requester MUST do the same for its cancels. A requester whose
request cannot be queued fails that request at once.

Folder sync (section 6) queues every message per peer and sends them in
order as the peer's queue allows; it drops nothing.

## 2. Envelope

Every message is `[u8 kind][payload]`:

| kind | payload |
|------|---------|
| 1 | Control: one UTF-8 JSON object, compact, with a string field `"t"` naming its type |
| 2 | Storage download data: `[u32 id, big-endian][bytes]` |
| 3 | Storage upload data: `[u32 id, big-endian][bytes]` |
| 4 | Sync block data: `[u32 id, big-endian][bytes]` (section 6) |

A receiver MUST ignore message kinds and `"t"` types it does not know. It
MUST also ignore fields it does not know. Numbers are JSON numbers:

- Request ids are unsigned 32-bit.
- Sizes and offsets are byte counts below 2^53.
- Times are milliseconds since the Unix epoch.

## 3. Session messages

As soon as a session with a peer is up, each side sends `hello`, `id` and
`perm.granted`, in that order.

### hello

```json
{"t":"hello", "v":1, "name":"Alice's Laptop", "os":"windows", "svc":["storage"]}
```

- `name`: what the device is called. Receivers SHOULD cut it to 64 characters.
- `os`: a short platform name, at most 32 characters.
- `svc`: the services the device offers: `storage`, `camera`, `screen`, `control`,
  `display` (it can be used as another device's extended display, 7.6).

Everything in it is the peer's own claim. Show it as plain text, never as markup.

### id: identity proof

Each device has a long-term Ed25519 key pair (RFC 8032). It proves that key
in every session:

```json
{"t":"id", "v":1, "key":"<64 hex chars>", "sig":"<128 hex chars>"}
```

`sig` is the Ed25519 signature over the following bytes:

```
"uconnect-desktop identity v1"  (28 ASCII bytes)
0x00
channel_binding                 (32 bytes, the uconnect session's Topic::channel_binding,
                                 the same at both ends)
public key                      (32 bytes)
```

- Hex is lowercase; receivers accept either case.
- A receiver verifies one proof per session and ignores later ones.
- Binding the proof to the session stops a device that knows the topic key from
  replaying someone else's proof.

The *fingerprint* shown to people is the first 16 bytes of
SHA-256(public key). It is written as 8 groups of 4 uppercase hex digits
separated by spaces.

### perm.granted

```json
{"t":"perm.granted", "perms":["storage.observe", "storage.read"]}
```

This tells the peer what it may do here, so its interface can show it. It
is sent again whenever that changes, for example after the peer's identity
is verified. It is informative only: every request is checked again when
it arrives.

## 4. Permissions

Each device decides what every peer may do, by the peer's verified key. A
peer without a verified key gets the default set.

| id | allows |
|----|--------|
| `storage.observe` | `fs.list`, `fs.watch` |
| `storage.read` | `fs.read` |
| `storage.upload` | `fs.write`, `fs.mkdir`, `fs.copy` |
| `storage.write` | `fs.rename`, `fs.delete`, `fs.move`, and `fs.write` with `"replace":true` |
| `camera.view`, `screen.view`, `screen.direct`, `control.input`, `control.direct` | screen, camera and control sessions (section 7) |
| `display.show`, `display.direct` | extended display requests, and accepting them without asking (7.6) |

A saved grant of `storage.download`, from an earlier version, means `storage.read`.

Folder sync does not use these ids. It has its own grants, **send** and
**accept**, per folder and per peer (6.1). They are not listed in
`perm.granted`; `sync.offers` tells a peer which folders it is sent.

## 5. Storage

### 5.1 Paths

Paths are *virtual*: a requester never learns where a shared folder really is.

- `/` lists the shared folders by name.
- `/<share>/<sub>/<file>` is a path inside one.

Path components are separated by `/`. The offerer MUST reject a component if:

- it is empty, `.` or `..`, or contains NUL;
- on Windows, it contains `\` or `:` (alternate data streams);
- on Windows, it is a device name such as `CON`, `NUL`, `COM1` or `LPT1`, with or without an extension.

### 5.2 Request ids

The requester chooses each request's `id`; ids MUST be unique among its own
unfinished requests. Each direction has its own ids, so device A's request
7 and device B's request 7 are unrelated. Uploads use kind 3 frames so
that upload and download ids never collide.

### 5.3 Errors

A failed request is answered with:

```json
{"t":"fs.err", "id":7, "code":"forbidden", "error":"human-readable text"}
```

Uploads fail with `fs.write.err` instead (5.6). `error` is for display only;
act on `code`.

| code | meaning |
|------|---------|
| `not_offered` | the device does not offer storage |
| `forbidden` | the peer lacks the permission |
| `not_found` | no such path |
| `denied` | the path resolves outside the share, or the OS refused |
| `not_a_dir` / `not_a_file` | wrong kind of entry |
| `busy` | too many transfers at once; try again later |
| `io` | reading or writing failed |
| `bad_request` | malformed request, invalid name, or wrong sizes |
| `exists` | the target name is taken (rename) |
| `root` | a shared folder itself cannot be renamed, moved or deleted |
| `inside` | a folder cannot be moved or copied into itself |
| `corrupt` | the file's SHA-256 did not match; it was discarded |
| `too_large` | the upload is larger than the device accepts |
| `no_space` | the upload or copy would leave the disk too full |
| `read_only` | the share is read-only |

A requester MUST treat unknown codes as a plain failure.

### 5.4 Listing

Request:

```json
{"t":"fs.list", "id":1, "path":"/Docs", "offset":0}
```

Reply:

```json
{"t":"fs.list.r", "id":1, "path":"/Docs", "offset":0, "total":1234, "readOnly":false,
 "entries":[{"name":"a.txt", "dir":false, "size":12, "mtime":1791338181302}]}
```

- Pages hold at most 500 entries. The requester asks again with the next
  `offset` until it has `total` entries.
- `readOnly`: the share is read-only (5.10).
- At `/`, each shared folder's entry carries its own `"readOnly"` instead.
- `size` is 0 for folders.
- An offerer leaves out entries it would refuse:
  - links that lead outside the share;
  - names that are not valid components;
  - uploads still in progress.
- Requesters SHOULD stop at 20 000 entries.

### 5.5 Download (read)

Request:

```json
{"t":"fs.read", "id":2, "path":"/Docs/a.txt", "offset":0}
```

The offerer replies, sends the data, then ends:

```
fs.read.r   {"t":"fs.read.r", "id":2, "size":12, "offset":0}
kind 2      [2][u32 id][bytes] ...     in order, each frame at most 256 KiB
fs.read.end {"t":"fs.read.end", "id":2, "sent":12, "sha256":"<64 hex>"}
```

- `sent` counts every byte of the file, including the `offset` bytes that
  were not sent.
- `sha256` is the SHA-256 of the whole file.
- A requester checks both fields and discards the file if either is wrong.
  A requester MUST accept an `fs.read.end` without `sha256`, as older peers
  do not send it.
- `{"t":"fs.cancel", "id":2}` stops a read. No reply follows.
- An offerer serves at most 4 reads per peer and answers `busy` beyond that.
  A requester SHOULD queue its downloads and keep at most 4 requested at once.

**Resuming.** A requester that kept part of a file asks with `"offset"`: the
number of bytes it has. The offerer:

- reads and hashes those bytes without sending them;
- replies with `"offset"` set to the same value;
- sends the rest.

If it cannot resume there (for example, the file is now shorter), it
replies with `"offset":0` and sends the whole file. If the file changed in
the meantime, the final `sha256` does not match. The requester then
discards its copy and starts over.

### 5.6 Upload (write)

```
requester  {"t":"fs.write", "id":3, "path":"/Docs", "name":"a.txt", "size":12, "replace":false}
offerer    {"t":"fs.write.r", "id":3}                       ready; or fs.write.err
requester  kind 3  [3][u32 id][bytes] ...                  each frame at most 256 KiB
requester  {"t":"fs.write.end", "id":3, "sent":12, "sha256":"<64 hex>"}
offerer    {"t":"fs.write.done", "id":3, "path":"/Docs/a.txt"}   the name it was saved as
           or {"t":"fs.write.err", "id":3, "code":"corrupt"}
```

**Name.** `name` MUST be a valid component that a safe-name filter would
leave unchanged (5.11). Otherwise the upload fails with `bad_request`.
Requesters clean names before sending.

**Replace or keep.** Without `replace`, an existing name gets a numbered
copy, such as `a (1).txt`. With `"replace":true` the existing file is
overwritten, which needs `storage.write`.

**Offerer behaviour:**

- It writes into a hidden temporary file in the target folder and lists it
  nowhere.
- It checks the permission again on every frame and at the end.
- It rejects frames over 256 KiB, and bytes beyond `size`.
- At the end it resolves the target folder again.
- It discards the file if `sent` ≠ `size` (`bad_request`) or the digest
  differs (`corrupt`).

**Cancel and limits.**

- `{"t":"fs.write.cancel", "id":3}` stops an upload; the offerer deletes the partial file.
- Before accepting, an offerer MAY refuse with `too_large` (its limit) or
  `no_space`. The desktop app keeps 256 MB free, counting uploads already in progress.
- It accepts at most 4 uploads per peer at once and answers `busy` beyond that.

### 5.7 Changes

| request | needs | effect |
|---------|-------|--------|
| `{"t":"fs.mkdir", "id", "path":"/Docs", "name":"New", "existing":false}` | upload | create a folder; a taken name gets a number. With `"existing":true`, answer with a folder of that name if one resolves inside the share (merging a folder upload). |
| `{"t":"fs.rename", "id", "path":"/Docs/a.txt", "name":"b.txt"}` | write | rename in place; `exists` if the name is taken (a change of case only is allowed) |
| `{"t":"fs.delete", "id", "path":"/Docs/a.txt"}` | write | to the trash if there is one, otherwise delete; folders recursively |
| `{"t":"fs.move", "id", "path":"/Docs/a.txt", "to":"/Other"}` | write | move into `to`; a taken name gets a number; moving to where it already is succeeds without doing anything |
| `{"t":"fs.copy", "id", "path":"/Docs/a.txt", "to":"/Other"}` | upload | copy into `to`; a taken name gets a number |

Success:

```json
{"t":"fs.done", "id":4, "path":"/Other/a.txt"}
```

`path` is the result. Some replies carry an extra field:

- `fs.delete` adds `"trashed":true` when the item went to the trash.
- `fs.mkdir` with `existing` adds `"existed":true` when an existing folder was reused.

A copy, or a move between drives, can take a while. While it runs, the
offerer sends `{"t":"fs.progress", "id", "done", "total"}` (bytes) at least
every few seconds. A requester extends its timeout on each one. At most 4
such jobs run per peer.

**Rules an offerer MUST follow.** These are what keep a peer inside the share:

- **Links are acted on, not followed.**
  - Rename, delete and move resolve the *parent* folder and then address the
    entry inside it.
  - Deleting a link removes the link, never its target.
  - Copying skips links inside the copied folder and refuses to copy a link itself (`denied`).
- **Shares stay put.** A shared folder itself cannot be renamed, moved or deleted (`root`).
- **No folder into itself.** A folder cannot be moved or copied into itself (`inside`).
- **Both ends must be allowed.** A move changes its source share and a copy
  changes its destination share. Either one being read-only gives `read_only`.

### 5.8 Watching a folder

```json
{"t":"fs.watch", "id":5, "path":"/Docs"}
{"t":"fs.unwatch", "id":5}
```

These need `storage.observe`, and neither gets a reply. While a watch
lasts, the offerer sends `{"t":"fs.changed", "id":5, "path":"/Docs"}` when
the folder's listing changes. The requester then lists the folder again.

A watch ends with one last `fs.changed` when the folder stops resolving or
storage stops being offered. It ends with no message when the permission
is withdrawn or the session ends. The desktop app compares a summary of
each watched folder every 2 seconds, allows 16 watches per peer, and
ignores uploads in progress. It does not use OS change notifications,
because on Windows they hold the folder open and block renaming it.

### 5.9 Timeouts

The desktop app uses these timeouts:

- A request with no answer fails after 20 s.
- A transfer or upload that stops making progress fails after 30 s.
- A requester that keeps the partial file of a download that failed this way
  can resume it later (5.5).

### 5.10 Read-only shares

An offerer can mark a share read-only. Nothing in it can then be uploaded,
created, renamed, deleted or moved out (`read_only`), whatever the
permissions. Listing and reading are unaffected.

### 5.11 Names a requester saves locally

Remote names are untrusted. Before using one as a local file name, a
requester:

- replaces control characters and any of `/ \ : * ? " < > |` with `_`;
- trims spaces, and drops trailing dots and spaces;
- uses `download` if nothing is left;
- prefixes Windows device names with `_`;
- cuts the name to 200 characters.

It also shows names as plain text only.

## 6. Folder sync

A synced folder stays the same on every device that takes part. No device
is in charge: each keeps an index of what it has, sends it to the others,
and fetches what it lacks from whichever device has it. Changes reach every
member through any chain of members that are online.

### 6.1 Members and permissions

- **Folder id.** A folder has a random 128-bit id (32 hex digits), the same
  on every member, and a label.
- **Who takes part.** Only a peer that has proven its identity (`id`,
  section 3) **and** that this device trusts. Any other peer gets no sync
  messages, and its sync messages are ignored.
- **Grants.** Each device decides two things, per folder and per peer key:
  - **send:** give the peer this folder's index and blocks.
  - **accept:** take the peer's changes into this device's copy.

  A device that is sent a folder but not accepted from keeps a copy without
  changing anyone else's. When a device changes its grants, it sends
  `sync.offers` again to connected peers, and `sync.hello` for any folder
  it had not yet greeted that peer in.
- **Member id.** A member's id in version vectors is the first 16 hex digits
  of SHA-256(its 32-byte public key).

### 6.2 Messages

| message | meaning |
|---|---|
| `{"t":"sync.offers", "folders":[{"id","label"}]}` | the folders this device sends the peer; sent on connect and when that changes |
| `{"t":"sync.hello", "folder", "epoch", "peerEpoch", "seen", "reply"}` | "I take part in this folder." `epoch`: the sender's index epoch. `peerEpoch`/`seen`: how much of the receiver's index the sender has (`""`/0 if none). A device answers a hello (with `reply:true`) while it accepts from the sender but has not had its index yet, since its own hello may have come before the sender had the folder; a reply is never answered |
| `{"t":"sync.index", "folder", "epoch", "full", "entries":[...]}` | index entries. `full:true` on the first message of a complete resend: the receiver drops what it held of that index |
| `{"t":"sync.get", "id", "folder", "path", "block", "offset", "size", "hash"}` | ask for one block of a file |
| kind 4 `[4][u32 id][bytes]` | the block, in frames of at most 256 KiB |
| `{"t":"sync.err", "id", "code"}` | the block cannot be given: `forbidden`, `not_found`, `changed` or `io` |

Epochs are unsigned 64-bit numbers, sent as decimal strings.

A device answers a `sync.hello` from a peer it sends the folder to by
sending its index:

- If `peerEpoch` is the device's current epoch, it sends only the entries
  changed after `seen`.
- Otherwise it sends everything, with `full:true`.

After that, it sends each change as it happens. Index messages hold about
256 KiB of JSON each.

A device sends **its own** index: what it has, including versions it got
from other members. That is how changes travel through a member to devices
that never meet the author.

### 6.3 Index entries

```json
{"p":"docs/plan.md", "v":{"3f2a...":2, "9c01...":1}, "q":118,
 "s":4096, "m":1791338181302, "h":"<sha256 hex>", "bs":131072, "b":["<sha256 hex>", ...]}
```

| field | meaning |
|---|---|
| `p` | path relative to the folder, `/`-separated; every component a synced name (6.6), none starting with `.syncpart-`, and never `.syncskip` or `.synctrash` at the top |
| `v` | version vector: member id -> how many changes that member made to this path |
| `q` | the sender's sequence number for this change; receivers remember the highest seen |
| `del` | `true`: deleted (a tombstone); no size or blocks |
| `dir` | `true`: a folder; no size or blocks |
| `skip` | `true`: the sender no longer takes part in this path: its `.syncskip` (6.5), or a name it holds back (6.6). Others leave the entry out, and neither take the file nor delete theirs |
| `s`, `m` | size, and modification time in ms |
| `h` | SHA-256 of the whole file |
| `bs`, `b` | block size, and the SHA-256 of each block in order |

A receiver MUST drop an entry with an invalid path, or whose block list
does not match its size.

**Block size** starts at 128 KiB and doubles until a file has at most 2000
blocks, up to 16 MiB. The last block is the remainder.

### 6.4 Versions and decisions

**Comparing versions.** Version X is *newer* than Y if every counter in X is
at least Y's and one is larger. If each has a counter higher than the
other's, they are *concurrent*. Clocks never decide anything.

**Local changes.** When a device finds a local change by scanning its
folder, it records a version: its current one, with its own counter raised
by one. A deleted path becomes a tombstone the same way.

- A file whose time changed but whose content did not is no change.
- How a device notices changes is its own business. The desktop app scans
  every 10 seconds, rereads only files whose size or time changed, and
  leaves files changed in the last 2 seconds for the next scan.

**Choosing.** For each path, a device looks at its own entry and the entries
of every peer it accepts from.

- Left out: entries marked `skip`, paths its `.syncignore` or `.syncskip`
  match, and names it holds back (6.6).
- It sets aside any entry that another entry is newer than, and groups what
  is left by content.

Then:

- **One content.** Take it.
  - If the device already has that content, it adopts the merged version
    (the highest counter of each member), with no change on disk.
  - Otherwise it fetches the file, creates the folder, or deletes.
- **Several contents: a conflict.** Nothing on disk is overwritten.
  - **The device's own change is one of them** (it holds one, with its own
    counter higher than in all the others): the conflict waits for the
    user.
  - **It holds one, but didn't make the change:** it stays on the chain it
    is on.
  - **It is behind all of them:** it follows the version held by the
    connected peer it has been connected to longest. If none is connected,
    it waits.

  In every case it tells the user.

**Resolving a conflict.** The user can:

- keep their own version;
- take a peer's version (fetched first);
- stop syncing the file (adds a pattern matching exactly that path to
  `.syncskip`, so its entry gets `skip:true`).

Whatever is kept gets the merged version of every candidate, plus one for
the resolving device. That makes it newer than all of them, so it settles
the conflict on every device it reaches.

### 6.5 .syncignore and .syncskip

Both use gitignore patterns:

- `*` and `?` stay within one name; `**` spans folders.
- A pattern with a `/` is anchored at the top.
- A trailing `/` matches folders only.
- `!` brings a match back, except inside an excluded folder.

**`.syncignore`** sits at the top of the folder and syncs like any file,
unless it ignores itself.

- Ignored paths are not synced.
- A path that was synced and becomes ignored counts as deleted for sync:
  its tombstone goes out, but the file on disk is left alone. Members that
  also ignore it disregard that tombstone.
- A receiver MUST NOT apply deletes while a newer `.syncignore` is still
  being fetched.

**`.syncskip`** belongs to one device and never syncs. Paths matching it
keep their local contents: changes to them are neither sent nor applied.
The device's entry for such a path carries `skip:true`.

### 6.6 Names

Synced names follow the strictest rules of the systems members may run, so
every member can hold every synced file. On **every** device, a name is not
synced if it:

- contains `\ / : * ? " < > |` or a control character;
- ends with a dot or a space;
- is a Windows device name (`CON`, `PRN`, `AUX`, `NUL`, `COM1`-`COM9`,
  `LPT1`-`LPT9`), with or without an extension;
- is longer than 255 characters;
- is not in Unicode normalization form C. macOS devices convert the names
  they read to form C, and find the files under either form.

Such a file stays where it is, takes no part in sync, and the user is told
why. If it was synced before, its entry is marked `skip:true`, so other
devices neither take it nor delete their copy.

**Names that differ only in case** (compared case-folded, in form C), such
as `report.txt` and `Report.txt`, cannot both exist on devices that ignore
case.

- Only the **older** file (by modification time; between folders, or at the
  same time, the lower name) is synced. The others are held back the same
  way, with a warning.
- When the older one goes, the next one is synced.
- Everything inside a held-back folder is held back with it.
- This applies both to names on one device and to names arriving from
  different devices.
- A device never writes into, over, or through a file or folder of its own
  whose name matches only when case is ignored.

### 6.7 Fetching and applying

**Renames.** On the wire, a rename is a new path plus a tombstone. When a
new path has the same content (hash and size) as a file about to be deleted
here, the device renames that file in place instead of assembling a copy.
That covers changes of case too. Nothing is fetched, and nothing is kept
aside.

**Assembling a file.** A file is assembled from blocks already at hand
before any are requested:

- a part file left by an earlier attempt;
- any file in the folder with the same block hash (renames, copies, earlier
  versions).

**Fetching missing blocks.**

- They are requested from any connected peer the device accepts from whose
  entry has the same content, at most 16 at a time per peer. That peer
  serves them only if it sends the folder to the device (6.8).
- Each block is checked against its hash.
- On `sync.err` or a 30-second timeout, another holder is tried.

**Replacing the file.**

- The file is assembled in `.syncpart-<first 8 hex of the file hash>-<name>`
  beside its target. Scanners skip such files.
- When it is complete, its modification time is set and it replaces the
  target in one step.
- Before replacing or deleting anything, the device checks that the disk
  still holds what its own index says. If not, it rescans first, so a
  local change becomes a conflict instead of being lost.

**Deletes** come after every fetch has taken the blocks it needs from the
files about to go.

- Deleted files are kept in `.synctrash` at the top of the folder: same
  relative path, with `~yyyyMMdd-HHmmss` (local time of removal) added
  before the extension. A file given up by taking another device's version
  in a conflict is kept the same way.
- After 30 days, kept files go to the system trash, or are deleted where
  there is none.
- `.synctrash` is never scanned, synced or written by peers.
- A folder that still holds files that are not synced is left in place.

### 6.8 Safety rules

These are MUSTs for any implementation:

- Every path written or read resolves, with links followed, to a place
  inside the folder.
- Links are never synced, and never written through.
- A device serves a block only if all of these hold:
  - the requester is a peer it sends the folder to;
  - the path is live in its own index;
  - the block's hash matches what is on disk now.

## 7. Screen, camera and remote control

Media uses uConnect's encrypted datagram channel (maximum 1100 bytes),
separate from reliable message kinds. A channel is delivered to the screen
service only while a consented session is active. A media channel needs a
proven identity; sharing additionally needs the host's trust (7.3).
`screen.view`, `screen.direct`, `control.input`, `control.direct`, and
`camera.view`, `display.show` and `display.direct` are trusted-device grants, never default permissions. Datagram kinds are separate from the reliable envelope in section
2: 1 = screen video, 2 = screen cursor, 3 = camera video, 4 = extended display video, 5 = extended display cursor.

### 7.1 Video fragments

All multibyte integers are unsigned, big endian. Header (16 bytes):

| offset | field |
|---|---|
| 0 | u8 kind = 1 |
| 1 | u8 flags: bit 0 keyframe, bit 1 retransmission |
| 2 | u16 nonzero session token |
| 4 | u32 frame sequence (wraps) |
| 8 | u16 fragment index (zero based) |
| 10 | u16 fragment count |
| 12 | u32 capture timestamp in monotonic milliseconds (wraps) |
| 16 | encoded payload, 1–1084 bytes |

Nonfinal fragments have exactly 1084 payload bytes. Frames are bounded to
4 MiB, at most 8 assemblies and 8 MiB of buffered payload per viewer.
Duplicate fragments do not affect feedback. Mismatched tokens, metadata,
unknown flags, oversized counts and malformed lengths are discarded.

Frames normally expire after two frame intervals plus measured jitter
(capped at 60 ms). An incomplete frame receives one `scr.nack` with its
`frame` and missing `indexes`; its deadline is extended by one interval
(at least 30 ms). While deltas are gated (below), only keyframes are
nacked. The sender keeps the fragments of frames sent in the last 500 ms
(at most 16 frames) and resends requested ones with the retransmission
flag. A frame that still cannot complete is a lost reference: delta delivery
is gated until a complete keyframe; `scr.keyframe` requests are rate limited
to once per 200 ms. Delivery follows frame sequence order, including
sequence wrap.

The viewer decodes complete frames in order on its decoder thread. Frames
completing while a decode runs wait for it (none can be skipped: each is a
reference for the next); if more than a second of video (at least 8 frames)
waits, they are dropped and a keyframe requested.

### 7.2 Reports and pacing

`scr.report` carries window counters `received`, `lost`, `late`, and numeric
`jitterMs`, `owdTrendMs`, `decodeMs`. Transit differences estimate delay trend
without assuming synchronized clocks. They are not glass-to-glass latency.

Each frame's datagrams are spread over about half a frame interval
(the first few at once), starting when the previous frame's are due. Up to 3
frames may wait; a further frame is dropped. A datagram the path refuses
(still opening, or the TCP fallback's queue full) is retried on the next
pump, not dropped. Frames older than four frame intervals (at least 150 ms)
are dropped with every frame queued behind them, and the next frame is a
keyframe, so a blocked path cannot accumulate stale video.

Bitrate starts at 4 Mbit/s on direct UDP, 1.5 Mbit/s on relay, 1 Mbit/s on
TCP, and never goes below 300 kbit/s. Loss above 5% or a delay trend above
15 ms cuts it by 20%, at most once per three reports; persistent congestion
at the floor lowers the frame rate (not below 15), then the width. Decoding
slower than the frame interval (by 20%, three reports running) lowers only
the frame rate, by a quarter (not below 5). Clean reports raise the bitrate
by 5% (at least 50 kbit/s) each; every 10 clean reports widen the picture by
160 pixels and, unless decoding is slow, add 5 fps. Configuration caps apply
throughout.

Hardware encoders restart with a keyframe on a bitrate change, so the host
applies the controller's bitrate in batches: cuts of more than 15% at most
once a second, raises of more than 25% at most every 5 s, smaller drift after
15 s. Changes beyond 2x reopen the encoder. Software OpenH264 follows every
change.

### 7.3 Screen sessions and consent

Reliable control messages have these types:

| type | fields / meaning |
|---|---|
| `scr.request` | nonzero u32 `request`, `wantControl`, `maxWidth`, `maxFps` (1-165), `codecs:["h264"]` |
| `scr.wait` | `request`; host is asking its user |
| `scr.err` | `request`, `code` (`forbidden`, `busy`, `codec`, `denied`, `transport`, `ended`) |
| `scr.started` | matching `request`, nonzero u16 `session`, `codec:"h264"`, `width`, `height`, `fps`, `monitors`, `control`, `controlDetail` (why control asked for was not granted; the session is view-only) |
| `scr.monitor` | `session`, zero-based `index` |
| `scr.monitorChanged` | `session`, `index`; shared source changed for all viewers |
| `scr.pipeline` | `session`, `name`, `cursorEmbedded`; capture/encoder path changed |
| `scr.keyframe` | `session` |
| `scr.nack` | `session`, `frame`, `indexes` |
| `scr.report` | `session` plus the report fields in 7.2 |
| `scr.error` | `session`, human-readable `detail`, followed by stop |
| `scr.status` | `session`, human-readable `detail`; the host has had nothing to send 5 s after starting (sent once) |
| `scr.stop` | `session`; cancelling a pending request also carries `request` |

Request IDs correlate approvals and responses; session tokens isolate media
and live controls. A late approval/response cannot start a cancelled request.
Tokens are scoped to an authenticated peer connection. Reliable operations
must match that connection and its current token.

Screen hosting must be offered, the requester must prove a trusted identity,
and it must hold `screen.view`. A free viewer slot is reserved during consent.
Without `screen.direct`, the host asks each time; the prompt expires in 30 s.
Remembering approval grants the trusted identity `screen.direct`. A viewer
requires only that the host has proven its identity: trust is the sharing
device's decision, not the watching one's. Revocation, disconnect, stop and
absence of valid feedback for 5 s end hosting. Pending viewer requests expire
in 35 s. A viewer with no decoded picture 8 s after `scr.started` tells its
user, along with any `scr.status` detail.

H.264 is carried as Annex B access units with parameter sets in keyframes;
there are no B frames. Capture and decode submissions are bounded and run
on dedicated codec threads. The viewer reports decode time, loss, bitrate,
frame rate and the transport path. `captureMs` is monotonic and cannot be
interpreted as absolute cross-device latency without clock synchronization.

### 7.4 Shared sources and camera

Screen hosting allows 1-8 viewers, including reserved consent slots. One
encoder serves every viewer; its width, frame rate and bitrate honor the
lowest requested/adaptive limits. Each peer has independent tokens, pacing,
retransmission history and reports. A blocked peer cannot queue unbounded video.
Stopping one viewer leaves capture running until the last live viewer stops.
Only one viewer may hold or await control. Source selection is shared; while
controlled, only that controlling viewer may switch it.

Camera hosting uses `cam.*` counterparts of the `scr.*` view messages, and
video fragment kind 3 with the same header as 7.1. It needs offered `camera`
and a trusted `camera.view` grant. Every request needs host consent; screen
and control direct grants do not bypass camera consent. There is no camera
input or cursor channel. `monitors` lists cameras and `cam.monitor` selects
one. Screen and camera streams may coexist; stopping either must preserve
the other's datagram connection. Camera quality/viewer limits share the
screen settings. There is no audio.

Windows native capture converts DXGI textures to NV12 on D3D11 and supplies
hardware surfaces to NVENC/QSV/AMF. macOS uses ScreenCaptureKit IOSurfaces
and VideoToolbox. Unsupported adapters, rotated displays or native failures
fall back to Qt capture and CPU color conversion. Hardware decoder output is
mapped for the Qt video sink, with software decoding when unavailable. On
Wayland the OS ScreenCast portal chooses the source; remote monitor selection
and input are unavailable. XOR parity is not part of version 1; no field-loss
evidence currently warrants adding it.

### 7.5 Remote control and cursor

An initial `wantControl:true` request needs `control.input` and explicit control
consent, unless both direct grants exist. During a view session,
`scr.control {session,request,on}` requests or drops control;
`scr.controlled {session,request,on,detail}` answers it. A request of zero in
the answer denotes host revocation. Control-only consent expires in 30 s.
Control never permits input after trust or its grant is withdrawn.

Reliable input messages all carry `session`: `in.key {usage,down}` uses USB
HID keyboard-page positions; `in.text {text}` carries at most 256 UTF-16
units; `in.mouse {x,y,buttons}` uses normalized monitor coordinates and
left/right/middle bits 1/2/4; `in.wheel {dx,dy}` uses 120 units per notch.
`in.release` releases held keys/buttons. Focus loss, Right Ctrl, stop,
revocation, timeout and disconnect release held input. Mouse moves are
coalesced below 120 Hz; button changes are immediate. Native input failure
ends control, retains failed releases and retries them.

`scr.cursor {session,png,hotspotX,hotspotY}` carries a base64 PNG bounded to
128 by 128 pixels and 49 KiB compressed. Cursor position datagrams are 16
bytes: `[u8=2][u16 session][u32 seq][i32 x][i32 y][u8 visible]`, big endian.
Positions use original monitor pixels; the latest wrapping sequence wins.
`scr.started.cursorEmbedded` tells the viewer whether to suppress its overlay.
`scr.monitorChanged {session,index}` acknowledges a monitor switch.

Windows input cannot reach elevated windows or secure desktops. macOS requires
Accessibility authorization. X11 requires XTest; Unicode text must exist in
the host keymap. Wayland control is unsupported. System-reserved shortcuts
may be handled locally before Qt sees them. File transfer into the remote
desktop and clipboard synchronization are outside this protocol version.

### 7.6 Extended display

A *host* asks a *display device* to act as one of its monitors. The host
creates a virtual monitor of the agreed mode, captures it like any screen
and sends it; the display device shows it, normally full screen. The
handshake runs the other way round from 7.3: the sending side asks.

**Who may ask.** The host asks only devices it trusts. The display device
offers `display` in `hello`, and answers `forbidden` unless the host has
proven a trusted identity holding `display.show`. Without `display.direct` it
asks its user each time; remembering approval grants `display.direct`.

| type | direction | fields / meaning |
|---|---|---|
| `dsp.request` | host → display | nonzero u32 `request`; `mode`: `"match"` (the display device's screen) or `{width, height, fps}`; `codecs:["h264"]` |
| `dsp.wait` | display → host | `request`; the display device is asking its user |
| `dsp.err` | either | `request`, `code` (`forbidden`, `busy`, `codec`, `denied`, `driver`, `transport`), optional human-readable `detail` |
| `dsp.accept` | display → host | `request`; `screen {width, height, fps, dpi}`, the screen it will be shown on, in pixels; `maxWidth`, `maxFps`, what it decodes |
| `dsp.stop` | either | before the media session starts: `request`; afterwards as `scr.stop` |

Then the host starts a screen session (7.1-7.3, 7.5) under `dsp.*` names:
`dsp.started` (matching `request`), `dsp.keyframe`, `dsp.nack`,
`dsp.report`, `dsp.cursor`, `dsp.pipeline`, `dsp.status`, `dsp.error`,
`dsp.stop`, with video on datagram kind 4 and cursor positions on kind 5.
There is no input, no `dsp.monitor` and no `dsp.control` in this version.

**Mode.** The host takes the requested mode, or for `"match"` the display
device's `screen`, then keeps it within what it can encode (at most 3840
pixels wide and 2400 high, shape kept), what the display device decodes,
24 Hz to `maxFps`, and H.264 level 5.2 (2,073,600 macroblocks a second).
`dsp.started` carries the resulting `width` and `fps`. A different mode is a
new session.

**Nearby only.** The media channel opens without a fallback: the session
needs direct UDP. If the path is relay, TCP or failed, or not direct 5 s
after the session starts, the host sends `dsp.error` and stops.

**Timing.** The display device's prompt denies after 30 s; the host gives up
on an unanswered request after 35 s. Creating the monitor takes up to a few
seconds; a display device waits 15 s for `dsp.started` after accepting. A
host that captures nothing for 15 s stops with `dsp.error`. Other timeouts
are those of 7.2 and 7.3.

**The monitor never outlives the session.** Every end (either side's stop,
disconnect, the 5 s report timeout, the path, withdrawn trust, the app
quitting) removes the virtual monitor. Hosts must also guarantee this when
the app crashes or hangs, through the platform: on Windows the uConnect
virtual display driver removes a monitor when its owner's handle closes or
goes silent for 3 s; on macOS the system removes it with the process.

One extended display at a time on each side in this version: a second
request gets `busy`.

## Changes

| revision | change |
|----------|--------|
| storage 1.0 | `fs.list`, `fs.read`, `fs.cancel` |
| storage 1.1 | uploads (`fs.write`…, kind 3); `fs.rename`, `fs.delete`, `fs.mkdir`; `replace`; `storage.download` folded into `storage.read` |
| storage 1.2 | `fs.mkdir` `existing`; `fs.move`, `fs.copy`, `fs.progress`; `fs.watch`, `fs.unwatch`, `fs.changed` |
| storage 1.3 | `sha256` on `fs.read.end` and `fs.write.end`; `fs.read` `offset`; codes `corrupt`, `too_large`, `no_space`, `read_only`; `readOnly` in listings |
| screen/camera 1.0 | consented screen/camera sessions, H.264 datagrams, feedback/recovery, trusted control, cursor, shared sources (section 7) |
| sync 1.0 | folder sync (section 6): `sync.offers`, `sync.hello` (with `reply`), `sync.index`, `sync.get`, `sync.err`, kind 4; per-folder send/accept grants; `skip` for `.syncskip` and held-back names; strict names and case clashes (6.6); renames in place; `.synctrash` |
| display 1.0 | extended display (7.6): `display` service, `display.show` / `display.direct`, `dsp.request`, `dsp.wait`, `dsp.err`, `dsp.accept`, the screen session under `dsp.*`, datagram kinds 4 and 5 |

Every change so far only adds messages, fields and codes, so an older peer
ignores what it does not know. A request type that a peer does not
understand gets no answer, so it fails by timeout.
