#pragma once

namespace prakiva
{
    // The app editor document contains nested controls, policies, curves and profiles.
    // Keep one bounded limit shared by protocol and persistent-storage parsing.
    constexpr unsigned int JSON_NESTING_LIMIT = 30;
}
