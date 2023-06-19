﻿#pragma once

#include "CoreMinimal.h"

struct CLIENTPREDICTION_API FClientPredictionModelId {
	FClientPredictionModelId() : OwningActor(nullptr) {}
	explicit FClientPredictionModelId(AActor* OwningActor) : OwningActor(OwningActor) { }
	explicit FClientPredictionModelId(const FClientPredictionModelId& ModelId) : OwningActor(ModelId.OwningActor) { }

	void Serialize(FArchive& Ar, class UPackageMap* Map) {
		Map->SerializeObject(Ar, AActor::StaticClass(), OwningActor);
	}

	bool operator==(const FClientPredictionModelId& Other) const {
		return Equals(Other);
	}

	bool Equals(const FClientPredictionModelId& Other) const {
		return OwningActor == Other.OwningActor;
	}

	uint32 GetTypeHash() const {
		return ::GetTypeHash(OwningActor);
	}

private:
	UObject* OwningActor = nullptr;
};

FORCEINLINE uint32 GetTypeHash(const FClientPredictionModelId& ModelId) {
	return GetTypeHash(ModelId.GetTypeHash());
}
