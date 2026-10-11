#include "core/Settings.h"

#include <doctest/doctest.h>

using rec::Settings;

TEST_CASE("settings round-trip through JSON") {
    Settings s;
    s.video.codec = rec::Codec::AV1;
    s.video.tenBit = true;
    s.replay.seconds = 300;
    s.hotkeys.saveReplay = "Ctrl+Shift+F10";
    s.detection.neverRecord = {"C:\\Tools\\fullscreen-thing.exe"};

    const Settings back = Settings::fromJson(s.toJson());
    CHECK(back.video.codec == rec::Codec::AV1);
    CHECK(back.video.tenBit);
    CHECK(back.replay.seconds == 300);
    CHECK(back.hotkeys.saveReplay == "Ctrl+Shift+F10");
    REQUIRE(back.detection.neverRecord.size() == 1);
    CHECK(back.detection.neverRecord[0] == "C:\\Tools\\fullscreen-thing.exe");
}

TEST_CASE("missing fields keep defaults") {
    const Settings s = Settings::fromJson(R"({"video": {"codec": "h264"}, "hotkeys": {"saveReplay": "F13"}})");
    CHECK(s.video.codec == rec::Codec::H264);
    CHECK(s.video.fps == 60);
    CHECK(s.hotkeys.saveReplay == "F13");
    CHECK(s.hotkeys.toggleRecording == "Alt+F9");
    CHECK(s.replay.seconds == 120);
}

TEST_CASE("malformed JSON falls back to defaults") {
    const Settings s = Settings::fromJson("{ not json");
    CHECK(s.video.codec == rec::Codec::HEVC);
    CHECK(s.hotkeys.saveReplay == "Alt+F10");
}
