// Round dial UI: one arc segment per agent around the rim, details of the selected agent in the middle.
#include "ui.h"

#include <Arduino.h>
#include <string.h>

#include "board.h"
#include "lvgl.h"

#define RING_SIZE 352
#define ARC_WIDTH 10
#define ARC_WIDTH_SELECTED 22
#define ARC_GAP_DEG 4

// After this long without touching the knob, a newly blocked agent grabs the selection.
#define AUTO_JUMP_IDLE_MS 8000

static void (*s_on_tap)(const char *agent_id);

static Agent s_agents[MAX_AGENTS];
static int s_count;
static int s_selected;
static bool s_have_state;  // false until the first snapshot, so startup doesn't buzz
static bool s_offline = true;
static char s_error[48];
static uint32_t s_last_knob_ms;

static lv_obj_t *s_arcs[MAX_AGENTS];
static lv_obj_t *s_status_bar;
static lv_obj_t *s_summary;
static lv_obj_t *s_status;
static lv_obj_t *s_name;
static lv_obj_t *s_meta;
static lv_obj_t *s_hint;

static lv_color_t status_color(AgentStatus status)
{
    switch (status) {
    case STATUS_WORKING: return lv_color_hex(0x3B82F6);
    case STATUS_BLOCKED: return lv_color_hex(0xEF4444);
    case STATUS_DONE:    return lv_color_hex(0x22C55E);
    case STATUS_IDLE:    return lv_color_hex(0x6B7280);
    default:             return lv_color_hex(0x374151);
    }
}

static const char *status_text(AgentStatus status)
{
    switch (status) {
    case STATUS_WORKING: return "WORKING";
    case STATUS_BLOCKED: return "NEEDS YOU";
    case STATUS_DONE:    return "DONE";
    case STATUS_IDLE:    return "IDLE";
    default:             return "UNKNOWN";
    }
}

static lv_obj_t *make_label(const lv_font_t *font, lv_coord_t y, lv_coord_t width)
{
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, width);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
    lv_label_set_text(label, "");
    return label;
}

static void show_message(const char *title, const char *body)
{
    for (int i = 0; i < MAX_AGENTS; i++) {
        lv_obj_add_flag(s_arcs[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_summary, "");
    lv_label_set_text(s_status, title);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x9CA3AF), 0);
    lv_label_set_text(s_name, body);
    lv_label_set_text(s_meta, "");
    lv_label_set_text(s_hint, "");
}

static void render(void)
{
    if (s_offline) {
        show_message("OFFLINE", "Waiting for the bridge (USB or Bluetooth)");
        return;
    }
    if (s_error[0]) {
        show_message("NO HERDR", s_error);
        return;
    }
    if (s_count == 0) {
        show_message("NO AGENTS", "Start an agent in herdr");
        return;
    }

    const int segment = 360 / s_count;
    const int gap = s_count > 1 ? ARC_GAP_DEG : 0;
    int working = 0, blocked = 0, done = 0;
    for (int i = 0; i < MAX_AGENTS; i++) {
        lv_obj_t *arc = s_arcs[i];
        if (i >= s_count) {
            lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const Agent &agent = s_agents[i];
        working += agent.status == STATUS_WORKING;
        blocked += agent.status == STATUS_BLOCKED;
        done += agent.status == STATUS_DONE;

        const bool selected = i == s_selected;
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_angles(arc, i * segment + gap / 2, (i + 1) * segment - gap / 2);
        lv_obj_set_style_arc_color(arc, status_color(agent.status), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(arc, selected ? ARC_WIDTH_SELECTED : ARC_WIDTH, LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(arc, selected ? LV_OPA_COVER : LV_OPA_60, LV_PART_INDICATOR);
    }

    char summary[64];
    int len = snprintf(summary, sizeof(summary), "%d agent%s", s_count, s_count == 1 ? "" : "s");
    if (blocked) len += snprintf(summary + len, sizeof(summary) - len, ", %d blocked", blocked);
    if (done) len += snprintf(summary + len, sizeof(summary) - len, ", %d done", done);
    if (working) snprintf(summary + len, sizeof(summary) - len, ", %d working", working);
    lv_label_set_text(s_summary, summary);

    const Agent &sel = s_agents[s_selected];
    lv_label_set_text(s_status, status_text(sel.status));
    lv_obj_set_style_text_color(s_status, status_color(sel.status), 0);
    lv_label_set_text(s_name, sel.name[0] ? sel.name : sel.id);
    lv_label_set_text_fmt(s_meta, "%s  %d/%d", sel.kind[0] ? sel.kind : "agent", s_selected + 1, s_count);
    lv_label_set_text(s_hint, sel.focused ? "on screen" : "tap to show");
}

static void on_screen_clicked(lv_event_t *e)
{
    if (!s_offline && !s_error[0] && s_count > 0 && s_on_tap) {
        s_on_tap(s_agents[s_selected].id);
    }
}

void ui_init(void (*on_tap)(const char *agent_id))
{
    s_on_tap = on_tap;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_clicked, LV_EVENT_CLICKED, NULL);

    for (int i = 0; i < MAX_AGENTS; i++) {
        lv_obj_t *arc = lv_arc_create(scr);
        lv_obj_set_size(arc, RING_SIZE, RING_SIZE);
        lv_obj_center(arc);
        lv_arc_set_rotation(arc, 270);  // angle 0 at 12 o'clock
        lv_arc_set_bg_angles(arc, 0, 360);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);
        lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
        s_arcs[i] = arc;
    }

    s_status_bar = make_label(&lv_font_montserrat_14, -138, 120);
    lv_obj_set_style_text_color(s_status_bar, lv_color_hex(0x6B7280), 0);
    s_summary = make_label(&lv_font_montserrat_14, -110, 220);
    lv_obj_set_style_text_color(s_summary, lv_color_hex(0x9CA3AF), 0);
    s_status = make_label(&lv_font_montserrat_28, -62, 260);
    s_name = make_label(&lv_font_montserrat_20, 4, 250);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_name, 3 * lv_font_get_line_height(&lv_font_montserrat_20));
    s_meta = make_label(&lv_font_montserrat_14, 72, 220);
    lv_obj_set_style_text_color(s_meta, lv_color_hex(0x9CA3AF), 0);
    s_hint = make_label(&lv_font_montserrat_14, 108, 200);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x6B7280), 0);

    render();
}

static const Agent *find_agent(const Agent *agents, int count, const char *id)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(agents[i].id, id) == 0) {
            return &agents[i];
        }
    }
    return NULL;
}

UiAlert ui_set_agents(const Agent *agents, int count, const char *error)
{
    if (count > MAX_AGENTS) {
        count = MAX_AGENTS;
    }
    strlcpy(s_error, error ? error : "", sizeof(s_error));

    UiAlert alert = ALERT_NONE;
    int jump_to = -1;
    for (int i = 0; i < count && s_have_state; i++) {
        const Agent *old = find_agent(s_agents, s_count, agents[i].id);
        const AgentStatus was = old ? old->status : STATUS_UNKNOWN;
        if (agents[i].status == STATUS_BLOCKED && was != STATUS_BLOCKED) {
            alert = ALERT_BLOCKED;
            if (jump_to < 0) jump_to = i;
        } else if (agents[i].status == STATUS_DONE && old && was != STATUS_DONE && alert == ALERT_NONE) {
            alert = ALERT_DONE;
        }
    }

    // Keep the same agent selected across updates, even if the list order shifts.
    char selected_id[sizeof(Agent::id)] = "";
    if (s_count > 0) {
        strlcpy(selected_id, s_agents[s_selected].id, sizeof(selected_id));
    }
    memcpy(s_agents, agents, count * sizeof(Agent));
    s_count = count;
    s_selected = 0;
    if (const Agent *keep = find_agent(s_agents, s_count, selected_id)) {
        s_selected = keep - s_agents;
    }
    if (jump_to >= 0 && millis() - s_last_knob_ms > AUTO_JUMP_IDLE_MS) {
        s_selected = jump_to;
    }

    s_have_state = true;
    render();
    return alert;
}

void ui_rotate(int steps)
{
    s_last_knob_ms = millis();
    if (s_count == 0) {
        return;
    }
    s_selected = ((s_selected + steps) % s_count + s_count) % s_count;
    render();
}

void ui_set_offline(bool offline)
{
    s_offline = offline;
    if (offline) {
        s_have_state = false;
    }
    render();
}

void ui_set_status(const char *text, bool warn)
{
    lv_label_set_text(s_status_bar, text);
    lv_obj_set_style_text_color(s_status_bar, warn ? lv_color_hex(0xEF4444) : lv_color_hex(0x6B7280), 0);
}
