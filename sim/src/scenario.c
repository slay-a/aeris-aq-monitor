#include <string.h>
#include "scenario.h"

void scenario_clear(scenario_t *s) { memset(s, 0, sizeof *s); }

void scenario_add(scenario_t *s, room_event_t ev, uint32_t duration_ms)
{
    if (s->n_steps >= SCENARIO_MAX_STEPS) return;
    s->step[s->n_steps].ev          = ev;
    s->step[s->n_steps].duration_ms = duration_ms;
    s->n_steps++;
}

/* Durations, in minutes, that each event plausibly lasts. */
static void random_event(room_t *r, room_event_t *ev, uint32_t *dur_ms)
{
    float u = room_rand(r);
    if      (u < 0.34f) { *ev = EV_BASELINE;    *dur_ms = (uint32_t)((8.f + 22.f * room_rand(r)) * 60000.f); }
    else if (u < 0.60f) { *ev = EV_OCCUPANCY;   *dur_ms = (uint32_t)((12.f + 48.f * room_rand(r)) * 60000.f); }
    else if (u < 0.78f) { *ev = EV_COOKING;     *dur_ms = (uint32_t)((9.f + 21.f * room_rand(r)) * 60000.f); }
    else if (u < 0.92f) { *ev = EV_VENTILATION; *dur_ms = (uint32_t)((7.f + 18.f * room_rand(r)) * 60000.f); }
    else                { *ev = EV_VOLATILE;    *dur_ms = (uint32_t)((5.f + 10.f * room_rand(r)) * 60000.f); }
}

void scenario_random(scenario_t *s, room_t *r, uint16_t n_events)
{
    scenario_clear(s);
    room_event_t prev = EV_N_CLASSES;
    for (uint16_t i = 0; i < n_events && i < SCENARIO_MAX_STEPS; i++) {
        room_event_t ev; uint32_t dur;
        /* No immediate repeats: back-to-back identical events would just be
         * one longer event and would skew the class balance. */
        do { random_event(r, &ev, &dur); } while (ev == prev);
        scenario_add(s, ev, dur);
        prev = ev;
    }
}

void scenario_acceptance(scenario_t *s)
{
    scenario_clear(s);
    /* A deliberate morning: settle, someone comes in, they cook, they open the
     * window to clear it, they wipe the counters down, then quiet again. Each
     * block is long enough to clear the settling hold-out. */
    scenario_add(s, EV_BASELINE,    20u * 60000u);
    scenario_add(s, EV_OCCUPANCY,   40u * 60000u);
    scenario_add(s, EV_COOKING,     22u * 60000u);
    scenario_add(s, EV_VENTILATION, 18u * 60000u);
    scenario_add(s, EV_VOLATILE,    12u * 60000u);
    scenario_add(s, EV_BASELINE,    25u * 60000u);
}

bool scenario_update(scenario_t *s, room_t *r, uint32_t now_ms)
{
    if (s->n_steps == 0) return false;

    if (!s->started) {
        s->started         = true;
        s->cur             = 0;
        s->step_started_ms = now_ms;
        room_set_event(r, s->step[0].ev);
        return true;
    }

    if (now_ms - s->step_started_ms >= s->step[s->cur].duration_ms) {
        if (s->cur + 1 >= s->n_steps) return false;
        s->cur++;
        s->step_started_ms = now_ms;
        room_set_event(r, s->step[s->cur].ev);
    }
    return true;
}

bool scenario_settled(const scenario_t *s, uint32_t now_ms)
{
    if (!s->started) return false;
    return (now_ms - s->step_started_ms) >= SCENARIO_SETTLE_MS;
}

room_event_t scenario_label(const scenario_t *s)
{
    if (!s->started || s->n_steps == 0) return EV_BASELINE;
    return s->step[s->cur].ev;
}
