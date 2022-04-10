﻿#pragma once

#include "ClientPredictionModelDriver.h"
#include "ClientPredictionModelTypes.h"

#include "../ClientPredictionNetSerialization.h"
#include "../Input.h"

static constexpr uint32 kInputWindowSize = 3;
static constexpr uint32 kClientForwardPredictionFrames = 5;

template <typename InputPacket, typename ModelState>
class ClientPredictionAutoProxyDriver : public IClientPredictionModelDriver<InputPacket, ModelState> {

public:

	ClientPredictionAutoProxyDriver() = default;

	// Simulation ticking
	
	virtual void Tick(Chaos::FReal Dt, UPrimitiveComponent* Component) override;
	virtual ModelState GenerateOutput(Chaos::FReal Alpha) override;

	// Input packet / state receiving
	
	virtual void ReceiveInputPackets(FNetSerializationProxy& Proxy) override;
	virtual void ReceiveAuthorityState(FNetSerializationProxy& Proxy) override;
	
private:

	virtual void Tick(Chaos::FReal Dt, UPrimitiveComponent* Component, bool bIsForcedSimulation);
	
	void Rewind_Internal(const FModelStateWrapper<ModelState>& State, UPrimitiveComponent* Component);
	void ForceSimulate(uint32 Ticks, Chaos::FReal TickDt, UPrimitiveComponent* Component);
	
private:
	
	/** At this frame the authority and the auto proxy agreed */
	uint32 AckedFrame = kInvalidFrame;
	uint32 NextFrame = 0;
	uint32 NextInputPacket = 0;

	/** All of the frames that have not been reconciled with the authority. */
	TQueue<FModelStateWrapper<ModelState>> History;
	
	FInputPacketWrapper<InputPacket> CurrentInputPacket;

	FModelStateWrapper<ModelState> LastAuthorityState;
	FModelStateWrapper<ModelState> CurrentState;
	ModelState LastState;

	/* We send each input with several previous inputs. In case a packet is dropped, the next send will also contain the new dropped input */
	TArray<FInputPacketWrapper<InputPacket>> SlidingInputWindow;
	FInputBuffer<FInputPacketWrapper<InputPacket>> InputBuffer;
};

template <typename InputPacket, typename ModelState>
void ClientPredictionAutoProxyDriver<InputPacket, ModelState>::Tick(Chaos::FReal Dt, UPrimitiveComponent* Component) {
	Tick(Dt, Component, false);
}

template <typename InputPacket, typename ModelState>
void ClientPredictionAutoProxyDriver<InputPacket, ModelState>::Tick(Chaos::FReal Dt, UPrimitiveComponent* Component, bool bIsForcedSimulation) {
	LastState = CurrentState.State;
	
	// Pre-tick
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
	CurrentState = FModelStateWrapper<ModelState>();
	CurrentState.FrameNumber = NextFrame++;
	CurrentState.InputPacketNumber = CurrentInputPacket.PacketNumber;
	
	// Tick
	Simulate(Dt, Component, LastState, CurrentState.State, CurrentInputPacket.Packet);

	// Post-tick
	History.Enqueue(CurrentState);
	 
	// If there are frames that are being used to fast-forward/resimulate no logic needs to be performed
	// for them
	if (bIsForcedSimulation) {
		return;
	}
	
	if (LastAuthorityState.FrameNumber == kInvalidFrame) {
		// Never received a frame from the server
		return;
	}

	if (LastAuthorityState.FrameNumber <= AckedFrame && AckedFrame != kInvalidFrame) {
		// Last state received from the server was already acknowledged
		return;
	}

	if (LastAuthorityState.InputPacketNumber == kInvalidFrame) {
		// Server has not started to consume input, ignore it since the client has been applying input since frame 0
		return;
	}
	
	if (LastAuthorityState.FrameNumber > CurrentState.FrameNumber) {
		// Server is ahead of the client. The client should just chuck out everything and resimulate
		UE_LOG(LogTemp, Warning, TEXT("Client was behind server. Jumping to frame %i and resimulating"), LastAuthorityState.FrameNumber);
		
		Rewind_Internal(LastAuthorityState, Component);
		ForceSimulate(FMath::Max(kClientForwardPredictionFrames, InputBuffer.RemoteBufferSize()), Dt, Component);
	} else {
		// Check history against the server state
		FModelStateWrapper<ModelState> HistoricState;
		bool bFound = false;
		
		while (!History.IsEmpty()) {
			History.Dequeue(HistoricState);
			if (HistoricState.FrameNumber == LastAuthorityState.FrameNumber) {
				bFound = true;
				break;
			}
		}

		check(bFound);

		if (HistoricState == LastAuthorityState) {
			// Server state and historic state matched, simulation was good up to LocalServerState.FrameNumber
			AckedFrame = LastAuthorityState.FrameNumber;
			InputBuffer.Ack(LastAuthorityState.InputPacketNumber);
			UE_LOG(LogTemp, Verbose, TEXT("Acked up to %i, input packet %i. Input buffer had %i elements"), AckedFrame, LastAuthorityState.InputPacketNumber, InputBuffer.RemoteBufferSize());
		} else {
			// Server/client mismatch. Resimulate the client
			UE_LOG(LogTemp, Error, TEXT("Rewinding and resimulating from frame %i which used input packet %i"), LastAuthorityState.FrameNumber, LastAuthorityState.InputPacketNumber);

			Rewind_Internal(LastAuthorityState, Component);
			ForceSimulate(FMath::Max(kClientForwardPredictionFrames, InputBuffer.RemoteBufferSize()), Dt, Component);
		}
		
	}
}

template <typename InputPacket, typename ModelState>
ModelState ClientPredictionAutoProxyDriver<InputPacket, ModelState>::GenerateOutput(Chaos::FReal Alpha) {
	ModelState InterpolatedState = LastState;
	InterpolatedState.Interpolate(Alpha, CurrentState.State);
	return InterpolatedState;
}

template <typename InputPacket, typename ModelState>
void ClientPredictionAutoProxyDriver<InputPacket, ModelState>::ReceiveInputPackets(FNetSerializationProxy& Proxy) {
	// No-op since the client is the one sending the packets
}

template <typename InputPacket, typename ModelState>
void ClientPredictionAutoProxyDriver<InputPacket, ModelState>::ReceiveAuthorityState(FNetSerializationProxy& Proxy) {
	FModelStateWrapper<ModelState> State;
	Proxy.NetSerializeFunc = [&State](FArchive& Ar) {
		State.NetSerialize(Ar);	
	};

	Proxy.Deserialize();
	LastAuthorityState = State;
}

template <typename InputPacket, typename ModelState>
void ClientPredictionAutoProxyDriver<InputPacket, ModelState>::Rewind_Internal(const FModelStateWrapper<ModelState>& State, UPrimitiveComponent* Component) {
	History.Empty();
	AckedFrame = State.FrameNumber;
	
	// Add here because the body is at State.FrameNumber so the next frame will be State.FrameNumber + 1
	NextFrame = State.FrameNumber + 1;

	InputBuffer.Rewind(State.InputPacketNumber);
	Rewind(State.State, Component);
}

template <typename InputPacket, typename ModelState>
void ClientPredictionAutoProxyDriver<InputPacket, ModelState>::ForceSimulate(uint32 Ticks, Chaos::FReal TickDt, UPrimitiveComponent* Component) {
	for (uint32 i = 0; i < Ticks; i++) {
		Tick(TickDt, Component, true);
	}
}
