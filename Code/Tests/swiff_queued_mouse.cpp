// [TEMP] [CHOSEN, not recovered] Content-free check of the queued menu-mouse rule
// (External/gameswf/SwiffPlayer/QueuedMouse.h). It models the two rates that meet in the queue:
// the loading screen queues the pointer state once per 30 Hz input tick
// (BetweenRoundsLogic::handleInput, every tick of a frame the same state) and the player hands
// the movie one queued sample per frame (SwiffPlayer::update). No movie, player or engine runs.
#include "../BF2/External/gameswf/SwiffPlayer/QueuedMouse.h"
#include <cstddef>
#include <cstdio>
#include <vector>

namespace {
unsigned checks = 0, failures = 0;
void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", message); }
}

struct Sample { int x, y, buttons; };
bool operator==(const Sample& a, const Sample& b) { return a.x == b.x && a.y == b.y && a.buttons == b.buttons; }
bool operator!=(const Sample& a, const Sample& b) { return !(a == b); }

// The screen's frame as the engine runs it: the player takes one sample, then the menu queues
// the current state once for every input tick the frame spans.
struct Screen
{
    explicit Screen(bool withRule) : rule(withRule) {}
    bool rule;
    std::vector<Sample> queue;
    std::vector<Sample> delivered;
    std::vector<int> deliveredAtFrame;
    int frames = 0;
    std::size_t deepest = 0;

    void frame(const Sample& state, int ticks)
    {
        if (queue.size() > deepest)
            deepest = queue.size();
        if (rule)
            dice::hfe::ui::dropRepeatedMouseSamples(queue);
        if (!queue.empty())
        {
            delivered.push_back(queue.front());
            deliveredAtFrame.push_back(frames);
            queue.erase(queue.begin());
        }
        for (int i = 0; i < ticks; ++i)
            queue.push_back(state);
        ++frames;
    }
    // The frame, counted from the first, in which the movie first saw `state`; -1 if never.
    int firstSeen(const Sample& state) const
    {
        for (std::size_t i = 0; i < delivered.size(); ++i)
            if (delivered[i] == state)
                return deliveredAtFrame[i];
        return -1;
    }
};

// Input ticks (30 Hz) that fall into frame number `frame` at `fps` frames a second.
int ticksInFrame(int frame, double fps)
{
    return static_cast<int>((frame + 1) * 30.0 / fps) - static_cast<int>(frame * 30.0 / fps);
}

// The states the movie sees, without the repeats: what its button and rollover logic reacts to.
std::vector<Sample> distinct(const std::vector<Sample>& samples)
{
    std::vector<Sample> out;
    for (const Sample& s : samples)
        if (out.empty() || out.back() != s)
            out.push_back(s);
    return out;
}

std::vector<Sample> afterRule(std::vector<Sample> queue)
{
    dice::hfe::ui::dropRepeatedMouseSamples(queue);
    return queue;
}
}

int main()
{
    const Sample rest = {400, 300, 0}, over = {2200, 1555, 0}, down = {2200, 1555, 1};

    // The rule itself.
    check(afterRule({}).empty(), "an empty queue stays empty");
    check(afterRule({rest}) == std::vector<Sample>({rest}), "a single sample stays");
    check(afterRule({rest, rest, rest, over}) == std::vector<Sample>({rest, over}),
          "leading repeats collapse to the one before the change");
    check(afterRule({rest, over, over}) == std::vector<Sample>({rest, over, over}),
          "a front sample its successor does not repeat stays, with what follows it");
    check(afterRule({over, down, over}) == std::vector<Sample>({over, down, over}),
          "a press and its release are never merged");
    check(afterRule({over, {2200, 1555, 2}}).size() == 2 && afterRule({over, {2201, 1555, 0}}).size() == 2
              && afterRule({over, {2200, 1556, 0}}).size() == 2,
          "buttons, x and y each make a sample distinct");

    // The reported case: 17.2 frames a second, the pointer resting for 66 s after the level
    // loaded, then a move onto JOIN GAME and a press held for two frames.
    const double fps = 17.2;
    const int waitFrames = static_cast<int>(66.0 * fps);
    for (int withRule = 0; withRule <= 1; ++withRule)
    {
        Screen screen(withRule != 0);
        int f = 0;
        for (; f < waitFrames; ++f)
            screen.frame(rest, ticksInFrame(f, fps));
        const std::size_t backlog = screen.queue.size();
        const int pressFrame = f + 1;
        screen.frame(over, ticksInFrame(f, fps)); ++f;
        screen.frame(down, ticksInFrame(f, fps)); ++f;
        screen.frame(down, ticksInFrame(f, fps)); ++f;
        for (int i = 0; i < 60 * 30; ++i, ++f)
            screen.frame(over, ticksInFrame(f, fps));
        const int seen = screen.firstSeen(down);
        const double delay = seen < 0 ? -1.0 : (seen - pressFrame) / fps;
        std::printf("17.2 fps, %s: %zu sample(s) queued after 66 s, deepest %zu, press seen %.2f s later\n",
                    withRule ? "with the rule" : "as recovered", backlog, screen.deepest, delay);
        if (!withRule)
        {
            check(backlog > 800 && backlog < 900, "as recovered: about 850 samples wait after 66 s at 17.2 fps");
            check(delay > 45.0 && delay < 55.0, "as recovered: the press reaches the movie most of a minute late");
        }
        else
        {
            check(backlog <= 2 && screen.deepest <= 2, "with the rule the queue holds one frame's ticks at most");
            check(seen == pressFrame + 1, "with the rule the movie sees the press on the next frame");
        }
    }

    // The same states in the same order, with and without the rule: a pointer path with a press, a
    // drag and a release, at 17.2 frames a second, then both queues drained.
    {
        Screen recovered(false), ruled(true);
        std::vector<Sample> path;
        for (int i = 0; i < 40; ++i)
            path.push_back({100 + 10 * (i / 2), 200, 0});               // moving, sometimes resting a frame
        path.push_back({300, 200, 1});
        for (int i = 0; i < 6; ++i)
            path.push_back({300 + 5 * i, 200, 1});                      // dragging
        path.push_back({325, 200, 0});
        path.push_back({325, 200, 0});
        path.push_back({325, 200, 1});                                  // a second click in place
        path.push_back({325, 200, 0});
        int f = 0;
        for (const Sample& s : path)
        {
            const int ticks = ticksInFrame(f++, fps);
            recovered.frame(s, ticks);
            ruled.frame(s, ticks);
        }
        const Sample last = path.back();
        for (int i = 0; i < 400; ++i)
        {
            recovered.frame(last, 0);
            ruled.frame(last, 0);
        }
        check(recovered.queue.empty() && ruled.queue.empty(), "both queues drain");
        check(distinct(recovered.delivered) == distinct(ruled.delivered),
              "the movie sees the same pointer states and button changes in the same order");
        check(distinct(ruled.delivered) == distinct(path), "and they are the states the menu had");
        check(ruled.delivered.size() < recovered.delivered.size(), "only repeats were dropped");
    }

    // 60 frames a second: a frame holds one tick at most, so there is nothing to drop and the
    // deliveries are the recovered ones, sample for sample.
    {
        Screen recovered(false), ruled(true);
        for (int f = 0; f < 600; ++f)
        {
            const Sample s = {100 + f, 200, (f / 40) % 2};
            const int ticks = ticksInFrame(f, 60.0);
            recovered.frame(s, ticks);
            ruled.frame(s, ticks);
        }
        check(recovered.delivered == ruled.delivered && recovered.deliveredAtFrame == ruled.deliveredAtFrame,
              "at 60 fps the rule changes nothing");
        check(ruled.deepest <= 1, "at 60 fps the queue never holds more than one sample");
    }

    std::printf("queued menu mouse: %u/%u checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
