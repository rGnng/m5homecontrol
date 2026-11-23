#include "dock_input.h"
#include <cassert>
#include <iostream>

int main() {
    DockInput dock;
    dock.begin(false, 0);
    assert(!dock.update(true, 100));
    assert(!dock.update(true, 149));
    assert(dock.update(true, 150));
    assert(dock.docked() && dock.dockingEvents() == 1);
    assert(!dock.update(true, 5000));  // A held short never retriggers.
    assert(!dock.update(false, 5010));
    assert(!dock.update(true, 5020));  // Brief release bounces do not re-arm.
    assert(!dock.update(true, 5100));
    assert(dock.docked() && dock.undockingEvents() == 0);
    assert(!dock.update(false, 5200));
    assert(!dock.update(false, 5250));
    assert(!dock.docked() && dock.undockingEvents() == 1);
    assert(!dock.update(true, 5300));
    assert(dock.update(true, 5350));
    assert(dock.dockingEvents() == 2);

    dock.begin(true, 0);  // Booting with a short establishes the initial state.
    assert(!dock.update(true, 1000));
    assert(dock.docked() && dock.dockingEvents() == 0);
    dock.begin(false, 0xffffffe0u);
    assert(!dock.update(true, 0xfffffff0u));
    assert(!dock.update(true, 0x21u));
    assert(dock.update(true, 0x22u));  // Debounce survives millis() rollover.
    dock.begin(false, 0);
    assert(!dock.update(true, 10));
    assert(!dock.update(false, 40));
    assert(!dock.update(false, 100));
    assert(!dock.docked() && dock.dockingEvents() == 0);
    std::cout << "PASS: docking debounce, held contact, release/re-arm, boot baseline and rollover\n";
}
