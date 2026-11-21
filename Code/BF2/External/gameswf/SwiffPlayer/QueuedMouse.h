// [CHOSEN, not recovered] Queued menu-mouse samples on frames slower than the input tick.
// SwiffPlayer::update hands the movie one queued sample per frame. The loading and end-of-round
// screen (BetweenRoundsLogic::handleInput) queues the pointer state once per 30 Hz input tick, so
// a frame that spans more than one tick queues the same state more than once and the queue grows
// by the difference on every frame: at 17 frames a second the movie falls 0.7 s further behind
// the pointer each second, and a click on JOIN GAME a minute after the level loaded reaches the
// button most of a minute later. The queueing and the one-per-frame hand-over are recovered
// behaviour; this rule is added to them on purpose.
// A sample that the next queued one repeats (same position, same buttons) tells the movie nothing
// the next one does not, so it is dropped before the frame's sample is taken. The movie still
// receives every distinct pointer state and every button change, in the queued order and one per
// frame; only the repeats and the delay they caused are gone.
#ifndef BF2_SWIFFPLAYER_QUEUEDMOUSE_H
#define BF2_SWIFFPLAYER_QUEUEDMOUSE_H

#include <cstddef>

namespace dice { namespace hfe { namespace ui {

// Removes from the front of queue every sample that its successor repeats, leaving the last of
// the leading run of identical samples in front. The element type has x, y and buttons.
template <class Queue>
inline void dropRepeatedMouseSamples(Queue& queue)
{
    std::size_t repeats = 0;
    while (repeats + 1 < queue.size()
           && queue[repeats].x == queue[repeats + 1].x
           && queue[repeats].y == queue[repeats + 1].y
           && queue[repeats].buttons == queue[repeats + 1].buttons)
        ++repeats;
    if (repeats)
        queue.erase(queue.begin(), queue.begin() + static_cast<typename Queue::difference_type>(repeats));
}

} } }
#endif
