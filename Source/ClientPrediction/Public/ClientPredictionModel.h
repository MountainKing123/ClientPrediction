﻿#pragma once

#include "ClientPredictionNetSerialization.h"
#include "Input.h"
#include "Declares.h"
#include "Physics/ImmediatePhysics/ImmediatePhysicsDeclares.h"

static constexpr uint32 kClientForwardPredictionFrames = 10;
static constexpr uint32 kAuthorityTargetInputBufferSize = 25;
static constexpr uint32 kInputWindowSize = 3;
static constexpr uint32 kSyncFrames = 5;

/**
 * The interface for the client prediction model. This exists so that the prediction component can hold a
 * reference to a templated model.
 */
class IClientPredictionModel {
	
public:

	IClientPredictionModel() = default;
	virtual ~IClientPredictionModel() = default;

	virtual void Initialize(UPrimitiveComponent* Component, ENetRole Role) = 0;

// Simulation ticking

	virtual void PreTick(Chaos::FReal Dt, bool bIsForcedSimulation, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle, ENetRole Role) = 0;
	virtual void PostTick(Chaos::FReal Dt, bool bIsForcedSimulation, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle, ENetRole Role) = 0;
	virtual void GameThreadTick(const float Dt, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle, ENetRole Role) = 0;

// Input packet / state receiving

	virtual void ReceiveInputPackets(FNetSerializationProxy& Proxy) = 0;
	virtual void ReceiveAuthorityState(FNetSerializationProxy& Proxy) = 0;
	
public:
	
	/** Simulate for the given number of tickets */
	TFunction<void(uint32)> ForceSimulate;

	/** These are the functions to queue RPC sends. The proxies should use functions that capture by value */
	TFunction<void(FNetSerializationProxy&)> EmitInputPackets;
	TFunction<void(FNetSerializationProxy&)> EmitAuthorityState;
	
};

/**********************************************************************************************************************/

/** Wraps a model state to include frame and input packet number */
template <typename ModelState>
struct FModelStateWrapper {

	uint32 FrameNumber = kInvalidFrame;
	uint32 InputPacketNumber = kInvalidFrame;
	
	ModelState State;

	void NetSerialize(FArchive& Ar);

	void Rewind(class UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle) const;

	bool operator ==(const FModelStateWrapper<ModelState>& Other) const;
};

template <typename ModelState>
void FModelStateWrapper<ModelState>::NetSerialize(FArchive& Ar)  {
	Ar << FrameNumber;
	Ar << InputPacketNumber;
		
	State.NetSerialize(Ar);
}

template <typename ModelState>
void FModelStateWrapper<ModelState>::Rewind(class UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle) const  {
	State.Rewind(Component, Handle);
}

template <typename ModelState>
bool FModelStateWrapper<ModelState>::operator==(const FModelStateWrapper<ModelState>& Other) const {
	return InputPacketNumber == Other.InputPacketNumber
		&& State == Other.State;
}

/**********************************************************************************************************************/

template <typename InputPacket>
struct FInputPacketWrapper {

	/** 
	 * Input frames have their own number independent of the frame number because they are not necessarily consumed in 
	 * lockstep with the frames they're generated on due to latency. 
	 */
	uint32 PacketNumber = kInvalidFrame;

	InputPacket Packet;

	void NetSerialize(FArchive& Ar);

	template <typename InputPacket_>
	friend FArchive& operator<<(FArchive& Ar, FInputPacketWrapper<InputPacket_>& Wrapper);
	
};

template <typename InputPacket>
void FInputPacketWrapper<InputPacket>::NetSerialize(FArchive& Ar) {
	Ar << PacketNumber;
	
	Packet.NetSerialize(Ar);
}

template <typename InputPacket>
FArchive& operator<<(FArchive& Ar, FInputPacketWrapper<InputPacket>& Wrapper) {
	Wrapper.NetSerialize(Ar);
	return Ar;
}

/**********************************************************************************************************************/

template <typename InputPacket, typename ModelState>
class BaseClientPredictionModel : public IClientPredictionModel {
	
public:

	BaseClientPredictionModel();
	virtual ~BaseClientPredictionModel() override = default;

	virtual void PreTick(Chaos::FReal Dt, bool bIsForcedSimulation, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle, ENetRole Role) override final;
	virtual void PostTick(Chaos::FReal Dt, bool bIsForcedSimulation, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle, ENetRole Role) override final;
	virtual void GameThreadTick(const float Dt, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle, ENetRole Role) override final;

	virtual void ReceiveInputPackets(FNetSerializationProxy& Proxy) override final;
	virtual void ReceiveAuthorityState(FNetSerializationProxy& Proxy) override final;

public:
	
	DECLARE_DELEGATE_OneParam(FInputProductionDelgate, InputPacket&)
	FInputProductionDelgate InputDelegate;
	
protected:

	virtual void Simulate(Chaos::FReal Dt, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle, const ModelState& PrevState, ModelState& OutState, const InputPacket& Input) = 0;
	virtual void PostSimulate(Chaos::FReal Dt, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle, ModelState& OutState, const InputPacket& Input) = 0;

private:

	void Rewind(const ModelState& State, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle);
	
	void PreTickAuthority(Chaos::FReal Dt, bool bIsForcedSimulation, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle);
	
	void PreTickRemote(Chaos::FReal Dt, bool bIsForcedSimulation, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle);
	
	void PostTickAuthority(Chaos::FReal Dt, bool bIsForcedSimulation, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle);
	
	void PostTickRemote(Chaos::FReal Dt, bool bIsForcedSimulation, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle);

	void Rewind_Internal(const FModelStateWrapper<ModelState>& State, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle);
	
private:
	
	/**
	 * If this object belongs to a client, the last acknowledged frame from the server.
	 * At this frame the client was identical to the server. 
	 */
	uint32 AckedServerFrame = kInvalidFrame;

	/** The index of the next frame on both the remote and authority */
	uint32 NextLocalFrame = 0;

	/** Remote index for the next input packet number */
	uint32 NextInputPacket = 0;

	/** Input packet used for the current frame */
	uint32 CurrentInputPacketIdx = kInvalidFrame;
	FInputPacketWrapper<InputPacket> CurrentInputPacket;
	
	/** On the client this is all of the frames that have not been reconciled with the server. */
	TQueue<FModelStateWrapper<ModelState>> ClientHistory;

	/**
	 * The last state that was received from the authority.
	 * We use atomic here because the state will be written to from whatever thread handles receiving the state
	 * from the authority and be read from the physics thread.
	 */
	std::atomic<FModelStateWrapper<ModelState>> LastAuthorityState;
	FModelStateWrapper<ModelState> CurrentState;

	FInputBuffer<FInputPacketWrapper<InputPacket>> InputBuffer;

	/* We send each input with several previous inputs. In case a packet is dropped, the next send will also contain the new dropped input */
	TArray<FInputPacketWrapper<InputPacket>> SlidingInputWindow;

};

template <typename InputPacket, typename ModelState>
BaseClientPredictionModel<InputPacket, ModelState>::BaseClientPredictionModel() {
	InputBuffer.SetAuthorityTargetBufferSize(kAuthorityTargetInputBufferSize);
}

template <typename InputPacket, typename ModelState>
void BaseClientPredictionModel<InputPacket, ModelState>::PreTick(Chaos::FReal Dt, bool bIsForcedSimulation, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle, ENetRole Role) {
	switch (Role) {
	case ENetRole::ROLE_Authority:
		PreTickAuthority(Dt, bIsForcedSimulation, Component, Handle);
		break;
	case ENetRole::ROLE_AutonomousProxy:
		PreTickRemote(Dt, bIsForcedSimulation, Component, Handle);
		break;
	default:
		return;
	}
}

template <typename InputPacket, typename ModelState>
void BaseClientPredictionModel<InputPacket, ModelState>::PostTick(Chaos::FReal Dt, bool bIsForcedSimulation, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle, ENetRole Role) {
	switch (Role) {
	case ENetRole::ROLE_Authority:
		PostTickAuthority(Dt, bIsForcedSimulation, Component, Handle);
		break;
	case ENetRole::ROLE_AutonomousProxy:
		PostTickRemote(Dt, bIsForcedSimulation, Component, Handle);
		break;
	default:
		return;
	}
}

template <typename InputPacket, typename ModelState>
void BaseClientPredictionModel<InputPacket, ModelState>::GameThreadTick(const float Dt, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle, ENetRole Role) {
	switch (Role) {
	case ENetRole::ROLE_SimulatedProxy: {
		// TODO make this interpolate from a buffer
		FModelStateWrapper<ModelState> LocalLastAuthorityState = LastAuthorityState;
		if (LocalLastAuthorityState.FrameNumber != kInvalidFrame) {
			LocalLastAuthorityState.Rewind(Component, Handle);
		}
		break;
	}
	default:
		break;
	}
}

template <typename InputPacket, typename ModelState>
void BaseClientPredictionModel<InputPacket, ModelState>::ReceiveInputPackets(FNetSerializationProxy& Proxy) {
	TArray<FInputPacketWrapper<InputPacket>> Packets;
	Proxy.NetSerializeFunc = [&Packets](FArchive& Ar) {
		Ar << Packets;
	};

	Proxy.Deserialize();
	for (const FInputPacketWrapper<InputPacket>& Packet : Packets) {
		InputBuffer.QueueInputAuthority(Packet);
	}
}

template <typename InputPacket, typename ModelState>
void BaseClientPredictionModel<InputPacket, ModelState>::ReceiveAuthorityState(FNetSerializationProxy& Proxy) {
	FModelStateWrapper<ModelState> State;
	Proxy.NetSerializeFunc = [&State](FArchive& Ar) {
		State.NetSerialize(Ar);	
	};

	Proxy.Deserialize();
	LastAuthorityState = State;
}

template <typename InputPacket, typename ModelState>
void BaseClientPredictionModel<InputPacket, ModelState>::Rewind(const ModelState& State, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle) {
	State.Rewind(Component, Handle);
}

template <typename InputPacket, typename ModelState>
void BaseClientPredictionModel<InputPacket, ModelState>::PreTickAuthority(Chaos::FReal Dt, bool bIsForcedSimulation, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle) {
	if (CurrentInputPacketIdx != kInvalidFrame || InputBuffer.AuthorityBufferSize() > InputBuffer.GetAuthorityTargetBufferSize()) {
		check(InputBuffer.ConsumeInputAuthority(CurrentInputPacket));
		CurrentInputPacketIdx = CurrentInputPacket.PacketNumber;
	}

	ModelState LastState = CurrentState.State;
	CurrentState = FModelStateWrapper<ModelState>();
	
	Simulate(Dt, Component, Handle, LastState, CurrentState.State, CurrentInputPacket.Packet);
}

template <typename InputPacket, typename ModelState>
void BaseClientPredictionModel<InputPacket, ModelState>::PreTickRemote(Chaos::FReal Dt, bool bIsForcedSimulation,
UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle) {
	if (!bIsForcedSimulation || InputBuffer.RemoteBufferSize() == 0) {
		FInputPacketWrapper<InputPacket> Packet;
		Packet.PacketNumber = NextInputPacket++;
		
		InputDelegate.ExecuteIfBound(Packet.Packet);
		InputBuffer.QueueInputRemote(Packet);

		EmitInputPackets.CheckCallable();

		if (SlidingInputWindow.Num() >= kInputWindowSize) {
			SlidingInputWindow.Pop();
		}
		
		// Capture by value here so that the proxy stores the input packets with it
		SlidingInputWindow.Insert(Packet, 0);
		FNetSerializationProxy Proxy([=](FArchive& Ar) mutable {
			Ar << SlidingInputWindow;
		});
		
		EmitInputPackets(Proxy);
	}
	
	check(InputBuffer.ConsumeInputRemote(CurrentInputPacket));
	CurrentInputPacketIdx = CurrentInputPacket.PacketNumber;
	
	ModelState LastState = CurrentState.State;
	CurrentState = FModelStateWrapper<ModelState>();

	Simulate(Dt, Component, Handle, LastState, CurrentState.State, CurrentInputPacket.Packet);
}

template <typename InputPacket, typename ModelState>
void BaseClientPredictionModel<InputPacket, ModelState>::PostTickAuthority(Chaos::FReal Dt, bool bIsForcedSimulation,
UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle) {
	PostSimulate(Dt, Component, Handle, CurrentState.State, CurrentInputPacket.Packet);
	
	CurrentState.FrameNumber = NextLocalFrame++;
	CurrentState.InputPacketNumber = CurrentInputPacketIdx;

	if (NextLocalFrame % kSyncFrames) {
		EmitAuthorityState.CheckCallable();

		// Capture by value here so that the proxy stores the state with it
		FModelStateWrapper<ModelState> LocalCurrentState = CurrentState;
		FNetSerializationProxy Proxy([=](FArchive& Ar) mutable {
			LocalCurrentState.NetSerialize(Ar);
		});
		
		EmitAuthorityState(Proxy);
	}
}

template <typename InputPacket, typename ModelState>
void BaseClientPredictionModel<InputPacket, ModelState>::PostTickRemote(Chaos::FReal Dt, bool bIsForcedSimulation,
	UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle) {
	PostSimulate(Dt, Component, Handle, CurrentState.State, CurrentInputPacket.Packet);
	
	CurrentState.FrameNumber = NextLocalFrame++;
	CurrentState.InputPacketNumber = CurrentInputPacketIdx;
	ClientHistory.Enqueue(CurrentState);

	// If there are frames that are being used to fast-forward/resimulate no logic needs to be performed
	// for them
	if (bIsForcedSimulation) {
		return;
	}
	
	FModelStateWrapper<ModelState> LocalLastAuthorityState = LastAuthorityState;
	
	if (LocalLastAuthorityState.FrameNumber == kInvalidFrame) {
		// Never received a frame from the server
		return;
	}

	if (LocalLastAuthorityState.FrameNumber <= AckedServerFrame && AckedServerFrame != kInvalidFrame) {
		// Last state received from the server was already acknowledged
		return;
	}

	if (LocalLastAuthorityState.InputPacketNumber == kInvalidFrame) {
		// Server has not started to consume input, ignore it since the client has been applying input since frame 0
		return;
	}
	
	if (LocalLastAuthorityState.FrameNumber > CurrentState.FrameNumber) {
		// Server is ahead of the client. The client should just chuck out everything and resimulate
		Rewind_Internal(LocalLastAuthorityState, Component, Handle);
		UE_LOG(LogTemp, Warning, TEXT("Client was behind server. Jumping to frame %i and resimulating"), LocalLastAuthorityState.FrameNumber);
		ForceSimulate(FMath::Max(kClientForwardPredictionFrames, InputBuffer.RemoteBufferSize()));
	} else {
		// Check history against the server state
		FModelStateWrapper<ModelState> HistoricState;
		bool bFound = false;
		
		while (!ClientHistory.IsEmpty()) {
			ClientHistory.Dequeue(HistoricState);
			if (HistoricState.FrameNumber == LocalLastAuthorityState.FrameNumber) {
				bFound = true;
				break;
			}
		}

		check(bFound);

		if (HistoricState == LocalLastAuthorityState) {
			// Server state and historic state matched, simulation was good up to LocalServerState.FrameNumber
			AckedServerFrame = LocalLastAuthorityState.FrameNumber;
			InputBuffer.Ack(LocalLastAuthorityState.InputPacketNumber);
			UE_LOG(LogTemp, Verbose, TEXT("Acked up to %i, input packet %i. Input buffer had %i elements"), AckedServerFrame, LocalLastAuthorityState.InputPacketNumber, InputBuffer.RemoteBufferSize());
		} else {
			// Server/client mismatch. Resimulate the client
			Rewind_Internal(LocalLastAuthorityState, Component, Handle);
			UE_LOG(LogTemp, Error, TEXT("Rewinding and resimulating from frame %i which used input packet %i"), LocalLastAuthorityState.FrameNumber, LocalLastAuthorityState.InputPacketNumber);
			ForceSimulate(FMath::Max(kClientForwardPredictionFrames, InputBuffer.RemoteBufferSize()));
		}
		
	}
}

template <typename InputPacket, typename ModelState>
void BaseClientPredictionModel<InputPacket, ModelState>::Rewind_Internal(const FModelStateWrapper<ModelState>& State, UPrimitiveComponent* Component, ImmediatePhysics::FActorHandle* Handle) {

	ClientHistory.Empty();
	AckedServerFrame = State.FrameNumber;
	
	// Add here because the body is at State.FrameNumber so the next frame will be State.FrameNumber + 1
	NextLocalFrame = State.FrameNumber + 1;

	InputBuffer.Rewind(State.InputPacketNumber);
	CurrentInputPacketIdx = State.InputPacketNumber;

	Rewind(State.State, Component, Handle);
}