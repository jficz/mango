#include "mango/manage/tagset.h"
#include "mango/common/server.h"
#include "mango/config/parse_config.h"
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

Monitor *st_client_owner(const Client *c, const Monitor *exclude) {
	Monitor *owner;

	if (!c || !config.single_tagset)
		return c ? c->mon : NULL;

	/* special/TAG0 windows stay on their own monitor */
	if (c->tags & TAG0_MASK)
		return c->mon;

	owner = st_monitor_showing_tags(c->tags, exclude);
	return owner ? owner : c->mon;
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
