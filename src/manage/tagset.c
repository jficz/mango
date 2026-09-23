#include "mango/manage/tagset.h"
#include "mango/animation/client.h"
#include "mango/common/server.h"
#include "mango/config/parse_config.h"
#include "mango/ipc/ipc.h"
#include "mango/layout/arrange.h"
#include "mango/manage/client.h"
#include "mango/manage/monitor.h"

/* Single tag set implementation. The query primitives below (up to the
 * "view planning" divider) are pure: they read existing state and mutate
 * nothing. Everything past the divider performs view transactions that
 * change tagsets and client tags as a unit. */

bool st_active(const Monitor *m) {
	return config.single_tagset && m && !m->isoverview && !m->iscleanuping &&
		   m->wlr_output && m->wlr_output->enabled;
}

Monitor *st_monitor_showing_tags(uint32_t tags, const Monitor *exclude) {
	Monitor *tm;

	if (!config.single_tagset || !(tags & TAGMASK))
		return NULL;

	wl_list_for_each(tm, &server.monitors, link) {
		if (tm == exclude || tm->isoverview || tm->iscleanuping ||
			!tm->wlr_output->enabled)
			continue;
		if (tm->tagset[tm->seltags] & tags & TAGMASK)
			return tm;
	}
	return NULL;
}

uint32_t st_other_used_tagset(const Monitor *m) {
	Monitor *tm;
	uint32_t used = 0;

	wl_list_for_each(tm, &server.monitors, link) {
		if (tm == m || !st_active(tm))
			continue;
		used |= tm->tagset[tm->seltags] & TAGMASK;
	}
	return used;
}

uint32_t st_unused_tag(void) {
	Monitor *m;
	uint32_t used = 0;

	wl_list_for_each(m, &server.monitors, link) {
		if (!st_active(m))
			continue;
		used |= m->tagset[m->seltags] & TAGMASK;
	}

	for (size_t i = 0; i < (size_t)config.tag_num; i++) {
		if (!(used & (1u << i)))
			return 1u << i;
	}
	return 1;
}

/* Set m's current view to a tagset no other monitor displays, keeping the
 * inactive slot consistent (it is read as history by the all-tags toggle)
 * and pertag pointing at a valid tag index. */
static void st_set_view(Monitor *m, uint32_t tags) {
	uint32_t cur;

	tags &= TAGMASK;
	if (!tags)
		tags = st_unused_tag();
	m->tagset[m->seltags] = tags;
	if (!(m->tagset[m->seltags ^ 1] & TAGMASK))
		m->tagset[m->seltags ^ 1] = tags;
	if (!m->pertag)
		return;
	cur = get_tags_first_tag_num(tags);
	m->pertag->curtag = cur;
	if (!m->pertag->prevtag || m->pertag->prevtag > (uint32_t)config.tag_num)
		m->pertag->prevtag = cur;
}

void st_take_unused_tag(Monitor *m) {
	m->tagset[0] = m->tagset[1] = st_unused_tag();
	if (m->pertag)
		m->pertag->curtag = m->pertag->prevtag =
			get_tags_first_tag_num(m->tagset[m->seltags]);
}

uint32_t st_client_tags(const Client *c, const Monitor *m) {
	Monitor *owner;

	if (!config.single_tagset || !m || m->isoverview)
		return c->tags;

	/* special views only show TAG0 clients; regular clients never match */
	if (is_special_active(m))
		return 0;

	owner = st_monitor_showing_tags(c->tags, NULL);
	if (!owner || owner == m)
		return c->tags;
	return 0;
}

/* ------------------------- view planning ------------------------------- */

/* Eviction landing policies: pick the tagset an evicted monitor gets when
 * another monitor takes over its tags. blocked is the union of tagsets the
 * chain already occupies, init is the monitor initiating the takeover. */

/* Land on the first tag displayed by no monitor (dwm singletagset style). */
static uint32_t st_land_unused(const Monitor *m, const Monitor *init,
							   uint32_t blocked) {
	return st_unused_tag();
}

/* Land on the tag the monitor displayed before its current one, if it is a
 * single regular tag nobody else in the chain displays; fall back to unused. */
static uint32_t st_land_history(const Monitor *m, const Monitor *init,
								uint32_t blocked) {
	/* prevtag 0 is the special workspace: no regular tag behind it */
	uint32_t hist = m->pertag && m->pertag->prevtag > 0
						? (1u << (m->pertag->prevtag - 1))
						: 0;

	hist &= TAGMASK & ~blocked;
	if (hist && !(hist & (hist - 1)))
		return hist;
	return st_unused_tag();
}

/* Swap: land on the view the initiator displayed before it took our tags.
 * The slot flip in client_view_on_monitor keeps that view in the initiator's
 * inactive slot. Falls back to history, then unused, when the initiator has
 * no usable previous view. */
static uint32_t st_land_swap(const Monitor *m, const Monitor *init,
							 uint32_t blocked) {
	uint32_t prev;

	if (init) {
		/* the initiator's current view is what it is taking from us; its
		 * inactive slot may hold anything (history toggle, stale copy), so
		 * only swap when it is a single regular tag we do not display */
		prev = init->tagset[init->seltags] & TAGMASK;
		if (prev && !(prev & (prev - 1)) && !(prev & m->tagset[m->seltags]))
			return prev;
	}
	return st_land_history(m, init, blocked);
}

static uint32_t (*st_evict_policy)(const Monitor *m, const Monitor *init,
								   uint32_t blocked) = st_land_unused;

void st_set_evict_policy(int32_t mode) {
	switch (mode) {
	case ST_EVICT_SWAP:
		st_evict_policy = st_land_swap;
		break;
	case ST_EVICT_HISTORY:
		st_evict_policy = st_land_history;
		break;
	default:
		st_evict_policy = st_land_unused;
		break;
	}
}

/* Move every regular client onto the monitor displaying its tags. Clients
 * whose tags nobody shows keep their tags and stay parked (hidden) where
 * they are: adopting a foreign view would destroy their tag assignment. */
void st_migrate_clients(uint32_t landing) {
	Client *c;
	Monitor *owner;

	wl_list_for_each(c, &server.clients, link) {
		if (c->iskilling || client_is_parked(c) || c->isminimized ||
			(c->tags & TAG0_MASK) || c->isglobal || c->isunglobal)
			continue;
		owner = st_monitor_showing_tags(c->tags, NULL);
		if (!owner)
			continue; /* orphan: hidden until its tag is viewed again */
		if (owner != c->mon) {
			if (c->mon && c->mon->sel == c)
				c->mon->sel = NULL;
			c->mon = owner;
			/* make sure the client actually gets resized on the new
			 * monitor: layouts skip clients that are not visible yet,
			 * so set the geometry directly (same as client_set_monitor) */
			resize(c, c->geom, 0);
		}
	}
}

/* Re-home clients after a monitor joined or left the layout: move clients to
 * the monitor showing their tags; clients whose tags nobody shows stay
 * parked on their monitor until their tag is viewed again. Duplicate views
 * are resolved so that after this call no tag is displayed by more than one
 * monitor. Arranges every active monitor. */
/* History signal: m's inactive slot holds exactly the tag its pertag history
 * remembers from before the current view. Such a monitor is "returning" to
 * that tag and wins it in a duplicate resolution. */
static bool st_returning_to(const Monitor *m, uint32_t tagbit) {
	return m->pertag && m->pertag->prevtag > 0 && tagbit &&
		   !(tagbit & (tagbit - 1)) && (m->tagset[m->seltags ^ 1] & tagbit) &&
		   (m->tagset[m->seltags ^ 1] & TAGMASK) == tagbit;
}

void st_rehome_clients(void) {
	Monitor *tm, *other;
	uint32_t seen, dup;

	if (!config.single_tagset)
		return;

	/* per-monitor views may pre-date the single tag set (feature toggled
	 * at runtime), or a monitor may have been destroyed leaving its tags
	 * unowned: resolve duplicates by history. A monitor whose history
	 * points back at a duplicated tag returns to it; the other copy is
	 * dropped and that monitor keeps the rest of its view. Only a monitor
	 * left with an empty view gets a fresh tag. */
	for (int32_t round = 0; round < config.tag_num + 2; round++) {
		seen = 0;
		dup = 0;
		tm = other = NULL;
		wl_list_for_each(tm, &server.monitors, link) {
			if (!st_active(tm))
				continue;
			other = st_monitor_showing_tags(tm->tagset[tm->seltags], tm);
			if (other) {
				dup = tm->tagset[tm->seltags] & other->tagset[other->seltags] &
					  seen & TAGMASK;
				if (!dup)
					dup = tm->tagset[tm->seltags] &
						  other->tagset[other->seltags] & TAGMASK;
				break;
			}
			seen |= tm->tagset[tm->seltags] & TAGMASK;
		}
		if (!tm || !other)
			break; /* no duplicates left */

		if (st_returning_to(tm, get_tags_first_tag(dup))) {
			/* tm is returning to the tag: other drops its copy */
			st_set_view(other, other->tagset[other->seltags] & ~dup);
			if (!(other->tagset[other->seltags] & TAGMASK))
				st_take_unused_tag(other);
		} else if (st_returning_to(other, get_tags_first_tag(dup))) {
			st_set_view(tm, tm->tagset[tm->seltags] & ~dup);
			if (!(tm->tagset[tm->seltags] & TAGMASK))
				st_take_unused_tag(tm);
		} else {
			/* no history signal: the later monitor yields */
			st_set_view(tm, tm->tagset[tm->seltags] & ~dup);
			if (!(tm->tagset[tm->seltags] & TAGMASK))
				st_take_unused_tag(tm);
		}

		/* views changed: repair focus before the next round reads sel */
		wl_list_for_each(other, &server.monitors, link) {
			if (!st_active(other))
				continue;
			if (other->sel && other->sel->mon != other)
				other->sel = NULL;
			if (!other->sel)
				other->sel = client_focus_top(other);
		}
	}

	st_migrate_clients(0);

	wl_list_for_each(tm, &server.monitors, link) {
		if (!st_active(tm))
			continue;
		/* focus may point at a client that moved to another monitor */
		if (tm->sel && tm->sel->mon != tm)
			tm->sel = NULL;
		if (!tm->sel)
			tm->sel = client_focus_top(tm);
		arrange(tm, false, false);
	}

	/* callers may only arrange the selected monitor: notify every watcher
	 * that tagsets and client homes changed on all of them */
	printstatus(IPC_WATCH_ARRANGGE);
}

/* Evict every monitor displaying parts of `newtags` so m can take them as
 * one transaction. The caller is responsible for actually switching m's
 * view: this only frees the tags from their current owners. Each evicted
 * monitor lands on a tag chosen by the eviction policy and gives up only
 * the tags the chain actually takes. If the eviction chain runs into m
 * again (cycle), the operation degrades to a swap: m adopts the tagset of
 * the monitor it collided with, so the caller's view switch completes the
 * exchange. Clients follow their tags onto the new owners; clients keeping
 * no visible tag are reassigned to their tag owner. Returns the tags evicted
 * monitors landed on (landing tags): the caller must pass them to
 * st_migrate_clients so clients homed there are pulled even when orphaned. */
uint32_t st_apply_view(Monitor *m, uint32_t newtags) {
	Monitor *chain[tag_num_MAX + 2];
	Monitor *tm;
	uint32_t blocked, taken, landed, land, oldset;
	int32_t depth, i;

	if (!st_active(m) || !(newtags & TAGMASK))
		return 0;

	tm = st_monitor_showing_tags(newtags, m);
	if (!tm)
		return 0; /* nothing to steal: plain local view switch */

	blocked = newtags & TAGMASK;
	taken = tm->tagset[tm->seltags] & blocked;
	landed = 0;
	chain[0] = m;
	depth = 1;

	/* evict the chain of monitors sitting on the tags we take. Every tagset
	 * write goes through st_set_view so both slots stay valid: arrange() and
	 * tag history read the inactive slot (UINT32_MAX view toggle) and stale
	 * copies of taken tags there would resurrect duplicate ownership. */
	while (tm && depth < tag_num_MAX + 2) {
		if (tm == m) {
			/* cycle: m already owns the tag we're trying to take. Nothing
			 * to evict; the caller's view switch is a no-op. */
			break;
		}
		chain[depth++] = tm;
		oldset = tm->tagset[tm->seltags];
		land = st_evict_policy(tm, m, blocked | taken) & TAGMASK;
		if (!land || (land & taken))
			land = st_unused_tag();
		/* keep the parts of its view the chain does not take */
		st_set_view(tm, (oldset & ~(blocked | taken)) | land);

		blocked |= oldset & TAGMASK;
		taken |= land;
		taken &= blocked;
		landed |= land;

		tm = st_monitor_showing_tags(newtags, m);
	}

	/* NOTE: migration happens after the caller writes the initiator's view
	 * (st_migrate_clients), because st_monitor_showing_tags must see the new
	 * ownership: clients pulled onto landing tags (swap) move there then.
	 * The caller must pass the returned landing tags to st_migrate_clients
	 * so clients homed on them are pulled even when orphaned. */

	/* arrange every monitor whose view or clients changed */
	for (i = 0; i < depth; i++) {
		if (st_active(chain[i]))
			arrange(chain[i], false, false);
	}

	/* focus may have pointed at a client that migrated away. Recover the
	 * initiator's focus without stealing the keyboard: client_focus() would
	 * follow the candidate to its monitor and flip selected_monitor, which
	 * makes the caller operate on the wrong monitor afterwards. */
	if (m->sel && m->sel->mon != m)
		m->sel = NULL;
	if (!m->sel) {
		Client *fc = client_focus_top(m);
		if (fc) {
			if (m->sel && m->sel != fc)
				m->sel->isfocusing = false;
			m->sel = fc;
			if (server.selected_monitor == m)
				client_focus(fc, 0);
		}
	}

	printstatus(IPC_WATCH_ARRANGGE);

	return landed;
}

void st_follow_client(Client *c) {
	Monitor *m;
	uint32_t cur;

	if (!config.single_tagset || !c || !c->mon || (c->tags & TAG0_MASK))
		return;

	m = c->mon;
	cur = m->tagset[m->seltags] & TAGMASK;
	if (cur && !(c->tags & cur)) {
		Monitor *owner = st_monitor_showing_tags(c->tags, m);
		if (owner) {
			/* Another monitor displays the client's tags: move the client
			 * there, don't switch this monitor's view */
			client_set_monitor(c, owner, 0, false);
		} else {
			/* Nobody displays the tags: bring them here via view switch */
			client_view_on_monitor(&(Arg){.ui = c->tags}, false, m, false);
		}
	}
}

bool st_client_shown(Client *c) { return c && c->mon && VISIBLEON(c, c->mon); }
