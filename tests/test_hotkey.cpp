#include "core/Hotkey.h"

#include <windows.h>

#include <doctest/doctest.h>

using rec::Hotkey;
using rec::formatHotkey;
using rec::parseHotkey;

TEST_CASE("parses common combos") {
    auto hk = parseHotkey("Alt+F10");
    REQUIRE(hk);
    CHECK(hk->modifiers == MOD_ALT);
    CHECK(hk->vk == VK_F10);

    hk = parseHotkey(" ctrl + shift + r ");
    REQUIRE(hk);
    CHECK(hk->modifiers == (MOD_CONTROL | MOD_SHIFT));
    CHECK(hk->vk == 'R');

    hk = parseHotkey("Win+PrintScreen");
    REQUIRE(hk);
    CHECK(hk->modifiers == MOD_WIN);
    CHECK(hk->vk == VK_SNAPSHOT);

    hk = parseHotkey("F13");
    REQUIRE(hk);
    CHECK(hk->modifiers == 0);
    CHECK(hk->vk == VK_F13);

    hk = parseHotkey("Ctrl+Num5");
    REQUIRE(hk);
    CHECK(hk->vk == VK_NUMPAD5);
}

TEST_CASE("empty string means unbound") {
    auto hk = parseHotkey("  ");
    REQUIRE(hk);
    CHECK_FALSE(hk->bound());
    CHECK(formatHotkey(*hk).empty());
}

TEST_CASE("formats canonically and round-trips") {
    for (const char* text : {"Ctrl+Alt+Shift+Win+F24", "Alt+F9", "Ctrl+PageDown", "Alt+=", "Ctrl+Alt+/", "Pause"}) {
        auto hk = parseHotkey(text);
        REQUIRE_MESSAGE(hk, text);
        CHECK(formatHotkey(*hk) == text);
        CHECK(parseHotkey(formatHotkey(*hk)).value() == *hk);
    }
    CHECK(formatHotkey(parseHotkey("shift+alt+control+x").value()) == "Ctrl+Alt+Shift+X");
}

TEST_CASE("rejects invalid combos") {
    CHECK_FALSE(parseHotkey("Alt+"));
    CHECK_FALSE(parseHotkey("Ctrl+Alt"));
    CHECK_FALSE(parseHotkey("Alt+Banana"));
    CHECK_FALSE(parseHotkey("Alt+F10+F11"));
    CHECK_FALSE(parseHotkey("F10+Alt"));
    CHECK_FALSE(parseHotkey("F25"));
}

TEST_CASE("typing keys need Ctrl, Alt or Win") {
    CHECK_FALSE(parseHotkey("R"));
    CHECK_FALSE(parseHotkey("Shift+R"));
    CHECK_FALSE(parseHotkey("Space"));
    CHECK(parseHotkey("Alt+R"));
    CHECK(parseHotkey("Win+Space"));
    CHECK(parseHotkey("Home"));
}
