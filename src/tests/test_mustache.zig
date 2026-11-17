const std = @import("std");
const zap = @import("zap");

const User = struct {
    name: []const u8,
    id: isize,
};

const data = .{
    .users = [_]User{
        .{
            .name = "Rene",
            .id = 1,
        },
        .{
            .name = "Caro",
            .id = 6,
        },
    },
    .nested = .{
        .item = "nesting works",
    },
};

test "mustacheData" {
    const template = "{{=<< >>=}}* Users:\n<<#users>><<id>>. <<& name>> (<<name>>)\n<</users>>\nNested: <<& nested.item >>.";

    var mustache = try zap.Mustache.fromData(template);
    defer mustache.deinit();

    const ret = mustache.build(data);
    defer ret.deinit();

    try std.testing.expectEqualSlices(u8, "* Users:\n1. Rene (Rene)\n6. Caro (Caro)\nNested: nesting works.", ret.str().?);
}

test "mustacheLoad" {
    var mustache = try zap.Mustache.fromFile("./src/tests/testtemplate.html");
    defer mustache.deinit();

    const ret = mustache.build(data);
    defer ret.deinit();

    try std.testing.expectEqualSlices(u8, "* Users:\n1. Rene (Rene)\n6. Caro (Caro)\nNested: nesting works.\n", ret.str().?);
}

// Exercise the reflected payload types as well as ordinary structs and arrays.
test "mustache tagged union and vector context" {
    const Value = union(enum) { text: []const u8, number: i32 };
    const Kind = enum(u8) { example = 7 };
    var mustache = try zap.Mustache.fromData("{{text}} {{number}} {{kind}}:{{#items}} item{{/items}}");
    defer mustache.deinit();

    const ret = mustache.build(.{
        .text = Value{ .text = "hello" },
        .number = Value{ .number = 42 },
        .kind = Kind.example,
        .items = @as(@Vector(3, u16), .{ 1, 2, 3 }),
    });
    defer ret.deinit();
    try std.testing.expectEqualStrings("hello 42 7: item item item", ret.str().?);
}
