#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Only the test harness needs to start the private task. The implementation below
// is included unchanged, so tests execute production debounce/action logic.
#define private public
#include "../../main/hal/hal.h"
#undef private
#include "../../main/hal/hal_button.cpp"

M5Unified M5;

namespace {
constexpr std::uint8_t A = 1;
constexpr std::uint8_t B = 2;
struct StopSimulation {};
void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}
struct Counts {
    std::vector<unsigned> a;
    std::vector<unsigned> b;
    std::vector<unsigned> long_a;
};
struct Simulation {
    Hal hal;
    TickType_t base = 0;
    unsigned elapsed = 0;
    unsigned duration = 0;
    std::uint8_t raw = 0;
    int critical_depth = 0;
    TaskFunction_t task = nullptr;
    void* argument = nullptr;
    Counts counts;
    std::function<std::uint8_t(unsigned)> pins;
    std::function<void(Simulation&)> observe;
    std::function<bool(unsigned)> poll_long = [](unsigned) { return true; };
};
Simulation* current = nullptr;
std::uint8_t held(unsigned time, unsigned begin, unsigned end, std::uint8_t buttons)
{
    return time >= begin && time < end ? buttons : 0;
}
Counts run(unsigned duration, std::function<std::uint8_t(unsigned)> pins,
           std::function<void(Simulation&)> observe = {}, TickType_t base = 0,
           std::function<bool(unsigned)> poll_long = [](unsigned) { return true; })
{
    Simulation simulation;
    simulation.duration = duration;
    simulation.pins = pins;
    simulation.observe = observe;
    simulation.base = base;
    simulation.poll_long = poll_long;
    simulation.raw = pins(0);
    current = &simulation;
    require(simulation.hal.initButtonInput(), "button task creation failed");
    require(simulation.task != nullptr, "no task created");
    try {
        simulation.task(simulation.argument);
    } catch (const StopSimulation&) {
    }
    require(simulation.critical_depth == 0, "critical section left locked");
    current = nullptr;
    return simulation.counts;
}
void expect(const Counts& result, unsigned a, unsigned b, unsigned long_a)
{
    require(result.a.size() == a, "unexpected short-A count: " + std::to_string(result.a.size()));
    require(result.b.size() == b, "unexpected short-B count: " + std::to_string(result.b.size()));
    require(result.long_a.size() == long_a, "unexpected long-A count: " + std::to_string(result.long_a.size()));
}
}

TickType_t xTaskGetTickCount() { return current->base + current->elapsed; }
int gpio_get_level(gpio_num_t pin) { return (current->raw & (pin == GPIO_NUM_4 ? A : B)) ? 0 : 1; }
void testEnterCritical(portMUX_TYPE*)
{
    require(current->critical_depth++ == 0, "unexpected nested critical section");
}
void testExitCritical(portMUX_TYPE*)
{
    require(--current->critical_depth == 0, "unbalanced critical section");
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char*, unsigned int, void* argument, int, void*, int)
{
    current->task = task;
    current->argument = argument;
    return pdPASS;
}
void vTaskDelay(TickType_t ticks)
{
    auto& s = *current;
    require(s.critical_depth == 0, "task delays inside critical section");
    if (s.observe) s.observe(s);
    if (s.hal.wasButtonAClicked()) s.counts.a.push_back(s.elapsed);
    if (s.hal.wasButtonBClicked()) s.counts.b.push_back(s.elapsed);
    if (s.poll_long(s.elapsed) && s.hal.wasButtonALongPressed()) s.counts.long_a.push_back(s.elapsed);
    if (s.elapsed >= s.duration) throw StopSimulation{};
    s.elapsed += ticks;
    s.raw = s.pins(s.elapsed);
}

int main()
{
    unsigned passed = 0;
    auto test = [&](const char* name, const std::function<void()>& body) {
        try { body(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            throw;
        }
    };
    test("short A retains 800-ms action delay", [] {
        const auto result = run(1200, [](unsigned t) { return held(t, 20, 120, A); });
        expect(result, 1, 0, 0);
        require(result.a[0] == 930, "wrong short-A delivery time");
    });
    test("short B retains 800-ms action delay", [] {
        const auto result = run(1200, [](unsigned t) { return held(t, 20, 120, B); });
        expect(result, 0, 1, 0);
        require(result.b[0] == 930, "wrong short-B delivery time");
    });
    test("A fires once at three seconds while still held, no release click", [] {
        const auto result = run(9000, [](unsigned t) { return held(t, 20, 7000, A); }, [](Simulation& s) {
            if (s.elapsed == 3030) require(s.hal.isButtonPressed(ButtonId::A), "A not held at threshold");
        });
        expect(result, 0, 0, 1);
        require(result.long_a[0] == 3030, "long A did not fire at three debounced seconds");
    });
    test("release before threshold cannot fire during release debounce", [] {
        expect(run(4500, [](unsigned t) { return held(t, 20, 3028, A); }), 1, 0, 0);
    });
    test("A at threshold then released produces only long event", [] {
        expect(run(4500, [](unsigned t) { return held(t, 20, 3032, A); }), 0, 0, 1);
    });
    test("debounce rejects press and release chatter", [] {
        const auto result = run(1500, [](unsigned t) {
            return held(t, 20, 24, A) | held(t, 28, 32, A) | held(t, 36, 40, A) |
                   held(t, 50, 100, A) | held(t, 104, 108, A);
        });
        expect(result, 1, 0, 0);
        require(result.a[0] == 918, "debounce window did not restart");
    });
    test("holding B alone never produces long A", [] {
        expect(run(5000, [](unsigned t) { return held(t, 20, 4000, B); }), 0, 1, 0);
    });
    test("chord before three seconds cancels A timer until full release", [] {
        expect(run(6500, [](unsigned t) { return held(t, 20, 5000, A) | held(t, 2500, 4000, B); }), 1, 0, 0);
    });
    test("B pressed at A threshold blocks long event during chord debounce", [] {
        expect(run(6500, [](unsigned t) { return held(t, 20, 5000, A) | held(t, 3030, 4000, B); }), 1, 0, 0);
    });
    test("chord clears already pending long A and suppresses its short release", [] {
        expect(run(6000, [](unsigned t) { return held(t, 20, 4500, A) | held(t, 3500, 4000, B); }, {}, 0,
                   [](unsigned t) { return t >= 3600; }), 0, 0, 0);
    });
    test("B first then chord never rearms long A while A stays down", [] {
        expect(run(7000, [](unsigned t) { return held(t, 20, 500, B) | held(t, 100, 6000, A); }), 1, 0, 0);
    });
    test("unarmed chord keeps existing last-release B behavior", [] {
        expect(run(1500, [](unsigned t) { return held(t, 20, 200, A) | held(t, 40, 300, B); }), 0, 1, 0);
    });
    test("simultaneous unarmed chord release keeps existing B preference", [] {
        expect(run(1500, [](unsigned t) { return held(t, 20, 300, A | B); }), 0, 1, 0);
    });
    test("armed shutdown chord cancellation produces no button actions", [] {
        expect(run(6000, [](unsigned t) { return held(t, 20, 4000, A) | held(t, 20, 4200, B); }, [](Simulation& s) {
            if (s.elapsed == 2000) s.hal.suppressButtonActionsUntilRelease();
        }), 0, 0, 0);
    });
    test("suppression clears pending short actions and permits a later press", [] {
        expect(run(3000, [](unsigned t) { return held(t, 20, 120, A) | held(t, 1500, 1600, B); }, [](Simulation& s) {
            if (s.elapsed == 300) s.hal.suppressButtonActionsUntilRelease();
        }), 0, 1, 0);
    });
    test("suppression during A hold cancels long and short until release", [] {
        expect(run(7500, [](unsigned t) { return held(t, 20, 4000, A) | held(t, 6000, 6100, A); }, [](Simulation& s) {
            if (s.elapsed == 1000) s.hal.suppressButtonActionsUntilRelease();
        }), 1, 0, 0);
    });
    test("suppression during initial press debounce waits for full release", [] {
        expect(run(7500, [](unsigned t) { return held(t, 20, 4000, A) | held(t, 6000, 6100, A); }, [](Simulation& s) {
            if (s.elapsed == 24) s.hal.suppressButtonActionsUntilRelease();
        }), 1, 0, 0);
    });
    test("provisioning entry/exit suppression does not leak the release", [] {
        expect(run(10000, [](unsigned t) { return held(t, 20, 4000, A) | held(t, 5000, 8100, A); }, [](Simulation& s) {
            if (s.elapsed == 3032 || s.elapsed == 4500) s.hal.suppressButtonActionsUntilRelease();
        }), 0, 0, 2);
    });
    test("suppression clears an unconsumed long event", [] {
        expect(run(5500, [](unsigned t) { return held(t, 20, 4000, A); }, [](Simulation& s) {
            if (s.elapsed == 3200) s.hal.suppressButtonActionsUntilRelease();
        }, 0, [](unsigned t) { return t > 3200; }), 0, 0, 0);
    });
    test("long event remains consumable after release and counts as interaction", [] {
        const auto result = run(4500, [](unsigned t) { return held(t, 20, 3200, A); }, [](Simulation& s) {
            if (s.elapsed == 3500) require(s.hal.isButtonInteractionActive(), "pending long event lost");
            if (s.elapsed == 4002) require(!s.hal.isButtonInteractionActive(), "consumed event still active");
        }, 0, [](unsigned t) { return t >= 4000; });
        expect(result, 0, 0, 1);
    });
    test("shutdown cancels queued actions and disables long press", [] {
        expect(run(6000, [](unsigned t) { return held(t, 20, 120, B) | held(t, 200, 5000, A); }, [](Simulation& s) {
            if (s.elapsed == 300) s.hal.beginShutdown();
            if (s.elapsed == 5500) require(s.hal.isShutdownInProgress(), "shutdown flag missing");
        }), 0, 0, 0);
    });
    test("shutdown clears a pending long event", [] {
        expect(run(5500, [](unsigned t) { return held(t, 20, 4000, A); }, [](Simulation& s) {
            if (s.elapsed == 3200) s.hal.beginShutdown();
        }, 0, [](unsigned t) { return t > 3200; }), 0, 0, 0);
    });
    test("short deadline works across tick rollover", [] {
        expect(run(1500, [](unsigned t) { return held(t, 20, 120, A); }, {}, UINT32_MAX - 500), 1, 0, 0);
    });
    test("long threshold works across tick rollover", [] {
        const auto result = run(6000, [](unsigned t) { return held(t, 20, 4200, A); }, {}, UINT32_MAX - 1000);
        expect(result, 0, 0, 1);
        require(result.long_a[0] == 3030, "wraparound changed long threshold");
    });
    test("A already held when task starts is recognized", [] {
        const auto result = run(5000, [](unsigned t) { return held(t, 0, 4000, A); });
        expect(result, 0, 0, 1);
        require(result.long_a[0] == 3000, "startup hold timer wrong");
    });
    test("initial chord cannot trigger long A after B release", [] {
        expect(run(6000, [](unsigned t) { return held(t, 0, 4500, A) | held(t, 0, 500, B); }), 1, 0, 0);
    });
    std::cout << passed << " production button tests passed\n";
}
