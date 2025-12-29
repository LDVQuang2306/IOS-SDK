
#include <format>

#include "../../Public/Unreal/UnrealTypes.h"
#include "../../Public/Unreal/NameArray.h"

#include "../../../Utils/Encoding/UnicodeNames.h"
#include "../../../Utils/Encoding/UtfN.hpp"
#include "../../../Menu/Logger.h"

std::string MakeNameValid(UnrealString&& Name)
{
	static constexpr const TCHAR* Numbers[10] =
	{
		TEXT("Zero"),
        TEXT("One"),
        TEXT("Two"),
        TEXT("Three"),
        TEXT("Four"),
        TEXT("Five"),
        TEXT("Six"),
        TEXT("Seven"),
        TEXT("Eight"),
        TEXT("Nine")
	};

	if (Name == TEXT("bool"))
		return "Bool";

	if (Name == TEXT("NULL"))
		return "NULLL";

	/* Replace 0 with Zero or 9 with Nine, if it is the first letter of the name. */
	if (Name[0] <= TEXT('9') && Name[0] >= TEXT('0'))
	{
		Name.replace(0, 1, Numbers[Name[0] - TEXT('0')]);
	}
	
	std::u32string Strrr;
	Strrr += UtfN::utf_cp32_t{ 200 };

    std::u32string Utf32Name;
    #if UEVERSION >= 421
    Utf32Name = UtfN::Utf16StringToUtf32String<std::u32string>(Name);
    #else
    Utf32Name.reserve(Name.size());
    for (TCHAR C : Name)
    {
        Utf32Name += static_cast<char32_t>(C);
    }
    #endif

	bool bIsFirstIteration = true;
	for (auto It = UtfN::utf32_iterator<std::u32string::iterator>(Utf32Name); It; ++It)
	{
		if (bIsFirstIteration && !IsUnicodeCharXIDStart(Name[0]))
		{
			/* Replace invalid starting character with 'm' character. 'm' for "member" */
			Name[0] = 'm';

			bIsFirstIteration = false;
		}

		if (!IsUnicodeCharXIDContinue((*It).Get()))
			It.Replace('_');
	}

	return UtfN::Utf32StringToUtf8String<std::string>(Utf32Name);;
}


FName::FName(const void* Ptr)
	: Address(static_cast<const uint8*>(Ptr))
{
}

// @@TODO: Fix this
void FName::Init(bool bForceGNames)
{
	LogInfo("Initializing FName system%s...", bForceGNames ? " (Forcing GNames)" : "");
	
	constexpr std::array<const char*, 6> PossibleSigs =
	{
		"48 8D ? ? 48 8D ? ? E8",
		"48 8D ? ? ? 48 8D ? ? E8",
		"48 8D ? ? 49 8B ? E8",
		"48 8D ? ? ? 49 8B ? E8",
		"48 8D ? ? 48 8B ? E8"
		"48 8D ? ? ? 48 8B ? E8",
        "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 F3 03 00 AA ? ? ? F0 ? ? ? F9 ? ? ? F9 ? ? ? F9 ? ? ? F0",
	};

	LogInfo("Searching for ForwardShadingQuality_ string...");
	MemAddress StringRef = FindByStringInAllSections("ForwardShadingQuality_");
	LogInfo("StringRef: 0x%p", (void*)StringRef);

	LogInfo("Searching for AppendString function using patterns...");
	int i = 0;
	while (!AppendString && i < PossibleSigs.size())
	{
		LogInfo("Trying pattern %d: %s", i, PossibleSigs[i]);
		AppendString = static_cast<void(*)(const void*, FString&)>(StringRef.RelativePattern(PossibleSigs[i], 0x50, -1 /* auto */));
		if (AppendString)
			LogSuccess("Found AppendString with pattern %d at 0x%p", i, (void*)AppendString);
		i++;
	}
	if (!AppendString)
		LogInfo("AppendString not found via patterns");

	Off::InSDK::Name::AppendNameToString = AppendString && !bForceGNames ? GetOffset((void*)AppendString) : 0x0;

	if (!AppendString || bForceGNames)
	{
		LogInfo("Attempting to initialize via NameArray (AppendString=%p, bForceGNames=%d)", (void*)AppendString, bForceGNames);
		const bool bInitializedSuccessfully = NameArray::TryInit();

		if (bInitializedSuccessfully)
		{
			ToStr = [](const void* Name) -> UnrealString
			{
				if (!Settings::Internal::bUseOutlineNumberName)
				{
					const uint32 Number = FName(Name).GetNumber();

                    if (Number > 0)
                        return NameArray::GetNameEntry(Name).GetWString() + TEXT('_') + ToUEString(Number - 1);
				}

				return NameArray::GetNameEntry(Name).GetWString();
			};

			LogSuccess("FName initialization complete via NameArray");
			return;
		}
		else /* Attempt to find FName::ToString as a final fallback */
		{
			LogInfo("NameArray initialization failed, trying fallback ToString method");
			/* Initialize GNames offset without committing to use GNames during the dumping process or in the SDK */
			NameArray::SetGNamesWithoutCommiting();
			FName::InitFallback();
		}
	}

	LogInfo("Setting GNames without committing");
	/* Initialize GNames offset without committing to use GNames during the dumping process or in the SDK */
	NameArray::SetGNamesWithoutCommiting();

	LogSuccess("Found FName::%s at Offset 0x%X", (Off::InSDK::Name::bIsUsingAppendStringOverToString ? "AppendString" : "ToString"), Off::InSDK::Name::AppendNameToString);

	LogInfo("Setting up final ToStr lambda with AppendString");
	ToStr = [](const void* Name) -> UnrealString
	{
		thread_local FFreableString TempString(1024);

		AppendString(Name, TempString);

		UnrealString OutputString = TempString.ToWString();
		TempString.ResetNum();

		return OutputString;
	};
	
	LogSuccess("FName::Init completed successfully");
}

void FName::Init(int32 OverrideOffset, EOffsetOverrideType OverrideType, bool bIsNamePool, const char* const ModuleName)
{
	if (OverrideType == EOffsetOverrideType::GNames)
	{
		const bool bInitializedSuccessfully = NameArray::TryInit(OverrideOffset, bIsNamePool, ModuleName);

		if (bInitializedSuccessfully)
		{
			ToStr = [](const void* Name) -> UnrealString
			{
				if (!Settings::Internal::bUseOutlineNumberName)
				{
					const uint32 Number = FName(Name).GetNumber();

					if (Number > 0)
						return NameArray::GetNameEntry(Name).GetWString() + TEXT('_') + ToUEString(Number - 1);
				}

				return NameArray::GetNameEntry(Name).GetWString();
			};
		}

		return;
	}

	AppendString = reinterpret_cast<void(*)(const void*, FString&)>(GetModuleBase(ModuleName) + OverrideOffset);

	Off::InSDK::Name::AppendNameToString = OverrideOffset;
	Off::InSDK::Name::bIsUsingAppendStringOverToString = OverrideType == EOffsetOverrideType::AppendString;

	ToStr = [](const void* Name) -> UnrealString
	{
		thread_local FFreableString TempString(1024);

		AppendString(Name, TempString);

		UnrealString OutputString = TempString.ToWString();
		TempString.ResetNum();

		return OutputString;
	};

	LogSuccess("Manual-Override: FName::%s --> Offset 0x%X", (Off::InSDK::Name::bIsUsingAppendStringOverToString ? "AppendString" : "ToString"), Off::InSDK::Name::AppendNameToString);
}

void FName::InitFallback()
{
	Off::InSDK::Name::bIsUsingAppendStringOverToString = false;

	MemAddress Conv_NameToStringAddress = FindUnrealExecFunctionByString("Conv_NameToString");
    
	constexpr std::array<const char*, 2> PossibleSigs =
	{
        "F4 4F BE A9 FD 7B 01 A9 FD 43 00 91 80 ? ? B4 F3 03 00 AA ? ? ? ? ? ? ? ? 80 02 40 F9",
        "08 00 40 F9 02 1D 40 F9 E1 03 13 AA FD 7B 41 A9 F4 4F C2 A8 40 00 1F D6",
	};

	int i = 0;
	while (!AppendString && i < PossibleSigs.size())
	{
		AppendString = static_cast<void(*)(const void*, FString&)>(Conv_NameToStringAddress.RelativePattern(PossibleSigs[i], 0x90, -1 /* auto */));

		i++;
	}

	Off::InSDK::Name::AppendNameToString = AppendString ? (int32)GetOffset((void*)AppendString) : 0x0;
}


UnrealString FName::ToRawWString() const
{
	if (!Address)
		return TEXT("None");

	return ToStr(Address);
}

UnrealString FName::ToWString() const
{
	UnrealString OutputString = ToRawWString();

	size_t pos = OutputString.rfind('/');

	if (pos == UnrealString::npos)
		return OutputString;

	return OutputString.substr(pos + 1);
}

std::string FName::ToRawString() const
{
	if (!Address)
		return "None";

	return UtfN::WStringToString(ToRawWString());
}

std::string FName::ToString() const
{
	if (!Address)
		return "None";

	return UtfN::WStringToString(ToWString());
}

std::string FName::ToValidString() const
{
	return MakeNameValid(ToWString());
}

int32 FName::GetCompIdx() const 
{
	return *reinterpret_cast<const int32*>(Address + Off::FName::CompIdx);
}

uint32 FName::GetNumber() const
{
	if (Settings::Internal::bUseOutlineNumberName)
		return 0x0;

	if (Settings::Internal::bUseNamePool)
		return *reinterpret_cast<const uint32*>(Address + Off::FName::Number); // The number is uint32 on versions <= UE4.23 

	return static_cast<uint32_t>(*reinterpret_cast<const int32*>(Address + Off::FName::Number));
}

bool FName::operator==(FName Other) const
{
	return GetCompIdx() == Other.GetCompIdx();
}

bool FName::operator!=(FName Other) const
{
	return GetCompIdx() != Other.GetCompIdx();
}

std::string FName::CompIdxToString(int CmpIdx)
{
	if (!Settings::Internal::bUseCasePreservingName)
	{
		struct FakeFName
		{
			int CompIdx;
			uint8 Pad[0x4];
		} Name{CmpIdx};

		return FName(&Name).ToString();
	}
	else
	{
		struct FakeFName
		{
			int CompIdx;
			uint8 Pad[0xC];
		} Name{CmpIdx};

		return FName(&Name).ToString();
	}
}

void* FName::DEBUGGetAppendString()
{
	return (void*)(AppendString);
}
