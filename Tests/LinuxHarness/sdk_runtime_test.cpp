#include <cstdio>
#include "SDK.hpp"

void BuildFakeDeltaForce();

int main()
{
    BuildFakeDeltaForce();

    using namespace SDK;
    printf("GObjects->Num() = %d\n", UObject::GObjects->Num());

    int Printed = 0;
    for (int i = 0; i < UObject::GObjects->Num(); i++)
    {
        UObject* Obj = UObject::GObjects->GetByIndex(i);
        if (!Obj) continue;
        if (i < 6 || Obj->GetName().find("DFCharacter") != std::string::npos || Obj->GetName().find("\xE6\xB5\x8B") != std::string::npos)
        {
            if (Printed++ < 40) printf("[%d] %s\n", i, Obj->GetFullName().c_str());
        }
    }

    UClass* Character = UObject::FindClassFast("DFCharacter");
    printf("FindClassFast(DFCharacter) = %p %s\n", (void*)Character, Character ? Character->GetFullName().c_str() : "null");
    printf("ADFCharacter::StaticClass() = %p\n", (void*)ADFCharacter::StaticClass());
    printf("IsA(AActor) = %d\n", Character && Character->IsSubclassOf(AActor::StaticClass()));
    return 0;
}

#include "Engine/Public/Unreal/DeltaForce.h"
namespace DeltaForce { FProfile SeedProfile() { return FProfile{}; } }
