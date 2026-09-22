#include "mango/manage/tagset.h"
#include "mango/common/server.h"
#include "mango/config/parse_config.h"
#include "mango/ipc/ipc.h"
#include "mango/layout/arrange.h"
#include "mango/manage/client.h"
#include "mango/manage/monitor.h"

/* Core primitives of the single tag set. Pure queries over existing state:
 * no tagsets or client tags are ever mutated here. */

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
 * chain already occupies. */

/* Land on the first tag displayed by no monitor (dwm singletagset style). */
static uint32_t st_land_unused(const Monitor *m, uint32_t blocked) {
	return st_unused_tag();
}

/* Land on the tag the monitor displayed before its current one, if it is a
 * single regular tag nobody else in the chain displays; fall back to unused. */
static uint32_t st_land_history(const Monitor *m, uint32_t blocked) {
	uint32_t hist = m->pertag ? (1u << (m->pertag->prevtag - 1)) : 0;

	hist &= TAGMASK & ~blocked;
	if (hist && !(hist & (hist - 1)))
		return hist;
	return st_unused_tag();
}

static uint32_t (*st_evict_policy)(const Monitor *m,
								   uint32_t blocked) = st_land_unused;

void st_set_evict_policy(int32_t history) {
	st_evict_policy = history ? st_land_history : st_land_unused;
}

/* Re-home clients whose tags are (or are not) displayed anywhere after a
 * monitor joined or left the layout: move clients to the monitor showing
 * their tags; orphan clients (tags shown by nobody) adopt the view of the
 * monitor they end up on. Arranges every active monitor. */
void st_rehome_clients(void) {
	Client *c;
	Monitor *tm, *owner;

	if (!config.single_tagset)
		return;

	wl_list_for_each(c, &server.clients, link) {
		if (c->iskilling || client_is_parked(c) || c->isminimized ||
			(c->tags & TAG0_MASK) || c->isglobal || c->isunglobal)
			continue;
		owner = st_monitor_showing_tags(c->tags, NULL);
		if (!owner) {
			if (c->mon && st_active(c->mon)) {
				client_set_tags(c, c->mon->tagset[c->mon->seltags]);
			} else if (server.selected_monitor &&
					   st_active(server.selected_monitor)) {
				if (c->mon && c->mon->sel == c)
					c->mon->sel = NULL;
				c->mon = server.selected_monitor;
				client_set_tags(c,
								server.selected_monitor
									->tagset[server.selected_monitor->seltags]);
			}
			continue;
		}
		if (owner != c->mon) {
			if (c->mon && c->mon->sel == c)
				c->mon->sel = NULL;
			c->mon = owner;
		}
		if (!(c->tags & owner->tagset[owner->seltags]))
			client_set_tags(c, owner->tagset[owner->seltags]);
	}

	wl_list_for_each(tm, &server.monitors, link) {
		if (st_active(tm))
			arrange(tm, false, false);
	}
}

/* Take `newtags` for monitor m, resolving tag ownership conflicts across
 * monitors as one transaction. Evicts whichever monitors display parts of
 * newtags: each evicted monitor lands on a tag chosen by the eviction
 * policy and gives up only the tags the chain actually takes. If the
 * eviction chain runs into m again (cycle), the operation degrades to a
 * swap: m adopts the tagset of the monitor it collided with. Clients
 * follow their tags onto the new owners; clients keeping no visible tag
 * are reassigned to their tag owner. */
void st_apply_view(Monitor *m, uint32_t newtags) {
	Client *c;
	Monitor *chain[tag_num_MAX + 2];
	Monitor *tm, *owner;
	uint32_t blocked, taken, land, oldset;
	int32_t depth, i;

	if (!st_active(m) || !(newtags & TAGMASK))
		return;

	tm = st_monitor_showing_tags(newtags, m);
	if (!tm)
		return; /* nothing to steal: plain local view switch */

	blocked = newtags & TAGMASK;
	taken = tm->tagset[tm->seltags] & blocked;
	chain[0] = m;
	depth = 1;

	/* evict the chain of monitors sitting on the tags we take */
	while (depth < tag_num_MAX + 2) {
		if (tm == m) {
			/* cycle: degrade to a swap with the collision partner */
			Monitor *other = chain[depth - 1];

			/* other keeps what it holds; m takes over its view */
			m->tagset[m->seltags] = other->tagset[other->seltags];
			if (m->pertag)
				m->pertag->curtag =
					get_tags_first_tag_num(m->tagset[m->seltags] & TAGMASK);
			break;
		}
		chain[depth++] = tm;
		oldset = tm->tagset[tm->seltags];
		land = st_evict_policy(tm, blocked | taken) & TAGMASK;
		if (!land || (land & taken))
			land = st_unused_tag();
		/* keep the parts of its view the chain does not take */
		tm->tagset[tm->seltags] = (oldset & ~(blocked | taken)) | land;
		if (tm->pertag)
			tm->pertag->curtag =
				get_tags_first_tag_num(tm->tagset[tm->seltags] & TAGMASK);

		blocked |= oldset & TAGMASK;
		taken |= land;
		taken &= blocked;

		tm = st_monitor_showing_tags(newtags, m);
	}

	/* the initiator finally takes the tags */
	m->tagset[m->seltags] = newtags;

	/* migrate clients to the monitors displaying their tags */
	wl_list_for_each(c, &server.clients, link) {
		if (c->iskilling || client_is_parked(c) || c->isminimized ||
			(c->tags & TAG0_MASK) || c->isglobal || c->isunglobal)
			continue;
		owner = st_monitor_showing_tags(c->tags, NULL);
		if (!owner)
			continue;
		if (owner != c->mon) {
			if (c->mon && c->mon->sel == c)
				c->mon->sel = NULL;
			c->mon = owner;
		}
		if (!(c->tags & owner->tagset[owner->seltags]))
			client_set_tags(c, owner->tagset[owner->seltags]);
	}

	/* arrange every monitor whose view or clients changed */
	for (i = 0; i < depth; i++) {
		if (st_active(chain[i]))
			arrange(chain[i], false, false);
	}

	/* focus may have pointed at a client that migrated away */
	if (m->sel && m->sel->mon != m)
		m->sel = NULL;
	if (server.selected_monitor == m && !m->sel)
		m->sel = client_focus_top(m);

	printstatus(IPC_WATCH_ARRANGGE);
}

void st_follow_client(Client *c) {
	Monitor *m;
	uint32_t cur;

	if (!config.single_tagset || !c || !c->mon || (c->tags & TAG0_MASK))
		return;

	m = c->mon;
	cur = m->tagset[m->seltags] & TAGMASK;
	if (cur && !(c->tags & cur) && st_monitor_showing_tags(c->tags, m))
		st_apply_view(m, c->tags & TAGMASK);
}
