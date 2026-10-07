/* scenario.h - a timed sequence of room events.
 *
 * Labels carry a settling delay. When cooking starts, the room's CO2, VOC and
 * PM take a minute or two to respond and the 5-minute slope features longer
 * still, so the first SETTLE_MS of an event is genuinely unlabelable. Those
 * rows are dropped from the dataset instead of being taught to the model as
 * if the signal were already there -- training on them is how you get a model
 * that looks good offline and is useless in the room.
 */
#ifndef SCENARIO_H
#define SCENARIO_H

#include <stdbool.h>
#include <stdint.h>
#include "room_model.h"

#define SCENARIO_SETTLE_MS 150000u   /* 2.5 min of hold-out per transition */
#define SCENARIO_MAX_STEPS 64

typedef struct {
    room_event_t ev;
    uint32_t     duration_ms;
} scenario_step_t;

typedef struct {
    scenario_step_t step[SCENARIO_MAX_STEPS];
    uint16_t n_steps;
    uint16_t cur;
    uint32_t step_started_ms;
    bool     started;
} scenario_t;

void scenario_clear(scenario_t *s);
void scenario_add(scenario_t *s, room_event_t ev, uint32_t duration_ms);

/* Build a randomised but realistic day: events in a plausible order, with
 * durations drawn from the ranges each one actually lasts. */
void scenario_random(scenario_t *s, room_t *r, uint16_t n_events);

/* Fixed sequence used by the acceptance run, so a regression is reproducible. */
void scenario_acceptance(scenario_t *s);

/* Advance; applies the next event to the room when the current one expires.
 * Returns false when the scenario has finished. */
bool scenario_update(scenario_t *s, room_t *r, uint32_t now_ms);

/* True when the current event has been running long enough to be labelable. */
bool scenario_settled(const scenario_t *s, uint32_t now_ms);

room_event_t scenario_label(const scenario_t *s);

#endif /* SCENARIO_H */
