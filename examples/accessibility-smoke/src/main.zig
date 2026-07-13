const std = @import("std");
const runner = @import("runner");
const native_sdk = @import("native_sdk");

pub const panic = std.debug.FullPanic(native_sdk.debug.capturePanic);

const canvas = native_sdk.canvas;
const geometry = native_sdk.geometry;

pub const canvas_label = "accessibility-canvas";
pub const window_width: f32 = 720;
pub const window_height: f32 = 760;
pub const lesson_count: usize = 1_000;
pub const lesson_row_extent: f32 = 32;
pub const lesson_viewport_height: f32 = 80;
pub const lesson_overscan: usize = 2;
pub const lesson_42_index: usize = 41;
pub const initial_scroll_offset: f32 = lesson_42_index * lesson_row_extent + 8;
pub const initial_text = "seed";

const shell_views = [_]native_sdk.ShellView{
    .{ .label = canvas_label, .kind = .gpu_surface, .fill = true, .role = "Accessibility fixture canvas", .accessibility_label = "Accessibility Smoke", .gpu_backend = .software, .gpu_pixel_format = .bgra8_unorm, .gpu_present_mode = .timer, .gpu_alpha_mode = .@"opaque", .gpu_color_space = .srgb, .gpu_vsync = true },
};
const shell_windows = [_]native_sdk.ShellWindow{.{
    .label = "main",
    .title = "Accessibility Smoke",
    .width = window_width,
    .height = window_height,
    .restore_state = false,
    .views = &shell_views,
}};
pub const shell_scene: native_sdk.ShellConfig = .{ .windows = &shell_windows };

pub const Msg = union(enum) {
    count,
    toggle_study,
    select_reading,
    search_edit: canvas.TextInputEvent,
    volume_changed: f32,
    toggle_details,
    remove_transient,
    restore_transient,
    transient_edit: canvas.TextInputEvent,
};

pub const Model = struct {
    count: u32 = 0,
    study_mode: bool = false,
    reading_mode: bool = false,
    search: canvas.TextBuffer(32) = .{},
    volume: f32 = 0.5,
    details_expanded: bool = false,
    transient_present: bool = true,
    transient: canvas.TextBuffer(32) = .{},

    pub fn init() Model {
        var model: Model = .{};
        model.search = canvas.TextBuffer(32).init(initial_text);
        model.search.selection = .{ .anchor = 1, .focus = 3 };
        model.search.composition = canvas.TextRange.init(2, 4);
        model.transient = canvas.TextBuffer(32).init("transient seed");
        return model;
    }
};

pub fn update(model: *Model, msg: Msg) void {
    switch (msg) {
        .count => model.count += 1,
        .toggle_study => model.study_mode = !model.study_mode,
        .select_reading => model.reading_mode = true,
        .search_edit => |edit| model.search.apply(edit),
        .volume_changed => |value| model.volume = value,
        .toggle_details => model.details_expanded = !model.details_expanded,
        .remove_transient => model.transient_present = false,
        .restore_transient => model.transient_present = true,
        .transient_edit => |edit| model.transient.apply(edit),
    }
}

pub const SmokeApp = native_sdk.UiApp(Model, Msg);
pub const SmokeUi = SmokeApp.Ui;

pub fn lessonWindow(scroll_offset: f32) canvas.VirtualListRange {
    return canvas.virtualListRange(.{
        .item_count = lesson_count,
        .item_extent = lesson_row_extent,
        .viewport_extent = lesson_viewport_height,
        .scroll_offset = scroll_offset,
        .overscan = lesson_overscan,
    });
}

fn lessonList(ui: *SmokeUi, scroll_offset: f32) SmokeUi.Node {
    const window = lessonWindow(scroll_offset);
    const rows = ui.arena.alloc(SmokeUi.Node, window.itemCount()) catch {
        ui.failed = true;
        return ui.list(.{}, .{});
    };
    for (rows, 0..) |*row, offset| {
        const index = window.start_index + offset;
        const label = ui.fmt("Lesson {d}", .{index + 1});
        var node = ui.listItem(.{
            .height = lesson_row_extent,
            .semantics = .{ .label = label, .focusable = true },
        }, label);
        node.key = .{ .int = @intCast(index) };
        row.* = node;
    }
    return ui.el(.list, .{
        .global_key = .{ .str = "lesson-list" },
        .value = window.layout_offset,
        .height = lesson_viewport_height,
        .virtualized = true,
        .virtual_item_extent = lesson_row_extent,
        .virtual_overscan = lesson_overscan,
        .virtual_item_count = lesson_count,
        .virtual_first_index = window.start_index,
        .semantics = .{ .label = "Lesson list" },
    }, .{rows});
}

fn resultText(ui: *SmokeUi, label: []const u8) SmokeUi.Node {
    return ui.text(.{ .size = .sm, .semantics = .{ .label = label } }, label);
}

fn searchField(ui: *SmokeUi, model: *const Model) SmokeUi.Node {
    var node = ui.el(.text_field, .{
        .width = 240,
        .text = model.search.text(),
        .placeholder = "Search the lesson list",
        .on_input = SmokeUi.inputMsg(.search_edit),
        .semantics = .{ .label = "Search lessons" },
    }, .{});
    node.widget.text_selection = model.search.selection;
    node.widget.text_composition = model.search.composition;
    return node;
}

fn lessonGrid(ui: *SmokeUi) SmokeUi.Node {
    return ui.el(.data_grid, .{
        .height = 32,
        .semantics = .{ .label = "Lesson grid" },
    }, ui.el(.data_row, .{
        .height = 32,
        .semantics = .{ .label = "Lesson row" },
    }, .{
        ui.el(.data_cell, .{ .text = "Lesson cell", .grow = 1, .semantics = .{ .label = "Lesson cell" } }, .{}),
        ui.el(.data_cell, .{ .text = "Ready", .grow = 1, .semantics = .{ .label = "State cell" } }, .{}),
    }));
}

fn transientRow(ui: *SmokeUi, model: *const Model) SmokeUi.Node {
    if (model.transient_present) {
        return ui.row(.{ .gap = 8, .cross = .center }, .{
            ui.el(.text_field, .{
                .global_key = .{ .str = "transient-target" },
                .width = 200,
                .text = model.transient.text(),
                .on_input = SmokeUi.inputMsg(.transient_edit),
                .semantics = .{ .label = "Transient target" },
            }, .{}),
            ui.button(.{ .global_key = .{ .str = "remove-transient" }, .on_press = .remove_transient, .semantics = .{ .label = "Remove transient" } }, "Remove transient"),
        });
    }
    return ui.row(.{ .gap = 8, .cross = .center }, .{
        ui.button(.{ .global_key = .{ .str = "restore-transient" }, .on_press = .restore_transient, .semantics = .{ .label = "Restore transient" } }, "Restore transient"),
    });
}

pub fn view(ui: *SmokeUi, model: *const Model) SmokeUi.Node {
    const count_result = ui.fmt("Count result: {d}", .{model.count});
    const study_result = ui.fmt("Study mode result: {s}", .{if (model.study_mode) "on" else "off"});
    const reading_result = ui.fmt("Reading mode result: {s}", .{if (model.reading_mode) "selected" else "unselected"});
    const search_result = ui.fmt("Search result: {s}", .{model.search.text()});
    const volume_result = ui.fmt("Volume result: {d}", .{@as(u32, @intFromFloat(model.volume * 100))});
    const details_result = ui.fmt("Details result: {s}", .{if (model.details_expanded) "expanded" else "collapsed"});
    const transient_result = ui.fmt("Transient result: {s}", .{if (model.transient_present) "present" else "removed"});

    return ui.column(.{ .padding = 12, .gap = 8, .semantics = .{ .label = "Accessibility smoke" } }, .{
        lessonList(ui, initial_scroll_offset),
        ui.column(.{ .gap = 6, .semantics = .{ .label = "Controls" } }, .{
            ui.row(.{ .gap = 8, .cross = .center }, .{
                ui.button(.{ .on_press = .count, .semantics = .{ .label = "Count action" } }, "Count action"),
                resultText(ui, count_result),
            }),
            ui.row(.{ .gap = 8, .cross = .center }, .{
                ui.checkbox(.{ .text = "Study mode", .checked = model.study_mode, .on_toggle = .toggle_study, .semantics = .{ .label = "Study mode" } }),
                resultText(ui, study_result),
            }),
            ui.el(.radio_group, .{ .gap = 8, .cross = .center, .semantics = .{ .label = "Reading options" } }, .{
                ui.el(.radio, .{ .text = "Reading mode", .selected = model.reading_mode, .on_press = .select_reading, .semantics = .{ .label = "Reading mode" } }, .{}),
                resultText(ui, reading_result),
            }),
            ui.row(.{ .gap = 8, .cross = .center }, .{
                searchField(ui, model),
                resultText(ui, search_result),
            }),
            ui.row(.{ .gap = 8, .cross = .center }, .{
                ui.el(.slider, .{ .width = 200, .value = model.volume, .on_value = SmokeUi.valueMsg(.volume_changed), .semantics = .{ .label = "Volume" } }, .{}),
                resultText(ui, volume_result),
                ui.el(.progress, .{ .width = 120, .value = 0.42, .semantics = .{ .label = "Completion" } }, .{}),
            }),
            lessonGrid(ui),
            ui.row(.{ .gap = 8, .cross = .start }, .{
                ui.el(.accordion, .{ .width = 240, .text = "Details", .selected = model.details_expanded, .on_toggle = .toggle_details, .semantics = .{ .label = "Details" } }, .{
                    ui.text(.{}, "Expanded details body"),
                }),
                resultText(ui, details_result),
            }),
            transientRow(ui, model),
            resultText(ui, transient_result),
        }),
    });
}

pub fn options() SmokeApp.Options {
    return .{
        .name = "accessibility-smoke",
        .scene = shell_scene,
        .canvas_label = canvas_label,
        .update = update,
        .view = view,
    };
}

pub fn main(init: std.process.Init) !void {
    const app_state = try std.heap.page_allocator.create(SmokeApp);
    defer std.heap.page_allocator.destroy(app_state);
    app_state.* = SmokeApp.init(std.heap.page_allocator, Model.init(), options());
    defer app_state.deinit();
    try runner.runWithOptions(app_state.app(), .{
        .app_name = "accessibility-smoke",
        .window_title = "Accessibility Smoke",
        .bundle_id = "dev.native_sdk.accessibility_smoke",
        .default_frame = geometry.RectF.init(0, 0, window_width, window_height),
        .restore_state = false,
        .js_window_api = false,
    }, init);
}

test {
    _ = @import("tests.zig");
}
