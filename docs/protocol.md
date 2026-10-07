# uConnect app protocol

This is the wire contract between uConnect apps: the Qt desktop app in this
repository and the separately maintained Flutter mobile app. An app that
follows this document can talk to any other, whatever language it is
written in.

Revision: **storage 1.3, sync 1.0** (2026-10). Changes are listed at the end.

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
- `svc`: the services the device offers: `storage`, `camera`, `screen`, `control`.

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
| `camera.view`, `screen.view`, `control.input` | reserved; those services are not specified yet |

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

## Changes

| revision | change |
|----------|--------|
| storage 1.0 | `fs.list`, `fs.read`, `fs.cancel` |
| storage 1.1 | uploads (`fs.write`…, kind 3); `fs.rename`, `fs.delete`, `fs.mkdir`; `replace`; `storage.download` folded into `storage.read` |
| storage 1.2 | `fs.mkdir` `existing`; `fs.move`, `fs.copy`, `fs.progress`; `fs.watch`, `fs.unwatch`, `fs.changed` |
| storage 1.3 | `sha256` on `fs.read.end` and `fs.write.end`; `fs.read` `offset`; codes `corrupt`, `too_large`, `no_space`, `read_only`; `readOnly` in listings |
| sync 1.0 | folder sync (section 6): `sync.offers`, `sync.hello` (with `reply`), `sync.index`, `sync.get`, `sync.err`, kind 4; per-folder send/accept grants; `skip` for `.syncskip` and held-back names; strict names and case clashes (6.6); renames in place; `.synctrash` |

Every change so far only adds messages, fields and codes, so an older peer
ignores what it does not know. A request type that a peer does not
understand gets no answer, so it fails by timeout.
