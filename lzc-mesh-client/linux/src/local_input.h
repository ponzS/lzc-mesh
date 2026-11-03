#pragma once
#include <stdbool.h>
#include <linux/input.h>
static inline bool local_input_event_activity(const struct input_event *e) {
    return (e->type==EV_KEY && e->value==1)
        || (e->type==EV_REL && e->value!=0)
        || (e->type==EV_ABS && (e->code==ABS_X || e->code==ABS_Y || e->code==ABS_MT_POSITION_X || e->code==ABS_MT_POSITION_Y || (e->code==ABS_MT_TRACKING_ID && e->value>=0)))
        || (e->type==EV_SYN && e->code==SYN_DROPPED);
}
// Observes already-readable physical evdev nodes only while receiving. Device
// discovery/cleanup run in workers; activity and availability callbacks run on
// the main context and are retired on stop. Never grabs a device, changes
// permissions, records key values or forwards raw input.
void local_input_start(void (*activity)(void), void (*ready)(bool available));
void local_input_stop(void);
