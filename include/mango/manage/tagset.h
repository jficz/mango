#ifndef __MANAGE_TAGSET_H__
#define __MANAGE_TAGSET_H__ 1

#include "mango/common/types.h"
#include "mango/manage/monitor.h"
#include <stdbool.h>
#include <stdint.h>

/* Single tag set: all monitors share one global set of tags. Each tag is
 * displayed by at most one monitor at a time and clients follow the monitor
 * displaying their tags. Ownership is derived from (c->tags, tagsets) only;
 * client tags are never rewritten by view changes. See docs/design/ for the
 * invariants. */

/* Declared in manage/monitor.h; repeated here so the VISIBLEON/TAGMATCH
 * macros can use it through this header alone. */
bool is_special_active(const Monitor *m);

/* True when the single tag set is active and m is a normal viewable monitor
 * (not overview, not being torn down, enabled). */
bool st_active(const Monitor *m);

/* Monitor currently displaying any of `tags`, or NULL. `exclude` is skipped
 * (may be NULL). Special/TAG0 views never own regular tags. */
Monitor *st_monitor_showing_tags(uint32_t tags, const Monitor *exclude);

/* Union of tags displayed by all other active monitors. */
uint32_t st_other_used_tagset(const Monitor *m);

/* First tag bit (1..tag_num) displayed by no monitor; fallback bit 1. */
uint32_t st_unused_tag(void);

/* Display owner of a client under the single tag set: the monitor showing
 * its tags, falling back to c->mon. Pass exclude != NULL to ignore one
 * monitor (e.g. the destination during a move). */
Monitor *st_client_owner(const Client *c, const Monitor *exclude);

/* Effective tags of c for tag matching on m under the single tag set: the
 * caller guarantees c is a regular (non-TAG0, non-global) client; returns
 * c->tags when m is the owner (or ownership is undetermined), and 0 when
 * another monitor owns the tags. Raw c->tags is never modified. */
uint32_t st_client_tags(const Client *c, const Monitor *m);

#endif
