#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "prakiva_component.h"

namespace prakiva
{

    // Fixed capacities keep registry memory use predictable on embedded targets.
    constexpr size_t MAX_REGISTERED_COMPONENT_FACTORIES = 16;
    constexpr size_t MAX_COMPONENT_INSTANCES = 24;

    using ComponentFactory = IComponent *(*)();

    enum class RegistryError : uint8_t
    {
        None = 0,
        InvalidArgument,
        FactoryCapacityReached,
        InstanceCapacityReached,
        DuplicateDriverType,
        DuplicateComponentId,
        FactoryNotFound,
        FactoryReturnedNull
    };

    class ComponentRegistry
    {
    public:
        ComponentRegistry() = default;
        ~ComponentRegistry();

        // The registry owns all instances it creates. Copying is prohibited.
        ComponentRegistry(const ComponentRegistry &) = delete;
        ComponentRegistry &operator=(const ComponentRegistry &) = delete;

        bool registerFactory(const char *driverType, ComponentFactory factory);

        // Creates and stores one instance. Component IDs must be unique.
        // The returned pointer is owned by the registry and remains valid until
        // the registry is destroyed.
        IComponent *create(const char *componentId, const char *driverType);

        // Returns a non-owning pointer, or nullptr when the ID is unknown.
        IComponent *find(const char *componentId) const;
        bool hasFactory(const char *driverType) const;
        bool hasComponent(const char *componentId) const;
        const char *componentIdAt(size_t index) const;

        // Initialises every instance that has not yet been initialised.
        // Returns false if one or more components fail to initialise.
        bool beginAll();
        // Initializes one instance by ID. Used by the product layer after it
        // has resolved any declared inter-component dependencies.
        bool beginComponent(const char *componentId);

        ComponentStatus getStatus(const char *componentId) const;

        size_t factoryCount() const;
        size_t instanceCount() const;
        RegistryError lastError() const;
        void clearError();

    private:
        struct FactoryEntry
        {
            String driverType;
            ComponentFactory factory = nullptr;
        };

        struct InstanceEntry
        {
            String componentId;
            IComponent *component = nullptr;
        };

        FactoryEntry _factories[MAX_REGISTERED_COMPONENT_FACTORIES];
        InstanceEntry _instances[MAX_COMPONENT_INSTANCES];
        size_t _factoryCount = 0;
        size_t _instanceCount = 0;
        RegistryError _lastError = RegistryError::None;

        ComponentFactory findFactory(const char *driverType) const;
        void setError(RegistryError error);
    };

} // namespace prakiva
