#pragma once
#include "components/core/prakiva_component_registry.h"
namespace prakiva {
// Register compiled-in platform drivers. Product JSON determines instantiated components.
bool registerPlatformDrivers(ComponentRegistry &registry);
}
