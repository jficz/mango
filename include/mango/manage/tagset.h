#ifndef __MANAGE_TAGSET_H__
#define __MANAGE_TAGSET_H__ 1

#include "mango/common/types.h"
#include "mango/manage/monitor.h"
#include <stdbool.h>
#include <stdint.h>

/* Single tag set: all monitors share one global set of tags. Each tag is
 * displayed by at most one monitor at a time and clients follow the monitor
 * displaying their tags. Ownership is derived from (c->tags, tagsets) only;
 * client tags are never rewritten by view changes. */

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

/* Point m's view and tag history at a tag no other monitor displays. */
void st_take_unused_tag(Monitor *m);

/* Effective tags of c for tag matching on m under the single tag set: the
 * caller guarantees c is a regular (non-TAG0, non-global) client; returns
 * c->tags when m is the owner (or ownership is undetermined), and 0 when
 * another monitor owns the tags. Raw c->tags is never modified. */
uint32_t st_client_tags(const Client *c, const Monitor *m);

/* View transaction: give monitor m the tagset newtags, evicting other
 * monitors that display parts of it (see st_apply_view in tagset.c). No-op
 * when single_tagset is off or no other monitor holds any of newtags. Does
 * NOT switch m's view: the caller writes it, then passes the returned
 * landing tags (what the evicted monitors landed on) to
 * st_migrate_clients and arranges every touched monitor. */
uint32_t st_apply_view(Monitor *m, uint32_t newtags);

/* After re-tagging a client in place: if its tags moved to a monitor other
 * than c->mon and are invisible there, follow them with a view transaction.
 * No-op when single_tagset is off. */
void st_follow_client(Client *c);

/* Migrate clients to the monitors displaying their tags. landing names
 * extra tags to pull clients for even when no monitor displays them yet
 * (swap targets, see st_apply_view). */
void st_migrate_clients(uint32_t landing);

/* True when c is displayed on its current monitor: equivalent to
 * VISIBLEON(c, c->mon), which honors the single tag set ownership rules. */
bool st_client_shown(Client *c);

enum {
	ST_EVICT_UNUSED = 0,  /* evicted monitors land on the first unused tag */
	ST_EVICT_HISTORY = 1, /* evicted monitors restore their previous tag */
	ST_EVICT_SWAP = 2,	  /* evicted monitors take the initiator's old view */
};

/* Eviction landing policy (see ST_EVICT_* values). Unknown values behave
 * like ST_EVICT_UNUSED. */
void st_set_evict_policy(int32_t mode);

/* Arrange every active monitor except skip: needed after a view
 * transaction, whose clients may land on other monitors' new tags while
 * callers typically only arrange the initiator. */
void st_arrange_others(const Monitor *skip, bool want_animation);

/* Re-home clients after monitors joined/left (hotplug, disable, close). */
void st_rehome_clients(void);

#endif
