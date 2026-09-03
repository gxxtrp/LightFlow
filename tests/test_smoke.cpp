#include <catch2/catch_test_macros.hpp>
#include <lightflow/lightflow.hpp>

TEST_CASE("LightFlow version and metadata", "[core][smoke]") {
    REQUIRE(lf::VERSION_MAJOR == 0);
    REQUIRE(lf::VERSION_MINOR == 1);
    REQUIRE(lf::VERSION_PATCH == 0);
    REQUIRE(lf::version() == "0.1.0");
}

TEST_CASE("LightFlow cacheline and slab sizing constants", "[core][smoke]") {
    REQUIRE(lf::CACHELINE_SIZE == 64);
    REQUIRE(lf::SLAB_SIZE == 64 * 1024);
}

TEST_CASE("LightFlow primitive type widths", "[core][smoke]") {
    STATIC_REQUIRE(sizeof(lf::u8) == 1);
    STATIC_REQUIRE(sizeof(lf::u16) == 2);
    STATIC_REQUIRE(sizeof(lf::u32) == 4);
    STATIC_REQUIRE(sizeof(lf::u64) == 8);

    STATIC_REQUIRE(sizeof(lf::i8) == 1);
    STATIC_REQUIRE(sizeof(lf::i16) == 2);
    STATIC_REQUIRE(sizeof(lf::i32) == 4);
    STATIC_REQUIRE(sizeof(lf::i64) == 8);

    STATIC_REQUIRE(sizeof(lf::usize) == sizeof(void*));
}

TEST_CASE("LightFlow Tracy profiler macro compilation", "[core][smoke]") {
    {
        LF_ZONE_SCOPED;
    }
    {
        LF_ZONE_SCOPED_N("SmokeTestZone");
    }
    {
        LF_ZONE_SCOPED_COLOR(0x00FF00);
    }
    {
        LF_ZONE_NAMED(smokeNamedZone, "NamedZone");
        LF_ZONE_NAMED_COLOR(smokeColorZone, "ColorZone", 0xFF00FF);
    }
    LF_FRAME_MARK;
    LF_FRAME_MARK_NAMED("SmokeFrame");
    LF_SET_THREAD_NAME("MainSmokeWorker");

    SUCCEED("Tracy macros expand and compile cleanly");
}
