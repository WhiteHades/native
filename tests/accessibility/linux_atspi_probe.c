#include <atspi/atspi.h>

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *app_name;
    const char *root_name;
    const char *list_name;
    const char *item_name;
    const char *offscreen_item_name;
    const char *full_item_name;
    const char *press_name;
    const char *toggle_name;
    const char *radio_name;
    const char *text_name;
    const char *value_name;
    const char *progress_name;
    const char *details_name;
    const char *remove_name;
    const char *transient_name;
    const char *count_before;
    const char *count_after;
    const char *toggle_before;
    const char *toggle_after;
    const char *radio_before;
    const char *radio_after;
    const char *text_before;
    const char *text_after;
    const char *value_before;
    const char *value_after;
    const char *details_before;
    const char *details_after;
    const char *transient_before;
    const char *transient_after;
    const char *replacement_text;
    gint timeout_ms;
    gint settle_ms;
    gint expected_position;
    gint expected_count;
    gint expected_rows;
    gint initial_selection_start;
    gint initial_selection_end;
    gint final_selection_start;
    gint final_selection_end;
    gdouble initial_value;
    gdouble final_value;
    gdouble progress_value;
} Config;

typedef struct {
    gint remaining;
} SearchBudget;

static gboolean failed = FALSE;

static void failf(const char *format, ...) {
    va_list args;

    failed = TRUE;
    fputs("FAIL: ", stderr);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

static void clear_error(GError **error, const char *operation) {
    if (!*error) return;
    fprintf(stderr, "AT-SPI error during %s: %s\n", operation, (*error)->message);
    g_clear_error(error);
}

static void pump_events(void) {
    while (g_main_context_iteration(NULL, FALSE)) {}
}

static void pause_poll(void) {
    pump_events();
    g_usleep(50 * 1000);
}

static gint64 deadline_after(gint timeout_ms) {
    return g_get_monotonic_time() + ((gint64)timeout_ms * 1000);
}

static gchar *accessible_name(AtspiAccessible *accessible) {
    GError *error = NULL;
    gchar *name;

    atspi_accessible_clear_cache(accessible);
    name = atspi_accessible_get_name(accessible, &error);
    if (error) {
        g_clear_error(&error);
        g_free(name);
        return NULL;
    }
    return name;
}

static AtspiAccessible *find_named_recursive(AtspiAccessible *root, const char *name, gint depth, SearchBudget *budget) {
    GError *error = NULL;
    gchar *candidate;
    gint child_count;

    if (!root || depth > 24 || budget->remaining-- <= 0) return NULL;
    candidate = accessible_name(root);
    if (candidate && strcmp(candidate, name) == 0) {
        g_free(candidate);
        return g_object_ref(root);
    }
    g_free(candidate);

    child_count = atspi_accessible_get_child_count(root, &error);
    if (error) {
        g_clear_error(&error);
        return NULL;
    }
    for (gint index = 0; index < child_count; index++) {
        AtspiAccessible *child = atspi_accessible_get_child_at_index(root, index, &error);
        AtspiAccessible *found;

        if (error) {
            g_clear_error(&error);
            continue;
        }
        if (!child) continue;
        found = find_named_recursive(child, name, depth + 1, budget);
        g_object_unref(child);
        if (found) return found;
    }
    return NULL;
}

static AtspiAccessible *find_named(AtspiAccessible *root, const char *name) {
    SearchBudget budget = {.remaining = 4096};
    return find_named_recursive(root, name, 0, &budget);
}

static AtspiAccessible *wait_named(AtspiAccessible *root, const char *name, gint timeout_ms) {
    const gint64 deadline = deadline_after(timeout_ms);

    do {
        AtspiAccessible *found = find_named(root, name);
        if (found) return found;
        pause_poll();
    } while (g_get_monotonic_time() < deadline);
    return NULL;
}

static AtspiAccessible *wait_application(AtspiAccessible *desktop, const char *name, gint timeout_ms) {
    const gint64 deadline = deadline_after(timeout_ms);

    do {
        GError *error = NULL;
        gint count;

        atspi_accessible_clear_cache(desktop);
        count = atspi_accessible_get_child_count(desktop, &error);
        if (error) {
            g_clear_error(&error);
            pause_poll();
            continue;
        }
        for (gint index = 0; index < count; index++) {
            AtspiAccessible *app = atspi_accessible_get_child_at_index(desktop, index, &error);
            gchar *candidate;

            if (error) {
                g_clear_error(&error);
                continue;
            }
            if (!app) continue;
            candidate = accessible_name(app);
            if (candidate && strcmp(candidate, name) == 0) {
                g_free(candidate);
                return app;
            }
            g_free(candidate);
            g_object_unref(app);
        }
        pause_poll();
    } while (g_get_monotonic_time() < deadline);
    return NULL;
}

static gboolean has_state(AtspiAccessible *accessible, AtspiStateType state) {
    AtspiStateSet *states;
    gboolean result;

    atspi_accessible_clear_cache(accessible);
    states = atspi_accessible_get_state_set(accessible);
    if (!states) return FALSE;
    result = atspi_state_set_contains(states, state);
    g_object_unref(states);
    return result;
}

static gboolean wait_state(AtspiAccessible *accessible, AtspiStateType state, gboolean expected, gint timeout_ms) {
    const gint64 deadline = deadline_after(timeout_ms);

    do {
        if (has_state(accessible, state) == expected) return TRUE;
        pause_poll();
    } while (g_get_monotonic_time() < deadline);
    return FALSE;
}

static AtspiRole role_of(AtspiAccessible *accessible) {
    GError *error = NULL;
    AtspiRole role = atspi_accessible_get_role(accessible, &error);
    if (error) {
        clear_error(&error, "reading role");
        return ATSPI_ROLE_INVALID;
    }
    return role;
}

static gboolean expect_role(AtspiAccessible *accessible, AtspiRole expected, const char *name) {
    const AtspiRole actual = role_of(accessible);
    gchar *actual_name;
    gchar *expected_name;

    if (actual == expected) return TRUE;
    actual_name = atspi_role_get_name(actual);
    expected_name = atspi_role_get_name(expected);
    failf("%s role is %s, expected %s", name, actual_name, expected_name);
    g_free(actual_name);
    g_free(expected_name);
    return FALSE;
}

static gboolean expect_parent(AtspiAccessible *child, const char *child_name, const char *parent_name, AtspiRole parent_role) {
    GError *error = NULL;
    AtspiAccessible *parent = atspi_accessible_get_parent(child, &error);
    gchar *actual_name;
    gchar *actual_role_name;
    gchar *expected_role_name;
    gboolean ok;

    if (error || !parent) {
        clear_error(&error, "reading parent");
        failf("%s has no readable parent", child_name);
        return FALSE;
    }
    actual_name = accessible_name(parent);
    ok = actual_name && strcmp(actual_name, parent_name) == 0 && role_of(parent) == parent_role;
    if (!ok) {
        actual_role_name = atspi_role_get_name(role_of(parent));
        expected_role_name = atspi_role_get_name(parent_role);
        failf("%s parent is name=%s role=%s, expected name=%s role=%s",
              child_name,
              actual_name ? actual_name : "<unavailable>",
              actual_role_name,
              parent_name,
              expected_role_name);
        g_free(actual_role_name);
        g_free(expected_role_name);
    }
    g_free(actual_name);
    g_object_unref(parent);
    return ok;
}

static gint direct_role_count(AtspiAccessible *parent, AtspiRole role) {
    GError *error = NULL;
    gint count = atspi_accessible_get_child_count(parent, &error);
    gint matches = 0;

    if (error) {
        clear_error(&error, "counting children");
        return -1;
    }
    for (gint index = 0; index < count; index++) {
        AtspiAccessible *child = atspi_accessible_get_child_at_index(parent, index, &error);
        if (error) {
            clear_error(&error, "reading child");
            continue;
        }
        if (child && role_of(child) == role) matches++;
        if (child) g_object_unref(child);
    }
    return matches;
}

static gboolean expect_integer_attribute(AtspiAccessible *accessible, const char *name, gint64 expected) {
    GError *error = NULL;
    GHashTable *attributes = atspi_accessible_get_attributes(accessible, &error);
    const char *value;
    gchar *end = NULL;
    gint64 parsed;

    if (error || !attributes) {
        clear_error(&error, "reading attributes");
        failf("attribute %s is unavailable", name);
        return FALSE;
    }
    value = g_hash_table_lookup(attributes, name);
    errno = 0;
    parsed = value ? g_ascii_strtoll(value, &end, 10) : 0;
    if (!value || errno != 0 || !end || *end != '\0' || parsed != expected) {
        failf("attribute %s is %s, expected %lld", name, value ? value : "<missing>", (long long)expected);
        g_hash_table_unref(attributes);
        return FALSE;
    }
    g_hash_table_unref(attributes);
    return TRUE;
}

static AtspiRect *component_extents(AtspiAccessible *accessible, const char *name) {
    GError *error = NULL;
    AtspiComponent *component;
    AtspiRect *rect;

    if (!atspi_accessible_is_component(accessible)) {
        failf("%s does not expose the AT-SPI Component interface", name);
        return NULL;
    }
    component = atspi_accessible_get_component_iface(accessible);
    if (!component) {
        failf("%s Component interface could not be acquired", name);
        return NULL;
    }
    rect = atspi_component_get_extents(component, ATSPI_COORD_TYPE_SCREEN, &error);
    g_object_unref(component);
    if (error || !rect) {
        clear_error(&error, "reading bounds");
        failf("%s has no readable screen bounds", name);
        g_free(rect);
        return NULL;
    }
    return rect;
}

static gint intersection_area(const AtspiRect *a, const AtspiRect *b) {
    const gint left = MAX(a->x, b->x);
    const gint top = MAX(a->y, b->y);
    const gint right = MIN(a->x + a->width, b->x + b->width);
    const gint bottom = MIN(a->y + a->height, b->y + b->height);
    if (right <= left || bottom <= top) return 0;
    return (right - left) * (bottom - top);
}

static gboolean action_name_matches(const char *actual, const char *const *preferred, gsize preferred_count) {
    for (gsize index = 0; index < preferred_count; index++) {
        if (g_ascii_strcasecmp(actual, preferred[index]) == 0) return TRUE;
    }
    return FALSE;
}

static gboolean perform_action(AtspiAccessible *accessible, const char *name, const char *const *preferred, gsize preferred_count) {
    GError *error = NULL;
    AtspiAction *action;
    gint count;
    gint selected = -1;

    if (!atspi_accessible_is_action(accessible)) {
        failf("%s does not expose the AT-SPI Action interface", name);
        return FALSE;
    }
    action = atspi_accessible_get_action_iface(accessible);
    if (!action) {
        failf("%s Action interface could not be acquired", name);
        return FALSE;
    }
    count = atspi_action_get_n_actions(action, &error);
    if (error) {
        clear_error(&error, "reading actions");
        g_object_unref(action);
        return FALSE;
    }
    for (gint index = 0; index < count; index++) {
        gchar *candidate = atspi_action_get_action_name(action, index, &error);
        if (error) {
            clear_error(&error, "reading action name");
            g_free(candidate);
            continue;
        }
        if (candidate && action_name_matches(candidate, preferred, preferred_count)) selected = index;
        g_free(candidate);
        if (selected >= 0) break;
    }
    if (selected < 0 && count == 1) selected = 0;
    if (selected < 0) {
        failf("%s has no matching AT-SPI action", name);
        g_object_unref(action);
        return FALSE;
    }
    if (!atspi_action_do_action(action, selected, &error) || error) {
        clear_error(&error, "performing action");
        failf("AT-SPI action failed for %s", name);
        g_object_unref(action);
        return FALSE;
    }
    g_object_unref(action);
    return TRUE;
}

static gboolean expect_status(AtspiAccessible *app, const char *name, gint timeout_ms) {
    AtspiAccessible *status = wait_named(app, name, timeout_ms);
    if (!status) {
        failf("result semantic %s did not appear", name);
        return FALSE;
    }
    g_object_unref(status);
    return TRUE;
}

static gboolean expect_text(AtspiAccessible *accessible, const char *name, const char *expected) {
    GError *error = NULL;
    AtspiText *text;
    gchar *actual;
    gboolean ok;

    if (!atspi_accessible_is_text(accessible)) {
        failf("%s does not expose the AT-SPI Text interface", name);
        return FALSE;
    }
    text = atspi_accessible_get_text_iface(accessible);
    actual = text ? atspi_text_get_text(text, 0, -1, &error) : NULL;
    if (text) g_object_unref(text);
    if (error) clear_error(&error, "reading text");
    ok = !error && actual && strcmp(actual, expected) == 0;
    if (!ok) failf("%s text is %s, expected %s", name, actual ? actual : "<unavailable>", expected);
    g_free(actual);
    return ok;
}

static gboolean selection_equals(AtspiAccessible *accessible, gint start, gint end, gint *actual_start, gint *actual_end) {
    GError *error = NULL;
    AtspiText *text;
    gint selections;
    AtspiRange *range;
    gboolean ok;

    *actual_start = -1;
    *actual_end = -1;
    if (!atspi_accessible_is_text(accessible)) {
        return FALSE;
    }
    text = atspi_accessible_get_text_iface(accessible);
    selections = text ? atspi_text_get_n_selections(text, &error) : 0;
    range = (!error && selections > 0) ? atspi_text_get_selection(text, 0, &error) : NULL;
    if (text) g_object_unref(text);
    if (error) clear_error(&error, "reading text selection");
    ok = !error && range && range->start_offset == start && range->end_offset == end;
    *actual_start = range ? range->start_offset : -1;
    *actual_end = range ? range->end_offset : -1;
    if (range) g_boxed_free(ATSPI_TYPE_RANGE, range);
    return ok;
}

static gboolean expect_selection(AtspiAccessible *accessible, const char *name, gint start, gint end) {
    gint actual_start;
    gint actual_end;

    if (selection_equals(accessible, start, end, &actual_start, &actual_end)) return TRUE;
    failf("%s selection is %d..%d, expected %d..%d", name, actual_start, actual_end, start, end);
    return FALSE;
}

static gboolean set_selection(AtspiAccessible *accessible, const char *name, gint start, gint end, gint timeout_ms) {
    GError *error = NULL;
    AtspiText *text;
    gint selections;
    gboolean changed;
    const gint64 deadline = deadline_after(timeout_ms);
    gint actual_start;
    gint actual_end;

    text = atspi_accessible_get_text_iface(accessible);
    if (!text) {
        failf("%s Text interface could not be acquired", name);
        return FALSE;
    }
    selections = atspi_text_get_n_selections(text, &error);
    changed = !error && (selections > 0
        ? atspi_text_set_selection(text, 0, start, end, &error)
        : atspi_text_add_selection(text, start, end, &error));
    g_object_unref(text);
    if (error || !changed) {
        clear_error(&error, "setting text selection");
        failf("AT-SPI could not set %s selection", name);
        return FALSE;
    }
    do {
        if (selection_equals(accessible, start, end, &actual_start, &actual_end)) return TRUE;
        pause_poll();
    } while (g_get_monotonic_time() < deadline);
    failf("%s selection did not settle at %d..%d", name, start, end);
    return FALSE;
}

static gboolean set_text(AtspiAccessible *accessible, const char *name, const char *replacement) {
    GError *error = NULL;
    AtspiEditableText *editable;
    gboolean changed;

    if (!atspi_accessible_is_editable_text(accessible)) {
        failf("%s does not expose the AT-SPI EditableText interface", name);
        return FALSE;
    }
    editable = atspi_accessible_get_editable_text_iface(accessible);
    changed = editable && atspi_editable_text_set_text_contents(editable, replacement, &error);
    if (editable) g_object_unref(editable);
    if (error || !changed) {
        clear_error(&error, "setting text");
        failf("AT-SPI could not set text on %s", name);
        return FALSE;
    }
    return TRUE;
}

static gboolean value_equals(AtspiAccessible *accessible, const char *name, gdouble expected, gdouble tolerance) {
    GError *error = NULL;
    AtspiValue *value;
    gdouble actual;

    if (!atspi_accessible_is_value(accessible)) {
        failf("%s does not expose the AT-SPI Value interface", name);
        return FALSE;
    }
    value = atspi_accessible_get_value_iface(accessible);
    actual = value ? atspi_value_get_current_value(value, &error) : NAN;
    if (value) g_object_unref(value);
    if (error) clear_error(&error, "reading value");
    if (!error && fabs(actual - expected) <= tolerance) return TRUE;
    failf("%s value is %.6f, expected %.6f", name, actual, expected);
    return FALSE;
}

static gboolean set_value(AtspiAccessible *accessible, const char *name, gdouble next) {
    GError *error = NULL;
    AtspiValue *value;
    gboolean changed;

    if (!atspi_accessible_is_value(accessible)) {
        failf("%s does not expose the AT-SPI Value interface", name);
        return FALSE;
    }
    value = atspi_accessible_get_value_iface(accessible);
    changed = value && atspi_value_set_current_value(value, next, &error);
    if (value) g_object_unref(value);
    if (error || !changed) {
        clear_error(&error, "setting value");
        failf("AT-SPI could not set value on %s", name);
        return FALSE;
    }
    return TRUE;
}

static gboolean removed_accessible_is_defunct(AtspiAccessible *accessible) {
    GError *error = NULL;
    gchar *name;
    gboolean defunct;

    pump_events();
    atspi_accessible_clear_cache(accessible);
    name = atspi_accessible_get_name(accessible, &error);
    if (error) {
        g_clear_error(&error);
        g_free(name);
        return TRUE;
    }
    g_free(name);
    defunct = has_state(accessible, ATSPI_STATE_DEFUNCT);
    return defunct;
}

static void dump_tree(AtspiAccessible *root, gint depth, gint *remaining) {
    GError *error = NULL;
    gchar *name;
    gchar *role;
    gint child_count;

    if (!root || depth > 16 || (*remaining)-- <= 0) return;
    name = atspi_accessible_get_name(root, &error);
    if (error) g_clear_error(&error);
    role = atspi_accessible_get_role_name(root, &error);
    if (error) g_clear_error(&error);
    fprintf(stderr, "%*s- name=%s role=%s visible=%d showing=%d focused=%d\n",
            depth * 2,
            "",
            name ? name : "<unavailable>",
            role ? role : "<unavailable>",
            has_state(root, ATSPI_STATE_VISIBLE),
            has_state(root, ATSPI_STATE_SHOWING),
            has_state(root, ATSPI_STATE_FOCUSED));
    g_free(name);
    g_free(role);

    child_count = atspi_accessible_get_child_count(root, &error);
    if (error) {
        g_clear_error(&error);
        return;
    }
    for (gint index = 0; index < child_count && *remaining > 0; index++) {
        AtspiAccessible *child = atspi_accessible_get_child_at_index(root, index, &error);
        if (error) {
            g_clear_error(&error);
            continue;
        }
        if (child) {
            dump_tree(child, depth + 1, remaining);
            g_object_unref(child);
        }
    }
}

static void event_callback(AtspiEvent *event, void *user_data) {
    (void)event;
    (void)user_data;
}

static gboolean parse_int(const char *option, const char *value, gint *out) {
    gchar *end = NULL;
    gint64 parsed;

    errno = 0;
    parsed = g_ascii_strtoll(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < 0 || parsed > G_MAXINT) {
        fprintf(stderr, "%s requires a non-negative integer, got %s\n", option, value);
        return FALSE;
    }
    *out = (gint)parsed;
    return TRUE;
}

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --app-name NAME              AT-SPI application name\n"
            "  --timeout-ms N               application discovery timeout\n"
            "  --settle-ms N                timeout for each action result\n"
            "  --expected-position N        one-based list position (default 42)\n"
            "  --expected-count N           logical list size (default 1000)\n"
            "  --expected-rows N            materialized listitems (default 7)\n"
            "  --root-name NAME             override fixture semantic names\n"
            "  --list-name NAME\n"
            "  --item-name NAME\n"
            "  --offscreen-item-name NAME\n"
            "  --full-item-name NAME\n",
            program);
}

static gboolean parse_options(int argc, char **argv, Config *config) {
    for (int index = 1; index < argc; index++) {
        const char *option = argv[index];
        const char *value;

        if (strcmp(option, "--help") == 0) {
            usage(argv[0]);
            exit(0);
        }
        if (index + 1 >= argc) {
            fprintf(stderr, "missing value for %s\n", option);
            return FALSE;
        }
        value = argv[++index];
#define STRING_OPTION(flag, field) if (strcmp(option, flag) == 0) { config->field = value; continue; }
        STRING_OPTION("--app-name", app_name)
        STRING_OPTION("--root-name", root_name)
        STRING_OPTION("--list-name", list_name)
        STRING_OPTION("--item-name", item_name)
        STRING_OPTION("--offscreen-item-name", offscreen_item_name)
        STRING_OPTION("--full-item-name", full_item_name)
        STRING_OPTION("--press-name", press_name)
        STRING_OPTION("--toggle-name", toggle_name)
        STRING_OPTION("--radio-name", radio_name)
        STRING_OPTION("--text-name", text_name)
        STRING_OPTION("--value-name", value_name)
        STRING_OPTION("--progress-name", progress_name)
        STRING_OPTION("--details-name", details_name)
        STRING_OPTION("--remove-name", remove_name)
        STRING_OPTION("--transient-name", transient_name)
        STRING_OPTION("--replacement-text", replacement_text)
#undef STRING_OPTION
        if (strcmp(option, "--timeout-ms") == 0 && parse_int(option, value, &config->timeout_ms)) continue;
        if (strcmp(option, "--settle-ms") == 0 && parse_int(option, value, &config->settle_ms)) continue;
        if (strcmp(option, "--expected-position") == 0 && parse_int(option, value, &config->expected_position)) continue;
        if (strcmp(option, "--expected-count") == 0 && parse_int(option, value, &config->expected_count)) continue;
        if (strcmp(option, "--expected-rows") == 0 && parse_int(option, value, &config->expected_rows)) continue;
        fprintf(stderr, "unknown or invalid option: %s\n", option);
        return FALSE;
    }
    return TRUE;
}

int main(int argc, char **argv) {
    static const char *const press_actions[] = {"click", "press", "activate"};
    static const char *const toggle_actions[] = {"toggle", "click", "press", "activate"};
    static const char *const select_actions[] = {"select", "click", "press", "activate"};
    Config config = {
        .app_name = "accessibility-smoke",
        .root_name = "Accessibility smoke",
        .list_name = "Lesson list",
        .item_name = "Lesson 42",
        .offscreen_item_name = "Lesson 40",
        .full_item_name = "Lesson 43",
        .press_name = "Count action",
        .toggle_name = "Study mode",
        .radio_name = "Reading mode",
        .text_name = "Search lessons",
        .value_name = "Volume",
        .progress_name = "Completion",
        .details_name = "Details",
        .remove_name = "Remove transient",
        .transient_name = "Transient target",
        .count_before = "Count result: 0",
        .count_after = "Count result: 1",
        .toggle_before = "Study mode result: off",
        .toggle_after = "Study mode result: on",
        .radio_before = "Reading mode result: unselected",
        .radio_after = "Reading mode result: selected",
        .text_before = "Search result: seed",
        .text_after = "Search result: typed",
        .value_before = "Volume result: 50",
        .value_after = "Volume result: 55",
        .details_before = "Details result: collapsed",
        .details_after = "Details result: expanded",
        .transient_before = "Transient result: present",
        .transient_after = "Transient result: removed",
        .replacement_text = "typed",
        .timeout_ms = 30000,
        .settle_ms = 5000,
        .expected_position = 42,
        .expected_count = 1000,
        .expected_rows = 7,
        .initial_selection_start = 1,
        .initial_selection_end = 3,
        .final_selection_start = 1,
        .final_selection_end = 4,
        .initial_value = 0.50,
        .final_value = 0.55,
        .progress_value = 0.42,
    };
    GError *error = NULL;
    AtspiEventListener *listener = NULL;
    AtspiAccessible *desktop;
    AtspiAccessible *app = NULL;
    AtspiAccessible *root = NULL;
    AtspiAccessible *list = NULL;
    AtspiAccessible *item = NULL;
    AtspiAccessible *offscreen_item = NULL;
    AtspiAccessible *full_item = NULL;
    AtspiAccessible *press = NULL;
    AtspiAccessible *toggle = NULL;
    AtspiAccessible *radio = NULL;
    AtspiAccessible *text = NULL;
    AtspiAccessible *value = NULL;
    AtspiAccessible *progress = NULL;
    AtspiAccessible *details = NULL;
    AtspiAccessible *remove = NULL;
    AtspiAccessible *transient = NULL;
    AtspiRect *list_rect = NULL;
    AtspiRect *item_rect = NULL;
    AtspiRect *offscreen_rect = NULL;
    AtspiRect *full_rect = NULL;
    int result = 1;

    if (!parse_options(argc, argv, &config)) {
        usage(argv[0]);
        return 2;
    }
    atspi_set_timeout(3000, 3000);
    if (atspi_init() != 0) {
        failf("atspi_init failed");
        goto cleanup;
    }
    listener = atspi_event_listener_new(event_callback, NULL, NULL);
    if (!listener || !atspi_event_listener_register(listener, "object:", &error)) {
        clear_error(&error, "registering event listener");
        failf("could not register the AT-SPI object event listener");
        goto cleanup;
    }
    desktop = atspi_get_desktop(0);
    if (!desktop) {
        failf("AT-SPI desktop 0 is unavailable");
        goto cleanup;
    }
    app = wait_application(desktop, config.app_name, config.timeout_ms);
    if (!app) {
        failf("application %s did not appear in the AT-SPI desktop", config.app_name);
        goto cleanup;
    }

#define FIND_REQUIRED(variable, semantic_name) \
    do { \
        variable = wait_named(app, semantic_name, config.settle_ms); \
        if (!variable) { \
            failf("semantic %s is missing", semantic_name); \
            goto cleanup; \
        } \
    } while (0)
    FIND_REQUIRED(root, config.root_name);
    FIND_REQUIRED(list, config.list_name);
    FIND_REQUIRED(item, config.item_name);
    FIND_REQUIRED(offscreen_item, config.offscreen_item_name);
    FIND_REQUIRED(full_item, config.full_item_name);
    FIND_REQUIRED(press, config.press_name);
    FIND_REQUIRED(toggle, config.toggle_name);
    FIND_REQUIRED(radio, config.radio_name);
    FIND_REQUIRED(text, config.text_name);
    FIND_REQUIRED(value, config.value_name);
    FIND_REQUIRED(progress, config.progress_name);
    FIND_REQUIRED(details, config.details_name);
    FIND_REQUIRED(remove, config.remove_name);
    FIND_REQUIRED(transient, config.transient_name);
#undef FIND_REQUIRED

    if (!expect_role(root, ATSPI_ROLE_GROUPING, config.root_name)) goto cleanup;
    if (!expect_role(list, ATSPI_ROLE_LIST, config.list_name)) goto cleanup;
    if (!expect_role(item, ATSPI_ROLE_LIST_ITEM, config.item_name)) goto cleanup;
    if (!expect_parent(list, config.list_name, config.root_name, ATSPI_ROLE_GROUPING)) goto cleanup;
    if (!expect_parent(item, config.item_name, config.list_name, ATSPI_ROLE_LIST)) goto cleanup;
    if (!expect_parent(offscreen_item, config.offscreen_item_name, config.list_name, ATSPI_ROLE_LIST)) goto cleanup;
    if (direct_role_count(list, ATSPI_ROLE_LIST_ITEM) != config.expected_rows) {
        failf("%s materializes %d listitems, expected %d",
              config.list_name,
              direct_role_count(list, ATSPI_ROLE_LIST_ITEM),
              config.expected_rows);
        goto cleanup;
    }
    if (!expect_integer_attribute(item, "posinset", config.expected_position)) goto cleanup;
    if (!expect_integer_attribute(item, "setsize", config.expected_count)) goto cleanup;

    list_rect = component_extents(list, config.list_name);
    item_rect = component_extents(item, config.item_name);
    offscreen_rect = component_extents(offscreen_item, config.offscreen_item_name);
    full_rect = component_extents(full_item, config.full_item_name);
    if (!list_rect || !item_rect || !offscreen_rect || !full_rect) goto cleanup;
    if (list_rect->width <= 0 || list_rect->height <= 0) {
        failf("%s has empty bounds (%d,%d %dx%d)", config.list_name, list_rect->x, list_rect->y, list_rect->width, list_rect->height);
        goto cleanup;
    }
    if (!has_state(list, ATSPI_STATE_VISIBLE) || !has_state(list, ATSPI_STATE_SHOWING)) {
        failf("%s is not both visible and showing", config.list_name);
        goto cleanup;
    }
    if (intersection_area(list_rect, offscreen_rect) != 0) {
        failf("%s is fully outside the viewport but its bounds intersect %s", config.offscreen_item_name, config.list_name);
        goto cleanup;
    }
    if (!has_state(item, ATSPI_STATE_VISIBLE) || !has_state(item, ATSPI_STATE_SHOWING)) {
        failf("%s is clipped but visible and should be showing", config.item_name);
        goto cleanup;
    }
    if (item_rect->width <= 0 || item_rect->height <= 0 || intersection_area(list_rect, item_rect) != item_rect->width * item_rect->height) {
        failf("%s bounds are not the visible clipped intersection of the list viewport", config.item_name);
        goto cleanup;
    }
    if (full_rect->height <= item_rect->height || item_rect->y != list_rect->y) {
        failf("%s is not partially clipped at the viewport top (item y=%d h=%d, list y=%d, full-row h=%d)",
              config.item_name,
              item_rect->y,
              item_rect->height,
              list_rect->y,
              full_rect->height);
        goto cleanup;
    }

    if (!expect_status(app, config.count_before, config.settle_ms)) goto cleanup;
    if (!perform_action(press, config.press_name, press_actions, G_N_ELEMENTS(press_actions))) goto cleanup;
    if (!expect_status(app, config.count_after, config.settle_ms)) goto cleanup;
    if (has_state(press, ATSPI_STATE_FOCUSED)) {
        puts("INFO: Count action activation also acquired AT-SPI focus");
    } else {
        puts("INFO: GTK activation did not acquire focus; GTK AT-SPI Component.GrabFocus is unsupported");
    }

    if (!expect_status(app, config.toggle_before, config.settle_ms)) goto cleanup;
    if (has_state(toggle, ATSPI_STATE_CHECKED)) {
        failf("%s starts checked", config.toggle_name);
        goto cleanup;
    }
    if (!perform_action(toggle, config.toggle_name, toggle_actions, G_N_ELEMENTS(toggle_actions))) goto cleanup;
    if (!expect_status(app, config.toggle_after, config.settle_ms) ||
        !wait_state(toggle, ATSPI_STATE_CHECKED, TRUE, config.settle_ms)) {
        failf("%s did not round-trip to checked", config.toggle_name);
        goto cleanup;
    }

    if (!expect_status(app, config.radio_before, config.settle_ms)) goto cleanup;
    if (!perform_action(radio, config.radio_name, select_actions, G_N_ELEMENTS(select_actions))) goto cleanup;
    if (!expect_status(app, config.radio_after, config.settle_ms) ||
        !wait_state(radio, ATSPI_STATE_CHECKED, TRUE, config.settle_ms)) {
        failf("%s did not round-trip to selected", config.radio_name);
        goto cleanup;
    }

    if (!expect_status(app, config.text_before, config.settle_ms)) goto cleanup;
    if (!expect_text(text, config.text_name, "seed")) goto cleanup;
    if (!expect_selection(text, config.text_name, config.initial_selection_start, config.initial_selection_end)) goto cleanup;
    if (!set_text(text, config.text_name, config.replacement_text)) goto cleanup;
    if (!expect_status(app, config.text_after, config.settle_ms)) goto cleanup;
    if (!expect_text(text, config.text_name, config.replacement_text)) goto cleanup;
    if (!set_selection(text, config.text_name, config.final_selection_start, config.final_selection_end, config.settle_ms)) goto cleanup;

    if (!expect_status(app, config.value_before, config.settle_ms)) goto cleanup;
    if (!value_equals(value, config.value_name, config.initial_value, 0.001)) goto cleanup;
    if (!set_value(value, config.value_name, config.final_value)) goto cleanup;
    if (!expect_status(app, config.value_after, config.settle_ms)) goto cleanup;
    if (!value_equals(value, config.value_name, config.final_value, 0.001)) goto cleanup;
    if (!value_equals(progress, config.progress_name, config.progress_value, 0.001)) goto cleanup;

    if (!expect_status(app, config.details_before, config.settle_ms)) goto cleanup;
    if (has_state(details, ATSPI_STATE_EXPANDED)) {
        failf("%s starts expanded", config.details_name);
        goto cleanup;
    }
    if (!perform_action(details, config.details_name, toggle_actions, G_N_ELEMENTS(toggle_actions))) goto cleanup;
    if (!expect_status(app, config.details_after, config.settle_ms) ||
        !wait_state(details, ATSPI_STATE_EXPANDED, TRUE, config.settle_ms)) {
        failf("%s did not round-trip to expanded", config.details_name);
        goto cleanup;
    }

    if (!expect_status(app, config.transient_before, config.settle_ms)) goto cleanup;
    if (!perform_action(remove, config.remove_name, press_actions, G_N_ELEMENTS(press_actions))) goto cleanup;
    if (!expect_status(app, config.transient_after, config.settle_ms)) goto cleanup;
    {
        AtspiAccessible *still_present = wait_named(app, config.transient_name, 500);
        if (still_present) {
            g_object_unref(still_present);
            failf("%s remains in the accessibility tree after removal", config.transient_name);
            goto cleanup;
        }
    }
    if (!removed_accessible_is_defunct(transient)) {
        failf("retained %s reference is neither defunct nor unavailable after removal", config.transient_name);
        goto cleanup;
    }

    puts("PASS: Linux AT-SPI hierarchy, virtualization, bounds, actions, and teardown");
    result = 0;

cleanup:
    if (result != 0 && app) {
        gint remaining = 256;
        fputs("---- AT-SPI application tree ----\n", stderr);
        dump_tree(app, 0, &remaining);
        fputs("---------------------------------\n", stderr);
    }
    g_free(list_rect);
    g_free(item_rect);
    g_free(offscreen_rect);
    g_free(full_rect);
#define UNREF_IF_SET(value_to_unref) if (value_to_unref) g_object_unref(value_to_unref)
    UNREF_IF_SET(transient);
    UNREF_IF_SET(remove);
    UNREF_IF_SET(details);
    UNREF_IF_SET(progress);
    UNREF_IF_SET(value);
    UNREF_IF_SET(text);
    UNREF_IF_SET(radio);
    UNREF_IF_SET(toggle);
    UNREF_IF_SET(press);
    UNREF_IF_SET(full_item);
    UNREF_IF_SET(offscreen_item);
    UNREF_IF_SET(item);
    UNREF_IF_SET(list);
    UNREF_IF_SET(root);
    UNREF_IF_SET(app);
#undef UNREF_IF_SET
    if (listener) {
        atspi_event_listener_deregister(listener, "object:", &error);
        if (error) g_clear_error(&error);
        g_object_unref(listener);
    }
    if (atspi_is_initialized()) atspi_exit();
    return result == 0 && !failed ? 0 : 1;
}
