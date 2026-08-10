#include "Modules/ModuleManager.h"

void RegisterAvenorSpineTerrainModifierBridge();
void UnregisterAvenorSpineTerrainModifierBridge();

class FAvenorEditorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        RegisterAvenorSpineTerrainModifierBridge();
    }

    virtual void ShutdownModule() override
    {
        UnregisterAvenorSpineTerrainModifierBridge();
    }
};

IMPLEMENT_MODULE(FAvenorEditorModule, AvenorEditor)
