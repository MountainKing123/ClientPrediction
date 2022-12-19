﻿#pragma once

#include "CoreMinimal.h"

#include "Driver/ClientPredictionRepProxy.h"
#include "Driver/ClientPredictionModelDriver.h"

#include "Driver/Drivers/ClientPredictionModelAuthDriver.h"
#include "Driver/Drivers/ClientPredictionModelAutoProxyDriver.h"
#include "World/ClientPredictionWorldManager.h"

namespace ClientPrediction {
    // Delegate
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    struct IPhysicsModelDelegate {
        virtual ~IPhysicsModelDelegate() = default;

        virtual void EmitInputPackets(FNetSerializationProxy& Proxy) = 0;
    };

    // Interface
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    struct FPhysicsModelBase {
        virtual ~FPhysicsModelBase() = default;

        virtual void Initialize(class UPrimitiveComponent* Component, IPhysicsModelDelegate* InDelegate) = 0;
        virtual void Cleanup() = 0;

        virtual void SetNetRole(ENetRole Role, bool bShouldTakeInput, FRepProxy& AutoProxyRep, FRepProxy& SimProxyRep, FRepProxy& ControlProxyRep) = 0;

        virtual void ReceiveInputPackets(FNetSerializationProxy& Proxy) const = 0;
    };

    // Sim output
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    template <typename StateType, typename EventType>
    struct FSimOutput {
        explicit FSimOutput(FPhysicsState<StateType>& PhysState)
            : PhysState(PhysState) {}

        StateType& State() const { return PhysState.Body; }

        void DispatchEvent(EventType Event) {
            check(Event < 8)
            PhysState.Events |= 0b1 << Event;
        }

    private:
        FPhysicsState<StateType>& PhysState;
    };


    // Model declaration
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    template <typename InputType, typename StateType, typename EventType>
    struct FPhysicsModel : public FPhysicsModelBase, public IModelDriverDelegate<InputType, StateType> {
        using SimOutput = FSimOutput<StateType, EventType>;

        /**
         * Simulates the model before physics has been run for this ticket.
         * WARNING: This is called on the physics thread, so any objects shared between the physics thread and the
         * game thread need to be properly synchronized.
         */
        virtual void SimulatePrePhysics(Chaos::FReal Dt, FPhysicsContext& Context, const InputType& Input, const StateType& PrevState, SimOutput& OutState) = 0;

        /**
         * Simulates the model after physics has been run for this ticket.
         * WARNING: This is called on the physics thread, so any objects shared between the physics thread and the
         * game thread need to be properly synchronized.
         */
        virtual void SimulatePostPhysics(Chaos::FReal Dt, const FPhysicsContext& Context, const InputType& Input, const StateType& PrevState, SimOutput& OutState) = 0;

        // FPhysicsModelBase
        virtual void Initialize(class UPrimitiveComponent* Component, IPhysicsModelDelegate* InDelegate) override final;
        virtual void Finalize(const StateType& State, Chaos::FReal Dt) override final;

        virtual ~FPhysicsModel() override = default;
        virtual void Cleanup() override final;

        virtual void SetNetRole(ENetRole Role, bool bShouldTakeInput, FRepProxy& AutoProxyRep, FRepProxy& SimProxyRep, FRepProxy& ControlProxyRep) override final;
        virtual void ReceiveInputPackets(FNetSerializationProxy& Proxy) const override final;

        virtual void SetTimeDilation(const Chaos::FReal TimeDilation) override final;
        virtual void ForceSimulate(const uint32 NumTicks) override final;

        // IModelDriverDelegate
        virtual void GenerateInitialState(FPhysicsState<StateType>& State) override final;

        virtual void EmitInputPackets(TArray<FInputPacketWrapper<InputType>>& Packets) override final;
        virtual void ProduceInput(FInputPacketWrapper<InputType>& Packet) override final;

        virtual void SimulatePrePhysics(Chaos::FReal Dt, FPhysicsContext& Context, const InputType& Input, const FPhysicsState<StateType>& PrevState,
                                        FPhysicsState<StateType>& OutState) override final;

        virtual void SimulatePostPhysics(Chaos::FReal Dt, const FPhysicsContext& Context, const InputType& Input, const FPhysicsState<StateType>& PrevState,
                                         FPhysicsState<StateType>& OutState) override final;

        virtual void DispatchEvents(const FPhysicsState<StateType>& State) override final;

    public:
        DECLARE_DELEGATE_OneParam(FPhysicsModelProduceInput, InputType&)
        FPhysicsModelProduceInput ProduceInputDelegate;

        DECLARE_DELEGATE_TwoParams(FPhysicsModelFinalize, const StateType&, Chaos::FReal Dt)
        FPhysicsModelFinalize FinalizeDelegate;

        DECLARE_DELEGATE_OneParam(FPhysicsModelDispatchEvent, EventType)
        FPhysicsModelDispatchEvent DispatchEventDelegate;

    private:
        class UPrimitiveComponent* CachedComponent = nullptr;
        struct FWorldManager* CachedWorldManager = nullptr;
        TUniquePtr<IModelDriver<InputType>> ModelDriver = nullptr;
        IPhysicsModelDelegate* Delegate = nullptr;
    };

    // Implementation
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    template <typename InputType, typename StateType, typename EventType>
    void FPhysicsModel<InputType, StateType, EventType>::Initialize(UPrimitiveComponent* Component, IPhysicsModelDelegate* InDelegate) {
        CachedComponent = Component;
        check(CachedComponent);

        Delegate = InDelegate;
        check(Delegate);

        const UWorld* World = CachedComponent->GetWorld();
        check(World);

        CachedWorldManager = FWorldManager::ManagerForWorld(World);
        check(CachedWorldManager)
    }

    template <typename InputType, typename StateType, typename EventType>
    void FPhysicsModel<InputType, StateType, EventType>::Finalize(const StateType& State, Chaos::FReal Dt) {
        FinalizeDelegate.ExecuteIfBound(State, Dt);
    }

    template <typename InputType, typename StateType, typename EventType>
    void FPhysicsModel<InputType, StateType, EventType>::Cleanup() {
        if (CachedWorldManager != nullptr && ModelDriver != nullptr) {
            CachedWorldManager->RemoveTickCallback(ModelDriver.Get());
        }

        CachedWorldManager = nullptr;
        ModelDriver = nullptr;
    }

    template <typename InputType, typename StateType, typename EventType>
    void FPhysicsModel<InputType, StateType, EventType>::SetNetRole(ENetRole Role, bool bShouldTakeInput, FRepProxy& AutoProxyRep,
                                                         FRepProxy& SimProxyRep, FRepProxy& ControlProxyRep) {
        check(CachedWorldManager)
        check(CachedComponent)
        check(Delegate)

        if (ModelDriver != nullptr) {
            CachedWorldManager->RemoveTickCallback(ModelDriver.Get());
        }

        int32 RewindBufferSize = CachedWorldManager->GetRewindBufferSize();
        switch (Role) {
        case ROLE_Authority:
            // TODO pass bShouldTakeInput here
            ModelDriver = MakeUnique<FModelAuthDriver<InputType, StateType>>(CachedComponent, this, AutoProxyRep, SimProxyRep, ControlProxyRep, RewindBufferSize);
            CachedWorldManager->AddTickCallback(ModelDriver.Get());
            break;
        case ROLE_AutonomousProxy: {
            auto NewDriver = MakeUnique<FModelAutoProxyDriver<InputType, StateType>>(CachedComponent, this, AutoProxyRep, ControlProxyRep, RewindBufferSize);
            CachedWorldManager->AddTickCallback(NewDriver.Get());
            CachedWorldManager->AddRewindCallback(NewDriver.Get());
            ModelDriver = MoveTemp(NewDriver);
        }
        break;
        case ROLE_SimulatedProxy:
            // TODO add in the sim proxy
            break;
        default:
            break;
        }
    }

    template <typename InputType, typename StateType, typename EventType>
    void FPhysicsModel<InputType, StateType, EventType>::ReceiveInputPackets(FNetSerializationProxy& Proxy) const {
        if (ModelDriver == nullptr) { return; }

        TArray<FInputPacketWrapper<InputType>> Packets;
        Proxy.NetSerializeFunc = [&](FArchive& Ar) { Ar << Packets; };

        Proxy.Deserialize();
        ModelDriver->ReceiveInputPackets(Packets);
    }

    template <typename InputType, typename StateType, typename EventType>
    void FPhysicsModel<InputType, StateType, EventType>::GenerateInitialState(FPhysicsState<StateType>& State) {
        State = {};

        // TODO Actually generate initial state
    }

    template <typename InputType, typename StateType, typename EventType>
    void FPhysicsModel<InputType, StateType, EventType>::EmitInputPackets(TArray<FInputPacketWrapper<InputType>>& Packets) {
        check(Delegate);

        FNetSerializationProxy Proxy;
        Proxy.NetSerializeFunc = [=](FArchive& Ar) mutable { Ar << Packets; };
        Delegate->EmitInputPackets(Proxy);
    }

    template <typename InputType, typename StateType, typename EventType>
    void FPhysicsModel<InputType, StateType, EventType>::SetTimeDilation(const Chaos::FReal TimeDilation) {
        check(CachedWorldManager)
        CachedWorldManager->SetTimeDilation(TimeDilation);
    }

    template <typename InputType, typename StateType, typename EventType>
    void FPhysicsModel<InputType, StateType, EventType>::ForceSimulate(const uint32 NumTicks) {
        check(CachedWorldManager)
        CachedWorldManager->ForceSimulate(NumTicks);
    }

    template <typename InputType, typename StateType, typename EventType>
    void FPhysicsModel<InputType, StateType, EventType>::ProduceInput(FInputPacketWrapper<InputType>& Packet) {
        ProduceInputDelegate.ExecuteIfBound(Packet.Body);
    }

    template <typename InputType, typename StateType, typename EventType>
    void FPhysicsModel<InputType, StateType, EventType>::SimulatePrePhysics(const Chaos::FReal Dt, FPhysicsContext& Context,
                                                                 const InputType& Input,
                                                                 const FPhysicsState<StateType>& PrevState,
                                                                 FPhysicsState<StateType>& OutState) {
        SimOutput Output(OutState);
        SimulatePrePhysics(Dt, Context, Input, PrevState.Body, Output);
    }

    template <typename InputType, typename StateType, typename EventType>
    void FPhysicsModel<InputType, StateType, EventType>::SimulatePostPhysics(const Chaos::FReal Dt, const FPhysicsContext& Context,
                                                                  const InputType& Input,
                                                                  const FPhysicsState<StateType>& PrevState,
                                                                  FPhysicsState<StateType>& OutState) {
        SimOutput Output(OutState);
        SimulatePostPhysics(Dt, Context, Input, PrevState.Body, Output);
    }

    template <typename InputType, typename StateType, typename EventType>
    void FPhysicsModel<InputType, StateType, EventType>::DispatchEvents(const FPhysicsState<StateType>& State) {
        for (uint8 Event = 0; Event < 8; ++Event) {
            if ((State.Events & (0b1 << Event)) != 0) {
                DispatchEventDelegate.ExecuteIfBound(static_cast<EventType>(Event));
            }
        }
    }
}
