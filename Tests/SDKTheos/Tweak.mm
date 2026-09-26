/*
* Uses the basic parts of a generated SDK so that their templates/inline functions are compiled too.
* Nothing runs on load: the library is only built to check that the SDK compiles for arm64 iOS.
*/
#import <Foundation/Foundation.h>

#include <string>

#include "SDK/Basic.hpp"
#include "SDK/CoreUObject_classes.hpp"
#include "SDK/CoreUObject_structs.hpp"

extern "C" __attribute__((visibility("default"))) int DFSDKTest_CountClasses(const char* NameFilter)
{
	using namespace SDK;

	int NumClasses = 0;

	for (int i = 0; i < UObject::GObjects->Num(); i++)
	{
		UObject* Object = UObject::GObjects->GetByIndex(i);

		if (!Object || Object->IsDefaultObject() || !Object->IsA(EClassCastFlags::Class))
			continue;

		const std::string Name = Object->GetName();

		if (!NameFilter || Name.find(NameFilter) != std::string::npos)
			NumClasses++;
	}

	UClass* ObjectClass = UObject::FindClassFast("Object");
	NSLog(@"[DFSDKTest] %d classes, UObject class %p, %s", NumClasses, ObjectClass, ObjectClass ? ObjectClass->GetFullName().c_str() : "null");

	return NumClasses;
}
