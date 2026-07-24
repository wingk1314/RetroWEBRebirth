#pragma once

#include "common.hpp"

namespace browservice {

// Fire-and-forget HTTP GET request in a detached thread.
// Logs warnings on failure. Thread-safe.
void sendHttpGetAsync(string url);

}
