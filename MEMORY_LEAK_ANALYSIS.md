# cs2kz-metamod Memory Bloat Analysis — Recording & Replay Subsystem

**Reported symptom:** With multiple players hopping between maps, server RSS grows
from ~2 GB to 10+ GB. Removing the `cs2kz` plugin restores normal memory usage.

**Scope of investigation:** `src/kz/recording/*` plus directly related code in
`src/kz/replays/*`, `src/utils/circularfifobuffer.h`, and player lifecycle hooks.

**Verdict in one line:** This is **not a classic `new`-without-`delete` leak**.
It is a combination of (a) a permanently-resident pre-allocated per-player
circular buffer (~34 MB per slot), (b) per-player recorders that fan out into
ever-growing detached write threads with no concurrency limit, and (c) map-change
paths that never drain in-flight work or shrink per-player allocations. Each of
these is survivable on its own; together they push the working set into
double-digit gigabytes.

---

## TL;DR — Root causes ranked by impact

| # | Root cause | Location | Severity |
|---|---|---|---|
| 1 | `ReplayFileWriter` spawns an **unbounded** number of detached threads; each thread closure holds a full `unique_ptr<Recorder>` + zstd compression buffers; no concurrency cap | `src/kz/recording/filewriter.cpp` | 🔴 Critical |
| 2 | On disconnect, **all** unfinished `jumpRecorders` are unconditionally dispatched to detached write threads (no `desiredStopTime` guard, unlike `runRecorders`) | `src/kz/recording/events.cpp` (`OnClientDisconnect`) | 🔴 Critical |
| 3 | `CircularRecorder` is allocated **full-size up front** (~34 MB) and is **never freed by `Reset()`** — only the read cursor is advanced. Map change → `OnActivateServer` → `Reset()` → allocation stays | `src/kz/recording/kz_recording.h`, `kz_recording.cpp::Reset`, `events.cpp::OnActivateServer` | 🔴 High |
| 4 | Map change (`OnActivateServer`) calls `Reset()` only — it does **not** stop/restart the file writer, does **not** cap in-flight threads, and does **not** free per-player circular buffers | `src/kz/recording/events.cpp` | 🟠 High |
| 5 | `JumpRecorder` copies 5 s of `cmdData` + `cmdSubtickData` even though jump replays don't need server-received raw cmds | `src/kz/recording/recorders.cpp` | 🟠 Medium |
| 6 | `Reset()` does not clear `earliestMode` / `earliestStyles`; stale data leaks across map changes (small, but semantically wrong) | `src/kz/recording/kz_recording.cpp::Reset` | 🟡 Low |

---

## Per-player memory footprint of the circular recorder

`CircularRecorder` (created lazily by `EnsureCircularRecorderInitialized`, but
that function is invoked from almost every hot path: `OnPlayerActive`,
`RecordTickData_PhysicsSimulatePost`, `RecordCommand`, `CheckWeapons`,
`CheckModeStyles`, `OnJumpFinish`, …):

```cpp
// src/kz/recording/kz_recording.h
CFIFOCircularBuffer<TickData,    64 * 60 * 2>      *tickData;        // 7680 slots
CFIFOCircularBuffer<SubtickData, 64 * 60 * 2>      *subtickData;     // 7680 slots
CFIFOCircularBuffer<CmdData,     64 * (60 * 2 + 20)> *cmdData;       // 8960 slots
CFIFOCircularBuffer<SubtickData, 64 * (60 * 2 + 20)> *cmdSubtickData;// 8960 slots
CFIFOCircularBuffer<RpEvent,     64 * (60 * 2 + 20)> *rpEvents;      // 8960 slots
std::deque<RpJumpStats> jumps;
```

`CFIFOCircularBuffer` allocates the **full capacity** in its constructor:

```cpp
// src/utils/circularfifobuffer.h
CFIFOCircularBuffer() : capacity(SIZE), readPos(0), writePos(0), count(0)
{
    buffer = std::make_unique<T[]>(capacity);   // ← always full size
}
```

Estimated per-element sizes (after counting the embedded structs/unions):

| Buffer            | sizeof(T)  | Slots | Bytes      |
|-------------------|------------|-------|------------|
| TickData          | ~320 B     | 7 680 | ~2.4 MB    |
| SubtickData (tick)| ~1 544 B*  | 7 680 | **~11.6 MB** |
| CmdData           | ~120 B     | 8 960 | ~1.0 MB    |
| SubtickData (cmd) | ~1 544 B*  | 8 960 | **~13.5 MB** |
| RpEvent (union)   | ~640 B     | 8 960 | ~5.7 MB    |
| jumps (deque)     | variable   | —     | ≤ few MB   |
| **Total per player** |          |       | **≈ 34 MB** |

*`SubtickData::subtickMoves[64]` is the dominant term; the struct is ~1.5 KB.*

For a 64-slot server **34 MB × 64 ≈ 2.2 GB** of memory is pinned after every
slot has been occupied at least once — and survives map changes because
`Reset()` does not free the allocation (see RC #3).

Note: `CircularRecorder::TrimOldData()` is called every tick and removes stale
*entries* (old commands, events, jumps), but this only moves buffer cursors
inside the fixed-capacity ring buffer — it does **not** shrink the underlying
`unique_ptr<T[]>` heap allocation. `jumps` (a `std::deque`) is trimmed by
removing old items, but the deque may or may not return memory to the OS
depending on the STL implementation.

---

## Root cause details

### RC #1 — Unbounded concurrent detached threads per write task

```cpp
// src/kz/recording/filewriter.cpp
template<typename F>
void ReplayFileWriter::SpawnThread(F &&work)
{
    m_activeThreads++;
    std::thread(
        [this, work = std::forward<F>(work)]() mutable
        {
            work();
            if (--m_activeThreads == 0)
            {
                m_shutdownCV.notify_all();
            }
        })
        .detach();
}
```

There is **no write queue**. Every call to `QueueWrite()` or `QueueWriteToFile()`
immediately spawns a new detached `std::thread` with no upper bound on
concurrency. The thread closure captures:

- `std::unique_ptr<Recorder>` — owns vectors of TickData, SubtickData, CmdData,
  RpEvent, RpJumpStats (with nested vectors), weaponTable.
- Zstd compression input/output buffers (allocated inside `Recorder::WriteToFile()`
  or `Recorder::WriteToMemory()`).

Callback marshalling uses `m_completedCallbacks` (a `std::queue<std::function<void()>>`
of lightweight closures, drained every frame by `RunFrame()`). This callback
queue is **not** a significant memory contributor — the problem is the live
thread count and the Recorder objects held alive inside each thread closure.

Under multi-player load, especially when multiple players finish runs or
disconnect simultaneously (see RC #2), the thread count and aggregate
in-flight memory spike without bound. A single `RunRecorder` for a long course
can be tens to hundreds of MB.

### RC #2 — `OnClientDisconnect` floods threads with unfinished jumps

```cpp
// src/kz/recording/events.cpp::OnClientDisconnect
for (auto &recorder : this->jumpRecorders)
{
    if (fileWriter)
    {
        auto recorderPtr = std::make_unique<JumpRecorder>(std::move(recorder));
        this->CopyWeaponsToRecorder(recorderPtr.get());
        fileWriter->QueueWriteToFile(std::move(recorderPtr));
    }
}
```

Every active `JumpRecorder` is dispatched unconditionally — note the **absence**
of a `desiredStopTime > 0` guard. Compare with the `runRecorders` loop directly
above it, which *does* check `desiredStopTime > 0.0f`.

On a busy KZ server, players regularly rage-quit / kick-leave; each disconnect
can spawn a dozen detached threads simultaneously, each holding a multi-MB
Recorder + compression buffers. Combined with RC #1's lack of a concurrency
cap, memory spikes from "manageable" to "GBs in flight" inside a single tick.

### RC #3 — `Reset()` does not free the circular recorder

```cpp
// src/kz/recording/kz_recording.cpp::Reset
void KZRecordingService::Reset()
{
    if (this->circularRecording)
    {
        this->circularRecording->tickData->Advance(GetReadAvailable());
        this->circularRecording->subtickData->Advance(GetReadAvailable());
        this->circularRecording->cmdData->Advance(GetReadAvailable());
        this->circularRecording->cmdSubtickData->Advance(GetReadAvailable());
        this->circularRecording->rpEvents->Advance(GetReadAvailable());
        this->circularRecording->jumps.clear();
    }
    this->runRecorders.clear();
    this->jumpRecorders.clear();
    // ...
    // NOTE: circularRecording is NOT deleted here
}
```

`Advance` only moves the read cursor — it does **not** deallocate the
underlying `unique_ptr<T[]>`. The ~34 MB allocation stays.

`Reset()` is called from:

1. `KZRecordingService::OnActivateServer()` — on every map change for **every
   `KZPlayer` slot** that has ever been touched.
2. `KZPlayer::Reset()` — on respawn / round reset.

`OnClientDisconnect()` is the **only** place that actually
`delete circularRecording`. But `KZPlayer` objects are slot-resident (not
destroyed on disconnect), so:

- A **disconnect** during a map will free the buffer ✅
- A **map change** for a player who stays on the slot will **not** free it ❌

**Net effect:** every slot that has hosted at least one player accumulates a
permanent ~34 MB allocation across all subsequent map changes, until the
player disconnects. With 64 slots that is ~2.2 GB pinned regardless of how
empty the server currently is.

### RC #4 — Map change does not drain the writer or shrink per-player buffers

`KZRecordingService::OnActivateServer()`:

```cpp
void KZRecordingService::OnActivateServer()
{
    for (i32 i = 0; i < MAXPLAYERS + 1; i++)
    {
        KZPlayer *player = g_pKZPlayerManager->ToPlayer(i);
        if (player && player->recordingService)
            player->recordingService->Reset();
    }
}
```

This does **not**:
- Stop and restart `ReplayFileWriter` (which would join all in-flight threads via `Stop()`).
- Cap or cancel in-flight write threads.
- Free the per-player `circularRecording` buffers.

So any detached write threads spawned during map N are still running (holding
their Recorder payloads) while players on map N+1 are already producing new
recorders and spawning new threads. The file writer's `Stop()` method — which
blocks until `m_activeThreads == 0` — is only called from `Shutdown()` (plugin
unload), never from a map change.

### RC #5 — `JumpRecorder` copies cmdData unnecessarily

`JumpRecorder::JumpRecorder(Jump *jump)` calls
`Recorder(jump->player, 5.0f, RP_JUMPSTATS, false, DistanceTier_None)`.
Inside `Recorder::Recorder(...)` the matching block copies both `cmdData` and
`cmdSubtickData` from the circular buffer into the Recorder's vectors.

Jump replays do not need server-received raw command data — playback uses
`TickData` + `SubtickData` to reconstruct movement. This roughly **doubles**
the per-jump memory cost (each `cmdSubtickData` entry is ~1.5 KB) for no
playback benefit.

### RC #6 — Stale `earliestMode` / `earliestStyles` across map changes

```cpp
// src/kz/recording/kz_recording.cpp::Reset
// only Advance() on buffers + jumps.clear()
// earliestMode / earliestStyles are never reset
```

After a map change, `earliestMode.has_value()` is still true and points at
the previous map's mode/style metadata. The first `Recorder::Recorder(...)`
on the new map will pick up the wrong baseline (and a stale `RpModeStyleInfo`
gets baked into the next saved replay). Memory cost is tiny (~150 B), but it
is broken behaviour and can prevent the optional from being shrunk back to
`nullopt` during the lifetime of the slot.

---

## Why the symptom shows up specifically across map changes

1. While the round is running on map N, players accumulate jumps, complete
   runs, etc. Most in-flight write threads finish within seconds.
2. At `changelevel` time CS2 disconnects nobody — players stay on their slots.
   `OnClientDisconnect` (the only path that frees `circularRecording`) does
   not fire. `OnActivateServer` fires instead and only calls `Reset()`.
3. `Reset()`:
   - Clears `runRecorders` and `jumpRecorders` (their contents are destroyed
     → memory freed for those vectors).
   - Does **not** free the circular buffer's underlying heap.
   - Does **not** stop or drain in-flight write threads.
4. Any detached threads spawned late in map N are still running into map N+1,
   holding their Recorder payloads. Meanwhile new threads are spawned for
   new jumps/runs.
5. `Stop()` / drain only happens on plugin unload (`Shutdown()`), not on
   map change. So in-flight threads accumulate across consecutive map cycles
   when write throughput can't keep up with spawn rate.
6. Repeat over several maps and the high-water mark climbs every cycle.

The `2 GB → 10+ GB` curve fits: ~2 GB is the "all slots warm" steady state
from the circular buffers alone (RC #3); the additional 8 GB is the aggregate
of concurrent detached threads each holding a Recorder + compression buffers
(RC #1 + RC #2 + RC #4) that never fully drain between maps.

---

## Recommended fixes (in execution order)

### P0 — Cap concurrent write threads

Introduce a semaphore or atomic counter that limits the number of concurrent
`SpawnThread` invocations. When the limit is hit, the calling thread (main
thread, during `OnPhysicsSimulate` or disconnect handling) either blocks or
drops the task:

```cpp
// src/kz/recording/filewriter.cpp
static constexpr i32 kMaxConcurrentWrites = 4;  // tune to taste

void ReplayFileWriter::QueueWriteToFile(std::unique_ptr<Recorder> recorder,
                                         DiskWriteSuccessCallback onSuccess,
                                         WriteFailureCallback onFailure)
{
    if (m_activeThreads.load() >= kMaxConcurrentWrites)
    {
        Warning("[KZ] Replay writer saturated (%d active threads), dropping replay\n",
                m_activeThreads.load());
        // Optionally invoke onFailure if provided
        return; // recorder is destroyed here, memory freed immediately
    }
    SpawnThread(/* ... same as before ... */);
}
```

Apply the same guard to `QueueWrite()`. This single change prevents the
"unbounded thread × unbounded Recorder memory" explosion.

### P0 — On map change, drain in-flight write threads

In `KZRecordingService::OnActivateServer()`:

```cpp
void KZRecordingService::OnActivateServer()
{
    // Drain all in-flight writes before resetting player state.
    if (fileWriter)
    {
        fileWriter->Stop();   // blocks until m_activeThreads == 0
    }
    // Now reset per-player state.
    for (i32 i = 0; i < MAXPLAYERS + 1; i++)
    {
        KZPlayer *player = g_pKZPlayerManager->ToPlayer(i);
        if (player && player->recordingService)
            player->recordingService->Reset();
    }
    // Note: fileWriter does not need an explicit Start() —
    // it has no persistent thread; threads are spawned per-task.
}
```

Caveat: `Stop()` blocks the main thread until all writes complete. If this
is unacceptable for large replays, use a soft timeout or cancel mechanism.

### P1 — In `OnClientDisconnect`, guard jumpRecorder dispatch

```cpp
// src/kz/recording/events.cpp::OnClientDisconnect
for (auto &recorder : this->jumpRecorders)
{
    if (recorder.desiredStopTime > 0.0f && fileWriter)
    {
        auto recorderPtr = std::make_unique<JumpRecorder>(std::move(recorder));
        this->CopyWeaponsToRecorder(recorderPtr.get());
        fileWriter->QueueWriteToFile(std::move(recorderPtr));
    }
}
this->jumpRecorders.clear();
```

This mirrors the guard already present in the `runRecorders` loop.

### P1 — Free the circular recorder in `Reset()`

```cpp
// src/kz/recording/kz_recording.cpp::Reset
void KZRecordingService::Reset()
{
    delete this->circularRecording;
    this->circularRecording = nullptr;

    this->runRecorders.clear();
    this->jumpRecorders.clear();
    this->lastCmdNumReceived = 0;
    this->currentRunUUID = UUID_t(false);
    this->currentTickData   = {};
    this->currentSubtickData= {};
    this->lastKnownMode     = {};
    this->lastKnownStyles.clear();
    this->currentWeaponID   = -1;
    this->weapons.clear();
    this->lastJumpUUID      = UUID_t(false);
}
```

The ~34 MB allocation will be re-created lazily by the next call to
`EnsureCircularRecorderInitialized()` if (and only if) the slot becomes
active again. On a half-empty server this immediately reclaims the bulk of
the resident set across map changes. This also implicitly fixes RC #6
(`earliestMode`/`earliestStyles` are destroyed with the CircularRecorder).

### P1 — Make `CFIFOCircularBuffer` lazy or shrink-on-reset

Either (a) defer the `std::make_unique<T[]>(SIZE)` until the first `Write`,
or (b) add a `Clear()` that releases the buffer and resets capacity:

```cpp
// src/utils/circularfifobuffer.h
void Clear()
{
    buffer.reset();
    capacity = 0;
    readPos = writePos = count = 0;
}
```

Then call `tickData->Clear()` etc. from `Reset()` as an alternative to
deleting the entire `CircularRecorder`. Combined with the P1 delete approach,
this is belt-and-braces.

### P1 — Stop copying cmdData into `JumpRecorder`

Add a flag to `Recorder::Recorder(...)` (or a new ctor parameter)
indicating whether cmd data should be copied. Pass `false` from
`JumpRecorder`. This roughly halves per-jump memory, with no playback
regression (jump replays don't read `cmdData`).

### P2 — Fix the `Recorder` copy in `OnClientDisconnect`

Currently a `move` + `make_unique<JumpRecorder>` is performed, which
re-constructs the Recorder from the moved-from source. This is correct but
wasteful. Consider adding a `Recorder::takeOwnership()` that directly adopts
the internal vectors without re-copying from the circular buffer.

---

## Estimated post-fix memory profile

Assuming a 64-slot server with 32 active players and reasonable churn:

| Source | Before | After P0 only | After P0+P1 |
|---|---|---|---|
| Per-slot circular buffers (warm slots) | 64 × 34 MB ≈ 2.2 GB | 2.2 GB | 32 × 34 MB ≈ 1.1 GB (only active players) |
| In-flight write threads (peak) | unbounded (8+ GB observed) | ≤ 4 × ~100 MB ≈ 400 MB | same upper bound, **drained on map change** |
| `JumpRecorder` memory (per jump) | ~1–3 MB | ~1–3 MB | ~0.5–1.5 MB |
| **Stable RSS** | 10+ GB | ~3–4 GB | **~1.5–2 GB** |

The P0 pair alone should bring the curve back under control; the P1 set
returns the steady-state RSS to roughly the pre-recording baseline.

---

## Quick verification plan

1. Apply P0 changes; rebuild.
2. On a test server, populate with 20+ bots/clients, force several map
   changes (`changelevel <map>`) over 30 minutes, and watch RSS via
   `top -p $(pgrep cs2)` or `/proc/<pid>/status`.
3. Toggle `kz_replay_recording_debug 1` and confirm:
   - "Replay writer saturated … dropping replay" appears under load
     (proves the thread cap is effective).
   - Map change no longer causes a monotonic RSS climb across cycles.
4. Verify functional regressions:
   - `kz_rpsave` still produces a file.
   - Run replays at timer end still save (subject to cap/drop policy).
   - Jump replays still save.
   - Cheater replays still save.
5. Once stable, layer in the P1 fixes one at a time, repeating the RSS
   measurement to confirm each step reduces the high-water mark.

---

## Files touched by the recommended fixes

- `src/kz/recording/filewriter.cpp` — thread concurrency cap.
- `src/kz/recording/events.cpp` — `OnClientDisconnect` guard on jumpRecorders;
  `OnActivateServer` drain via `fileWriter->Stop()`.
- `src/kz/recording/kz_recording.cpp` — `Reset()` deletes `circularRecording`.
- `src/kz/recording/recorders.cpp` — skip `cmdData`/`cmdSubtickData` copy in `JumpRecorder`.
- `src/utils/circularfifobuffer.h` (optional) — add `Clear()` / lazy allocation.

No changes are required outside the recording / replay subsystem, and none
of the proposed fixes alter the on-disk replay format.

---

## Corrections from code review pass

The following items from the initial rapid analysis were found to be
**inaccurate** after a thorough re-read of the current codebase:

| Initial claim | Corrected finding |
|---|---|
| `ReplayFileWriter` has an unbounded `m_writeQueue` consumed by a single thread | There is **no queue**. Each write task spawns a detached `std::thread` via `SpawnThread()`. The only queue is `m_completedCallbacks` (lightweight `std::function<void()>` closures drained every frame). |
| Fix suggestion: "bound the write queue with max size" | Correct fix: **cap concurrent threads** via `m_activeThreads` limit in `QueueWriteToFile` / `QueueWrite`. |
| `m_completedWrites` is an unbounded queue of results | Actual name is `m_completedCallbacks`; it holds small callbacks, not Recorder data. Not a significant contributor. |
| `OnClientDisconnect` does not free `circularRecording` | It **does** — `delete this->circularRecording; this->circularRecording = nullptr;` at the end of the function. The gap is only on the **map change** path (`Reset()`). |
| `data.cpp` contains placement-new UB (`vector::data()` + `delete[]`) | This pattern was **not found** in the current master branch. If it existed in an older revision or different branch, it is not present now. |
