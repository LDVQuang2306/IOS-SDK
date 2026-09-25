#pragma once

#include <string>

#include "Unreal/Enums.h"

/*
 * Selects the dumper's TCHAR width based on the target UE version.
 *
 * Verified against working ARK 2.0 (4.17) and ARK Revamp (4.26) iOS tweak references:
 *   - UE  <  4.21 : FString uses 32-bit chars on iOS → TCHAR = wchar_t,  TEXT(x) = L##x
 *   - UE >=  4.21 : FString uses 16-bit chars       → TCHAR = char16_t, TEXT(x) = u##x
 *
 * Set this to match the *target game's* UE version. This drives:
 *   - the dumper's internal string reads,
 *   - the emitted SDK's `using TCHAR = ...;` alias,
 *   - the emitted STATIC_NAME_IMPL literal prefix (L"..." vs u"...").
 */
#define UEVERSION 426

#if UEVERSION >= 421

    #define TEXT(x) u##x
    typedef char16_t TCHAR;

    using UnrealString = std::u16string;

    template<typename T>
    inline UnrealString ToUEString(T IntType)
    {
        std::string Str = std::to_string(IntType);
        return std::u16string(Str.begin(), Str.end());
    }

#else

    #define TEXT(x) L##x
    typedef wchar_t TCHAR;

    using UnrealString = std::wstring;

    template<typename T>
    inline UnrealString ToUEString(T IntType)
    {
        return std::to_wstring(IntType);
    }

#endif

namespace Settings
{
	namespace General
	{
		/* This option determines whether calls to FindByStringInAllSections should only search executable sections, or all sections. */
		constexpr bool bSearchOnlyExecutableSectionsForStrings = true;

		/* If the target module is not the main executable, specify it here (iOS: the Mach-O image name, e.g. "DeltaForceClient") */
		constexpr const char* DefaultModuleName = nullptr;
	}

	inline constexpr const char* GlobalConfigPath = "";

	namespace Config
	{
		inline int SleepTimeout = 0;
		inline int DumpKey = 0;
		inline std::string SDKNamespaceName = "SDK";

		void Load();
		void DelayDumperStart();
	};

	namespace EngineCore
	{
		/* A special setting to fix UEnum::Names where the type is sometimes TArray<FName> and sometimes TArray<TPair<FName, Some8ByteData>> */
		constexpr bool bCheckEnumNamesInUEnum = false;

		/* Enables support for TEncryptedObjectProperty */
		constexpr bool bEnableEncryptedObjectPropertySupport = false;
	}

	namespace Generator
	{
		/* Auto generated if no override is provided */
		inline std::string GameName = "";
		inline std::string GameVersion = "";

		inline std::string SDKGenerationPath = getenv("HOME") ? getenv("HOME") : "";
	}

	namespace CppGenerator
	{
		/* No prefix for files->FilePrefix = "" */
		constexpr const char* FilePrefix = "";

		/* No seperate namespace for Params -> ParamNamespaceName = nullptr */
		constexpr const char* ParamNamespaceName = "Params";

		/* XOR function name, that will be wrapped around any generated string. e.g. "xorstr_" -> xorstr_("Pawn") etc. */
		constexpr const char* XORString = nullptr;
		/* XOR header file name. e.g. "xorstr.hpp" */
		constexpr const char* XORStringInclude = nullptr;

		/* Customizable part of Cpp code to allow for a custom 'uintptr_t InSDKUtils::GetImageBase()' function */
		constexpr const char* GetImageBaseFuncBody =
R"({
	return reinterpret_cast<uintptr_t>(_dyld_get_image_header(0));
}
)";
		/* Customizable part of Cpp code to allow for a custom 'InSDKUtils::CallGameFunction' function */
		constexpr const char* CallGameFunction =
R"(
	template<typename FuncType, typename... ParamTypes>
	requires std::invocable<FuncType, ParamTypes...>
	inline auto CallGameFunction(FuncType Function, ParamTypes&&... Args)
	{
		return Function(std::forward<ParamTypes>(Args)...);
	}
)";
		/* An option to force the UWorld::GetWorld() function in the SDK to get the world through an instance of UEngine. Useful for games on which the dumper finds the wrong GWorld offset. */
		constexpr bool bForceNoGWorldInSDK = false;

		/* This will allow the user to manually initialize global variable addresses in the SDK (eg. GObjects, GNames, AppendString). */
		constexpr bool bAddManualOverrideOptions = true;

		/* Adds the 'final' specifier to classes with no loaded child class at SDK-generation time. */
		constexpr bool bAddFinalSpecifier = true;
	}

	namespace MappingGenerator
	{
		/* Whether the MappingGenerator should check if a name was written to the nametable before. Exists to reduce mapping size. */
		constexpr bool bShouldCheckForDuplicatedNames = true;

		/* Whether EditorOnly should be excluded from the mapping file. */
		constexpr bool bExcludeEditorOnlyProperties = true;

		/* Which compression method to use when generating the file. */
		constexpr EUsmapCompressionMethod CompressionMethod = EUsmapCompressionMethod::ZStandard;
	}

	/* Partially implemented  */
	namespace Debug
	{
		/* Generates a dedicated file defining macros for static asserts (Make sure InlineAssertions are off) */
		inline constexpr bool bGenerateAssertionFile = false;

		/* Prefix for assertion macros in assertion file. Example for "MyPackage_params.hpp": #define DUMPER7_ASSERTS_PARAMS_MyPackage */
		inline constexpr const char* AssertionMacroPrefix = "DUMPER7_ASSERTS_";


		/* Adds static_assert for struct-size, as well as struct-alignment.
		 * NOTE (iOS): leave OFF — a handful of UE delegate-binding/template parent
		 * structs (e.g. FBlueprintInputDelegateBinding, FMovieSceneEvalTemplateBase)
		 * use the `bHasReusedTrailingPadding` trick. On clang `alignas` overrides
		 * `#pragma pack(1)` so the layout the dumper observed (parent ends at byte 1)
		 * can't be reproduced, and the static_asserts fail. The runtime layout in
		 * the live process is still correct — only standalone use of those ~6
		 * obscure parents would be wrong, and tweaks don't typically touch them.
		 */
		inline constexpr bool bGenerateInlineAssertionsForStructSize = false;

		/* Adds static_assert for member-offsets. See note above. */
		inline constexpr bool bGenerateInlineAssertionsForStructMembers = false;


		/* Prints debug information during Mapping-Generation */
		inline constexpr bool bShouldPrintMappingDebugData = false;
	}

	//* * * * * * * * * * * * * * * * * * * * *// 
	// Do **NOT** change any of these settings //
	//* * * * * * * * * * * * * * * * * * * * *//
	namespace Internal
	{
		/* Whether UEnum::Names stores only the name of the enum value, or a Pair<Name, Value> */
		inline bool bIsEnumNameOnly = false; // EDemoPlayFailure

		/* Whether the 'Value' component in the Pair<Name, Value> UEnum::Names is a uint8 value, rather than the default int64 */
		inline bool bIsSmallEnumValue = false;

		/* Whether UEnum::Names is of the new 'FNameData' type, rather than TArray<...> */
		inline bool bIsNewUE5EnumNamesContainer = false;

		/* Whether TWeakObjectPtr contains 'TagAtLastTest' */
		inline bool bIsWeakObjectPtrWithoutTag = false;

		/* Whether this games' engine version uses FProperty rather than UProperty */
		inline bool bUseFProperty = false;

		/* Whether this games' engine version uses FNamePool rather than TNameEntryArray */
		inline bool bUseNamePool = false;

		/* Whether UObject::Name or UObject::Class is first. Affects the calculation of the size of FName in fixup code. Not used after Off::Init(); */
		inline bool bIsObjectNameBeforeClass = false;

		/* Whether this games uses case-sensitive FNames, adding int32 DisplayIndex to FName */
		inline bool bUseCasePreservingName = false;

		/* Whether this games uses FNameOutlineNumber, moving the 'Number' component from FName into FNameEntry inside of FNamePool */
		inline bool bUseOutlineNumberName = false;

		/* Whether this game uses the 'FFieldPathProperty' cast flags for a custom property 'FObjectPtrProperty' */
		inline bool bIsObjPtrInsteadOfFieldPathProperty = false;

		/* Whether this games' engine version uses a contexpr flag to determine whether a FFieldVariant holds a UObject* or FField* */
		inline bool bUseMaskForFieldOwner = false;

		/* Whether this games' engine version uses double for FVector, instead of float. Aka, whether the engine version is UE5.0 or higher. */
		inline bool bUseLargeWorldCoordinates = false;

		/* Whether this game uses uint8 for UEProperty::ArrayDim, instead of int32 */
		inline bool bUseUint8ArrayDim = false;
	}

	extern void InitWeakObjectPtrSettings();
	extern void InitLargeWorldCoordinateSettings();

	extern void InitObjectPtrPropertySettings();
	extern void InitArrayDimSizeSettings();
}
