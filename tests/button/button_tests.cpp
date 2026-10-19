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
    std::vector<unsigned> triple_b;
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
    std::function<bool(unsigned)> poll_triple = [](unsigned) { return true; };
};
Simulation* current = nullptr;
std::uint8_t held(unsigned time, unsigned begin, unsigned end, std::uint8_t buttons)
{
    return time >= begin && time < end ? buttons : 0;
}
Counts run(unsigned duration, std::function<std::uint8_t(unsigned)> pins,
           std::function<void(Simulation&)> observe = {}, TickType_t base = 0,
           std::function<bool(unsigned)> poll_long = [](unsigned) { return true; },
           std::function<bool(unsigned)> poll_triple = [](unsigned) { return true; })
{
    Simulation simulation;
    simulation.duration = duration;
    simulation.pins = pins;
    simulation.observe = observe;
    simulation.base = base;
    simulation.poll_long = poll_long;
    simulation.poll_triple = poll_triple;
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
void expect(const Counts& result, unsigned a, unsigned b, unsigned long_a, unsigned triple_b = 0)
{
    require(result.a.size() == a, "unexpected short-A count: " + std::to_string(result.a.size()));
    require(result.b.size() == b, "unexpected short-B count: " + std::to_string(result.b.size()));
    require(result.long_a.size() == long_a, "unexpected long-A count: " + std::to_string(result.long_a.size()));
    require(result.triple_b.size() == triple_b, "unexpected triple-B count: " + std::to_string(result.triple_b.size()));
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
    if (s.poll_triple(s.elapsed) && s.hal.wasButtonBTripleClicked()) s.counts.triple_b.push_back(s.elapsed);
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
    test("three short B releases produce exactly one triple with no short B", [] {
        const auto result = run(2200, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B);
        });
        expect(result, 0, 0, 0, 1);
        require(result.triple_b[0] == 410, "triple did not fire on third debounced release");
    });
    test("double B retains coalesced single fallback after final release", [] {
        const auto result = run(1600, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B);
        });
        expect(result, 0, 1, 0);
        require(result.b[0] == 1050, "double B fallback deadline wrong");
    });
    test("triple accepts exact 400-ms release gaps and 800-ms total", [] {
        const auto result = run(2200, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 420, 480, B) | held(t, 820, 880, B);
        });
        expect(result, 0, 0, 0, 1);
        require(result.triple_b[0] == 890, "inclusive triple boundary wrong");
    });
    test("release gap over 400 ms rejects triple", [] {
        expect(run(2200, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 422, 482, B) | held(t, 580, 640, B);
        }), 0, 1, 0);
    });
    test("three independent slow B clicks remain three singles", [] {
        expect(run(3600, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 1200, 1260, B) | held(t, 2400, 2460, B);
        }), 0, 3, 0);
    });
    test("third B held past 400 ms is not a triple", [] {
        expect(run(2400, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 300, 800, B);
        }), 0, 1, 0);
    });
    test("long first B is not counted toward following two clicks", [] {
        expect(run(2400, [](unsigned t) {
            return held(t, 20, 520, B) | held(t, 600, 660, B) | held(t, 740, 800, B);
        }), 0, 1, 0);
    });
    test("held B cannot auto-repeat into a triple", [] {
        expect(run(9000, [](unsigned t) { return held(t, 20, 7000, B); }), 0, 1, 0);
    });
    test("B held at startup does not seed a triple", [] {
        expect(run(1600, [](unsigned t) {
            return held(t, 0, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B);
        }), 0, 1, 0);
    });
    test("B bounce cannot create extra clicks or disturb valid triple", [] {
        expect(run(2000, [](unsigned t) {
            return held(t, 20, 24, B) | held(t, 28, 32, B) | held(t, 40, 80, B) |
                   held(t, 84, 88, B) | held(t, 180, 240, B) | held(t, 244, 248, B) |
                   held(t, 340, 400, B);
        }), 0, 0, 0, 1);
    });
    test("four rapid clicks produce one triple and no trailing single", [] {
        expect(run(2200, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B) |
                   held(t, 500, 560, B);
        }), 0, 0, 0, 1);
    });
    test("six rapid clicks stay one train until a quiet interval", [] {
        expect(run(2500, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B) |
                   held(t, 500, 560, B) | held(t, 660, 720, B) | held(t, 820, 880, B);
        }), 0, 0, 0, 1);
    });
    test("single after 400-ms quiet interval works normally", [] {
        const auto result = run(2200, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B) |
                   held(t, 900, 960, B);
        });
        expect(result, 0, 1, 0, 1);
        require(result.b[0] == 1770, "post-triple single delay changed");
    });
    test("two separated triple gestures can enter and leave clock", [] {
        expect(run(2500, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B) |
                   held(t, 900, 960, B) | held(t, 1060, 1120, B) | held(t, 1220, 1280, B);
        }), 0, 0, 0, 2);
    });
    test("application mode suppression keeps fourth-click guard", [] {
        expect(run(2400, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B) |
                   held(t, 500, 560, B);
        }, [](Simulation& s) {
            if (s.elapsed == 412) s.hal.suppressButtonActionsUntilRelease();
        }), 0, 0, 0, 1);
    });
    test("suppression clears a partially counted triple", [] {
        expect(run(2200, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B);
        }, [](Simulation& s) {
            if (s.elapsed == 260) s.hal.suppressButtonActionsUntilRelease();
        }), 0, 1, 0);
    });
    test("suppression during third hold prevents both triple and release single", [] {
        expect(run(2200, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B);
        }, [](Simulation& s) {
            if (s.elapsed == 380) s.hal.suppressButtonActionsUntilRelease();
        }), 0, 0, 0);
    });
    test("suppression clears an unconsumed triple event", [] {
        expect(run(2200, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B);
        }, [](Simulation& s) {
            if (s.elapsed == 500) s.hal.suppressButtonActionsUntilRelease();
        }, 0, [](unsigned) { return true; }, [](unsigned t) { return t >= 600; }), 0, 0, 0);
    });
    test("A press interrupts partial B triple counting", [] {
        expect(run(2200, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 100, 140, A) | held(t, 180, 240, B) |
                   held(t, 340, 400, B);
        }), 0, 1, 0);
    });
    test("B chord release never counts as a triple click", [] {
        expect(run(2200, [](unsigned t) {
            return held(t, 20, 80, A | B) | held(t, 180, 240, B) | held(t, 340, 400, B);
        }), 0, 1, 0);
    });
    test("unarmed chord interrupts B candidate while retaining B fallback", [] {
        expect(run(2200, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, A | B);
        }), 0, 1, 0);
    });
    test("armed shutdown chord cancels partial B triple and all fallbacks", [] {
        expect(run(4800, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 3000, A | B);
        }, [](Simulation& s) {
            if (s.elapsed == 2000) s.hal.suppressButtonActionsUntilRelease();
        }), 0, 0, 0);
    });
    test("chord clears unconsumed triple before mode transition", [] {
        expect(run(2500, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B) |
                   held(t, 500, 800, A | B);
        }, {}, 0, [](unsigned) { return true; }, [](unsigned t) { return t >= 900; }), 0, 1, 0);
    });
    test("shutdown clears an unconsumed triple", [] {
        expect(run(2200, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B);
        }, [](Simulation& s) {
            if (s.elapsed == 500) s.hal.beginShutdown();
        }, 0, [](unsigned) { return true; }, [](unsigned t) { return t >= 600; }), 0, 0, 0);
    });
    test("shutdown during third press blocks triple and release action", [] {
        expect(run(2200, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B);
        }, [](Simulation& s) {
            if (s.elapsed == 380) s.hal.beginShutdown();
        }), 0, 0, 0);
    });
    test("pending triple is an interaction and survives delayed consumer", [] {
        const auto result = run(1800, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B);
        }, [](Simulation& s) {
            if (s.elapsed == 1000) require(s.hal.isButtonInteractionActive(), "pending triple lost");
            if (s.elapsed == 1202) require(!s.hal.isButtonInteractionActive(), "consumed triple still active");
        }, 0, [](unsigned) { return true; }, [](unsigned t) { return t >= 1200; });
        expect(result, 0, 0, 0, 1);
        require(result.triple_b[0] == 1200, "delayed triple consumption wrong");
    });
    test("fourth click cannot overwrite unconsumed triple", [] {
        expect(run(2200, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B) |
                   held(t, 500, 560, B);
        }, {}, 0, [](unsigned) { return true; }, [](unsigned t) { return t >= 1200; }), 0, 0, 0, 1);
    });
    test("later B cannot overwrite unconsumed triple after cooldown", [] {
        expect(run(2600, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B) |
                   held(t, 1000, 1060, B);
        }, {}, 0, [](unsigned) { return true; }, [](unsigned t) { return t >= 2000; }), 0, 0, 0, 1);
    });
    test("triple window and cooldown work across tick rollover", [] {
        expect(run(2500, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B) |
                   held(t, 500, 560, B) | held(t, 1100, 1160, B) | held(t, 1260, 1320, B) |
                   held(t, 1420, 1480, B);
        }, {}, UINT32_MAX - 250), 0, 0, 0, 2);
    });
    test("B fallback deadline works across tick rollover", [] {
        const auto result = run(1500, [](unsigned t) { return held(t, 20, 120, B); }, {}, UINT32_MAX - 500);
        expect(result, 0, 1, 0);
        require(result.b[0] == 930, "B rollover delay changed");
    });
    test("first B hold exactly 400 ms can begin a triple", [] {
        expect(run(2200, [](unsigned t) {
            return held(t, 20, 420, B) | held(t, 500, 560, B) | held(t, 640, 700, B);
        }), 0, 0, 0, 1);
    });
    test("first B hold longer than 400 ms cannot begin a triple", [] {
        expect(run(2200, [](unsigned t) {
            return held(t, 20, 422, B) | held(t, 500, 560, B) | held(t, 640, 700, B);
        }), 0, 1, 0);
    });
    test("400-ms quiet boundary re-arms the next press", [] {
        expect(run(2200, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B) |
                   held(t, 810, 870, B);
        }), 0, 1, 0, 1);
    });
    test("press before quiet boundary remains absorbed through a long hold", [] {
        expect(run(3000, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B) |
                   held(t, 808, 1500, B);
        }), 0, 0, 0, 1);
    });
    test("A short still works immediately after triple", [] {
        expect(run(2200, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B) |
                   held(t, 500, 600, A);
        }), 1, 0, 0, 1);
    });
    test("A three-second hold still works immediately after triple", [] {
        const auto result = run(5200, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B) |
                   held(t, 500, 4000, A);
        });
        expect(result, 0, 0, 1, 1);
        require(result.long_a[0] == 3510, "post-triple long A threshold changed");
    });
    std::cout << passed << " production button tests passed\n";
}
