#ifndef WHOLE_HOUSE_TIMER_H
#define WHOLE_HOUSE_TIMER_H

#include "loop_task.h"
#include "neato_serial.h"

// Early-return timer for whole-house clean. Armed by POST /api/clean-timer?action=arm after a
// house clean has started. Self-disarms in tick() -- regardless of frontend cooperation -- once
// the clean it was armed for is no longer the active one (a new clean started, from any caller:
// scheduler, another tab, a fresh manual start) or has ended on its own (natural completion,
// user stop/dock, error, pickup). On expiry of a still-active, still-same-session clean, sends
// the robot home via the same SetEvent dock path the Home button uses — never a bare Clean Stop
// (see neato_serial.cpp comment at the dock branch of clean()).
class WholeHouseTimer : public LoopTask {
public:
    explicit WholeHouseTimer(NeatoSerial& serial) : LoopTask(1000), neato(serial) { TaskRegistry::add(this); }

    void arm(unsigned long durationMinutes);
    void cancel();
    bool isArmed() const { return armed; }
    long remainingSec() const;

protected:
    void tick() override;

private:
    NeatoSerial& neato;
    bool armed = false;
    unsigned long startMs = 0;
    unsigned long durationMinutes = 0;
    // The clean session (see NeatoSerial::currentCleanGeneration()) this timer was armed for.
    // A mismatch at tick() time means a different clean has since started -- this timer no
    // longer belongs to anything running and must not touch it.
    unsigned long armedGeneration = 0;
};

#endif // WHOLE_HOUSE_TIMER_H
