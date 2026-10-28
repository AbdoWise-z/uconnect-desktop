# uConnect app protocol

This is the wire contract between uConnect apps: the Qt desktop app in this
repository and the separately maintained Flutter mobile app. An app that
follows this document can talk to any other, whatever language it is
written in.

Revision: **storage 1.3, sync 1.1, screen/camera 1.1, display 1.1, rules 1.1** (2026-10). Changes are listed at the end.

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
"uconnect identity v1"          (20 ASCII bytes)
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

### rules.get / rules.list / rules.err: copying network rules

```json
{"t":"rules.get"}
{"t":"rules.list", "rules":[{"pattern":"m.m.m.x", "label":"Home", "perms":["storage.observe"]}]}
{"t":"rules.err", "code":"forbidden"}
```

A device may ask another for its network rules on the topic they share (4.2),
for example to set itself up quickly on a network the other already uses.
The asked device answers `rules.list` only if the asker has proven its
identity and is covered by its special rules or one of its network rules (not
just the defaults); otherwise `rules.err` with `forbidden`. The asking device
MUST show the rules to its user and change nothing until they accept. A
receiver drops invalid patterns and unknown permission ids, and keeps at most
64 rules.

## 4. Permissions

Each device decides what every peer may do. A peer gets every permission
that any of these that apply to it grants:

1. **Special rules**: permissions this device set for the peer's verified
   key. Adding them is the moment a user compares fingerprints.
2. **Network rules** (4.2): the first rule of this topic whose pattern
   matches the address the peer's direct connection comes from, once its
   identity is verified.
3. **Defaults**: for every peer, including peers without a verified key.

Special rules may be marked as an **exception**: then they alone apply, and
the peer can be given less than a network rule or the defaults grant.

Camera, screen, control and display ids are **sensitive**. Any of the three
may grant them, but since any holder of the invite link gets the defaults,
apps warn when the defaults include one.

| id | allows |
|----|--------|
| `storage.observe` | `fs.list`, `fs.watch` |
| `storage.read` | `fs.read` |
| `storage.upload` | `fs.write`, `fs.mkdir`, `fs.copy` |
| `storage.write` | `fs.rename`, `fs.delete`, `fs.move`, and `fs.write` with `"replace":true` |
| `camera.view`, `screen.view`, `screen.direct`, `control.input`, `control.direct` | screen, camera and control sessions (section 7) |
| `display.show`, `display.direct` | extended display requests, and accepting them without asking (7.6) |

A saved grant of `storage.download`, from an earlier version, means `storage.read`.

### 4.2 Network rules

A device keeps an ordered list of network rules per topic. Each has a
pattern, an optional label and a permission set. A pattern has four
dot-separated parts, one per IPv4 byte: `x` (any value), `m` (the same as one
of this device's own addresses, all `m` parts against the same address) or a
number 0-255. `m.m.m.x` is this device's /24, `100.x.x.x` anything starting
100, `x.x.x.x` any address (IPv6 too). Patterns are matched against where the
peer's direct connection arrives from, never against addresses a peer reports
about itself; a peer reached through the relay matches no rule.

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
  section 3) **and** has special rules on this device (4), and only in folders
  whose grants name its key. Network rules and defaults never reach sync.
  Any other peer gets no sync messages, and its sync messages are ignored;
  removing a device's special rules takes it out of every folder.
- **Grants.** Each device decides two things, per folder and per peer key:
  - **send:** give the peer this folder's index and blocks.
  - **accept:** take the peer's changes into this device's copy.

  A device that is sent a folder but not accepted from keeps a copy without
  changing anyone else's. A device may also **pause** a peer in a folder:
  its grants are kept but grant nothing until it is resumed. When a device
  changes its grants, it sends `sync.offers` again to connected peers, and
  `sync.hello` again for any folder whose grants for that peer changed.
- **Member id.** A member's id in version vectors is the first 16 hex digits
  of SHA-256(its 32-byte public key).

### 6.2 Messages

| message | meaning |
|---|---|
| `{"t":"sync.offers", "folders":[{"id","label"}]}` | the folders this device sends the peer; sent on connect and when that changes. A folder that was in the sender's offers and is no longer is one the receiver MUST forget the sender's index of: it is not the sender's to act on any more, and the sender's next `sync.hello` for it brings a full index (sync 1.2) |
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
proven identity; the host's permissions for it (special rules, network
rules or defaults, 4) then decide what it may do (7.3). `screen.view`,
`screen.direct`, `control.input`, `control.direct`, `camera.view`,
`display.show` and `display.direct` are sensitive (4). Datagram kinds are separate from the reliable envelope in section
2: 1 = screen video, 2 = screen cursor, 3 = camera video, 4 = extended display video, 5 = extended display cursor.

### 7.1 Media packets (screen/camera 2.0)

Every datagram a host sends in a session, video and repair alike, is
numbered in one sequence, so the viewer can tell the host exactly which
arrived and when. All multibyte integers are unsigned, big endian. Header
(21 bytes):

| offset | field |
|---|---|
| 0 | u8 kind (1 screen video, 3 camera video, 4 display video) |
| 1 | u8 type: 0 media, 1 repair, 2 feedback (7.2) |
| 2 | u16 nonzero session token |
| 4 | u32 packet sequence (wraps; given as the packet leaves, so the numbers have no gaps) |
| 8 | u32 frame number (wraps) |
| 12 | u16 fragment index (zero based) |
| 14 | u16 fragment count |
| 16 | u32 capture timestamp in monotonic milliseconds (wraps) |
| 20 | u8 flags: bit 0 keyframe, bit 1 repeat (sent again on request) |
| 21 | payload |

A media packet carries one fragment of a frame, at most 1076 bytes (1100 less
the header and a repair packet's own 3 bytes, so a repair fits in a
datagram). Frames are bounded to 4 MiB; a viewer holds at most 32 frames
being assembled.

A repair packet carries the frame fields of its group, `fragment index`
being the first of a group of consecutive fragments, and as payload the
group's size (u8), the XOR of the fragments' lengths (u16) and the XOR of
their payloads (zero padded, as long as the longest): any one fragment of
the group that is lost is rebuilt from the others. The host sizes the groups
to what the path loses, by the worst recent spell rather than the mean:
none under 2%, one repair per 8 fragments under 8%, per 4 under 20%, per 2
under 45%, per 1 above (each fragment twice, in effect); a keyframe's
groups are never larger than 2.

The lag budget. One figure, 1 s, bounds how far behind live a viewer may
fall before the stream is brought back, on both ends. A frame that makes no
progress for the budget (at least eight frame intervals) is given up with
the frames behind it, and the viewer waits for a keyframe, which it asks for
in its feedback; a viewer holds up to two seconds' worth of frames under
assembly. A session's sequence numbers start at 0, so packets before the
first one a viewer sees count as missing too. A complete keyframe is delivered ahead of
whatever incomplete frames precede it. Delivery otherwise follows frame
number order, including wrap.

### 7.2 Feedback, rate and pacing

Every 50 ms (every 500 ms while nothing arrives, as a keepalive; sooner when
a keyframe is wanted) the viewer sends the host a feedback datagram of the
media kind with type 2:

| offset | field |
|---|---|
| 0 | u8 kind, u8 type = 2 |
| 2 | u16 session token |
| 4 | u32 base sequence: the first packet reported on |
| 8 | u16 count of arrivals that follow (at most 200) |
| 10 | u32 reference time, the viewer's clock in milliseconds, 16 s ago (wraps) |
| 14 | u16 held: milliseconds from the newest arrival to this feedback leaving |
| 16 | u32 newest complete frame's number; u32 its capture timestamp |
| 24 | u16 playout delay in use; u16 decode time in tenths of a millisecond |
| 28 | u8 flags: bit 0 keyframe wanted, bit 1 the newest-frame fields are valid |
| 29 | count x u16 arrival: quarter milliseconds after the reference time, or 0xFFFF for a packet that has not arrived |
| then | u8 count of repeats wanted (at most 32), then that many u32 sequences |

Each feedback reports every sequence from the base to the highest seen,
once; the next starts after it. Clocks are never compared across devices:
the host reads the spacing of arrivals against the spacing of its own
departures. A packet reported missing is counted lost; one the viewer still
wants (missing for 10 ms and newer than the last frame it delivered) is
listed as a repeat, and the host sends it again, with the repeat flag and its
original sequence, at most once per 40 ms, ahead of new packets, from the
last 2 s it has sent. The round trip is the time from the newest packet
reported leaving to its feedback arriving, less the held time.

Bandwidth estimate. The host runs a delay-gradient estimator in the manner
of Google congestion control: a trend line through the accumulated
inter-arrival minus inter-departure deltas of the last 20 packets, weighted
by how many have been seen, against an adaptive threshold (12.5 ms at the
start). A rising trend held for 10 ms is overuse: the target drops to 85% of
the receive rate (bytes arrived over the last 500 ms). Otherwise it grows,
by 8% per 100 ms until the path has pushed back once, then by about a packet
per round trip while near the last cut. More than 10% loss cuts it by half
the loss share, at most once per 300 ms or three round trips, but only with
a clear delay rise or the round trip half again above its minimum: loss
with no delay behind it is a path losing at random (a mobile uplink), which
repair and repeats carry across. The target never exceeds 1.5x the receive
rate (plus the floor) once the path is being used, holds while the source
sends under 70% of it, and stays within the session's cap and the floor of
150 kbit/s. The host starts at 1.5 Mbit/s (a direct link may start higher).

The encoder is asked for the target less the repair share and the loss
(what repeats take); the quality ladder steps the picture down a rung
(3840, 2560, 1920, 1280, 854 on the long side needing about 8, 4, 2, 0.8 and
0 Mbit/s at 30 fps, scaled by the frame rate) when the path has shown its
limit and the source asks for more than it gets, and back up with a third
to spare; the frame rate goes to 30 under 1 Mbit/s, 15 under 250 kbit/s and
10 under 150 kbit/s. Changes are at least 1.5 s apart and decided on the
encoder rate averaged over a second.

Pacing. Packets leave at 1.5x the target, a few at a time, from a queue
that normally drains as fast as it fills and may swell to the lag budget of
the target before the host skips captures (an encoded frame is never
dropped, since the frames after it depend on it; a phone, whose encoder
cannot skip, drops the frame and asks its encoder for a keyframe). A viewer
behind by more than the budget (its newest capture timestamp against the
newest sent, the same clock) has the queue dropped and a keyframe sent next,
at most once per 1.5 s; one behind by half the budget for a couple of
seconds brings the target down 15%, since no loss figure shows a path that
queues. A keyframe is produced only on request (encoders run without a
keyframe schedule, at a constant bitrate, refreshing intra where they can),
at most once per 150 ms or a round trip.

Transport. Media is carried over UDP only: punched direct, or through the
relay when punching fails (never in "direct" relay mode). It is never
carried inside the TCP connection; a host waits up to 40 s for first
feedback while the channel opens, 5 s between feedbacks after that.

Playout. A viewer shows each picture at its `captureMs` plus a playout
clock plus a buffer, so pictures keep the host's pace whatever the network's
jitter. The clock follows the 5th percentile of the transit times of the
last 15 s and slews at most a tenth faster or slower than real time, so a
change in the path's delay is taken up gradually; pictures behind their
time are shown a tenth faster than captured (counted late), never skipped,
unless the lag has passed the budget, when playback jumps to the newest.
The buffer is the user's choice or automatic: the frame interval plus twice
the jitter plus the longest a recent gap took to fill, within the budget,
raised at once and lowered slowly. Without a buffer each picture is shown
as soon as it is decoded.

Once a second the host sends each viewer `scr.stats` (`session` and
numbers, all optional): `fps` (frames encoded), `encodeMs`, `convertMs`,
`threads`, `queue`, `queuePeak`, `dropped` and `throttled` (captures dropped
or skipped, per second), `sentKbps`, `receiveKbps` (what the viewer
reports arriving), `targetFps`, `targetKbps` and `width` (the ladder's),
`senderDrops`, `lagMs`, `rttMs`, `lossPct`, `fec` (fragments per repair
packet, 0 none), `repeats` (packets sent again in all), the strings
`transport` (`udp`), `estimator` (`increase`, `hold`, `decrease`) and
`pipeline`. They are for display; a viewer may ignore them.

### 7.3 Screen sessions and consent

Reliable control messages have these types:

| type | fields / meaning |
|---|---|
| `scr.request` | nonzero u32 `request`, `wantControl`, `maxWidth`, `maxFps` (1-165), optional `maxBitrate` (bit/s; 0 or absent: the host's own limit), `codecs:["h264"]` |
| `scr.wait` | `request`; host is asking its user |
| `scr.err` | `request`, `code` (`forbidden`, `busy`, `codec`, `denied`, `transport`, `ended`) |
| `scr.started` | matching `request`, nonzero u16 `session`, `codec:"h264"`, `width`, `height`, `fps`, `monitors`, `control`, `controlDetail` (why control asked for was not granted; the session is view-only) |
| `scr.monitor` | `session`, zero-based `index` |
| `scr.monitorChanged` | `session`, `index`; shared source changed for all viewers |
| `scr.pipeline` | `session`, `name`, `cursorEmbedded`; capture/encoder path changed |
| `scr.keyframe` | `session`; a viewer may also ask in its feedback (7.2), which is the usual way |
| `scr.stats` | host → viewer, `session` plus the figures in 7.2; once a second |
| `scr.error` | `session`, human-readable `detail`, followed by stop |
| `scr.status` | `session`, human-readable `detail`; the host has had nothing to send 5 s after starting (sent once) |
| `scr.stop` | `session`; cancelling a pending request also carries `request` |

Request IDs correlate approvals and responses; session tokens isolate media
and live controls. A late approval/response cannot start a cancelled request.
Tokens are scoped to an authenticated peer connection. Reliable operations
must match that connection and its current token.

Screen hosting must be offered, the requester must prove its identity, and
it must hold `screen.view` through special or network rules. A free viewer slot is reserved during consent.
Without `screen.direct`, the host asks each time; the prompt expires in 30 s.
Remembering approval adds `screen.direct` to the requester's special rules.
A viewer requires only that the host has proven its identity: what is allowed
is the sharing device's decision, not the watching one's. Revocation, disconnect, stop and
absence of valid feedback for 5 s end hosting. Pending viewer requests expire
in 35 s. A viewer with no decoded picture 8 s after `scr.started` tells its
user, along with any `scr.status` detail.

H.264 is carried as Annex B access units with parameter sets in keyframes;
there are no B frames. Capture and decode submissions are bounded and run
on dedicated codec threads. The viewer reports arrivals, decode time and
playout in its feedback datagrams (7.2). `captureMs` is monotonic and cannot be
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
and a `camera.view` grant from special or network rules. Every request needs host consent; screen
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
Control never permits input after its grant is withdrawn.

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

**Who may ask.** The host asks only devices its own rules cover. The display
device offers `display` in `hello`, and answers `forbidden` unless the host
has proven its identity and holds `display.show` through special or network
rules. Without `display.direct` it
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

**Wi-Fi tolerance.** Nearby links are usually Wi-Fi, where packets arrive in
bursts, are held up for a moment, or are lost in bursts. A display device
sends up to three `dsp.nack` for an incomplete frame, 50 ms apart, before
dropping it (7.1: one, then 30 ms), keeps up to 16 frames in flight (7.1
says 8), and shows decoded frames one per frame interval. A host answers up
to three repair requests per frame (7.1: one), and spreads each frame over
a quarter of a frame interval but no faster than three times its bitrate,
at most 60 ms (7.2: half an interval), so a keyframe is not one burst. A host cuts the bitrate only for more than
10% loss or a 30 ms delay trend in two reports running, and lowers the
frame rate for slow decoding only after six reports (7.2 says 5%, 15 ms,
one report, three reports).

**Nearby only.** The media channel opens without a fallback: the session
needs direct UDP. If the path is relay, TCP or failed, or not direct 5 s
after the session starts, the host sends `dsp.error` and stops.

**Timing.** The display device's prompt denies after 30 s; the host gives up
on an unanswered request after 35 s. Creating the monitor takes up to a few
seconds; a display device waits 15 s for `dsp.started` after accepting. A
host that captures nothing for 15 s stops with `dsp.error`. Other timeouts
are those of 7.2 and 7.3.

**The monitor never outlives the session.** Every end (either side's stop,
disconnect, the 5 s report timeout, the path, a withdrawn grant, the app
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
| screen/camera 1.0 | consented screen/camera sessions, H.264 datagrams, feedback/recovery, control, cursor, shared sources (section 7) |
| sync 1.0 | folder sync (section 6): `sync.offers`, `sync.hello` (with `reply`), `sync.index`, `sync.get`, `sync.err`, kind 4; per-folder send/accept grants; `skip` for `.syncskip` and held-back names; strict names and case clashes (6.6); renames in place; `.synctrash` |
| sync 1.2 | a folder that leaves a peer's `sync.offers` is forgotten by the receiver; `sync.hello` again after a grant change; a peer may be paused in a folder. No message changed shape |
| display 1.0 | extended display (7.6): `display` service, `display.show` / `display.direct`, `dsp.request`, `dsp.wait`, `dsp.err`, `dsp.accept`, the screen session under `dsp.*`, datagram kinds 4 and 5 |
| rules 1.0 | trust replaced by special rules; network rules per topic (4.2); `rules.get`, `rules.list`, `rules.err`. Sync membership needs a proven identity, special rules and the folder's grants (sync 1.1); sharing needs a proven identity, and any source may grant sensitive permissions, defaults included (screen/camera 1.1, display 1.1). Only local policy changed: no message changed shape |
| screen/camera 1.2 | the viewer chooses the frame rate, width and (new `maxBitrate`) bitrate to receive, within the host's limits; `scr.stats` and the playout fields of `scr.report` (7.2); frames converted in parallel but sent in order; higher starting bitrates |
| rules 1.1 | Permissions from special rules, the matching network rule and the defaults add up; special rules may be an exception that applies alone (4). Local policy only |
| screen/camera 2.0 | media packets numbered in one sequence with repair packets, feedback datagrams every 50 ms in place of `scr.nack` and `scr.report`, a delay-gradient bandwidth estimate with pacing and a quality ladder, encoders without a keyframe schedule, a lag budget of one second on both ends, a slewed playout clock with an automatic buffer, media over UDP only (relay, never TCP) (7.1, 7.2). The media header changed shape: both ends update together |
| identity | the proof context string (3) was `"uconnect-desktop identity v1"` before this revision; it is now the implementation-neutral `"uconnect identity v1"`. The `id` message keeps `"v":1`; peers from before and after do not verify each other's identity, so both ends update together. Connections and storage with default permissions are unaffected |

Almost every change only adds messages, fields and codes, so an older peer
ignores what it does not know. A request type that a peer does not
understand gets no answer, so it fails by timeout. The one exception is the
identity-context change above, which both ends adopt together.
