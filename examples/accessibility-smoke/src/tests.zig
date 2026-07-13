const std = @import("std");
const native_sdk = @import("native_sdk");
const main = @import("main.zig");

const geometry = native_sdk.geometry;
const testing = std.testing;

const Harness = struct {
    harness: *native_sdk.TestHarness(),
    app_state: *main.SmokeApp,
    app: native_sdk.App,

    fn create() !Harness {
        const harness = try native_sdk.TestHarness().create(testing.allocator, .{ .size = geometry.SizeF.init(main.window_width, main.window_height) });
        errdefer harness.destroy(testing.allocator);
        harness.null_platform.gpu_surfaces = true;

        const app_state = try testing.allocator.create(main.SmokeApp);
        errdefer testing.allocator.destroy(app_state);
        app_state.* = main.SmokeApp.init(std.heap.page_allocator, main.Model.init(), main.options());
        errdefer app_state.deinit();
        const app = app_state.app();
        try harness.start(app);
        try harness.runtime.dispatchPlatformEvent(app, .{ .gpu_surface_frame = .{
            .label = main.canvas_label,
            .size = geometry.SizeF.init(main.window_width, main.window_height),
            .scale_factor = 1,
            .frame_index = 1,
            .timestamp_ns = 1_000_000,
            .nonblank = true,
        } });
        return .{ .harness = harness, .app_state = app_state, .app = app };
    }

    fn destroy(self: *Harness) void {
        self.app_state.deinit();
        testing.allocator.destroy(self.app_state);
        self.harness.destroy(testing.allocator);
    }

    fn snapshot(self: *Harness) native_sdk.automation.snapshot.Input {
        return self.harness.runtime.automationSnapshot("Accessibility Smoke");
    }

    fn action(self: *Harness, id: u64, action_kind: native_sdk.runtime.CanvasWidgetAccessibilityActionKind) !void {
        _ = try self.harness.runtime.dispatchCanvasWidgetAccessibilityAction(self.app, 1, main.canvas_label, .{ .id = id, .action = action_kind });
    }
};

fn widgetNamed(snapshot: native_sdk.automation.snapshot.Input, role: []const u8, name: []const u8) ?native_sdk.automation.snapshot.Widget {
    for (snapshot.widgets) |widget| {
        if (!std.mem.eql(u8, widget.view_label, main.canvas_label)) continue;
        if (!std.mem.eql(u8, widget.role, role)) continue;
        if (std.mem.eql(u8, widget.name, name)) return widget;
    }
    return null;
}

test "virtual lesson semantics keep a stable two-level parent and bounded window" {
    var live = try Harness.create();
    defer live.destroy();

    var snapshot = live.snapshot();
    const list = widgetNamed(snapshot, "list", "Lesson list").?;
    const lesson_42 = widgetNamed(snapshot, "listitem", "Lesson 42").?;
    const lesson_40 = widgetNamed(snapshot, "listitem", "Lesson 40").?;

    try testing.expectEqual(list.id, lesson_42.parent_id.?);
    try testing.expect(lesson_42.list.present);
    try testing.expectEqual(@as(u32, 41), lesson_42.list.item_index);
    try testing.expectEqual(@as(u32, main.lesson_count), lesson_42.list.item_count);
    try testing.expect(list.virtual_range.present);
    try testing.expectEqual(@as(u32, 39), list.virtual_range.start_index);
    try testing.expectEqual(@as(u32, 46), list.virtual_range.end_index);
    try testing.expectEqual(@as(u32, 7), list.virtual_range.rendered_count);
    try testing.expectEqual(@as(usize, 7), live.app_state.tree.?.root.children[0].children.len);

    try testing.expect(lesson_40.bounds.y + lesson_40.bounds.height <= list.bounds.y);
    try testing.expect(lesson_42.bounds.y < list.bounds.y);
    try testing.expect(lesson_42.bounds.y + lesson_42.bounds.height > list.bounds.y);

    const list_id = list.id;
    const lesson_42_id = lesson_42.id;
    _ = try live.harness.runtime.dispatchCanvasWidgetAccessibilityAction(live.app, 1, main.canvas_label, .{
        .id = list_id,
        .action = .scroll_by,
        .text = "0.85",
    });
    snapshot = live.snapshot();
    try testing.expect(widgetNamed(snapshot, "listitem", "Lesson 40") == null);
    try testing.expect(widgetNamed(snapshot, "listitem", "Lesson 48") != null);
    try testing.expectEqual(list_id, widgetNamed(snapshot, "list", "Lesson list").?.id);
    try testing.expectEqual(lesson_42_id, widgetNamed(snapshot, "listitem", "Lesson 42").?.id);

    try live.action(widgetNamed(snapshot, "button", "Count action").?.id, .press);
    snapshot = live.snapshot();
    try testing.expectEqual(list_id, widgetNamed(snapshot, "list", "Lesson list").?.id);
    try testing.expectEqual(lesson_42_id, widgetNamed(snapshot, "listitem", "Lesson 42").?.id);
    try testing.expectEqual(list_id, widgetNamed(snapshot, "listitem", "Lesson 42").?.parent_id.?);
}

test "typed accessibility actions update the model and semantic results" {
    var live = try Harness.create();
    defer live.destroy();

    var snapshot = live.snapshot();
    const initial_field = widgetNamed(snapshot, "textbox", "Search lessons").?;
    try testing.expect(widgetNamed(snapshot, "button", "Count action").?.actions.focus);
    try testing.expect(widgetNamed(snapshot, "button", "Count action").?.actions.press);
    try testing.expect(widgetNamed(snapshot, "checkbox", "Study mode").?.actions.toggle);
    try testing.expect(widgetNamed(snapshot, "radio", "Reading mode").?.actions.select);
    try testing.expect(initial_field.actions.set_text);
    try testing.expect(initial_field.actions.set_selection);
    try testing.expect(widgetNamed(snapshot, "slider", "Volume").?.actions.increment);
    try testing.expect(widgetNamed(snapshot, "slider", "Volume").?.actions.decrement);
    try testing.expect(widgetNamed(snapshot, "group", "Details").?.actions.toggle);
    try testing.expectEqualStrings(main.initial_text, initial_field.text_value);
    try testing.expectEqualDeep(native_sdk.automation.snapshot.TextRange{ .start = 1, .end = 3 }, initial_field.text_selection.?);
    try testing.expectEqualDeep(native_sdk.automation.snapshot.TextRange{ .start = 2, .end = 4 }, initial_field.text_composition.?);

    try live.action(widgetNamed(snapshot, "button", "Count action").?.id, .press);
    try testing.expectEqual(@as(u32, 1), live.app_state.model.count);
    snapshot = live.snapshot();
    try testing.expect(widgetNamed(snapshot, "text", "Count result: 1") != null);

    try live.action(widgetNamed(snapshot, "checkbox", "Study mode").?.id, .toggle);
    try testing.expect(live.app_state.model.study_mode);
    snapshot = live.snapshot();
    try testing.expect(widgetNamed(snapshot, "text", "Study mode result: on") != null);

    try live.action(widgetNamed(snapshot, "radio", "Reading mode").?.id, .select);
    try testing.expect(live.app_state.model.reading_mode);
    snapshot = live.snapshot();
    try testing.expect(widgetNamed(snapshot, "radio", "Reading mode").?.selected);
    try testing.expect(widgetNamed(snapshot, "text", "Reading mode result: selected") != null);

    const field_id = widgetNamed(snapshot, "textbox", "Search lessons").?.id;
    _ = try live.harness.runtime.dispatchCanvasWidgetAccessibilityAction(live.app, 1, main.canvas_label, .{ .id = field_id, .action = .set_text, .text = "typed" });
    try testing.expectEqualStrings("typed", live.app_state.model.search.text());
    snapshot = live.snapshot();
    try testing.expect(widgetNamed(snapshot, "text", "Search result: typed") != null);

    _ = try live.harness.runtime.dispatchCanvasWidgetAccessibilityAction(live.app, 1, main.canvas_label, .{ .id = field_id, .action = .set_selection, .selection = .{ .anchor = 1, .focus = 4 } });
    snapshot = live.snapshot();
    try testing.expectEqualDeep(native_sdk.automation.snapshot.TextRange{ .start = 1, .end = 4 }, widgetNamed(snapshot, "textbox", "Search lessons").?.text_selection.?);

    _ = try live.harness.runtime.dispatchCanvasWidgetAccessibilityAction(live.app, 1, main.canvas_label, .{ .id = field_id, .action = .set_composition, .text = "ime" });
    snapshot = live.snapshot();
    try testing.expectEqualDeep(native_sdk.automation.snapshot.TextRange{ .start = 1, .end = 4 }, widgetNamed(snapshot, "textbox", "Search lessons").?.text_composition.?);
    _ = try live.harness.runtime.dispatchCanvasWidgetAccessibilityAction(live.app, 1, main.canvas_label, .{ .id = field_id, .action = .commit_composition });
    snapshot = live.snapshot();
    try testing.expect(widgetNamed(snapshot, "textbox", "Search lessons").?.text_composition == null);

    try live.action(widgetNamed(snapshot, "slider", "Volume").?.id, .increment);
    try testing.expectApproxEqAbs(@as(f32, 0.55), live.app_state.model.volume, 0.001);
    snapshot = live.snapshot();
    try testing.expect(widgetNamed(snapshot, "text", "Volume result: 55") != null);
    const volume_id = widgetNamed(snapshot, "slider", "Volume").?.id;
    _ = try live.harness.runtime.dispatchCanvasWidgetAccessibilityAction(live.app, 1, main.canvas_label, .{ .id = volume_id, .action = .set_value, .value = 0.73 });
    try testing.expectApproxEqAbs(@as(f32, 0.73), live.app_state.model.volume, 0.001);
    snapshot = live.snapshot();
    try testing.expect(widgetNamed(snapshot, "text", "Volume result: 73") != null);
    const progress = widgetNamed(snapshot, "progressbar", "Completion").?;
    try testing.expectApproxEqAbs(@as(f32, 0.42), progress.value.?, 0.001);
    try testing.expect(!progress.actions.increment and !progress.actions.decrement);

    const grid = widgetNamed(snapshot, "grid", "Lesson grid").?;
    const row = widgetNamed(snapshot, "row", "Lesson row").?;
    const cell = widgetNamed(snapshot, "gridcell", "Lesson cell").?;
    try testing.expectEqual(grid.id, row.parent_id.?);
    try testing.expectEqual(row.id, cell.parent_id.?);
    try testing.expectEqual(@as(?usize, 1), grid.grid_row_count);
    try testing.expectEqual(@as(?usize, 2), grid.grid_column_count);

    const details_id = widgetNamed(snapshot, "group", "Details").?.id;
    try live.action(details_id, .toggle);
    try testing.expect(live.app_state.model.details_expanded);
    snapshot = live.snapshot();
    try testing.expectEqual(@as(?bool, true), widgetNamed(snapshot, "group", "Details").?.expanded);
    try testing.expect(widgetNamed(snapshot, "text", "Details result: expanded") != null);

    const transient = widgetNamed(snapshot, "textbox", "Transient target").?;
    const transient_id = transient.id;
    try testing.expect(transient.actions.set_text);
    try live.action(widgetNamed(snapshot, "button", "Remove transient").?.id, .press);
    try testing.expect(!live.app_state.model.transient_present);
    snapshot = live.snapshot();
    try testing.expect(widgetNamed(snapshot, "textbox", "Transient target") == null);
    try testing.expect(widgetNamed(snapshot, "text", "Transient result: removed") != null);
    try testing.expectError(error.InvalidCommand, live.harness.runtime.dispatchCanvasWidgetAccessibilityAction(live.app, 1, main.canvas_label, .{ .id = transient_id, .action = .set_text, .text = "stale" }));

    try live.action(widgetNamed(snapshot, "button", "Restore transient").?.id, .press);
    try testing.expect(live.app_state.model.transient_present);
    snapshot = live.snapshot();
    try testing.expectEqual(transient_id, widgetNamed(snapshot, "textbox", "Transient target").?.id);
    _ = try live.harness.runtime.dispatchCanvasWidgetAccessibilityAction(live.app, 1, main.canvas_label, .{ .id = transient_id, .action = .set_text, .text = "restored" });
    try testing.expectEqualStrings("restored", live.app_state.model.transient.text());
}
