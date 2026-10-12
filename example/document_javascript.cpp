// Copyright (c) 2026 WebCpp.org
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// https://www.boost.org/LICENSE_1_0.txt)

// JavaScript's ways with JSON, in C++: navigation that never fails, reading with a fallback,
// assignment, delete, Object.assign, deep copies, iteration, the sugar that throws as JavaScript
// does, and where the document differs from JavaScript on purpose. Each line it prints starts with
// the JavaScript expression it stands for.

// tag::includes[]
#include <webcpp/document.hpp>

#include <cstdint>
#include <iostream>
#include <string_view>

namespace wd = webcpp::document;
// end::includes[]

namespace {

// tag::show[]
/** Prints a JavaScript expression and the value it reads, `undefined` when nothing is there. */
void show(std::string_view expression, const wd::const_value& v) {
    std::cout << expression << " -> " << (v.is_undefined() ? "undefined" : v.stringify()) << '\n';
}

// end::show[]

/** Prints a JavaScript expression and its boolean or number. */
template <class T>
void say(std::string_view expression, const T& v) {
    std::cout << expression << " -> " << std::boolalpha << v << '\n';
}

/** Navigation, reading, writing, copies and iteration: the core, which never throws. */
bool core() {
    // tag::navigation[]
    constexpr std::string_view text = R"({"user":{"name":"Ada","tags":["math"]},"n":3})";
    wd::result<wd::document> made = wd::parse(text);
    if (made.has_error()) {
        return false;
    }
    wd::document& doc = *made;
    wd::value user = doc["user"];

    show("user.name", user["name"]);
    show("user.nick", user["nick"]);        // a missing member: set creates it
    show("doc.n.x.y", doc["n"]["x"]["y"]);  // nothing: JavaScript throws a TypeError here
    show("user.tags[0]", user["tags"][0]);
    show("user.tags.at(-1)", user["tags"].at(-1));
    // end::navigation[]

    // tag::reading[]
    say("typeof user.name === 'string'", user["name"].is_string());
    say("'tags' in user", user.contains("tags"));
    if (const wd::result<std::uint64_t> length = user["name"].length()) {
        say("user.name.length", *length);
    }
    say("doc.n ?? 0", doc["n"].value_or(std::int64_t{0}));
    say("user.nick ?? 'none'", user["nick"].value_or(std::string_view("none")));
    say("Number.isInteger(doc.n)", doc["n"].is_integer());
    // end::reading[]

    // tag::writing[]
    // user.nick = "Countess"; user.age = 36; user.tags.push("poetry"); user.home = {...};
    if (!user["nick"].set("Countess") || !user["age"].set(36) || !user["tags"].push("poetry") ||
        !user["home"].set(wd::object{{"city", "London"}, {"rooms", wd::array{1, 2}}})) {
        return false;
    }
    show("user", user);

    // delete user.age; user.nick = undefined, which erases it; Object.assign(user, {...}).
    const wd::result<bool> erased = user.erase("age");
    if (!erased || !user["nick"].set(wd::undefined)) {
        return false;
    }
    say("delete user.age", *erased);
    wd::result<wd::document> defaults = wd::parse(R"({"lang":"en","name":"Lovelace"})");
    if (defaults.has_error() || !user.assign_from(defaults->root())) {
        return false;
    }
    show("user", user);
    // end::writing[]

    // tag::copy[]
    // doc.copy = doc.user shares one object in JavaScript; here it is a copy of the subtree.
    if (!doc["copy"].set(user) || !doc["copy"]["name"].set("Byron")) {
        return false;
    }
    show("user.name", user["name"]);
    show("copy.name", doc["copy"]["name"]);
    // end::copy[]

    // tag::iteration[]
    // for (const [key, value] of Object.entries(user))
    for (auto [key, value] : user.entries()) {
        std::cout << key << ": " << value.stringify() << '\n';
    }
    // for (const tag of user.tags)
    for (const wd::const_value tag : user["tags"]) {
        std::cout << "tag " << tag.stringify() << '\n';
    }
    // end::iteration[]
    return true;
}

/** The sugar that throws, which exists only with exceptions. */
void sugar() {
    // tag::sugar[]
    wd::document doc = wd::parse_or_throw(R"({"n":1,"user":{"name":"Ada"}})");
    doc["n"] = 2;                      // doc.n = 2
    doc["user"]["nick"] = "Countess";  // creates the member
    const auto n = doc["n"].as<std::int64_t>();
    const std::string_view name = doc["user"]["name"].as_string();
    std::cout << n << ' ' << name << '\n';

    // Assignment writes at the handle's place, a deep copy, and never rebinds the handle.
    wd::value place = doc["n"];
    place = doc["user"]["name"];
    show("doc.n", doc["n"]);
    doc["user"]["nick"] = wd::undefined;  // erases the member
    show("doc.user", doc["user"]);

    try {
        doc["n"]["x"]["y"] = 1;  // a write through nothing
    } catch (const wd::type_error& e) {
        std::cout << "TypeError: " << e.what() << '\n';
    }
    try {
        static_cast<void>(doc["user"].as<bool>());
    } catch (const wd::type_error& e) {
        std::cout << "TypeError: " << e.what() << '\n';
    }
    try {
        static_cast<void>(wd::parse_or_throw("[1,]"));
    } catch (const wd::syntax_error& e) {
        std::cout << "SyntaxError: " << e.what() << " at " << e.offset() << '\n';
    }

    const wd::document empty;  // holds null
    wd::document twin = doc;   // an equal and independent copy
    twin["n"] = 3;
    std::cout << empty.stringify() << ' ' << doc.stringify() << ' ' << twin.stringify() << '\n';
    // end::sugar[]
}

/** Where the document differs from JavaScript on purpose. */
bool differences() {
    // tag::differences[]
    wd::result<wd::document> made = wd::parse(R"({"list":[10,20],"o":{"3":"three"},"z":-0})");
    if (made.has_error()) {
        return false;
    }
    wd::document& doc = *made;
    show("doc.list['0']", doc["list"]["0"]);  // JavaScript: 10, an index written as a key
    show("doc.o[3]", doc["o"][3]);            // JavaScript: "three", a key written as an index
    show("doc.z", doc["z"]);                  // -0 is 0, as JSON.stringify writes it
    say(R"(JSON.parse('"\ud800"') refused)", wd::parse(R"("\ud800")").error() == wd::errc::syntax);
    // end::differences[]
    return true;
}

}  // namespace

int main() {
    if (!core()) {
        std::cout << "error\n";
        return 1;
    }
    sugar();
    if (!differences()) {
        std::cout << "error\n";
        return 1;
    }
}
