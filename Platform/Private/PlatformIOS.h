#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <functional>

#include "Settings.h"

/*
iOS port of the PlatformWindows interface. Implementations wrap the Mach-O / ARM64 helpers
in Utils.h. Section names use PE-style strings (".text", ".rdata", ".data") and are
translated to the equivalent Mach-O segments inside PlatformIOS.cpp.
*/

struct SectionInfo
{
private:
	uint8_t Data[0x18] = { 0x0 };

public:
	SectionInfo() = default;

public:
	inline bool IsValid() const
	{
		for (int i = 0; i < sizeof(Data); i++)
		{
			if (Data[i] != 0x0)
				return true;
		}

		return false;
	}
};

namespace Platform
{
	template<typename T>
	T* FinAlignedValueInRange(const T, const int32_t, uintptr_t, uint32_t);

	template<typename T>
	T* FindAlignedValueInSection(const SectionInfo&, T, const int32_t);

	template<typename T>
	T* FindAlignedValueInAllSections(const T Value, const int32_t Alignment = alignof(T), const uintptr_t StartAddress = 0x0, int32_t Range = 0x0, const char* const ModuleName = Settings::General::DefaultModuleName);
}

class PlatformPrivateImplHelper
{
public:
	template<typename T>
	friend T* Platform::FinAlignedValueInRange(const T, const int32_t, uintptr_t, uint32_t);

	template<typename T>
	friend T* Platform::FindAlignedValueInSection(const SectionInfo&, T, const int32_t);

	template<typename T>
	friend T* Platform::FindAlignedValueInAllSections(const T, const int32_t, const uintptr_t, int32_t, const char* const);

private:
	using ValueCompareFuncType = bool(*)(const void* Value, const void* PotentialValueAddress);

private:
	static void* FinAlignedValueInRangeImpl(const void* ValuePtr, ValueCompareFuncType ComparisonFunction, const int32_t ValueTypeSize, const int32_t Alignment, uintptr_t StartAddress, uint32_t Range);
	static void* FindAlignedValueInSectionImpl(const SectionInfo& Info, const void* ValuePtr, ValueCompareFuncType ComparisonFunction, const int32_t ValueTypeSize, const int32_t Alignment);
	static void* FindAlignedValueInAllSectionsImpl(const void* ValuePtr, ValueCompareFuncType ComparisonFunction, const int32_t ValueTypeSize, const int32_t Alignment, const uintptr_t StartAddress, int32_t Range, const char* const ModuleName);
};

namespace Platform
{
	consteval bool Is32Bit()
	{
		return false;
	}

	uintptr_t GetModuleBase(const char* const ModuleName = Settings::General::DefaultModuleName);
	uintptr_t GetOffset(const uintptr_t Address, const char* const ModuleName = Settings::General::DefaultModuleName);
	uintptr_t GetOffset(const void* Address, const char* const ModuleName = Settings::General::DefaultModuleName);

	SectionInfo GetSectionInfo(const std::string& SectionName, const char* const ModuleName = Settings::General::DefaultModuleName);
	void* IterateSectionWithCallback(const SectionInfo& Info, const std::function<bool(void* Address)>& Callback, uint32_t Granularity = 0x4, uint32_t OffsetFromEnd = 0x0);
	void* IterateAllSectionsWithCallback(const std::function<bool(void* Address)>& Callback, uint32_t Granularity = 0x4, uint32_t OffsetFromEnd = 0x0, const char* const ModuleName = Settings::General::DefaultModuleName);

	bool IsAddressInAnyModule(const uintptr_t Address);
	bool IsAddressInAnyModule(const void* Address);
	bool IsAddressInProcessRange(const uintptr_t Address);
	bool IsAddressInProcessRange(const void* Address);
	bool IsBadReadPtr(const uintptr_t Address);
	bool IsBadReadPtr(const void* Address);

	const void* GetAddressOfImportedFunction(const char* SearchModuleName, const char* ModuleToImportFrom, const char* SearchFunctionName);
	const void* GetAddressOfImportedFunctionFromAnyModule(const char* ModuleToImportFrom, const char* SearchFunctionName);

	const void* GetAddressOfExportedFunction(const char* SearchModuleName, const char* SearchFunctionName);

	template<bool bShouldResolve32BitJumps = true>
	std::pair<const void*, int32_t> IterateVTableFunctions(void** VTable, const std::function<bool(const uint8_t* Address, int32_t Index)>& CallBackForEachFunc, int32_t NumFunctions = 0x150, int32_t OffsetFromStart = 0x0);

	void* FindPattern(const char* Signature, const uint32_t Offset = 0, const bool bSearchAllSections = false, const uintptr_t StartAddress = 0x0, const char* const ModuleName = Settings::General::DefaultModuleName);
	void* FindPatternInRange(const char* Signature, const void* Start, const uintptr_t Range, const bool bRelative = false, const uint32_t Offset = 0);
	void* FindPatternInRange(const char* Signature, const uintptr_t Start, const uintptr_t Range, const bool bRelative = false, const uint32_t Offset = 0);
	void* FindPatternInRange(std::vector<int>&& Signature, const void* Start, const uintptr_t Range, const bool bRelative = false, uint32_t Offset = 0, const uint32_t SkipCount = 0);


	template<bool bCheckIfLeaIsStrPtr = false, typename CharType = char>
	void* FindByStringInAllSections(const CharType* RefStr, const uintptr_t StartAddress = 0x0, int32_t Range = 0x0, const bool bSearchOnlyExecutableSections = true, const char* const ModuleName = Settings::General::DefaultModuleName);

	template<bool bCheckIfLeaIsStrPtr, typename CharType>
	void* FindStringInRange(const CharType* RefStr, const uintptr_t StartAddress, const int32_t Range);


	template<typename T>
	T* FinAlignedValueInRange(const T Value, const int32_t Alignment, uintptr_t StartAddress, uint32_t Range)
	{
		auto ComparisonFunction = [](const void* ValueAddr, const void* PotentialMatchAddr) -> bool
		{
			return *static_cast<const T*>(ValueAddr) == *static_cast<const T*>(PotentialMatchAddr);
		};

		return static_cast<T*>(PlatformPrivateImplHelper::FinAlignedValueInRangeImpl(&Value, ComparisonFunction, sizeof(Value), Alignment, StartAddress, Range));
	}

	template<typename T>
	T* FindAlignedValueInSection(const SectionInfo& Info, const T Value, const int32_t Alignment)
	{
		auto ComparisonFunction = [](const void* ValueAddr, const void* PotentialMatchAddr) -> bool
		{
			return *static_cast<const T*>(ValueAddr) == *static_cast<const T*>(PotentialMatchAddr);
		};

		return static_cast<T*>(PlatformPrivateImplHelper::FindAlignedValueInSectionImpl(Info, &Value, ComparisonFunction, sizeof(Value), Alignment));
	}

	template<typename T>
	T* FindAlignedValueInAllSections(const T Value, const int32_t Alignment, const uintptr_t StartAddress, int32_t Range, const char* const ModuleName)
	{
		auto ComparisonFunction = [](const void* ValueAddr, const void* PotentialMatchAddr) -> bool
		{
			return *static_cast<const T*>(ValueAddr) == *static_cast<const T*>(PotentialMatchAddr);
		};

		return static_cast<T*>(PlatformPrivateImplHelper::FindAlignedValueInAllSectionsImpl(&Value, ComparisonFunction, sizeof(Value), Alignment, StartAddress, Range, ModuleName));
	}

	template<typename T>
	std::vector<T*> FindAllAlignedValuesInProcess(const T Value, const int32_t Alignment = alignof(T), const uintptr_t StartAddress = 0x0, int32_t Range = 0x0, const char* const ModuleName = Settings::General::DefaultModuleName)
	{
		std::vector<T*> Ret;

		uintptr_t LastFoundValueAddress = StartAddress;
		while (T* ValuePtr = FindAlignedValueInAllSections(Value, Alignment, LastFoundValueAddress, Range, ModuleName))
		{
			Ret.push_back(ValuePtr);
			LastFoundValueAddress = (reinterpret_cast<uintptr_t>(ValuePtr) + sizeof(T) + Alignment - 1) & ~(static_cast<uintptr_t>(Alignment) - 1);
		}

		return Ret;
	}
}
