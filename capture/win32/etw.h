#pragma once

// Starts a real-time ETW session on the Kernel-Process provider and spawns a
// dedicated background thread that consumes process start/exit events as
// they happen (see project notes on why this needs its own thread).
// Requires the program to be running as Administrator - kernel-level ETW
// providers cannot be enabled otherwise.
bool etw_init();

// Stops the ETW session, which unblocks the consumer thread's ProcessTrace
// call, then waits for that thread to finish cleanly.
void etw_shutdown();
