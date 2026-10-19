#include <atomic>
#include <array>
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
    unsigned idle_waits = 0;
    unsigned poll_waits = 0;
    unsigned main_notifications = 0;
    bool wake_ready = false;
    bool ended_idle = false;
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
    std::array<gpio_isr_t, 2> handlers{};
    std::array<void*, 2> handler_arguments{};
    std::array<bool, 2> interrupts_enabled{};
    unsigned notifications = 0;
    unsigned last_observation = UINT32_MAX;
    bool before_wait_called = false;
    std::function<void(Simulation&)> before_wait;
    std::function<void(Simulation&, gpio_num_t)> before_enable;
    std::function<std::uint8_t(unsigned)> pins;
    std::function<void(Simulation&)> observe;
    std::function<bool(unsigned)> poll_long = [](unsigned) { return true; };
    std::function<bool(unsigned)> poll_triple = [](unsigned) { return true; };
};
Simulation* current = nullptr;
bool simulate_idle_mode = false;
int fail_handler_pin = -1;
int fail_enable_pin = -1;
bool existing_isr_service = false;
std::function<void(Simulation&)> configure_simulation;
TaskHandle_t inputHandle() { return reinterpret_cast<TaskHandle_t>(1); }
TaskHandle_t mainHandle() { return reinterpret_cast<TaskHandle_t>(2); }
unsigned pinIndex(gpio_num_t pin) { return pin == GPIO_NUM_4 ? 0 : 1; }
void deliverInterrupts(Simulation& s)
{
    for (unsigned i = 0; i < 2; ++i) {
        if (s.interrupts_enabled[i] && s.handlers[i] && (s.raw & (1U << i))) {
            s.handlers[i](s.handler_arguments[i]);
        }
    }
}
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
    if (configure_simulation) configure_simulation(simulation);
    current = &simulation;
    require(simulation.hal.initButtonInput(), "button task creation failed");
    require(simulation.task != nullptr, "no task created");
    simulation.hal.setButtonWakeTask(mainHandle());
    simulation.hal.setButtonIdleMode(simulate_idle_mode);
    try {
        simulation.task(simulation.argument);
    } catch (const StopSimulation&) {
    }
    require(simulation.critical_depth == 0, "critical section left locked");
    simulation.counts.wake_ready = simulation.hal.buttonWakeReady();
    simulation.counts.ended_idle = simulation.hal.isButtonIdle();
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
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char*, unsigned int, void* argument, int, TaskHandle_t* handle, int)
{
    current->task = task;
    current->argument = argument;
    if (handle) *handle = inputHandle();
    return pdPASS;
}
esp_err_t gpio_install_isr_service(int) { return existing_isr_service ? ESP_ERR_INVALID_STATE : ESP_OK; }
esp_err_t gpio_set_intr_type(gpio_num_t, gpio_int_type_t) { return ESP_OK; }
esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t handler, void* argument)
{
    if (pin == fail_handler_pin) return ESP_FAIL;
    const auto index = pinIndex(pin);
    current->handlers[index] = handler;
    current->handler_arguments[index] = argument;
    current->interrupts_enabled[index] = true;
    deliverInterrupts(*current);
    return ESP_OK;
}
esp_err_t gpio_isr_handler_remove(gpio_num_t pin)
{
    current->handlers[pinIndex(pin)] = nullptr;
    current->interrupts_enabled[pinIndex(pin)] = false;
    return ESP_OK;
}
esp_err_t gpio_intr_enable(gpio_num_t pin)
{
    if (pin == fail_enable_pin) return ESP_FAIL;
    if (current->before_enable) current->before_enable(*current, pin);
    current->interrupts_enabled[pinIndex(pin)] = true;
    deliverInterrupts(*current);
    return ESP_OK;
}
esp_err_t gpio_intr_disable(gpio_num_t pin)
{
    current->interrupts_enabled[pinIndex(pin)] = false;
    return ESP_OK;
}
void xTaskNotifyGive(TaskHandle_t handle)
{
    if (handle == inputHandle()) ++current->notifications;
    else if (handle == mainHandle()) ++current->counts.main_notifications;
}
void vTaskNotifyGiveFromISR(TaskHandle_t handle, BaseType_t* woken)
{
    xTaskNotifyGive(handle);
    *woken = pdTRUE;
}
void observeCurrent()
{
    auto& s = *current;
    require(s.critical_depth == 0, "task delays inside critical section");
    if (s.last_observation == s.elapsed) return;
    s.last_observation = s.elapsed;
    if (s.observe) s.observe(s);
    if (s.hal.wasButtonAClicked()) s.counts.a.push_back(s.elapsed);
    if (s.hal.wasButtonBClicked()) s.counts.b.push_back(s.elapsed);
    if (s.poll_long(s.elapsed) && s.hal.wasButtonALongPressed()) s.counts.long_a.push_back(s.elapsed);
    if (s.poll_triple(s.elapsed) && s.hal.wasButtonBTripleClicked()) s.counts.triple_b.push_back(s.elapsed);
    if (s.elapsed >= s.duration) throw StopSimulation{};
}
void advanceTime(TickType_t ticks)
{
    auto& s = *current;
    s.elapsed += ticks;
    s.raw = s.pins(s.elapsed);
    deliverInterrupts(s);
}
void vTaskDelay(TickType_t ticks)
{
    require(ticks > 0, "zero tick polling delay");
    ++current->counts.poll_waits;
    observeCurrent();
    advanceTime(ticks);
}
std::uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout)
{
    auto& s = *current;
    require(clear == pdTRUE && timeout == portMAX_DELAY, "unexpected idle wait");
    ++s.counts.idle_waits;
    if (s.before_wait && !s.before_wait_called && s.notifications == 0) {
        s.before_wait_called = true;
        s.before_wait(s);
    }
    while (s.notifications == 0) {
        observeCurrent();
        if (s.notifications == 0) advanceTime(2);
    }
    const auto result = s.notifications;
    s.notifications = 0;
    return result;
}

int main()
{
    unsigned passed = 0;
    auto test = [&](const char* name, const std::function<void()>& body) {
        try {
            for (const bool idle : {false, true}) {
                simulate_idle_mode = idle;
                fail_handler_pin = -1;
                fail_enable_pin = -1;
                existing_isr_service = false;
                configure_simulation = {};
                body();
            }
            ++passed;
            std::cout << "PASS " << name << " (polling + clock idle)\n";
        }
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
    test("clock idle blocks without periodic input polling; normal mode stays fast", [] {
        const auto result = run(10000, [](unsigned) { return 0; });
        require(result.wake_ready, "interrupts not ready");
        if (simulate_idle_mode) {
            require(result.idle_waits > 0 && result.idle_waits <= 2, "idle did not block indefinitely");
            require(result.poll_waits == 0 && result.ended_idle, "clock idle kept polling");
        } else {
            require(result.idle_waits == 0 && result.poll_waits >= 5000, "normal polling changed");
            require(!result.ended_idle, "normal mode advertised sleep eligibility");
        }
    });
    test("press while idle clears sleep eligibility before main notification", [] {
        const auto result = run(1400, [](unsigned t) { return held(t, 100, 200, B); }, [](Simulation& s) {
            if (s.elapsed == 100) {
                require(!s.hal.isButtonIdle(), "ISR did not clear sticky idle state");
                require(s.counts.main_notifications > 0, "main task not notified by first press");
                require(!s.interrupts_enabled[0] && !s.interrupts_enabled[1], "held-low IRQ storm possible");
            }
        });
        expect(result, 0, 1, 0);
    });
    test("first press arriving between final pin sample and wait is not lost", [] {
        simulate_idle_mode = true;
        configure_simulation = [](Simulation& s) {
            s.before_wait = [](Simulation& active) {
                active.raw = B;
                deliverInterrupts(active);
            };
        };
        const auto result = run(1600, [](unsigned t) {
            return held(t, 2, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B);
        });
        expect(result, 0, 0, 0, 1);
        require(result.triple_b[0] == 410, "first wake press did not seed triple");
    });
    test("press already low during interrupt rearm is not lost", [] {
        simulate_idle_mode = true;
        configure_simulation = [](Simulation& s) {
            s.before_enable = [](Simulation& active, gpio_num_t pin) {
                if (active.elapsed == 0 && pin == GPIO_NUM_3) active.raw = B;
            };
        };
        expect(run(1600, [](unsigned t) {
            return held(t, 2, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B);
        }), 0, 0, 0, 1);
    });
    test("completed triple returns to interrupt wait only after quiet guard", [] {
        simulate_idle_mode = true;
        expect(run(2500, [](unsigned t) {
            return held(t, 20, 80, B) | held(t, 180, 240, B) | held(t, 340, 400, B) |
                   held(t, 1200, 1260, B) | held(t, 1360, 1420, B) | held(t, 1520, 1580, B);
        }, [](Simulation& s) {
            if (s.elapsed == 600) require(!s.hal.isButtonIdle(), "post-triple guard slept early");
            if (s.elapsed == 1000) require(s.hal.isButtonIdle(), "input failed to rearm after guard");
        }), 0, 0, 0, 2);
    });
    test("mode exit wakes blocked input and restores normal polling", [] {
        simulate_idle_mode = true;
        const auto result = run(1400, [](unsigned t) { return held(t, 200, 300, B); }, [](Simulation& s) {
            if (s.elapsed == 100) s.hal.setButtonIdleMode(false);
        });
        expect(result, 0, 1, 0);
        require(result.poll_waits >= 600 && !result.ended_idle, "input remained blocked after clock exit");
    });
    test("suppression and shutdown wake a blocked input task", [] {
        simulate_idle_mode = true;
        const auto result = run(1500, [](unsigned t) { return held(t, 200, 300, B); }, [](Simulation& s) {
            if (s.elapsed == 100) {
                s.hal.suppressButtonActionsUntilRelease();
                require(!s.hal.isButtonIdle(), "suppression retained stale idle state");
            }
            if (s.elapsed == 400) s.hal.beginShutdown();
        });
        expect(result, 0, 0, 0);
        require(!result.ended_idle, "shutdown became sleep eligible");
    });
    test("existing GPIO ISR service can be shared", [] {
        existing_isr_service = true;
        const auto result = run(1200, [](unsigned t) { return held(t, 20, 120, B); });
        expect(result, 0, 1, 0);
        require(result.wake_ready, "existing service incorrectly rejected");
    });
    test("missing A interrupt falls back to polling without claiming wake readiness", [] {
        fail_handler_pin = GPIO_NUM_4;
        const auto result = run(1200, [](unsigned t) { return held(t, 20, 120, B); });
        expect(result, 0, 1, 0);
        require(!result.wake_ready && result.idle_waits == 0, "missing IRQ enabled sleeping");
    });
    test("missing B interrupt removes A handler and keeps gesture polling", [] {
        fail_handler_pin = GPIO_NUM_3;
        const auto result = run(1200, [](unsigned t) { return held(t, 20, 120, A); }, [](Simulation& s) {
            require(s.handlers[0] == nullptr, "partial interrupt setup not cleaned up");
        });
        expect(result, 1, 0, 0);
        require(!result.wake_ready && result.idle_waits == 0, "partial IRQ setup enabled sleeping");
    });
    test("interrupt rearm failure fails closed and restores polling", [] {
        simulate_idle_mode = true;
        fail_enable_pin = GPIO_NUM_3;
        const auto result = run(1200, [](unsigned t) { return held(t, 20, 120, B); });
        expect(result, 0, 1, 0);
        require(!result.wake_ready && result.idle_waits == 0, "rearm failure enabled sleeping");
        require(result.main_notifications > 0, "rearm failure did not wake main");
    });
    std::cout << passed << " production button tests passed\n";
}
