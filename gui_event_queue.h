// This file is used to deliver messages and events from the backend to the frontend GUI thread.
// It is a thread-safe queue for outgoing events (chat messages, file transfer status, etc).
#pragma once
#include <string>
#include "blocking_queue.h"

// Outgoing event queue for GUI delivery
using GuiEventQueue = BlockingQueue<std::string>;
