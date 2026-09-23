# Design: Single Tag Set (`single_tagset`)

Status: implemented on branch `single_tagset_v2`, gated by the `single_tagset`
config option (default off). Issue: #830.

## Problem

Normally every monitor owns a private copy of the tag set: tag 3 can be
visible on two monitors at once, and each monitor's tag bits are independent.

With `single_tagset = 1` all monitors share **one global set of tags**:

- Each tag is displayed by **at most one monitor** at any time.
- Viewing a tag that another monitor displays **transfers** it; the other
  monitor must visibly switch somewhere else.
- Clients **follow their tags**: a window lives on the monitor that displays
  its tags, not on the monitor it was opened on.

Inspired by the dwm/dwl `singletagset` patch, extended with configurable
eviction landing policies (including a true swap) and client-follow
semantics.

## Core model: derived ownership

The central design decision: **ownership is derived state, never stored**.

- `c->tags` is the client's tag assignment. View changes **never rewrite it**.
- A tag is "owned" by whichever monitor currently has its bit in
  `m->tagset[m->seltags]`.
- The single source of truth for "who shows tag X" is
  `st_monitor_showing_tags(tags, exclude)` — an O(monitors) scan.
- Visibility goes through `CLIENT_TAGS(c, m)` / `st_client_tags()`:
  a client is visible on `m` iff `m` displays any of its tags. A client whose
  tags are displayed nowhere is an **orphan**: it stays parked (hidden) on
  its current monitor with its tag assignment intact until the tag is viewed
  again.

Consequences of this model:

- No bookkeeping structures to keep in sync; impossible to "lose" a client.
- Orphans are a first-class, recoverable state (parked ≠ destroyed).
- Cost: every visibility test resolves ownership with a monitor scan.
  `CLIENT_TAGS` is documented as not-per-pixel-safe for this reason.

### Exclusions

These always keep classic per-monitor behavior and are excluded from every
query and transaction (`st_active()` is the gate):

- the special workspace (tag `00` / TAG0_MASK),
- global (`isglobal`) and universal-global (`isunglobal`) windows,
- overview mode monitors (`m->isoverview`), cleanup monitors, disabled
  outputs,
- minimized and parked clients.

`single_tagset` is incompatible with `tag_gather` (auto-disabled): gathering
would fight tag ownership.

## Architecture

All logic lives in `src/manage/tagset.c` (`st_*` API, header
`include/mango/manage/tagset.h`). The file is split into two layers:

1. **Pure queries** (top half): `st_active`, `st_monitor_showing_tags`,
   `st_other_used_tagset`, `st_unused_tag`, `st_client_tags`. They read
   state and mutate nothing.
2. **View transactions** (bottom half): `st_apply_view`,
   `st_migrate_clients`, `st_rehome_clients`, `st_follow_client`, plus the
   eviction policy table. These change tagsets and client homes as a unit.

Integration points are deliberately thin:

| Site | Role |
| --- | --- |
| `client_view_on_monitor()` | slot-flip view path: calls `st_apply_view` *before* flipping, `st_migrate_clients(landed)` *after* writing the new view |
| `toggle_view()`, `combo_view()`, overview exit | direct view writers: same before/after protocol |
| `client_set_monitor()` | redirects a cross-monitor move to the tag owner; runs a takeover transaction when the client brings unseen tags |
| `st_follow_client()` | after a client is re-tagged in place: move it to the owner, or pull the tags to it |
| `st_rehome_clients()` | monitor hotplug/disable/close: resolve duplicate views, re-home everyone |
| `override_config()` | clamps `single_tagset_evict`, installs the policy via `st_set_evict_policy()` |

## The view transaction

Taking over a tag can cascade: A takes tag from B, B lands on a tag held by
C, C's tag is taken by the chain, ... `st_apply_view(m, newtags)` runs this
as **one atomic transaction**:

```
landed = st_apply_view(m, newtags);   /* free newtags from other owners */
m->tagset[m->seltags] = newtags;      /* caller writes its OWN view */
st_migrate_clients(landed);           /* clients follow the new ownership */
arrange(every touched monitor);
```

Invariants enforced by this protocol:

- `st_apply_view` **never touches the initiator's view**. The caller writes
  it (via slot flip in `client_view_on_monitor`, directly elsewhere). This
  keeps tag history (`pertag->prevtag/curtag`, inactive slot) intact on the
  initiator.
- Migration runs **after** the initiator's write, because
  `st_monitor_showing_tags` must already see the new ownership — otherwise
  clients pulled onto landing tags would find no owner and stay behind.
- Every evicted monitor gives up **only the bits the chain actually takes**
  (`blocked | taken`); the rest of its view survives.
- All tagset writes go through `st_set_view()` so the **inactive slot stays
  consistent**: `arrange()` and the all-tags toggle read the inactive slot,
  and a stale copy of a taken tag there would resurrect duplicate ownership.
- Cycles degrade to swaps: if the eviction chain runs back into `m`, the
  loop stops; the caller's pending view switch completes the exchange.
- `st_apply_view` returns the **landing tags** (union of tags evicted
  monitors landed on). On a swap these are exactly the initiator's old view
  — its clients are momentarily orphans from the migration scan's point of
  view, so `st_migrate_clients(landing)` pulls them explicitly.

### Eviction landing policies (`single_tagset_evict`)

When monitor B is evicted, where does it land? Policy table
(`st_land_*` in `tagset.c`), selected by `st_set_evict_policy()`:

| Value | Name | Behavior | Fallback |
| --- | --- | --- | --- |
| `0` | unused | first tag displayed by nobody (dwm style) | — |
| `1` | history | the tag B displayed before its current one (`pertag->prevtag`), if it is a single regular tag nobody in the chain holds | unused |
| `2` | swap | the view the **initiator displayed before the switch** (`init->tagset[init->seltags]` at eviction time — the slot flip hasn't happened yet), if single regular tag not displayed by the evictee | history → unused |

Notes on the swap policy:

- The initiator's *inactive* slot must **not** be used as the swap target:
  at eviction time it holds stale history, not the outgoing view. This was a
  real bug (fake fallbacks, empty monitors) caught during development.
- Multi-tag or TAG0 views on either side disable the swap; it degrades to
  history.
- Cascades: in a chain A→B→C each evicted monitor takes the *initiator's*
  old view only if still free; otherwise it degrades, so at most one true
  swap happens per transaction.

## Client migration

`st_migrate_clients(landing)` walks all regular clients:

- Tags shown by monitor X → `c->mon = X` (clearing stale `sel` pointers),
  followed by `resize(c, c->geom, 0)` so tiled layouts place the client
  correctly even though layouts normally skip not-yet-visible clients.
- Tags shown by nobody → **orphan**: keep tags, stay parked hidden. Earlier
  designs re-tagged orphans onto their monitor's view; that silently
  destroyed user tag assignments and was reverted.
- `landing` bits are treated as "shown" for the duration of the call so
  swap targets pull their clients even mid-transaction.

Callers must `arrange()` every monitor whose view or clients changed —
including the *other* monitors in a swap. Missing those arranges was the
"windows keep old size until I revisit the tag" bug.

## Focus handling

A transaction can move the focused client to another monitor. Rules:

- Inside `st_apply_view`, the initiator repairs `m->sel` **directly** and
  only calls `client_focus()` when `m` is the selected monitor.
  `client_focus()` follows the candidate to its monitor and flips
  `selected_monitor`, which would make the caller operate on the wrong
  monitor (observed: keyboard jumping to the other screen mid-swap).
- `st_rehome_clients()` repairs `sel` on every monitor after each duplicate
  resolution round, because view writes invalidate previous repairs.

## Monitor topology changes (`st_rehome_clients`)

Called on hotplug, output disable, monitor destruction. Steps:

1. **Duplicate resolution loop** (bounded rounds): find two monitors sharing
   a tag bit. The monitor whose history "returns to" the duplicated tag
   (inactive slot holds exactly that single tag = `pertag->prevtag`) wins it;
   otherwise the later monitor yields. The loser keeps the rest of its view;
   only a monitor left with an empty view gets a fresh tag.
2. `st_migrate_clients(0)` re-homes everyone.
3. Per-monitor focus repair + `arrange()`, then a global
   `printstatus(IPC_WATCH_ARRANGGE)` because callers typically only arrange
   the selected monitor.

Deliberate limitation accepted: on config **reload** rehoming is restricted
(tag-count shrink only) — a plain reload must not shuffle client homes.
Unplugging a monitor can leave its tags unowned until the user views them;
accepted as reasonable.

## Configuration

```
single_tagset = 0        # master switch (runtime-reloadable)
single_tagset_evict = 0  # 0 unused | 1 history | 2 swap
```

Parsed in `src/config/parse_config.c`, clamped in `override_config()`
(unknown values behave like `0`), documented in
`docs/configuration/miscellaneous.md`.

## Dispatch semantics worth knowing

These behaviors were trimmed out of the user-facing docs table (kept to one
compact row there) and are specified here instead:

- **Takeover is visible.** Viewing a tag another monitor displays transfers
  it, and the evicted monitor visibly switches somewhere else — it is never
  left blank. Which tag it lands on is the `single_tagset_evict` policy
  (see above); cycles degrade to a swap.
- **All-bits-held toggles are no-ops.** If *every* bit of the resulting
  tagset is displayed by other monitors, `toggle_view`/`combo_view` do
  nothing rather than evict everyone (`newtagset &=
  ~st_other_used_tagset(...)` guard). A partial overlap still proceeds and
  evicts only the overlapping bits' owners.
- **Monitor-targeted commands keep working.** `viewcrossmon`,
  `tagcrossmon` and friends operate on the targeted monitor as expected:
  viewing a tag owned elsewhere takes it over **there**, not on the
  selected monitor. The redirect in `client_set_monitor` handles the
  tag-carrying variants (a move that brings tags follows the owner).
- **Exclusions keep classic behavior.** Special workspace (tag `00`) and
  global/universal-global windows are outside the ownership model entirely
  (`st_active()` gate + per-query filters).
- **`tag_gather` is force-disabled** with a warning when `single_tagset` is
  on: gathering compacts tags per monitor inside `arrange()` and would
  silently collapse two monitors onto the same tag.
- **Overview entry is untouched**; overview **exit** may target a tag owned
  by another monitor, so it runs a takeover transaction too.
- **Runtime enable** (`setoption`/reload): per-monitor views predating the
  feature may duplicate; `reset_option()` detects the off→on transition and
  runs `st_rehome_clients()` to resolve duplicates and re-home clients.

## Testing notes

Verified manually on a dual-monitor setup (issue reporter's hardware):
single-bit and cascaded takeovers, cycles→swaps in all three policies,
multi-window swaps, `viewcrossmon`/`tagcrossmon`, overview exit onto owned
tags, config reload, monitor unplug. Known accepted quirks: none blocking;
unplug leaves orphan tags by design.

## File map

| Path | Contents |
| --- | --- |
| `src/manage/tagset.c` | entire feature (queries, transactions, policies) |
| `include/mango/manage/tagset.h` | public `st_*` API, `ST_EVICT_*` enum |
| `src/dispatch/bind.c` | `toggle_view`, `combo_view`, overview exit integration |
| `src/manage/client.c` | `client_view_on_monitor`, `client_set_monitor` integration |
| `src/config/parse_config.c` | option parsing, clamping, policy install |
| `include/mango/manage/client.h` | `CLIENT_TAGS` / `VISIBLEON` ownership-aware macros |
| `docs/configuration/miscellaneous.md` | user-facing option documentation |
