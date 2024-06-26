#include "acquire.h"
#include "device/hal/device.manager.h"
#include "platform.h"
#include "logger.h"

#include <cstdio>
#include <stdexcept>

/// Helper for passing size static strings as function args.
/// For a function: `f(char*,size_t)` use `f(SIZED("hello"))`.
/// Expands to `f("hello",5)`.
#define SIZED(str) str, sizeof(str)

#define L (aq_logger)
#define LOG(...) L(0, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGE(...) L(1, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define EXPECT(e, ...)                                                         \
    do {                                                                       \
        if (!(e)) {                                                            \
            char buf[1 << 8] = { 0 };                                          \
            LOGE(__VA_ARGS__);                                                 \
            snprintf(buf, sizeof(buf) - 1, __VA_ARGS__);                       \
            throw std::runtime_error(buf);                                     \
        }                                                                      \
    } while (0)
#define CHECK(e) EXPECT(e, "Expression evaluated as false: %s", #e)
#define DEVOK(e) CHECK(Device_Ok == (e))
#define OK(e) CHECK(AcquireStatus_Ok == (e))

void
reporter(int is_error,
         const char* file,
         int line,
         const char* function,
         const char* msg)
{
    auto stream = is_error ? stderr : stdout;
    fprintf(stream,
            "%s%s(%d) - %s: %s\n",
            is_error ? "ERROR " : "",
            file,
            line,
            function,
            msg);
    fflush(stream);
}

void
setup(AcquireRuntime* runtime)
{
    CHECK(runtime);

    auto dm = acquire_device_manager(runtime);
    CHECK(dm);

    AcquireProperties props = {};
    OK(acquire_get_configuration(runtime, &props));

    DEVOK(device_manager_select(dm,
                                DeviceKind_Camera,
                                SIZED(".*Kinetix.*") - 1,
                                &props.video[0].camera.identifier));
    DEVOK(device_manager_select(dm,
                                DeviceKind_Storage,
                                SIZED("tiff") - 1,
                                &props.video[0].storage.identifier));

    storage_properties_init(&props.video[0].storage.settings,
                            0,
                            SIZED(TEST ".tif"),
                            nullptr,
                            0,
                            { .x = 1, .y = 1 },
                            0);

    props.video[0].camera.settings.binning = 1;
    props.video[0].camera.settings.pixel_type = SampleType_u16;
    props.video[0].camera.settings.shape = { .x = 3200, .y = 3200 };
    props.video[0].camera.settings.exposure_time_us = 1e4; // 10 ms
    props.video[0].max_frame_count = 100;

    OK(acquire_configure(runtime, &props));
}

void
acquire(AcquireRuntime* runtime)
{
    AcquireProperties props = {};
    OK(acquire_get_configuration(runtime, &props));

    const auto next = [](VideoFrame* cur) -> VideoFrame* {
        return (VideoFrame*)(((uint8_t*)cur) + cur->bytes_of_frame);
    };

    const auto consumed_bytes = [](const VideoFrame* const cur,
                                   const VideoFrame* const end) -> size_t {
        return (uint8_t*)end - (uint8_t*)cur;
    };

    struct clock clock_{};
    static double time_limit_ms = 20000.0;
    clock_init(&clock_);
    clock_shift_ms(&clock_, time_limit_ms);
    OK(acquire_start(runtime));

    uint64_t nframes = 0;
    while (nframes < props.video[0].max_frame_count) {
        struct clock throttle{};
        clock_init(&throttle);
        EXPECT(clock_cmp_now(&clock_) < 0,
               "Timeout at %f ms",
               clock_toc_ms(&clock_) + time_limit_ms);
        VideoFrame *beg, *end, *cur;
        OK(acquire_map_read(runtime, 0, &beg, &end));

        for (cur = beg; cur < end; cur = next(cur)) {
            LOG("stream %d counting frame w id %d", 0, cur->frame_id);
            CHECK(cur->shape.dims.width ==
                  props.video[0].camera.settings.shape.x);
            CHECK(cur->shape.dims.height ==
                  props.video[0].camera.settings.shape.y);
            ++nframes;
        }

        {
            size_t n = consumed_bytes(beg, end);
            OK(acquire_unmap_read(runtime, 0, n));
            if (n)
                LOG("stream %d consumed bytes %d", 0, n);
        }

        clock_sleep_ms(&throttle, 100.0f);

        LOG("stream %d nframes %d. remaining time %f s",
            0,
            nframes,
            -1e-3 * clock_toc_ms(&clock_));
    }

    CHECK(nframes == props.video[0].max_frame_count);

    OK(acquire_abort(runtime));
    LOG("OK");
}

int
main()
{
    auto* runtime = acquire_init(reporter);
    try {
        setup(runtime);
        acquire(runtime);

        OK(acquire_shutdown(runtime));
        LOG("OK");
        return 0;
    } catch (const std::exception& exc) {
        LOGE("Exception: %s", exc.what());
    } catch (...) {
        LOGE("Uncaught exception");
    }

    acquire_shutdown(runtime);
    return 1;
}
