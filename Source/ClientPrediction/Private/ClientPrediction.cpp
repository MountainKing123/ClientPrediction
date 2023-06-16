#include "ClientPrediction.h"

#include "ISettingsModule.h"

#include "ClientPredictionSettings.h"
#include "World/ClientPredictionWorldManager.h"

#define LOCTEXT_NAMESPACE "FClientPredictionModule"

DEFINE_LOG_CATEGORY(LogClientPrediction);

FDelegateHandle FClientPredictionModule::OnPostWorldInitializationDelegate;
FDelegateHandle FClientPredictionModule::OnWorldCleanupDelegate;

void FClientPredictionModule::StartupModule() {
#if WITH_EDITOR
    ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
    if (SettingsModule != nullptr) {
        SettingsModule->RegisterSettings("Project", "Project", "ClientPrediction",
                                         LOCTEXT("ClientPredictionSettingsName", "ClientPrediction"),
                                         LOCTEXT("ClientPredictionSettingsDescription", "Settings for ClientPrediction"),
                                         GetMutableDefault<UClientPredictionSettings>()
        );
    }
#endif

    OnPostWorldInitializationDelegate = FWorldDelegates::OnPostWorldInitialization.AddStatic(&OnPostWorldInitialize);
    OnWorldCleanupDelegate = FWorldDelegates::OnWorldCleanup.AddStatic(&OnWorldCleanup);
}

void FClientPredictionModule::ShutdownModule() {
    FWorldDelegates::OnPostWorldInitialization.Remove(OnPostWorldInitializationDelegate);
    FWorldDelegates::OnWorldCleanup.Remove(OnWorldCleanupDelegate);
}

void FClientPredictionModule::OnPostWorldInitialize(UWorld* InWorld, const UWorld::InitializationValues) {
    ClientPrediction::FWorldManager::InitializeWorld(InWorld);
}

void FClientPredictionModule::OnWorldCleanup(UWorld* InWorld, bool bSessionEnded, bool bCleanupResources) {
    ClientPrediction::FWorldManager::CleanupWorld(InWorld);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FClientPredictionModule, ClientPrediction)
