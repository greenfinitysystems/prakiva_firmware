#include "components/core/prakiva_component_registry.h"

namespace prakiva
{

    ComponentRegistry::~ComponentRegistry()
    {
        for (size_t i = 0; i < _instanceCount; ++i)
        {
            delete _instances[i].component;
            _instances[i].component = nullptr;
        }
        _instanceCount = 0;
    }

    bool ComponentRegistry::registerFactory(const char *driverType, ComponentFactory factory)
    {
        if (driverType == nullptr || driverType[0] == '\0' || factory == nullptr)
        {
            setError(RegistryError::InvalidArgument);
            return false;
        }

        if (findFactory(driverType) != nullptr)
        {
            setError(RegistryError::DuplicateDriverType);
            return false;
        }

        if (_factoryCount >= MAX_REGISTERED_COMPONENT_FACTORIES)
        {
            setError(RegistryError::FactoryCapacityReached);
            return false;
        }

        _factories[_factoryCount].driverType = driverType;
        _factories[_factoryCount].factory = factory;
        ++_factoryCount;
        clearError();
        return true;
    }

    IComponent *ComponentRegistry::create(const char *componentId, const char *driverType)
    {
        if (componentId == nullptr || componentId[0] == '\0' ||
            driverType == nullptr || driverType[0] == '\0')
        {
            setError(RegistryError::InvalidArgument);
            return nullptr;
        }

        if (find(componentId) != nullptr)
        {
            setError(RegistryError::DuplicateComponentId);
            return nullptr;
        }

        if (_instanceCount >= MAX_COMPONENT_INSTANCES)
        {
            setError(RegistryError::InstanceCapacityReached);
            return nullptr;
        }

        ComponentFactory factory = findFactory(driverType);
        if (factory == nullptr)
        {
            setError(RegistryError::FactoryNotFound);
            return nullptr;
        }

        IComponent *component = factory();
        if (component == nullptr)
        {
            setError(RegistryError::FactoryReturnedNull);
            return nullptr;
        }

        _instances[_instanceCount].componentId = componentId;
        _instances[_instanceCount].component = component;
        ++_instanceCount;
        clearError();
        return component;
    }

    IComponent *ComponentRegistry::find(const char *componentId) const
    {
        if (componentId == nullptr || componentId[0] == '\0')
            return nullptr;

        for (size_t i = 0; i < _instanceCount; ++i)
        {
            if (_instances[i].componentId == componentId)
                return _instances[i].component;
        }
        return nullptr;
    }

    bool ComponentRegistry::hasFactory(const char *driverType) const
    {
        return findFactory(driverType) != nullptr;
    }

    bool ComponentRegistry::hasComponent(const char *componentId) const
    {
        return find(componentId) != nullptr;
    }

    const char *ComponentRegistry::componentIdAt(size_t index) const
    {
        if (index >= _instanceCount)
            return nullptr;
        return _instances[index].componentId.c_str();
    }

    bool ComponentRegistry::beginAll()
    {
        bool allReady = true;
        for (size_t i = 0; i < _instanceCount; ++i)
        {
            if (!beginComponent(_instances[i].componentId.c_str()))
                allReady = false;
        }
        clearError();
        return allReady;
    }

    bool ComponentRegistry::beginComponent(const char *componentId)
    {
        IComponent *component = find(componentId);
        if (component == nullptr)
        {
            setError(RegistryError::InvalidArgument);
            return false;
        }

        bool beginSucceeded = true;
        if (component->status() == ComponentStatus::NotInitialized)
            beginSucceeded = component->begin();

        const bool ready = beginSucceeded && component->status() == ComponentStatus::Ready;
        clearError();
        return ready;
    }

    ComponentStatus ComponentRegistry::getStatus(const char *componentId) const
    {
        IComponent *component = find(componentId);
        return component != nullptr ? component->status() : ComponentStatus::Error;
    }

    size_t ComponentRegistry::factoryCount() const
    {
        return _factoryCount;
    }

    size_t ComponentRegistry::instanceCount() const
    {
        return _instanceCount;
    }

    RegistryError ComponentRegistry::lastError() const
    {
        return _lastError;
    }

    void ComponentRegistry::clearError()
    {
        _lastError = RegistryError::None;
    }

    ComponentFactory ComponentRegistry::findFactory(const char *driverType) const
    {
        if (driverType == nullptr)
            return nullptr;

        for (size_t i = 0; i < _factoryCount; ++i)
        {
            if (_factories[i].driverType == driverType)
                return _factories[i].factory;
        }
        return nullptr;
    }

    void ComponentRegistry::setError(RegistryError error)
    {
        _lastError = error;
    }

} // namespace prakiva
