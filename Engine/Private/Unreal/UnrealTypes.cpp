
#include <format.h>

#include "Unreal/UnrealTypes.h"
#include "Unreal/NameArray.h"

#include "Utils/Encoding/UnicodeNames.h"
#include "Utils/Encoding/UtfN.hpp"
#include "Menu/Logger.h"

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
    /* TCHAR = char16_t : use UTF-16 → UTF-32 conversion. */
    Utf32Name = UtfN::Utf16StringToUtf32String<std::u32string>(Name);
#else
    /* TCHAR = wchar_t (32-bit on Apple) : already UTF-32-sized, widen each codepoint. */
    Utf32Name.reserve(Name.size());
    for (TCHAR C : Name)
        Utf32Name += static_cast<char32_t>(C);
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

void FName::SetGNamesToStr()
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

bool FName::Init(bool bForceGNames)
{
	LogInfo("Initializing FName system...");

	/* GNames (FNamePool / TNameEntryArray) is located through its data, which also detects Delta Force's name encryption. */
	if (NameArray::TryInit())
	{
		SetGNamesToStr();
		Off::InSDK::Name::AppendNameToString = 0x0;

		LogSuccess("FName initialization complete via GNames");
		return true;
	}

	/*
	* Upstream falls back to FName::AppendString found by x86 code patterns. On ARM64 those patterns only match random functions,
	* and calling a wrong function with (FName*, FString&) crashes the game, so there is no automatic fallback here.
	* If you know the address, use FName::Init(Offset, FName::EOffsetOverrideType::AppendString).
	*/
	LogError("FName::Init: GNames couldn't be found or used");
	return false;
}

bool FName::Init(int32 OverrideOffset, EOffsetOverrideType OverrideType, bool bIsNamePool, const char* const ModuleName)
{
	if (OverrideType == EOffsetOverrideType::GNames)
	{
		if (!NameArray::TryInit(OverrideOffset, bIsNamePool, ModuleName))
			return false;

		SetGNamesToStr();
		Off::InSDK::Name::AppendNameToString = 0x0;
		return true;
	}

	const uintptr_t ImageBase = GetModuleBase(ModuleName);
	const uintptr_t FunctionAddress = ImageBase + OverrideOffset;

	if (!ImageBase || OverrideOffset <= 0 || (FunctionAddress & 0x3) != 0 || !IsInProcessRange(FunctionAddress) || IsBadReadPtr(FunctionAddress))
	{
		LogError("Manual-Override: FName::%s offset 0x%X is invalid", OverrideType == EOffsetOverrideType::AppendString ? "AppendString" : "ToString", OverrideOffset);
		return false;
	}

	AppendString = reinterpret_cast<void(*)(const void*, FString&)>(FunctionAddress);

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
	return true;
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
	if (!Address || !ToStr)
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

	// DecryptNameString runs at the raw-bytes level inside NameArray::GetStr,
	// so the wide string here is already decrypted.
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
	if (!Address)
		return 0;

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
		} Name{ CmpIdx };

		return FName(&Name).ToString();
	}
	else
	{
		struct FakeFName
		{
			int CompIdx;
			uint8 Pad[0xC];
		} Name{ CmpIdx };

		return FName(&Name).ToString();
	}
}

void* FName::DEBUGGetAppendString()
{
	return (void*)(AppendString);
}
