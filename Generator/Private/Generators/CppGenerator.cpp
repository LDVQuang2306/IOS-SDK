#include <format.h>
#include <vector>
#include <array>
#include <algorithm>

#include <cctype>

#include "Unreal/ObjectArray.h"
#include "Unreal/NameArray.h"
#include "Generators/CppGenerator.h"
#include "Wrappers/MemberWrappers.h"
#include "Managers/MemberManager.h"

#include "Settings.h"
#include "Platform.h"

#include "Menu/Logger.h"
/*
 * SDK emit knobs that follow Settings.h's UEVERSION.
 *
 * UE  < 4.21 : FString chars are 32-bit on iOS  → emit `using TCHAR = wchar_t;`  + L"..." literals
 * UE >= 4.21 : FString chars are 16-bit         → emit `using TCHAR = char16_t;` + u"..." literals
 *
 * See memory: dumper7-ue-version-layout-comparison.md
 */
#if UEVERSION >= 421
    constexpr const char* SDKTCharAlias        = "using TCHAR = char16_t;";
    constexpr const char  SDKWideLiteralPrefix = 'u';
#else
    constexpr const char* SDKTCharAlias        = "using TCHAR = wchar_t;";
    constexpr const char  SDKWideLiteralPrefix = 'L';
#endif

constexpr std::string GetTypeFromSize(uint8 Size)
{
	switch (Size)
	{
	case 1:
		return "uint8";
	case 2:
		return "uint16";
	case 4:
		return "uint32";
	case 8:
		return "uint64";
	default:
		return "INVALID_TYPE_SIZE_FOR_BIT_PADDING";
	}
}

std::string CppGenerator::MakeMemberString(const std::string& Type, const std::string& Name, std::string&& Comment)
{
	//<tab><--45 chars--><-------50 chars----->
	//     Type          MemberName;           // Comment
	int NumSpacesToComment;

	if (Type.length() < 45)
	{
		NumSpacesToComment = 50;
	}
	else if ((Type.length() + Name.length()) > 95)
	{
		NumSpacesToComment = 1;
	}
	else
	{
		NumSpacesToComment = 50 - (Type.length() - 45);
	}

	return fmt::format("\t{:{}} {:{}} // {}\n", Type, 45, Name + ";", NumSpacesToComment, std::move(Comment));
}

std::string CppGenerator::MakeMemberStringWithoutName(const std::string& Type)
{
	return '\t' + Type + ";\n";
}

std::string CppGenerator::GenerateBytePadding(const int32 Offset, const int32 PadSize, std::string&& Reason)
{
	return MakeMemberString("uint8", fmt::format("Pad_{:X}[0x{:X}]", Offset, PadSize), fmt::format("0x{:04X}(0x{:04X})({})", Offset, PadSize, std::move(Reason)));
}

std::string CppGenerator::GenerateBitPadding(uint8 UnderlayingSizeBytes, const uint8 PrevBitPropertyEndBit, const int32 Offset, const int32 PadSize, std::string&& Reason)
{
	return MakeMemberString(GetTypeFromSize(UnderlayingSizeBytes), fmt::format("BitPad_{:X}_{:X} : {:d}", Offset, PrevBitPropertyEndBit, PadSize), fmt::format("0x{:04X}(0x{:04X})({})", Offset, UnderlayingSizeBytes, std::move(Reason)));
}

std::string CppGenerator::GenerateMembers(const StructWrapper& Struct, const MemberManager& Members, int32 SuperSize, int32 SuperLastMemberEnd, int32 SuperAlign, int32 PackageIndex)
{
	constexpr uint64 EstimatedCharactersPerLine = 0xF0;

	const bool bIsUnion = Struct.IsUnion();

	std::string OutMembers;
	OutMembers.reserve(Members.GetNumMembers() * EstimatedCharactersPerLine);

	bool bEncounteredZeroSizedVariable = false;
	bool bEncounteredStaticVariable = false;
	bool bAddedSpaceZeroSized = false;
	bool bAddedSpaceStatic = false;

	auto AddSpaceBetweenSticAndNormalMembers = [&](const PropertyWrapper& Member)
	{
		if (!bAddedSpaceStatic && bEncounteredZeroSizedVariable && !Member.IsZeroSizedMember()) [[unlikely]]
		{
			OutMembers += '\n';
			bAddedSpaceZeroSized = true;
		}

		if (!bAddedSpaceStatic && bEncounteredStaticVariable && !Member.IsStatic()) [[unlikely]]
		{
			OutMembers += '\n';
			bAddedSpaceStatic = true;
		}

		bEncounteredZeroSizedVariable = Member.IsZeroSizedMember();
		bEncounteredStaticVariable = Member.IsStatic() && !Member.IsZeroSizedMember();
	};

	bool bLastPropertyWasBitField = false;

	int32 PrevPropertyEnd = SuperSize;
	int32 PrevBitPropertyEnd = 0;
	int32 PrevBitPropertyEndBit = 1;

	uint8 PrevBitPropertySize = 0x1;
	int32 PrevBitPropertyOffset = 0x0;
	uint64 PrevNumBitsInUnderlayingType = 0x8;

	const int32 SuperTrailingPaddingSize = SuperSize - SuperLastMemberEnd;
	bool bIsFirstSizedMember = true;

	for (const PropertyWrapper& Member : Members.IterateMembers())
	{
		AddSpaceBetweenSticAndNormalMembers(Member);

		const int32 MemberOffset = Member.GetOffset();
		const int32 MemberSize = Member.GetSize();

		const int32 CurrentPropertyEnd = MemberOffset + MemberSize;

		std::string Comment = fmt::format("0x{:04X}(0x{:04X})({})", MemberOffset, MemberSize, Member.GetFlagsOrCustomComment());

		const bool bIsBitField = Member.IsBitField();

		/* Padding between two bitfields at different byte-offsets */
		if (CurrentPropertyEnd > PrevPropertyEnd && bLastPropertyWasBitField && bIsBitField && PrevBitPropertyEndBit < PrevNumBitsInUnderlayingType && !bIsUnion)
		{
			OutMembers += GenerateBitPadding(PrevBitPropertySize, PrevBitPropertyEndBit, PrevBitPropertyOffset, PrevNumBitsInUnderlayingType - PrevBitPropertyEndBit, "Fixing Bit-Field Size For New Byte [ Dumper-7 ]");
			PrevBitPropertyEndBit = 0;
		}

		if (MemberOffset > PrevPropertyEnd && !bIsUnion)
			OutMembers += GenerateBytePadding(PrevPropertyEnd, MemberOffset - PrevPropertyEnd, "Fixing Size After Last Property [ Dumper-7 ]");

		bIsFirstSizedMember = Member.IsZeroSizedMember() || Member.IsStatic();

		if (bIsBitField)
		{
			const uint8 BitFieldIndex = Member.GetBitIndex();
			const uint8 BitSize = Member.GetBitCount();

			if (CurrentPropertyEnd > PrevPropertyEnd)
				PrevBitPropertyEndBit = 0x0;

			std::string BitfieldInfoComment = fmt::format("BitIndex: 0x{:02X}, PropSize: 0x{:04X} ({})", BitFieldIndex, MemberSize, Member.GetFlagsOrCustomComment());
			Comment = fmt::format("0x{:04X}(0x{:04X})({})", MemberOffset, MemberSize, BitfieldInfoComment);

			if (PrevBitPropertyEnd < MemberOffset)
				PrevBitPropertyEndBit = 0;

			if (PrevBitPropertyEndBit < BitFieldIndex && !bIsUnion)
				OutMembers += GenerateBitPadding(MemberSize, PrevBitPropertyEndBit, MemberOffset, BitFieldIndex - PrevBitPropertyEndBit, "Fixing Bit-Field Size Between Bits [ Dumper-7 ]");

			PrevBitPropertyEndBit = BitFieldIndex + BitSize;
			PrevBitPropertyEnd = MemberOffset  + MemberSize;

			PrevBitPropertySize = MemberSize;
			PrevBitPropertyOffset = MemberOffset;

			PrevNumBitsInUnderlayingType = (MemberSize * 0x8ull);
		}

		bLastPropertyWasBitField = bIsBitField;

		if (!Member.IsStatic()) [[likely]]
			PrevPropertyEnd = MemberOffset + (MemberSize * Member.GetArrayDim());

		std::string MemberName = Member.GetName();

		if (Member.GetArrayDim() > 1)
		{
			MemberName += fmt::format("[0x{:X}]", Member.GetArrayDim());
		}
		else if (bIsBitField)
		{
			MemberName += (" : " + std::to_string(Member.GetBitCount()));
		}

		if (Member.HasDefaultValue()) [[unlikely]]
			MemberName += (" = " + Member.GetDefaultValue());

		const bool bAllowForConstPtrMembers = Struct.IsFunction();

		/* using directives */
		if (Member.IsZeroSizedMember()) [[unlikely]]
		{
			OutMembers += MakeMemberStringWithoutName(GetMemberTypeString(Member, PackageIndex, bAllowForConstPtrMembers));
		}
		else [[likely]]
		{
			OutMembers += MakeMemberString(GetMemberTypeString(Member, PackageIndex, bAllowForConstPtrMembers), MemberName, std::move(Comment));
		}
	}

	const int32 MissingByteCount = Struct.GetUnalignedSize() - PrevPropertyEnd;

	if (MissingByteCount > 0x0 /* >=Struct.GetAlignment()*/)
		OutMembers += GenerateBytePadding(PrevPropertyEnd, MissingByteCount, "Fixing Struct Size After Last Property [ Dumper-7 ]");

	return OutMembers;
}

CppGenerator::FunctionInfo CppGenerator::GenerateFunctionInfo(const FunctionWrapper& Func)
{
	FunctionInfo RetFuncInfo;

	if (Func.IsPredefined())
	{
		RetFuncInfo.RetType = Func.GetPredefFuncReturnType();
		RetFuncInfo.FuncNameWithParams = Func.GetPredefFuncNameWithParams();

		return RetFuncInfo;
	}

	MemberManager FuncParams = Func.GetMembers();

	RetFuncInfo.FuncFlags = Func.GetFunctionFlags();
	RetFuncInfo.bIsReturningVoid = true;
	RetFuncInfo.RetType = "void";
	RetFuncInfo.FuncNameWithParams = Func.GetName() + "(";

	bool bIsFirstParam = true;

	RetFuncInfo.UnrealFuncParams.reserve(5);

	for (const PropertyWrapper& Param : FuncParams.IterateMembers())
	{
		if (!Param.HasPropertyFlags(EPropertyFlags::Parm))
			continue;

		std::string Type = GetMemberTypeString(Param);

		const bool bIsConst = Param.HasPropertyFlags(EPropertyFlags::ConstParm);

		ParamInfo PInfo;

		const bool bIsRef = Param.HasPropertyFlags(EPropertyFlags::ReferenceParm);
		const bool bIsOut = bIsRef || Param.HasPropertyFlags(EPropertyFlags::OutParm);
		const bool bIsRet = Param.IsReturnParam();

		if (bIsConst && (!bIsOut || bIsRef || bIsRet))
			Type = "const " + Type;

		if (Param.IsReturnParam())
		{
			RetFuncInfo.RetType = Type;
			RetFuncInfo.bIsReturningVoid = false;

			PInfo.PropFlags = Param.GetPropertyFlags();
			PInfo.bIsConst = false;
			PInfo.Name = Param.GetName();
			PInfo.Type = Type;
			PInfo.bIsRetParam = true;
			RetFuncInfo.UnrealFuncParams.push_back(PInfo);
			continue;
		}

		const bool bIsMoveType = Param.IsType(EClassCastFlags::StructProperty | EClassCastFlags::ArrayProperty | EClassCastFlags::StrProperty | EClassCastFlags::TextProperty | EClassCastFlags::MapProperty | EClassCastFlags::SetProperty);

		if (bIsOut)
			Type += bIsRef ? '&' : '*';

		if (!bIsOut && !bIsRef && bIsMoveType)
		{
			Type += "&";

			if (!bIsConst)
				Type = "const " + Type;
		}

		std::string ParamName = Param.GetName();

		PInfo.bIsOutPtr = bIsOut && !bIsRef;
		PInfo.bIsOutRef = bIsOut && bIsRef;
		PInfo.bIsMoveParam = bIsMoveType;
		PInfo.bIsRetParam = false;
		PInfo.bIsConst = bIsConst;
		PInfo.PropFlags = Param.GetPropertyFlags();
		PInfo.Name = ParamName;
		PInfo.Type = Type;
		RetFuncInfo.UnrealFuncParams.push_back(PInfo);

		if (!bIsFirstParam)
			RetFuncInfo.FuncNameWithParams += ", ";

		RetFuncInfo.FuncNameWithParams += Type + " " + ParamName;

		bIsFirstParam = false;
	}

	RetFuncInfo.FuncNameWithParams += ")";

	return RetFuncInfo;
}

std::string CppGenerator::GenerateSingleFunction(const FunctionWrapper& Func, const std::string& StructName, StreamType& FunctionFile, StreamType& ParamFile, StreamType& AssertionFile)
{
	namespace CppSettings = Settings::CppGenerator;

	std::string InHeaderFunctionText;

	FunctionInfo FuncInfo = GenerateFunctionInfo(Func);

	const bool bHasInlineBody = Func.HasInlineBody();
	const std::string TemplateText = (bHasInlineBody && Func.HasCustomTemplateText() ? (Func.GetPredefFunctionCustomTemplateText() + "\n\t") : "");

	const bool bIsConstFunc = Func.IsConst() && !Func.IsStatic();

	// Function declaration and inline-body generation
	InHeaderFunctionText += fmt::format("\t{}{}{}{}{}{}", TemplateText, (Func.IsStatic() ? "static " : ""), FuncInfo.RetType, (FuncInfo.RetType.empty() ? "" : " "), FuncInfo.FuncNameWithParams, bIsConstFunc ? " const" : "");
	InHeaderFunctionText += (bHasInlineBody ? ("\n\t" + Func.GetPredefFunctionInlineBody()) : ";") + "\n";

	if (bHasInlineBody)
		return InHeaderFunctionText;

	if (Func.IsPredefined())
	{
		std::string CustomComment = Func.GetPredefFunctionCustomComment();

		FunctionFile << fmt::format(R"(
// Predefined Function
{}
{}{}{}::{}{}
{}

)"
, !CustomComment.empty() ? ("// " + CustomComment + '\n') : ""
, Func.GetPredefFuncReturnType()
, Func.GetPredefFuncReturnType().empty() ? "" : " "
, StructName
, Func.GetPredefFuncNameWithParamsForCppFile()
, bIsConstFunc ? " const" : ""
, Func.GetPredefFunctionBody());

		return InHeaderFunctionText;
	}

	std::string ParamStructName = Func.GetParamStructName();

	// Parameter struct generation for unreal-functions
	if (!Func.IsPredefined() && Func.GetParamStructSize() > 0x0)
		GenerateStruct(Func.AsStruct(), ParamFile, FunctionFile, ParamFile, AssertionFile, -1, ParamStructName);


	std::string ParamVarCreationString = fmt::format(R"(
	{}{} Parms{{}};
)", CppSettings::ParamNamespaceName ? fmt::format("{}::", CppSettings::ParamNamespaceName) : "", ParamStructName);

	constexpr const char* StoreFunctionFlagsString = R"(
	auto Flgs = Func->FunctionFlags;
	Func->FunctionFlags |= 0x400;
)";

	std::string ParamDescriptionCommentString = "// Parameters:\n";
	std::string ParamAssignments;
	std::string OutPtrAssignments;
	std::string OutRefAssignments;

	const bool bHasParams = !FuncInfo.UnrealFuncParams.empty();
	bool bHasParamsToInit = false;
	bool bHasOutPtrParamsToInit = false;
	bool bHasOutRefParamsToInit = false;

	for (const ParamInfo& PInfo : FuncInfo.UnrealFuncParams)
	{
		ParamDescriptionCommentString += fmt::format("// {:{}}{:{}}({})\n", PInfo.Type, 40, PInfo.Name, 55, StringifyPropertyFlags(PInfo.PropFlags));

		if (PInfo.bIsRetParam)
			continue;

		if (PInfo.bIsOutPtr)
		{
			OutPtrAssignments += !PInfo.bIsMoveParam ? fmt::format(R"(

	if ({0} != nullptr)
		*{0} = Parms.{0};)", PInfo.Name) : fmt::format(R"(

	if ({0} != nullptr)
		*{0} = std::move(Parms.{0});)", PInfo.Name);
			bHasOutPtrParamsToInit = true;
		}
		else
		{
			ParamAssignments += PInfo.bIsMoveParam ? fmt::format("\tParms.{0} = std::move({0});\n", PInfo.Name) : fmt::format("\tParms.{0} = {0};\n", PInfo.Name);
			bHasParamsToInit = true;
		}

		if (PInfo.bIsOutRef && !PInfo.bIsConst)
		{
			OutRefAssignments += PInfo.bIsMoveParam ? fmt::format("\n\t{0} = std::move(Parms.{0});", PInfo.Name) : fmt::format("\n\t{0} = Parms.{0};", PInfo.Name);
			bHasOutRefParamsToInit = true;
		}
	}

	ParamAssignments = '\n' + ParamAssignments;
	OutRefAssignments = '\n' + OutRefAssignments;

	constexpr const char* RestoreFunctionFlagsString = R"(

	Func->FunctionFlags = Flgs;)";

	constexpr const char* ReturnValueString = R"(

	return Parms.ReturnValue;)";

	UEFunction UnrealFunc = Func.GetUnrealFunction();

	const bool bIsNativeFunc = Func.HasFunctionFlag(EFunctionFlags::Native);

	static auto PrefixQuotsWithBackslash = [](std::string&& Str) -> std::string
	{
		for (int i = 0; i < Str.size(); i++)
		{
			if (Str[i] == '"')
			{
				Str.insert(i, "\\");
				i++;
			}
		}

		return Str;
	};

	std::string FixedOuterName = PrefixQuotsWithBackslash(UnrealFunc.GetOuter().GetName());
	std::string FixedFunctionName = PrefixQuotsWithBackslash(UnrealFunc.GetName());

	// Function implementation generation
	std::string FunctionImplementation = fmt::format(R"(
// {}
// ({})
{}
{} {}::{}{}
{{
	static class UFunction* Func = nullptr;

	if (Func == nullptr)
		Func = {}->GetFunction({}, {});
{}{}{}
	{}ProcessEvent(Func, {});{}{}{}{}
}}

)", UnrealFunc.GetFullName()
, StringifyFunctionFlags(FuncInfo.FuncFlags)
, bHasParams ? ParamDescriptionCommentString : ""
, FuncInfo.RetType
, StructName
, FuncInfo.FuncNameWithParams
, bIsConstFunc ? " const" : ""
, Func.IsStatic() ? "StaticClass()" : Func.IsInInterface() ? "AsUObject()->Class" : "Class"
, CppSettings::XORString ? fmt::format("{}(\"{}\")", CppSettings::XORString, FixedOuterName) : fmt::format("\"{}\"", FixedOuterName)
, CppSettings::XORString ? fmt::format("{}(\"{}\")", CppSettings::XORString, FixedFunctionName) : fmt::format("\"{}\"", FixedFunctionName)
, bHasParams ? ParamVarCreationString : ""
, bHasParamsToInit ? ParamAssignments : ""
, bIsNativeFunc ? StoreFunctionFlagsString : ""
, Func.IsStatic() ? "GetDefaultObj()->" : Func.IsInInterface() ? "AsUObject()->" : "UObject::"
, bHasParams ? "&Parms" : "nullptr"
, bIsNativeFunc ? RestoreFunctionFlagsString : ""
, bHasOutRefParamsToInit ? OutRefAssignments : ""
, bHasOutPtrParamsToInit ? OutPtrAssignments : ""
, !FuncInfo.bIsReturningVoid ? ReturnValueString : "");

	FunctionFile << FunctionImplementation;

	return InHeaderFunctionText;
}

std::string CppGenerator::GenerateFunctions(const StructWrapper& Struct, const MemberManager& Members, const std::string& StructName, StreamType& FunctionFile, StreamType& ParamFile, StreamType& AssertionFile)
{
	namespace CppSettings = Settings::CppGenerator;

	static PredefinedFunction StaticClass;
	static PredefinedFunction StaticName;
	static PredefinedFunction GetDefaultObj;

	static PredefinedFunction Interface_AsObject;
	static PredefinedFunction Interface_AsObject_Const;

	if (StaticClass.NameWithParams.empty())
		StaticClass = {
		.CustomComment = "Used with 'IsA' to check if an object is of a certain type",
		.ReturnType = "class UClass*",
		.NameWithParams = "StaticClass()",

		.bIsStatic = true,
		.bIsConst = false,
		.bIsBodyInline = true,
	};
	if (StaticName.NameWithParams.empty())
		StaticName = {
		.CustomComment = "Used with 'IsA' to check if an object is of a certain Blueprint class type",
		.ReturnType = "const class FName&",
		.NameWithParams = "StaticName()",

		.bIsStatic = true,
		.bIsConst = false,
		.bIsBodyInline = true,
	};


	if (GetDefaultObj.NameWithParams.empty())
		GetDefaultObj = {
		.CustomComment = "Only use the default object to call \"static\" functions",
		.NameWithParams = "GetDefaultObj()",

		.bIsStatic = true,
		.bIsConst = false,
		.bIsBodyInline = true,
	};
	if (Interface_AsObject.NameWithParams.empty())
	{
		Interface_AsObject = {
		.CustomComment = "UObject inheritance was removed from interfaces to avoid virtual inheritance in the SDK.",
		.ReturnType = "class UObject*",
		.NameWithParams = "AsUObject()",

		.bIsStatic = false,
		.bIsConst = false,
		.bIsBodyInline = true,
		};

		Interface_AsObject.Body = "{\n\treturn reinterpret_cast<UObject*>(this);\n}";
	}

	if (Interface_AsObject_Const.NameWithParams.empty())
	{
		Interface_AsObject_Const = {
		.CustomComment = "UObject inheritance was removed from interfaces to avoid virtual inheritance in the SDK.",
		.ReturnType = "const class UObject*",
		.NameWithParams = "AsUObject()",

		.bIsStatic = false,
		.bIsConst = true,
		.bIsBodyInline = true,
		};

		Interface_AsObject_Const.Body = "{\n\treturn reinterpret_cast<const UObject*>(this);\n}";
	}

	std::string InHeaderFunctionText;

	bool bIsFirstIteration = true;
	bool bDidSwitch = false;
	bool bWasLastFuncStatic = false;
	bool bWasLastFuncInline = false;
	bool bWaslastFuncConst = false;

	const bool bIsInterface = Struct.IsInterface();

	for (const FunctionWrapper& Func : Members.IterateFunctions())
	{
		/* The function is no callable function, but instead just the signature of a TDelegate or TMulticastInlineDelegate */
		if (Func.GetFunctionFlags() & EFunctionFlags::Delegate)
			continue;

		// Handeling spacing between static and non-static, const and non-const, as well as inline and non-inline functions
		if (bWasLastFuncInline != Func.HasInlineBody() && !bIsFirstIteration)
		{
			InHeaderFunctionText += "\npublic:\n";
			bDidSwitch = true;
		}

		if ((bWasLastFuncStatic != Func.IsStatic() || bWaslastFuncConst != Func.IsConst()) && !bIsFirstIteration && !bDidSwitch)
			InHeaderFunctionText += '\n';

		bWasLastFuncStatic = Func.IsStatic();
		bWasLastFuncInline = Func.HasInlineBody();
		bWaslastFuncConst = Func.IsConst();
		bIsFirstIteration = false;
		bDidSwitch = false;

		InHeaderFunctionText += GenerateSingleFunction(Func, StructName, FunctionFile, ParamFile, AssertionFile);
	}

	/* Skip predefined classes, all structs and classes which don't inherit from UObject (very rare). */
	if (!Struct.IsUnrealStruct() || !Struct.IsClass() || !Struct.GetSuper().IsValid())
		return InHeaderFunctionText;

	/* Special spacing for UClass specific functions 'StaticClass' and 'GetDefaultObj' */
	if (bWasLastFuncInline != StaticClass.bIsBodyInline && !bIsFirstIteration)
	{
		InHeaderFunctionText += "\npublic:\n";
		bDidSwitch = true;
	}

	if ((bWasLastFuncStatic != StaticClass.bIsStatic || bWaslastFuncConst != StaticClass.bIsConst) && !bIsFirstIteration && !bDidSwitch)
		InHeaderFunctionText += '\n';

	static UEClass BPGeneratedClass = nullptr;
	
	if (BPGeneratedClass == nullptr)
		BPGeneratedClass = ObjectArray::FindClassFast("BlueprintGeneratedClass");

	const bool bIsBPStaticClass = Struct.IsAClassWithType(BPGeneratedClass);

	const bool bIsNameUnique = Struct.GetUniqueName().second;

	std::string Name = bIsNameUnique ? Struct.GetRawName() : Struct.GetFullName();
	std::string NameText = CppSettings::XORString ? fmt::format("{}(\"{}\")", CppSettings::XORString, Name) : fmt::format("\"{}\"", Name);

	if (bIsBPStaticClass)
	{
		StaticClass.Body = fmt::format(
			R"({{
	BP_STATIC_CLASS_IMPL{}({})
}})", (bIsNameUnique ? "" : "_FULLNAME"), NameText);
	}
	else
	{
		StaticClass.Body = fmt::format(
R"({{
	STATIC_CLASS_IMPL{}({})
}})", (bIsNameUnique ? "" : "_FULLNAME"), NameText);
	}

	/* ClassName uses the short name. Literal prefix follows UEVERSION: 'L' for wchar_t (UE<4.21), 'u' for char16_t (UE>=4.21). */
	NameText = CppSettings::XORString
		? fmt::format("{}({}\"{}\")", CppSettings::XORString, SDKWideLiteralPrefix, Struct.GetRawName())
		: fmt::format("{}\"{}\"", SDKWideLiteralPrefix, Struct.GetRawName());

	StaticName.Body = fmt::format(
R"({{
	STATIC_NAME_IMPL({})
}})", NameText);

	/* Set class-specific parts of 'GetDefaultObj' */
	GetDefaultObj.ReturnType = fmt::format("class {}*", StructName);
	GetDefaultObj.Body = fmt::format(
R"({{
	return GetDefaultObjImpl<{}>();
}})",StructName);

	std::shared_ptr<StructWrapper> CurrentStructPtr = std::make_shared<StructWrapper>(Struct);
	InHeaderFunctionText += GenerateSingleFunction(FunctionWrapper(CurrentStructPtr, &StaticClass), StructName, FunctionFile, ParamFile, AssertionFile);
	InHeaderFunctionText += GenerateSingleFunction(FunctionWrapper(CurrentStructPtr, &StaticName), StructName, FunctionFile, ParamFile, AssertionFile);
	InHeaderFunctionText += GenerateSingleFunction(FunctionWrapper(CurrentStructPtr, &GetDefaultObj), StructName, FunctionFile, ParamFile, AssertionFile);

	if (bIsInterface)
	{
		InHeaderFunctionText += '\n';

		InHeaderFunctionText += GenerateSingleFunction(FunctionWrapper(CurrentStructPtr, &Interface_AsObject), StructName, FunctionFile, ParamFile, AssertionFile);
		InHeaderFunctionText += GenerateSingleFunction(FunctionWrapper(CurrentStructPtr, &Interface_AsObject_Const), StructName, FunctionFile, ParamFile, AssertionFile);
	}

	return InHeaderFunctionText;
}

void CppGenerator::GenerateStruct(const StructWrapper& Struct, StreamType& StructFile, StreamType& FunctionFile, StreamType& ParamFile, StreamType& AssertionFile, int32 PackageIndex, const std::string& StructNameOverride)
{
	if (!Struct.IsValid())
		return;

	const std::string UniqueName = StructNameOverride.empty() ? GetStructPrefixedName(Struct) : StructNameOverride;
	std::string UniqueSuperName;

	const int32 StructSize = Struct.GetSize();
	int32 SuperSize = 0x0;
	int32 UnalignedSuperSize = 0x0;
	int32 SuperAlignment = 0x0;
	int32 SuperLastMemberEnd = 0x0;
	bool bIsReusingTrailingPaddingFromSuper = false;

	StructWrapper Super = Struct.GetSuper();

	const bool bHasValidSuper = Super.IsValid() && !Struct.IsFunction() && !Struct.IsInterface();

	/* Ignore UFunctions with a valid Super field, parameter structs are not supposed inherit from eachother. */
	if (bHasValidSuper)
	{
		UniqueSuperName = GetStructPrefixedName(Super);
		SuperSize = Super.GetSize();
		UnalignedSuperSize = Super.GetUnalignedSize();
		SuperAlignment = Super.GetAlignment();
		SuperLastMemberEnd = Super.GetLastMemberEnd();

		bIsReusingTrailingPaddingFromSuper = Super.HasReusedTrailingPadding();

		if (Super.IsCyclicWithPackage(PackageIndex)) [[unlikely]]
			UniqueSuperName = GetCycleFixupType(Super, true);
	}

	const int32 StructSizeWithoutSuper = StructSize - SuperSize;

	const bool bIsClass = Struct.IsClass();
	const bool bIsUnion = Struct.IsUnion();

	const bool bHasReusedTrailingPadding = Struct.HasReusedTrailingPadding();

	const bool bIsTemplatedType = Struct.HasCustomTemplateText();

	std::string AlignmentString = "";

	if (Struct.ShouldUseExplicitAlignment())
		AlignmentString = fmt::format("alignas(0x{:02X}) ", Struct.GetAlignment());
	else if (bHasReusedTrailingPadding)
		AlignmentString = fmt::format("SDK_ALIGN(0x{:02X}) ", Struct.GetAlignment());

	StructFile << fmt::format(R"(
// {}
// 0x{:04X} (0x{:04X} - 0x{:04X})
{}{}{} {}{}{}{}
{{
)", Struct.GetFullName()
  , StructSizeWithoutSuper
  , StructSize
  , SuperSize
  , bHasReusedTrailingPadding ? "#pragma pack(push, 0x1)\n" : ""
  , bIsTemplatedType ? (Struct.GetCustomTemplateText() + "\n") : ""
  , bIsClass ? "class" : (bIsUnion ? "union" : "struct")
  , AlignmentString
  , UniqueName
  , Settings::CppGenerator::bAddFinalSpecifier && Struct.IsFinal() ? " final" : ""
  , bHasValidSuper ? (" : public " + UniqueSuperName) : "");

	MemberManager Members = Struct.GetMembers();

	const bool bHasStaticClass = (bIsClass && Struct.IsUnrealStruct());

	const bool bHasMembers = Members.HasMembers() || (StructSizeWithoutSuper >= Struct.GetAlignment());
	const bool bHasFunctions = (Members.HasFunctions() && !Struct.IsFunction()) || bHasStaticClass;

	if (bHasMembers || bHasFunctions)
		StructFile << "public:\n";

	if (bHasMembers)
	{
		StructFile << GenerateMembers(Struct, Members, bIsReusingTrailingPaddingFromSuper ? UnalignedSuperSize : SuperSize, SuperLastMemberEnd, SuperAlignment, PackageIndex);

		if (bHasFunctions)
			StructFile << "\npublic:\n";
	}

	if (bHasFunctions)
	{
		StreamType& FuncParamsAssertionFile = Settings::Debug::bGenerateAssertionFile ? AssertionFile : ParamFile;

		StructFile << GenerateFunctions(Struct, Members, UniqueName, FunctionFile, ParamFile, FuncParamsAssertionFile);
	}

	StructFile << "};\n";

	if (bHasReusedTrailingPadding)
		StructFile << "#pragma pack(pop)\n";

	if constexpr (Settings::Debug::bGenerateAssertionFile)
	{
		if (bIsTemplatedType)
			return;

		const std::string AssertionMacroName = GetAssertionMacroString(UniqueName);

		// Place the macro below the struct so members etc. are verified
		StructFile << AssertionMacroName << ";\n";

		// Start definition of one macro for struct-size/-alignment and member-offset assertions
		AssertionFile << "#define " << AssertionMacroName << " \\\n";
	}

	constexpr const char* AssertionNewLineStr = Settings::Debug::bGenerateAssertionFile ? " \\\n" : "\n";

	if constexpr (Settings::Debug::bGenerateInlineAssertionsForStructSize || Settings::Debug::bGenerateAssertionFile)
	{
		if (bIsTemplatedType)
			return;

		const int32 StructSize = Struct.GetSize();

		// Alignment assertions
		AssertionFile << fmt::format("static_assert(alignof({0}) == 0x{1:06X}, \"Wrong alignment on {0}\");{2}", UniqueName, Struct.GetAlignment(), AssertionNewLineStr);

		// Size assertions
		AssertionFile << fmt::format("static_assert(sizeof({}) == 0x{:06X}, \"Wrong size on {}\");{}", UniqueName, (StructSize > 0x0 ? StructSize : 0x1), UniqueName, AssertionNewLineStr);
	}


	if constexpr (Settings::Debug::bGenerateInlineAssertionsForStructMembers || Settings::Debug::bGenerateAssertionFile)
	{
		for (const PropertyWrapper& Member : Members.IterateMembers())
		{
			if (Member.IsBitField() || Member.IsZeroSizedMember() || Member.IsStatic())
				continue;

			AssertionFile << fmt::format("static_assert(offsetof({0}, {1}) == 0x{2:06X}, \"Member '{0}::{1}' has a wrong offset!\");{3}", UniqueName, Member.GetName(), Member.GetOffset(), AssertionNewLineStr);
		}
	}

	if constexpr (Settings::Debug::bGenerateAssertionFile)
	{
		AssertionFile << '\n';
	}
}

void CppGenerator::GenerateEnum(const EnumWrapper& Enum, StreamType& StructFile)
{
	if (!Enum.IsValid())
		return;

	CollisionInfoIterator EnumValueIterator = Enum.GetMembers();

	int32 NumValues = 0x0;
	std::string MemberString;

	const std::string UnderlyingType = GetEnumUnderlayingType(Enum);
	for (const EnumCollisionInfo& Info : EnumValueIterator)
	{
		NumValues++;
		/* Cast to the enum's underlying type so sentinel values like uint64-max (-1) don't
		 * trigger "value cannot be narrowed to uintN" when emitted as a raw decimal literal. */
		MemberString += fmt::format("\t{:{}} = static_cast<{}>({}),\n", Info.GetUniqueName(), 40, UnderlyingType, Info.GetValue());
	}

	if (!MemberString.empty()) [[likely]]
		MemberString.pop_back();

	StructFile << fmt::format(R"(
// {}
// NumValues: 0x{:04X}
enum class {} : {}
{{
{}
}};
)", Enum.GetFullName()
  , NumValues
  , GetEnumPrefixedName(Enum)
  , UnderlyingType
  , MemberString);
}

std::string CppGenerator::GetStructPrefixedName(const StructWrapper& Struct)
{
	if (Struct.IsFunction())
		return Struct.GetUnrealStruct().GetOuter().GetValidName() + "_" + Struct.GetName();

	auto [ValidName, bIsUnique] = Struct.GetUniqueName();

	if (bIsUnique) [[likely]]
		return ValidName;

	/* Package::FStructName */
	return PackageManager::GetName(Struct.GetUnrealStruct().GetPackageIndex()) + "::" + ValidName;
}

std::string CppGenerator::GetEnumPrefixedName(const EnumWrapper& Enum)
{
	auto [ValidName, bIsUnique] = Enum.GetUniqueName();

	if (bIsUnique) [[likely]]
		return ValidName;

	/* Collision fallback: `Package_RawValidName`.
	 *   - '_' (not '::') because enums sit at file scope; `enum class Pkg::Name` is ill-formed.
	 *   - RAW name (Enum.GetValidName(), no 'E'-prefix munging) because two enums in the
	 *     same package can collide AFTER 'E' prefixing — e.g. UE has both `EWeaponType`
	 *     and `WeaponType` in SGFramework; both prefixed → `EWeaponType` → ambiguous.
	 *     The raw names differ, so the disambiguator does too. */
	return PackageManager::GetName(Enum.GetUnrealEnum().GetPackageIndex())
		+ "_" + Enum.GetUnrealEnum().GetValidName();
}

std::string CppGenerator::GetEnumUnderlayingType(const EnumWrapper& Enum)
{
	static constexpr std::array<const char*, 8> UnderlayingTypesBySize = {
		"uint8",
		"uint16",
		"InvalidEnumSize",
		"uint32",
		"InvalidEnumSize",
		"InvalidEnumSize",
		"InvalidEnumSize",
		"uint64"
	};

	return Enum.GetUnderlyingTypeSize() <= 0x8 ? UnderlayingTypesBySize[static_cast<size_t>(Enum.GetUnderlyingTypeSize()) - 1] : "uint8";
}

std::string CppGenerator::GetAssertionMacroString(const std::string& PrefixedStructUniqueName)
{
	std::string MacroStructName = PrefixedStructUniqueName;
	std::replace(MacroStructName.begin(), MacroStructName.end(), ':', '_');

	return Settings::Debug::AssertionMacroPrefix + MacroStructName;
}

std::string CppGenerator::GetCycleFixupType(const StructWrapper& Struct, bool bIsForInheritance)
{
	static int32 UObjectSize = 0x0;
	static int32 AActorSize = 0x0;

	if (UObjectSize == 0x0 || AActorSize == 0x0)
	{
		UObjectSize = StructWrapper(ObjectArray::FindClassFast("Object")).GetSize();
		AActorSize = StructWrapper(ObjectArray::FindClassFast("Actor")).GetSize();
	}

	/* Predefined structs can not be cyclic, unless you did something horribly wrong when defining the predefined struct! */
	if (!Struct.IsUnrealStruct())
		return "Invalid+Fixup+Type";

	/* ToDo: find out if we need to differ between using Unaligned-/Aligned-Size on Inheritance/Members */
	const int32 OwnSize = Struct.GetUnalignedSize();
	const int32 Align = Struct.GetAlignment();

	std::string Name = GetStructPrefixedName(Struct);

	/* For structs the fixup is always the same, so we handle them before classes */
	if (!Struct.IsClass())
		return fmt::format("TStructCycleFixup<struct {}, 0x{:04X}, 0x{:02X}>", Name, OwnSize, Align);

	const bool bIsActor = Struct.GetUnrealStruct().IsA(EClassCastFlags::Actor);

	if (bIsActor)
		return fmt::format("TActorBasedCycleFixup<class {}, 0x{:04X}, 0x{:02X}>", Name, (OwnSize - AActorSize), Align);

	return fmt::format("TObjectBasedCycleFixup<class {}, 0x{:04X}, 0x{:02X}>", Name, (OwnSize - UObjectSize), Align);
}

std::string CppGenerator::GetMemberTypeString(const PropertyWrapper& MemberWrapper, int32 PackageIndex, bool bAllowForConstPtrMembers)
{
	if (!MemberWrapper.IsUnrealProperty())
	{
		if (MemberWrapper.IsStatic() && !MemberWrapper.IsZeroSizedMember())
			return "static " + MemberWrapper.GetType();

		return MemberWrapper.GetType();
	}

	return GetMemberTypeString(MemberWrapper.GetUnrealProperty(), PackageIndex, bAllowForConstPtrMembers);
}

std::string CppGenerator::GetMemberTypeString(UEProperty Member, int32 PackageIndex, bool bAllowForConstPtrMembers)
{
	static auto IsMemberPtr = [](UEProperty Mem) -> bool
	{
		if (Mem.IsA(EClassCastFlags::ClassProperty))
			return !Mem.HasPropertyFlags(EPropertyFlags::UObjectWrapper);

		return Mem.IsA(EClassCastFlags::ObjectProperty);
	};

	if (bAllowForConstPtrMembers && Member.HasPropertyFlags(EPropertyFlags::ConstParm) && IsMemberPtr(Member))
		return "const " + GetMemberTypeStringWithoutConst(Member, PackageIndex);

	return GetMemberTypeStringWithoutConst(Member, PackageIndex);
}

std::string CppGenerator::GetMemberTypeStringWithoutConst(UEProperty Member, int32 PackageIndex, bool* bOutIsUnknownProperty)
{
	auto [Class, FieldClass] = Member.GetClass();

	EClassCastFlags Flags = Class ? Class.GetCastFlags() : FieldClass.GetCastFlags();

	if (Flags & EClassCastFlags::ByteProperty)
	{
		if (UEEnum Enum = Member.Cast<UEByteProperty>().GetEnum())
			return GetEnumPrefixedName(Enum);

		return "uint8";
	}
	else if (Flags & EClassCastFlags::UInt16Property)
	{
		return "uint16";
	}
	else if (Flags & EClassCastFlags::UInt32Property)
	{
		return "uint32";
	}
	else if (Flags & EClassCastFlags::UInt64Property)
	{
		return "uint64";
	}
	else if (Flags & EClassCastFlags::Int8Property)
	{
		return "int8";
	}
	else if (Flags & EClassCastFlags::Int16Property)
	{
		return "int16";
	}
	else if (Flags & EClassCastFlags::IntProperty)
	{
		return "int32";
	}
	else if (Flags & EClassCastFlags::Int64Property)
	{
		return "int64";
	}
	else if (Flags & EClassCastFlags::FloatProperty)
	{
		return "float";
	}
	else if (Flags & EClassCastFlags::DoubleProperty)
	{
		return "double";
	}
	else if (Flags & EClassCastFlags::ClassProperty)
	{
		if (Member.HasPropertyFlags(EPropertyFlags::UObjectWrapper))
			return fmt::format("TSubclassOf<class {}>", GetStructPrefixedName(Member.Cast<UEClassProperty>().GetMetaClass()));

		return "class UClass*";
	}
	else if (Flags & EClassCastFlags::NameProperty)
	{
		return "class FName";
	}
	else if (Flags & EClassCastFlags::StrProperty)
	{
		return "class FString";
	}
	else if (Flags & EClassCastFlags::TextProperty)
	{
		return "class FText";
	}
	else if (Flags & EClassCastFlags::BoolProperty)
	{
		return Member.Cast<UEBoolProperty>().IsNativeBool() ? "bool" : GetTypeFromSize(Member.GetSize());
	}
	else if (Flags & EClassCastFlags::StructProperty)
	{
		const StructWrapper& UnderlayingStruct = Member.Cast<UEStructProperty>().GetUnderlayingStruct();

		if (UnderlayingStruct.IsCyclicWithPackage(PackageIndex)) [[unlikely]]
			return fmt::format("{}", GetCycleFixupType(UnderlayingStruct, false));

		return fmt::format("struct {}", GetStructPrefixedName(UnderlayingStruct));
	}
	else if (Flags & EClassCastFlags::ArrayProperty)
	{
		return fmt::format("TArray<{}>", GetMemberTypeStringWithoutConst(Member.Cast<UEArrayProperty>().GetInnerProperty(), PackageIndex));
	}
	else if (Flags & EClassCastFlags::WeakObjectProperty)
	{
		if (UEClass PropertyClass = Member.Cast<UEWeakObjectProperty>().GetPropertyClass())
			return fmt::format("TWeakObjectPtr<class {}>", GetStructPrefixedName(PropertyClass));

		return "TWeakObjectPtr<class UObject>";
	}
	else if (Flags & EClassCastFlags::LazyObjectProperty)
	{
		if (UEClass PropertyClass = Member.Cast<UELazyObjectProperty>().GetPropertyClass())
			return fmt::format("TLazyObjectPtr<class {}>", GetStructPrefixedName(PropertyClass));

		return "TLazyObjectPtr<class UObject>";
	}
	else if (Flags & EClassCastFlags::SoftClassProperty)
	{
		if (UEClass PropertyClass = Member.Cast<UESoftClassProperty>().GetPropertyClass())
			return fmt::format("TSoftClassPtr<class {}>", GetStructPrefixedName(PropertyClass));

		return "TSoftClassPtr<class UObject>";
	}
	else if (Flags & EClassCastFlags::SoftObjectProperty)
	{
		if (UEClass PropertyClass = Member.Cast<UESoftObjectProperty>().GetPropertyClass())
			return fmt::format("TSoftObjectPtr<class {}>", GetStructPrefixedName(PropertyClass));

		return "TSoftObjectPtr<class UObject>";
	}
	else if (Flags & EClassCastFlags::ObjectProperty)
	{
		if (UEClass PropertyClass = Member.Cast<UEObjectProperty>().GetPropertyClass())
			return fmt::format("class {}*", GetStructPrefixedName(PropertyClass));

		return "class UObject*";
	}
	else if (Settings::EngineCore::bEnableEncryptedObjectPropertySupport && Flags & EClassCastFlags::ObjectPropertyBase)
	{
		if (UEClass PropertyClass = Member.Cast<UEObjectProperty>().GetPropertyClass())
			return fmt::format("TEncryptedObjPtr<class {}>", GetStructPrefixedName(PropertyClass));

		return "TEncryptedObjPtr<class UObject>";
	}
	else if (Flags & EClassCastFlags::MapProperty)
	{
		UEMapProperty MemberAsMapProperty = Member.Cast<UEMapProperty>();

		return fmt::format("TMap<{}, {}>", GetMemberTypeStringWithoutConst(MemberAsMapProperty.GetKeyProperty(), PackageIndex), GetMemberTypeStringWithoutConst(MemberAsMapProperty.GetValueProperty(), PackageIndex));
	}
	else if (Flags & EClassCastFlags::SetProperty)
	{
		return fmt::format("TSet<{}>", GetMemberTypeStringWithoutConst(Member.Cast<UESetProperty>().GetElementProperty(), PackageIndex));
	}
	else if (Flags & EClassCastFlags::EnumProperty)
	{
		if (UEEnum Enum = Member.Cast<UEEnumProperty>().GetEnum())
			return GetEnumPrefixedName(Enum);

		return GetMemberTypeStringWithoutConst(Member.Cast<UEEnumProperty>().GetUnderlayingProperty(), PackageIndex);
	}
	else if (Flags & EClassCastFlags::InterfaceProperty)
	{
		if (UEClass PropertyClass = Member.Cast<UEInterfaceProperty>().GetPropertyClass())
			return fmt::format("TScriptInterface<class {}>", GetStructPrefixedName(PropertyClass));

		return "TScriptInterface<class IInterface>";
	}
	else if (Flags & EClassCastFlags::DelegateProperty)
	{
		if (UEFunction SignatureFunc = Member.Cast<UEDelegateProperty>().GetSignatureFunction()) [[likely]]
			return fmt::format("TDelegate<{}>", GetFunctionSignature(SignatureFunc));

		return "TDelegate<void()>";
	}
	else if (Flags & EClassCastFlags::MulticastInlineDelegateProperty)
	{
		if (UEFunction SignatureFunc = Member.Cast<UEMulticastInlineDelegateProperty>().GetSignatureFunction()) [[likely]]
			return fmt::format("TMulticastInlineDelegate<{}>", GetFunctionSignature(SignatureFunc));

		return "TMulticastInlineDelegate<void()>";
	}
	else if (Flags & EClassCastFlags::FieldPathProperty)
	{
		if (Settings::Internal::bIsObjPtrInsteadOfFieldPathProperty)
		{
			if (UEClass PropertyClass = Member.Cast<UEObjectProperty>().GetPropertyClass())
				return fmt::format("class {}*", GetStructPrefixedName(PropertyClass));

			return "class UObject*";
		}

		return fmt::format("TFieldPath<class {}>", Member.Cast<UEFieldPathProperty>().GetFieldClass().GetCppName());
	}
	else if (Flags & EClassCastFlags::OptionalProperty)
	{
		UEProperty ValueProperty = Member.Cast<UEOptionalProperty>().GetValueProperty();

		/* Check if there is an additional 'bool' flag in the TOptional to check if the value is set */
		if (Member.GetSize() > ValueProperty.GetSize()) [[likely]]
			return fmt::format("TOptional<{}>", GetMemberTypeStringWithoutConst(ValueProperty, PackageIndex));

		return fmt::format("TOptional<{}, true>", GetMemberTypeStringWithoutConst(ValueProperty, PackageIndex));
	}
	else if (Flags & EClassCastFlags::Utf8StrProperty)
	{
		return "FUtf8String";
	}
	else if (Flags & EClassCastFlags::AnsiStrProperty)
	{
		return "FUtf8String";
	}
	else
	{
		if (bOutIsUnknownProperty)
			*bOutIsUnknownProperty = true;

		/* When changing this also change 'GetUnknownProperties()' */
		return (Class ? Class.GetCppName() : FieldClass.GetCppName()) + "_";
	}
}

std::string CppGenerator::GetFunctionSignature(UEFunction Func)
{
	std::string RetType = "void";

	std::string OutParameters;

	bool bIsFirstParam = true;

	std::vector<UEProperty> Params = Func.GetProperties();
	std::sort(Params.begin(), Params.end(), CompareUnrealProperties);

	for (UEProperty Param : Params)
	{
		std::string Type = GetMemberTypeString(Param);

		const bool bIsConst = Param.HasPropertyFlags(EPropertyFlags::ConstParm);

		if (Param.HasPropertyFlags(EPropertyFlags::ReturnParm))
		{
			if (bIsConst)
				RetType = "const " + Type;

			continue;
		}

		const bool bIsRef = Param.HasPropertyFlags(EPropertyFlags::ReferenceParm);
		const bool bIsOut = bIsRef || Param.HasPropertyFlags(EPropertyFlags::OutParm);
		const bool bIsMoveType = Param.IsType(EClassCastFlags::StructProperty | EClassCastFlags::ArrayProperty | EClassCastFlags::StrProperty | EClassCastFlags::MapProperty | EClassCastFlags::SetProperty);

		if (bIsConst && (!bIsOut || bIsRef))
			Type = "const " + Type;

		if (bIsOut)
			Type += bIsRef ? '&' : '*';

		if (!bIsOut && !bIsRef && bIsMoveType)
		{
			Type += "&";

			if (!bIsConst)
				Type = "const " + Type;
		}

		std::string ParamName = Param.GetValidName();

		if (!bIsFirstParam)
			OutParameters += ", ";

		OutParameters += Type + " " + ParamName;

		bIsFirstParam = false;
	}

	return RetType + "(" + OutParameters + ")";
}


std::unordered_map<std::string, UEProperty> CppGenerator::GetUnknownProperties()
{
	std::unordered_map<std::string, UEProperty> PropertiesWithNames;

	for (UEObject Obj : ObjectArray())
	{
		if (!Obj.IsA(EClassCastFlags::Struct))
			continue;

		for (UEProperty Prop : Obj.Cast<UEStruct>().GetProperties())
		{
			bool bIsUnknownProperty = false;
			const std::string TypeName = GetMemberTypeStringWithoutConst(Prop, -1, &bIsUnknownProperty);

			if (bIsUnknownProperty)
				PropertiesWithNames[TypeName] = Prop;
		}
	}

	return PropertiesWithNames;
}

void CppGenerator::GeneratePropertyFixupFile(StreamType& PropertyFixup)
{
	WriteFileHead(PropertyFixup, nullptr, EFileType::PropertyFixup, "PROPERTY-FIXUP");

	std::unordered_map<std::string, UEProperty> UnknownProperties = GetUnknownProperties();

	for (const auto& [Name, Property] : UnknownProperties)
	{
		PropertyFixup << fmt::format("\nclass alignas(0x{:02X}) {}\n{{\n\tuint8_t Pad[0x{:X}];\n}};\n", Property.GetAlignment(), Name, Property.GetSize());
	}

	WriteFileEnd(PropertyFixup, EFileType::PropertyFixup);
}

void CppGenerator::GenerateEnumFwdDeclarations(StreamType& ClassOrStructFile, PackageInfoHandle Package, bool bIsClassFile)
{
	const std::vector<std::pair<int32, bool>>& FwdDeclarations = Package.GetEnumForwardDeclarations();

	for (const auto [EnumIndex, bIsForClassFile] : FwdDeclarations)
	{
		if (bIsForClassFile != bIsClassFile)
			continue;

		EnumWrapper Enum = EnumWrapper(ObjectArray::GetByIndex<UEEnum>(EnumIndex));

		ClassOrStructFile << fmt::format("enum class {} : {};\n", GetEnumPrefixedName(Enum), GetEnumUnderlayingType(Enum));
	}
}

void CppGenerator::GenerateNameCollisionsInl(StreamType& NameCollisionsFile)
{
	namespace CppSettings = Settings::CppGenerator;

	WriteFileHead(NameCollisionsFile, nullptr, EFileType::NameCollisionsInl, "FORWARD DECLARATIONS");

	const StructManager::OverrideMapType& StructInfoMap = StructManager::GetStructInfos();
	const EnumManager::OverrideMaptType& EnumInfoMap = EnumManager::GetEnumInfos();

	std::unordered_map<int32 /* PackageIdx */, std::pair<std::string, int32>> PackagesAndForwardDeclarations;

	for (const auto& [Index, Info] : StructInfoMap)
	{
		if (StructManager::IsStructNameUnique(Info.Name))
			continue;

		UEStruct Struct = ObjectArray::GetByIndex<UEStruct>(Index);

		if (Struct.IsA(EClassCastFlags::Function))
			continue;

		auto& [ForwardDeclarations, Count] = PackagesAndForwardDeclarations[Struct.GetPackageIndex()];

		ForwardDeclarations += fmt::format("\t{} {};\n", Struct.IsA(EClassCastFlags::Class) ? "class" : "struct", Struct.GetCppName());
		Count++;
	}

	for (const auto& [Index, Info] : EnumInfoMap)
	{
		if (EnumManager::IsEnumNameUnique(Info))
			continue;

		UEEnum Enum = ObjectArray::GetByIndex<UEEnum>(Index);

		auto& [ForwardDeclarations, Count] = PackagesAndForwardDeclarations[Enum.GetPackageIndex()];

		ForwardDeclarations += fmt::format("\tenum class {} : {};\n", Enum.GetEnumPrefixedName(), GetEnumUnderlayingType(Enum));
		Count++;
	}

	bool bHasSingleLineForwardDeclarations = false;

	for (const auto& [PackageIndex, ForwardDeclarations] : PackagesAndForwardDeclarations)
	{
		std::string ForwardDeclString = ForwardDeclarations.first.substr(0, ForwardDeclarations.first.size() - 1);
		std::string PackageName = PackageManager::GetName(PackageIndex);

		/* Only print packages with a single forward declaration at first */
		if (ForwardDeclarations.second > 1)
			continue;

		bHasSingleLineForwardDeclarations = true;

		NameCollisionsFile << fmt::format("\nnamespace {} {{ {} }}\n", PackageName, ForwardDeclString.c_str() + 1);
	}

	if (bHasSingleLineForwardDeclarations)
		NameCollisionsFile << "\n";

	for (const auto& [PackageIndex, ForwardDeclarations] : PackagesAndForwardDeclarations)
	{
		std::string ForwardDeclString = ForwardDeclarations.first.substr(0, ForwardDeclarations.first.size() - 1);
		std::string PackageName = PackageManager::GetName(PackageIndex);

		/* Now print all packages with several forward declarations */
		if (ForwardDeclarations.second <= 1)
			continue;

		NameCollisionsFile << fmt::format(R"(
namespace {}
{{
{}
}}
)", PackageName, ForwardDeclString);
	}

	WriteFileEnd(NameCollisionsFile, EFileType::NameCollisionsInl);
}

void CppGenerator::GenerateDebugAssertions(StreamType& AssertionStream)
{
	WriteFileHead(AssertionStream, nullptr, EFileType::DebugAssertions, "Debug assertions to verify member-offsets and struct-sizes");

	auto GenerateAssertionsForStruct = [](StreamType& AssertionStream, const StructWrapper& Struct, const std::string& ParamStructName = "")
	{
		const std::string UniquePrefixedName = ParamStructName.empty() ? GetStructPrefixedName(Struct) : ParamStructName;

		AssertionStream << fmt::format("\\\n/* {} {} */ \\\n", (Struct.IsClass() ? "class" : "struct"), UniquePrefixedName);

		// Alignment assertions
		AssertionStream << fmt::format("static_assert(alignof({}) == 0x{:06X}); \\\n", UniquePrefixedName, Struct.GetAlignment());

		const int32 StructSize = Struct.GetSize();

		// Size assertions
		AssertionStream << fmt::format("static_assert(sizeof({}) == 0x{:06X}); \\\n", UniquePrefixedName, (StructSize > 0x0 ? StructSize : 0x1));

		AssertionStream << "\\\n";

		// Member offset assertions
		const MemberManager Members = Struct.GetMembers();

		for (const PropertyWrapper& Member : Members.IterateMembers())
		{
			if (Member.IsStatic() || Member.IsZeroSizedMember() || Member.IsBitField())
				continue;

			AssertionStream << fmt::format("static_assert(offsetof({}, {}) == 0x{:06X}); \\\n", UniquePrefixedName, Member.GetName(), Member.GetOffset());
		}

		AssertionStream << "\\\n";
	};

	DependencyManager::OnVisitCallbackType GenerateStructAssertionsCallback = [&AssertionStream, GenerateAssertionsForStruct](int32 Index) -> void
	{
		GenerateAssertionsForStruct(AssertionStream, ObjectArray::GetByIndex<UEStruct>(Index));
	};

	DependencyManager::OnVisitCallbackType GenerateParamStructAssertionsCallback = [&AssertionStream, GenerateAssertionsForStruct](int32 ClassIndex) -> void
	{
		const StructWrapper Class = ObjectArray::GetByIndex<UEClass>(ClassIndex);

		const MemberManager Members = Class.GetMembers();

		for (const FunctionWrapper& Func : Members.IterateFunctions())
		{
			if (Func.GetFunctionFlags() & EFunctionFlags::Delegate)
				continue;

			if (!Func.IsPredefined() && Func.GetParamStructSize() > 0x0)
				GenerateAssertionsForStruct(AssertionStream, Func.AsStruct(), Func.GetParamStructName());
		}
	};

	for (PackageInfoHandle Package : PackageManager::IterateOverPackageInfos())
	{
		const std::string PackageName = Package.GetName();

		if (Package.HasStructs())
		{
			const DependencyManager& Structs = Package.GetSortedStructs();

			AssertionStream << fmt::format("\n#define {}_STRUCTS_{} \\\n", Settings::Debug::AssertionMacroPrefix, PackageName);

			Structs.VisitAllNodesWithCallback(GenerateStructAssertionsCallback);
		}

		if (Package.HasClasses())
		{
			const DependencyManager& Classes = Package.GetSortedClasses();

			AssertionStream << fmt::format("\n#define {}_CLASSES_{} \\\n", Settings::Debug::AssertionMacroPrefix, PackageName);
			Classes.VisitAllNodesWithCallback(GenerateStructAssertionsCallback);

			AssertionStream << fmt::format("\n#define {}_PARAMS_{} \\\n", Settings::Debug::AssertionMacroPrefix, PackageName);
			Classes.VisitAllNodesWithCallback(GenerateParamStructAssertionsCallback);
		}
	}

	WriteFileEnd(AssertionStream, EFileType::DebugAssertions);
}

void CppGenerator::GenerateSDKHeader(StreamType& SdkHpp)
{
	WriteFileHead(SdkHpp, nullptr, EFileType::SdkHpp, "Includes the entire SDK. Include files directly for faster compilation!");


	auto ForEachElementCallback = [&SdkHpp](const PackageManagerIterationParams& OldParams, const PackageManagerIterationParams& NewParams, bool bIsStruct) -> void
	{
		PackageInfoHandle CurrentPackage = PackageManager::GetInfo(NewParams.RequiredPackage);

		const bool bHasClassesFile = CurrentPackage.HasClasses();
		const bool bHasStructsFile = (CurrentPackage.HasStructs() || CurrentPackage.HasEnums());

		if (bIsStruct && bHasStructsFile)
			SdkHpp << fmt::format("#include \"SDK/{}_structs.hpp\"\n", CurrentPackage.GetName());

		if (!bIsStruct && bHasClassesFile)
			SdkHpp << fmt::format("#include \"SDK/{}_classes.hpp\"\n", CurrentPackage.GetName());
	};

	PackageManager::IterateDependencies(ForEachElementCallback);


	WriteFileEnd(SdkHpp, EFileType::SdkHpp);
}

void CppGenerator::WriteFileHead(StreamType& File, PackageInfoHandle Package, EFileType Type, const std::string& CustomFileComment, const std::string& CustomIncludes)
{
	namespace CppSettings = Settings::CppGenerator;

	/* Write the utf8 BOM to indicate that this is a utf8 encoded file. */
	File << "\xEF\xBB\xBF";

	File << R"(#pragma once

/*
* SDK generated by Dumper-7
*
* https://github.com/Encryqed/Dumper-7
*/
)";

	if (Type == EFileType::SdkHpp)
		File << fmt::format("\n// {}\n// {}\n", Settings::Generator::GameName, Settings::Generator::GameVersion);
	

	File << fmt::format("\n// {}\n\n", Package.IsValidHandle() ? fmt::format("Package: {}", Package.GetName()) : CustomFileComment);


	if (!CustomIncludes.empty())
		File << CustomIncludes + "\n";

	if (Type != EFileType::BasicHpp && Type != EFileType::NameCollisionsInl && Type != EFileType::PropertyFixup && Type != EFileType::SdkHpp && Type != EFileType::DebugAssertions && Type != EFileType::UnrealContainers && Type != EFileType::UnicodeLib)
		File << "#include \"Basic.hpp\"\n";

	if (Type == EFileType::SdkHpp)
		File << "#include \"SDK/Basic.hpp\"\n";

	if (Type == EFileType::BasicHpp)
	{
		File << "#include \"../PropertyFixup.hpp\"\n";
		File << "#include \"../UnrealContainers.hpp\"\n";

		if constexpr (Settings::Debug::bGenerateAssertionFile)
		{
			File << "#include \"../Assertions.inl\"\n";
		}

		if constexpr (Settings::CppGenerator::XORStringInclude)
		{
			File << fmt::format("#include \"{}\"\n", Settings::CppGenerator::XORStringInclude);
		}
	}

	if (Type == EFileType::BasicCpp)
	{
		File << "\n#include \"CoreUObject_classes.hpp\"";
		File << "\n#include \"CoreUObject_structs.hpp\"\n";
		File << "#include \"Engine_classes.hpp\"\n";
	}

	if (Type == EFileType::Functions && (Package.HasClasses() || Package.HasParameterStructs()))
	{
		std::string PackageName = Package.GetName();

		File << "\n";

		if (Package.HasClasses())
			File << fmt::format("#include \"{}_classes.hpp\"\n", PackageName);

		if (Package.HasParameterStructs())
			File << fmt::format("#include \"{}_parameters.hpp\"\n", PackageName);

		File << "\n";
	}
	else if (Package.IsValidHandle())
	{
		File << "\n";

		const DependencyInfo& Dep = Package.GetPackageDependencies();
		const DependencyListType& CurrentDependencyList = Type == EFileType::Structs ? Dep.StructsDependencies : (Type == EFileType::Classes ? Dep.ClassesDependencies : Dep.ParametersDependencies);

		bool bAddNewLine = false;

		for (const auto& [PackageIndex, Requirements] : CurrentDependencyList)
		{
			bAddNewLine = true;

			std::string DependencyName = PackageManager::GetName(PackageIndex);

			if (Requirements.bShouldIncludeStructs)
				File << fmt::format("#include \"{}_structs.hpp\"\n", DependencyName);

			if (Requirements.bShouldIncludeClasses)
				File << fmt::format("#include \"{}_classes.hpp\"\n", DependencyName);
		}

		if (bAddNewLine)
			File << "\n";
	}

	if (Type == EFileType::SdkHpp || Type == EFileType::NameCollisionsInl || Type == EFileType::UnrealContainers || Type == EFileType::UnicodeLib)
		return; /* No namespace or packing in SDK.hpp or NameCollisions.inl */


	File << "\n";

	if constexpr (Platform::Is32Bit())
	{
		File << "#pragma pack(push, 0x4)\n";
	}

	if (!Settings::Config::SDKNamespaceName.empty())
	{
		File << "SDK_NAMESPACE_START\n";
		
		if (Type == EFileType::Parameters && CppSettings::ParamNamespaceName)
			File << "SDK_PARAM_NAMESPACE_START\n";
	}
	else if constexpr (CppSettings::ParamNamespaceName)
	{
		if (Type == EFileType::Parameters)
			File << "SDK_PARAM_NAMESPACE_START\n";
	}
}

void CppGenerator::WriteFileEnd(StreamType& File, EFileType Type)
{
	namespace CppSettings = Settings::CppGenerator;

	if (Type == EFileType::SdkHpp || Type == EFileType::NameCollisionsInl || Type == EFileType::UnrealContainers || Type == EFileType::UnicodeLib)
		return; /* No namespace or packing in SDK.hpp or NameCollisions.inl */

	if (!Settings::Config::SDKNamespaceName.empty())
	{
		File << "\n";

		if (Type == EFileType::Parameters && CppSettings::ParamNamespaceName)
			File << "SDK_PARAM_NAMESPACE_END\n";
		
		File << "SDK_NAMESPACE_END\n";
	}
	else if constexpr (CppSettings::ParamNamespaceName)
	{
		if (Type == EFileType::Parameters)
			File << "\nSDK_PARAM_NAMESPACE_START\n";
	}

	if constexpr (Platform::Is32Bit())
	{
		File << "#pragma pack(pop)\n";
	}
}

void CppGenerator::Generate()
{
	// Generate SDK.hpp with sorted packages
	StreamType SdkHpp(MainFolder / "SDK.hpp");
	GenerateSDKHeader(SdkHpp);

	// Generate PropertyFixup.hpp
	StreamType PropertyFixup(MainFolder / "PropertyFixup.hpp");
	GeneratePropertyFixupFile(PropertyFixup);

	// Generate NameCollisions.inl file containing forward declarations for classes in namespaces (potentially requires lock)
	StreamType NameCollisionsInl(MainFolder / "NameCollisions.inl");
	GenerateNameCollisionsInl(NameCollisionsInl);

	// Generate UnrealContainers.hpp
	StreamType UnrealContainers(MainFolder / "UnrealContainers.hpp");
	GenerateUnrealContainers(UnrealContainers);

	StreamType DebugAssertions;

	if constexpr (Settings::Debug::bGenerateAssertionFile)
	{
		// Generate Assertions.inl file containing assertions on struct-size, struct-align and member offsets
		DebugAssertions.open(MainFolder / "Assertions.inl");
		WriteFileHead(DebugAssertions, nullptr, EFileType::DebugAssertions, "Debug assertions to verify member-offsets and struct-sizes");

		//GenerateDebugAssertions(DebugAssertions);
	}

	// Generate Basic.hpp and Basic.cpp files
	StreamType BasicHpp(Subfolder / "Basic.hpp");
	StreamType BasicCpp(Subfolder / "Basic.cpp");
	GenerateBasicFiles(BasicHpp, BasicCpp, (Settings::Debug::bGenerateAssertionFile ? DebugAssertions : BasicHpp));


	// Generates all packages and writes them to files
	for (PackageInfoHandle Package : PackageManager::IterateOverPackageInfos())
	{
		if (Package.IsEmpty())
			continue;

		const std::string FileName = Settings::CppGenerator::FilePrefix + Package.GetName();
		const std::u8string U8FileName = reinterpret_cast<const std::u8string&>(FileName);

		StreamType ClassesFile;
		StreamType StructsFile;
		StreamType ParametersFile;
		StreamType FunctionsFile;

		/* Create files and handles namespaces and includes */
		if (Package.HasClasses())
		{
			ClassesFile = StreamType(Subfolder / (U8FileName + u8"_classes.hpp"));

			if (!ClassesFile.is_open())
				LogError("%s", fmt::format("Error opening file \"{}\"", (FileName + "_classes.hpp")).c_str());

			WriteFileHead(ClassesFile, Package, EFileType::Classes);

			/* Write enum foward declarations before all of the classes */
			GenerateEnumFwdDeclarations(ClassesFile, Package, true);
		}

		if (Package.HasStructs() || Package.HasEnums())
		{
			StructsFile = StreamType(Subfolder / (U8FileName + u8"_structs.hpp"));

			if (!StructsFile.is_open())
				LogError("%s", fmt::format("Error opening file \"{}\"", (FileName + "_structs.hpp")).c_str());

			WriteFileHead(StructsFile, Package, EFileType::Structs);

			/* Write enum foward declarations before all of the structs */
			GenerateEnumFwdDeclarations(StructsFile, Package, false);
		}

		if (Package.HasParameterStructs())
		{
			ParametersFile = StreamType(Subfolder / (U8FileName + u8"_parameters.hpp"));

			if (!ParametersFile.is_open())
				LogError("%s", fmt::format("Error opening file \"{}\"", (FileName + "_parameters.hpp")).c_str());

			WriteFileHead(ParametersFile, Package, EFileType::Parameters);
		}

		if (Package.HasFunctions())
		{
			FunctionsFile = StreamType(Subfolder / (U8FileName + u8"_functions.cpp"));

			if (!FunctionsFile.is_open())
				LogError("%s", fmt::format("Error opening file \"{}\"", (FileName + "_functions.cpp")).c_str());

			WriteFileHead(FunctionsFile, Package, EFileType::Functions);
		}

		const int32 PackageIndex = Package.GetIndex();

		/* 
		* Generate classes/structs/enums/functions directly into the respective files
		* 
		* Note: Some filestreams aren't opened but passed as parameters anyway because the function demands it, they are not used if they are closed
		*/
		for (int32 EnumIdx : Package.GetEnums())
		{
			GenerateEnum(ObjectArray::GetByIndex<UEEnum>(EnumIdx), StructsFile);
		}

		if (Package.HasStructs())
		{
			const DependencyManager& Structs = Package.GetSortedStructs();

			StreamType& FileForAssertions = Settings::Debug::bGenerateAssertionFile ? DebugAssertions : StructsFile;

			DependencyManager::OnVisitCallbackType GenerateStructCallback = [&](int32 Index) -> void
			{
				GenerateStruct(ObjectArray::GetByIndex<UEStruct>(Index), StructsFile, FunctionsFile, ParametersFile, FileForAssertions, PackageIndex);
			};

			Structs.VisitAllNodesWithCallback(GenerateStructCallback);
		}

		if (Package.HasClasses())
		{
			const DependencyManager& Classes = Package.GetSortedClasses();

			StreamType& FileForAssertions = Settings::Debug::bGenerateAssertionFile ? DebugAssertions : ClassesFile;

			DependencyManager::OnVisitCallbackType GenerateClassCallback = [&](int32 Index) -> void
			{
				GenerateStruct(ObjectArray::GetByIndex<UEStruct>(Index), ClassesFile, FunctionsFile, ParametersFile, FileForAssertions, PackageIndex);
			};

			Classes.VisitAllNodesWithCallback(GenerateClassCallback);
		}


		/* Closes any namespaces if required */
		if (Package.HasClasses())
			WriteFileEnd(ClassesFile, EFileType::Classes);

		if (Package.HasStructs() || Package.HasEnums())
			WriteFileEnd(StructsFile, EFileType::Structs);

		if (Package.HasParameterStructs())
			WriteFileEnd(ParametersFile, EFileType::Parameters);

		if (Package.HasFunctions())
			WriteFileEnd(FunctionsFile, EFileType::Functions);
	}

	if constexpr (Settings::Debug::bGenerateAssertionFile)
	{
		WriteFileEnd(DebugAssertions, EFileType::DebugAssertions);
	}
}

void CppGenerator::InitPredefinedMembers()
{
	static auto SortMembers = [](std::vector<PredefinedMember>& Members) -> void
	{
		std::sort(Members.begin(), Members.end(), ComparePredefinedMembers);
	};

	/* Assumes Members to be sorted */
	static auto InitStructSize = [](PredefinedStruct& Struct) -> void
	{
		if (Struct.Size > 0x0)
			return;

		if (Struct.Properties.empty() && Struct.Super)
			Struct.Size = Struct.Super->Size;

		const PredefinedMember& LastMember = Struct.Properties[Struct.Properties.size() - 1];
		Struct.Size = LastMember.Offset + LastMember.Size;
	};


	if (Off::InSDK::ULevel::Actors != -1)
	{
		UEClass Level = ObjectArray::FindClassFast("Level");

		if (Level == nullptr)
			Level = ObjectArray::FindClassFast("level");

		PredefinedElements& ULevelPredefs = PredefinedMembers[Level.GetIndex()];
		ULevelPredefs.Members =
		{
			PredefinedMember {
				.Comment = "THIS IS THE ARRAY YOU'RE LOOKING FOR! [NOT AUTO-GENERATED PROPERTY]",
				.Type = "class TArray<class AActor*>", .Name = "Actors", .Offset = Off::InSDK::ULevel::Actors, .Size = sizeof(TArray<int>), .ArrayDim = 0x1, .Alignment = alignof(TArray<int>),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
		};
	}

	UEClass DataTable = ObjectArray::FindClassFast("DataTable");

	PredefinedElements& UDataTablePredefs = PredefinedMembers[DataTable.GetIndex()];
	UDataTablePredefs.Members =
	{
		PredefinedMember {
			.Comment = "So, here's a RowMap. Good luck with it.",
			.Type = "TMap<class FName, uint8*>", .Name = "RowMap", .Offset = Off::InSDK::UDataTable::RowMap, .Size = sizeof(TMap<int, int>), .ArrayDim = 0x1, .Alignment = alignof(TMap<int, int>),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	PredefinedElements& UObjectPredefs = PredefinedMembers[ObjectArray::FindClassFast("Object").GetIndex()];
	UObjectPredefs.Members = 
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "inline class TUObjectArrayWrapper", .Name = "GObjects", .Offset = 0x0, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = true, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "void*", .Name = "VTable", .Offset = Off::UObject::Vft, .Size = sizeof(void**), .ArrayDim = 0x1, .Alignment = alignof(void**),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "EObjectFlags", .Name = "Flags", .Offset = Off::UObject::Flags, .Size = sizeof(EObjectFlags), .ArrayDim = 0x1, .Alignment = alignof(EObjectFlags),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "int32", .Name = "Index", .Offset = Off::UObject::Index, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "class UClass*", .Name = "Class", .Offset = Off::UObject::Class, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "class FName", .Name = "Name", .Offset = Off::UObject::Name, .Size = Off::InSDK::Name::FNameSize, .ArrayDim = 0x1, .Alignment = alignof(int32),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "class UObject*", .Name = "Outer", .Offset = Off::UObject::Outer, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	const UEClass UField = ObjectArray::FindClassFast("Field");
	PredefinedElements& UFieldPredefs = PredefinedMembers[UField.GetIndex()];

	// Starting from UE5.7 UField::Next is reflected and doesn't need to be added manually anymore
	if (!UField.FindMember("Next", EClassCastFlags::ObjectProperty))
	{
		UFieldPredefs.Members.insert(UFieldPredefs.Members.begin(),
			PredefinedMember{
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "class UField*", .Name = "Next", .Offset = Off::UField::Next, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			});
	}

	PredefinedElements& UEnumPredefs = PredefinedMembers[ObjectArray::FindClassFast("Enum").GetIndex()];
	UEnumPredefs.Members =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "class TArray<class TPair<class FName, int64>>", .Name = "Names", .Offset = Off::UEnum::Names, .Size = sizeof(TArray<int>), .ArrayDim = 0x1, .Alignment = alignof(TArray<int>),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	UEClass UStruct = ObjectArray::FindClassFast("Struct");

	if (UStruct == nullptr)
		UStruct = ObjectArray::FindClassFast("struct");

	PredefinedElements& UStructPredefs = PredefinedMembers[UStruct.GetIndex()];
	UStructPredefs.Members =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "int16", .Name = "MinAlignment", .Offset = Off::UStruct::MinAlignment, .Size = sizeof(int16), .ArrayDim = 0x1, .Alignment = alignof(int16),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "int32", .Name = "Size", .Offset = Off::UStruct::Size, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	// Starting from UE5.7 UStruct::SuperStruct is reflected and doesn't need to be added manually anymore
	if (!UStruct.FindMember("SuperStruct", EClassCastFlags::ObjectProperty))
	{
		UStructPredefs.Members.insert(UStructPredefs.Members.begin(),
			PredefinedMember{
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "class UStruct*", .Name = "SuperStruct", .Offset = Off::UStruct::SuperStruct, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			});
	}
	// Starting from UE5.7 UStruct::Children is reflected and doesn't need to be added manually anymore
	if (!UStruct.FindMember("Children", EClassCastFlags::ObjectProperty))
	{
		UStructPredefs.Members.insert(UStructPredefs.Members.begin(),
			PredefinedMember{
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "class UField*", .Name = "Children", .Offset = Off::UStruct::Children, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			});
	}

	if (Settings::Internal::bUseFProperty)
	{
		UStructPredefs.Members.push_back({
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "class FField*", .Name = "ChildProperties", .Offset = Off::UStruct::ChildProperties, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		});
	}

	if (Off::UStruct::StructBaseChain != -1)
	{
		UStructPredefs.Members.push_back({
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "struct FStructBaseChain", .Name = "BaseChain", .Offset = Off::UStruct::StructBaseChain, .Size = sizeof(void*) + sizeof(int32) + sizeof(uint32) /* PAD */, .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		});
	}

	PredefinedElements& UFunctionPredefs = PredefinedMembers[ObjectArray::FindClassFast("Function").GetIndex()];
	UFunctionPredefs.Members =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "using FNativeFuncPtr = void (*)(void* Context, void* TheStack, void* Result)", .Name = "", .Offset = 0x0, .Size = 0x00, .ArrayDim = 0x1, .Alignment = 0x0,
			.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "uint32", .Name = "FunctionFlags", .Offset = Off::UFunction::FunctionFlags, .Size = sizeof(EFunctionFlags), .ArrayDim = 0x1, .Alignment = alignof(EFunctionFlags),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "FNativeFuncPtr", .Name = "ExecFunction", .Offset = Off::UFunction::ExecFunction, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	const UEClass UClass = ObjectArray::FindClassFast("Class");
	PredefinedElements& UClassPredefs = PredefinedMembers[UClass.GetIndex()];
	UClassPredefs.Members =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "EClassCastFlags", .Name = "CastFlags", .Offset = Off::UClass::CastFlags, .Size = sizeof(EClassCastFlags), .ArrayDim = 0x1, .Alignment = alignof(EClassCastFlags),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	// Starting from UE5.7 UClass::ClassDefaultObject is reflected and doesn't need to be added manually anymore
	if (!UClass.FindMember("ClassDefaultObject", EClassCastFlags::ObjectProperty))
	{
		UClassPredefs.Members.insert(UClassPredefs.Members.begin(),
			PredefinedMember{
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "class UObject*", .Name = "ClassDefaultObject", .Offset = Off::UClass::ClassDefaultObject, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			});
	}

	std::string PropertyTypePtr = Settings::Internal::bUseFProperty ? "class FProperty*" : "class UProperty*";

	std::vector<PredefinedMember> PropertyMembers =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "int32", .Name = "ArrayDim", .Offset = Off::Property::ArrayDim, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "int32", .Name = "ElementSize", .Offset = Off::Property::ElementSize, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
			.bIsStatic = false, .bIsZeroSizeMember = false,  .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "uint64", .Name = "PropertyFlags", .Offset = Off::Property::PropertyFlags, .Size = sizeof(uint64), .ArrayDim = 0x1, .Alignment = alignof(uint64),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "int32", .Name = "Offset", .Offset = Off::Property::Offset_Internal, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	std::vector<PredefinedMember> BytePropertyMembers =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "class UEnum*", .Name = "Enum", .Offset = Off::ByteProperty::Enum, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	std::vector<PredefinedMember> BoolPropertyMembers =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "uint8", .Name = "FieldSize", .Offset = Off::BoolProperty::Base, .Size = sizeof(uint8), .ArrayDim = 0x1, .Alignment = alignof(uint8),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "uint8", .Name = "ByteOffset", .Offset = Off::BoolProperty::Base + 0x1, .Size = sizeof(uint8), .ArrayDim = 0x1, .Alignment = alignof(uint8),
			.bIsStatic = false, .bIsZeroSizeMember = false,  .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "uint8", .Name = "ByteMask", .Offset = Off::BoolProperty::Base + 0x2, .Size = sizeof(uint8), .ArrayDim = 0x1, .Alignment = alignof(uint8),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "uint8", .Name = "FieldMask", .Offset = Off::BoolProperty::Base + 0x3, .Size = sizeof(uint8), .ArrayDim = 0x1, .Alignment = alignof(uint8),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	std::vector<PredefinedMember> ObjectPropertyBaseMembers =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "class UClass*", .Name = "PropertyClass", .Offset = Off::ObjectProperty::PropertyClass, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	std::vector<PredefinedMember> ClassPropertyMembers =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "class UClass*", .Name = "MetaClass", .Offset = Off::ClassProperty::MetaClass, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	std::vector<PredefinedMember> StructPropertyMembers =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "class UStruct*", .Name = "Struct", .Offset = Off::StructProperty::Struct, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	std::vector<PredefinedMember> ArrayPropertyMembers =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = PropertyTypePtr, .Name = "InnerProperty", .Offset = Off::ArrayProperty::Inner, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	std::vector<PredefinedMember> DelegatePropertyMembers =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "class UFunction*", .Name = "SignatureFunction", .Offset = Off::DelegateProperty::SignatureFunction, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	std::vector<PredefinedMember> MapPropertyMembers =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = PropertyTypePtr, .Name = "KeyProperty", .Offset = Off::MapProperty::Base, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = PropertyTypePtr, .Name = "ValueProperty", .Offset = Off::MapProperty::Base + (int)sizeof(void*), .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	std::vector<PredefinedMember> SetPropertyMembers =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = PropertyTypePtr, .Name = "ElementProperty", .Offset = Off::SetProperty::ElementProp, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	std::vector<PredefinedMember> EnumPropertyMembers =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = PropertyTypePtr, .Name = "UnderlayingProperty", .Offset = Off::EnumProperty::Base, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "class UEnum*", .Name = "Enum", .Offset = Off::EnumProperty::Base + (int)sizeof(void*), .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	std::vector<PredefinedMember> FieldPathPropertyMembers =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "class FFieldClass*", .Name = "FieldClass", .Offset = Off::FieldPathProperty::FieldClass, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	std::vector<PredefinedMember> OptionalPropertyMembers =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = PropertyTypePtr, .Name = "ValueProperty", .Offset = Off::OptionalProperty::ValueProperty, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	SortMembers(UObjectPredefs.Members);
	SortMembers(UFieldPredefs.Members);
	SortMembers(UEnumPredefs.Members);
	SortMembers(UStructPredefs.Members);
	SortMembers(UFunctionPredefs.Members);
	SortMembers(UClassPredefs.Members);

	SortMembers(PropertyMembers);
	SortMembers(BytePropertyMembers);
	SortMembers(BoolPropertyMembers);
	SortMembers(ObjectPropertyBaseMembers);
	SortMembers(ClassPropertyMembers);
	SortMembers(StructPropertyMembers);
	SortMembers(ArrayPropertyMembers);
	SortMembers(DelegatePropertyMembers);
	SortMembers(MapPropertyMembers);
	SortMembers(SetPropertyMembers);
	SortMembers(FieldPathPropertyMembers);
	SortMembers(OptionalPropertyMembers);

	if (!Settings::Internal::bUseFProperty)
	{
		auto AssignValueIfObjectIsFound = [](const std::string& ClassName, std::vector<PredefinedMember>&& Members) -> void
		{
			if (UEClass Class = ObjectArray::FindClassFast(ClassName))
				PredefinedMembers[Class.GetIndex()].Members = std::move(Members);
		};

		AssignValueIfObjectIsFound("Property", std::move(PropertyMembers));
		AssignValueIfObjectIsFound("ByteProperty", std::move(BytePropertyMembers));
		AssignValueIfObjectIsFound("BoolProperty", std::move(BoolPropertyMembers));
		AssignValueIfObjectIsFound("ObjectPropertyBase", std::move(ObjectPropertyBaseMembers));
		AssignValueIfObjectIsFound("ClassProperty", std::move(ClassPropertyMembers));
		AssignValueIfObjectIsFound("StructProperty", std::move(StructPropertyMembers));
		AssignValueIfObjectIsFound("DelegateProperty", std::move(DelegatePropertyMembers));
		AssignValueIfObjectIsFound("ArrayProperty", std::move(ArrayPropertyMembers));
		AssignValueIfObjectIsFound("MapProperty", std::move(MapPropertyMembers));
		AssignValueIfObjectIsFound("SetProperty", std::move(SetPropertyMembers));
		AssignValueIfObjectIsFound("EnumProperty", std::move(EnumPropertyMembers));

		// FieldPathProperty and OptionalProperty don't exist in UE versions which are using UProperty
		return;
	}
	else
	{
		/* Reserving enough space is required because otherwise the vector could reallocate and invalidate some structs' 'Super' pointer */
		PredefinedStructs.reserve(0x20);

		PredefinedStruct& FFieldClass = PredefinedStructs.emplace_back(PredefinedStruct{
			.UniqueName = "FFieldClass", .Size = 0x0, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = false, .bIsClass = true, .Super = nullptr
		});
		PredefinedStruct& FFieldVariant = PredefinedStructs.emplace_back(PredefinedStruct{
			.UniqueName = "FFieldVariant", .Size = 0x0, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = false, .bIsClass = true, .Super = nullptr
		});

		PredefinedStruct& FField = PredefinedStructs.emplace_back(PredefinedStruct{
			.UniqueName = "FField", .Size = 0x0, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = false, .bIsClass = true, .Super = nullptr
		});

		PredefinedStruct& FProperty = PredefinedStructs.emplace_back(PredefinedStruct{
			.UniqueName = "FProperty", .Size = Off::InSDK::Properties::PropertySize, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = false, .bIsClass = true, .Super = &FField  /* FField */
		});
		PredefinedStruct& FByteProperty = PredefinedStructs.emplace_back(PredefinedStruct{
			.UniqueName = "FByteProperty", .Size = 0x0, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .Super = &FProperty  /* FProperty */
		});
		PredefinedStruct& FBoolProperty = PredefinedStructs.emplace_back(PredefinedStruct{
			.UniqueName = "FBoolProperty", .Size = 0x0, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .Super = &FProperty  /* FProperty */
		});
		PredefinedStruct& FObjectPropertyBase = PredefinedStructs.emplace_back(PredefinedStruct{
			.UniqueName = "FObjectPropertyBase", .Size = 0x0, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = false, .bIsClass = true, .Super = &FProperty  /* FProperty */
		});
		PredefinedStruct& FClassProperty = PredefinedStructs.emplace_back(PredefinedStruct{
			.UniqueName = "FClassProperty", .Size = 0x0, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .Super = &FObjectPropertyBase  /* FObjectPropertyBase */
		});
		PredefinedStruct& FStructProperty = PredefinedStructs.emplace_back(PredefinedStruct{
			.UniqueName = "FStructProperty", .Size = 0x0, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .Super = &FProperty  /* FProperty */
		});
		PredefinedStruct& FArrayProperty = PredefinedStructs.emplace_back(PredefinedStruct{
			.UniqueName = "FArrayProperty", .Size = 0x0, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .Super = &FProperty  /* FProperty */
		});
		PredefinedStruct& FDelegateProperty = PredefinedStructs.emplace_back(PredefinedStruct{
			.UniqueName = "FDelegateProperty", .Size = 0x0, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .Super = &FProperty  /* FProperty */
		});
		PredefinedStruct& FMapProperty = PredefinedStructs.emplace_back(PredefinedStruct{
			.UniqueName = "FMapProperty", .Size = 0x0, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .Super = &FProperty  /* FProperty */
		});
		PredefinedStruct& FSetProperty = PredefinedStructs.emplace_back(PredefinedStruct{
			.UniqueName = "FSetProperty", .Size = 0x0, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .Super = &FProperty  /* FProperty */
		});
		PredefinedStruct& FEnumProperty = PredefinedStructs.emplace_back(PredefinedStruct{
			.UniqueName = "FEnumProperty", .Size = 0x0, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .Super = &FProperty  /* FProperty */
		});
		PredefinedStruct& FFieldPathProperty = PredefinedStructs.emplace_back(PredefinedStruct{
			.UniqueName = "FFieldPathProperty", .Size = 0x0, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .Super = &FProperty  /* FProperty */
		});
		PredefinedStruct& FOptionalProperty = PredefinedStructs.emplace_back(PredefinedStruct{
			.UniqueName = "FOptionalProperty", .Size = 0x0, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .Super = &FProperty  /* FProperty */
		});

		FFieldClass.Properties =
		{
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "FName", .Name = "Name", .Offset = Off::FFieldClass::Name, .Size = Off::InSDK::Name::FNameSize, .ArrayDim = 0x1, .Alignment = alignof(int32),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "uint64", .Name = "Id", .Offset = Off::FFieldClass::Id, .Size = sizeof(uint64), .ArrayDim = 0x1, .Alignment = alignof(uint64),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "uint64", .Name = "CastFlags", .Offset = Off::FFieldClass::CastFlags, .Size = sizeof(uint64), .ArrayDim = 0x1, .Alignment = alignof(uint64),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "EClassFlags", .Name = "ClassFlags", .Offset = Off::FFieldClass::ClassFlags, .Size = sizeof(EClassFlags), .ArrayDim = 0x1, .Alignment = alignof(EClassFlags),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "class FFieldClass*", .Name = "SuperClass", .Offset = Off::FFieldClass::SuperClass, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
		};


		const int32 FFieldVariantSize = Settings::Internal::bUseMaskForFieldOwner ? 0x8 : 0x10;
		const int32 IdentifierOffset = Settings::Internal::bUseMaskForFieldOwner ? 0x0 : 0x8;
		std::string UObjectIdentifierType = (Settings::Internal::bUseMaskForFieldOwner ? "constexpr uint64" : "bool");
		std::string UObjectIdentifierName = (Settings::Internal::bUseMaskForFieldOwner ? "UObjectMask = 0x1" : "bIsUObject");

		FFieldVariant.Properties =
		{
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "using ContainerType = union { class FField* Field; class UObject* Object; }", .Name = "", .Offset = 0x0, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "ContainerType", .Name = "Container", .Offset = 0x0, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = UObjectIdentifierType, .Name = UObjectIdentifierName, .Offset = IdentifierOffset, .Size = 0x01, .ArrayDim = 0x1, .Alignment = 0x8,
				.bIsStatic = Settings::Internal::bUseMaskForFieldOwner, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
		};


		FField.Properties =
		{
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "void*", .Name = "VTable", .Offset = Off::FField::Vft, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "class FFieldClass*", .Name = "ClassPrivate", .Offset = Off::FField::Class, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "FFieldVariant", .Name = "Owner", .Offset = Off::FField::Owner, .Size = FFieldVariantSize, .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "class FField*", .Name = "Next", .Offset = Off::FField::Next, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "FName", .Name = "Name", .Offset = Off::FField::Name, .Size = Off::InSDK::Name::FNameSize, .ArrayDim = 0x1, .Alignment = alignof(int32),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "int32", .Name = "ObjFlags", .Offset = Off::FField::Flags, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
		};

		SortMembers(FFieldClass.Properties);
		SortMembers(FFieldVariant.Properties);

		SortMembers(FField.Properties);

		FProperty.Properties = PropertyMembers;
		FByteProperty.Properties = BytePropertyMembers;
		FBoolProperty.Properties = BoolPropertyMembers;
		FObjectPropertyBase.Properties = ObjectPropertyBaseMembers;
		FClassProperty.Properties = ClassPropertyMembers;
		FStructProperty.Properties = StructPropertyMembers;
		FArrayProperty.Properties = ArrayPropertyMembers;
		FDelegateProperty.Properties = DelegatePropertyMembers;
		FMapProperty.Properties = MapPropertyMembers;
		FSetProperty.Properties = SetPropertyMembers;
		FEnumProperty.Properties = EnumPropertyMembers;
		FFieldPathProperty.Properties = FieldPathPropertyMembers;
		FOptionalProperty.Properties = OptionalPropertyMembers;

		/* Init PredefindedStruct::Size **after** sorting the members */
		InitStructSize(FFieldClass);
		InitStructSize(FFieldVariant);

		InitStructSize(FField);

		InitStructSize(FProperty);
		InitStructSize(FByteProperty);
		InitStructSize(FBoolProperty);
		InitStructSize(FObjectPropertyBase);
		InitStructSize(FClassProperty);
		InitStructSize(FStructProperty);
		InitStructSize(FArrayProperty);
		InitStructSize(FDelegateProperty);
		InitStructSize(FMapProperty);
		InitStructSize(FSetProperty);
		InitStructSize(FEnumProperty);
		InitStructSize(FFieldPathProperty);
		InitStructSize(FOptionalProperty);
	}
}


void CppGenerator::InitPredefinedFunctions()
{
	static auto SortFunctions = [](std::vector<PredefinedFunction>& Functions) -> void
	{
		std::sort(Functions.begin(), Functions.end(), ComparePredefinedFunctions);
	};

	PredefinedElements& UObjectPredefs = PredefinedMembers[ObjectArray::FindClassFast("Object").GetIndex()];

	UObjectPredefs.Functions =
	{
		/* static non-inline functions */
		PredefinedFunction {
			.CustomComment = "Finds a UObject in the global object array by full-name, optionally with ECastFlags to reduce heavy string comparison",
			.ReturnType = "class UObject*", .NameWithParams = "FindObjectImpl(const std::string& FullName, EClassCastFlags RequiredType = EClassCastFlags::None)",
			.NameWithParamsWithoutDefaults = "FindObjectImpl(const std::string& FullName, EClassCastFlags RequiredType)", .Body =
R"({
	for (int i = 0; i < GObjects->Num(); ++i)
	{
		UObject* Object = GObjects->GetByIndex(i);
	
		if (!Object)
			continue;
		
		if (Object->HasTypeFlag(RequiredType) && Object->GetFullName() == FullName)
			return Object;
	}

	return nullptr;
})",
			.bIsStatic = true, .bIsConst = false, .bIsBodyInline = false
		},
		PredefinedFunction {
			.CustomComment = "Finds a UObject in the global object array by name, optionally with ECastFlags to reduce heavy string comparison",
			.ReturnType = "class UObject*", .NameWithParams = "FindObjectFastImpl(const std::string& Name, EClassCastFlags RequiredType = EClassCastFlags::None)",
			.NameWithParamsWithoutDefaults = "FindObjectFastImpl(const std::string& Name, EClassCastFlags RequiredType)", .Body =
R"({
	for (int i = 0; i < GObjects->Num(); ++i)
	{
		UObject* Object = GObjects->GetByIndex(i);
	
		if (!Object)
			continue;
		
		if (Object->HasTypeFlag(RequiredType) && Object->GetName() == Name)
			return Object;
	}

	return nullptr;
})",
			.bIsStatic = true, .bIsConst = false, .bIsBodyInline = false
		},

		/* static inline functions */
		PredefinedFunction {
			.CustomComment = "",
			.CustomTemplateText = "template<typename UEType = UObject>",
			.ReturnType = "UEType*", .NameWithParams = "FindObject(const std::string& Name, EClassCastFlags RequiredType = EClassCastFlags::None)", .Body =
R"({
	return static_cast<UEType*>(FindObjectImpl(Name, RequiredType));
})",
			.bIsStatic = true, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.CustomTemplateText = "template<typename UEType = UObject>",
			.ReturnType = "UEType*", .NameWithParams = "FindObjectFast(const std::string& Name, EClassCastFlags RequiredType = EClassCastFlags::None)", .Body =
R"({
	return static_cast<UEType*>(FindObjectFastImpl(Name, RequiredType));
})",
			.bIsStatic = true, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "class UClass*", .NameWithParams = "FindClass(const std::string& ClassFullName)", .Body =
R"({
	return FindObject<class UClass>(ClassFullName, EClassCastFlags::Class);
})",
			.bIsStatic = true, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "class UClass*", .NameWithParams = "FindClassFast(const std::string& ClassName)", .Body =
R"({
	return FindObjectFast<class UClass>(ClassName, EClassCastFlags::Class);
}
)",
			.bIsStatic = true, .bIsConst = false, .bIsBodyInline = true
		},


		/* non-static non-inline functions */
		PredefinedFunction {
			.CustomComment = "Retuns the name of this object",
			.ReturnType = "std::string", .NameWithParams = "GetName()", .Body =
R"({
	return this ? Name.ToString() : "None";
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = false
		},
		PredefinedFunction {
			.CustomComment = "Returns the name of this object in the format 'Class Package.Outer.Object'",
			.ReturnType = "std::string", .NameWithParams = "GetFullName()", .Body =
R"({
	if (this && Class)
	{
		std::string Temp;

		for (UObject* NextOuter = Outer; NextOuter; NextOuter = NextOuter->Outer)
		{
			Temp = NextOuter->GetName() + "." + Temp;
		}

		std::string Name = Class->GetName();
		Name += " ";
		Name += Temp;
		Name += GetName();

		return Name;
	}

	return "None";
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = false
		},
		PredefinedFunction{
			.CustomComment = "Checks a UObjects' type by Class",
			.ReturnType = "bool", .NameWithParams = "IsA(const class UClass* TypeClass)", .Body =
R"({
	return Class->IsSubclassOf(TypeClass);
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = false
		},
		PredefinedFunction{
			.CustomComment = "Checks a UObjects' type by Class name",
			.ReturnType = "bool", .NameWithParams = "IsA(const class FName& ClassName)", .Body =
R"({
	return Class->IsSubclassOf(ClassName);
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = false
		},
		PredefinedFunction{
			.CustomComment = "Checks a UObjects' type by TypeFlags",
			.ReturnType = "bool", .NameWithParams = "IsA(EClassCastFlags TypeFlags)", .Body =
R"({
	return (Class->CastFlags & TypeFlags);
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = false
		},
		PredefinedFunction{
			.CustomComment = "Checks Class->FunctionFlags for TypeFlags",
			.ReturnType = "bool", .NameWithParams = "HasTypeFlag(EClassCastFlags TypeFlags)", .Body =
R"({
	return (Class->CastFlags & TypeFlags);
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = false
		},
		PredefinedFunction{
			.CustomComment = "Checks whether this object is a classes' default-object",
			.ReturnType = "bool", .NameWithParams = "IsDefaultObject()", .Body =
		R"({
	return (Flags & EObjectFlags::ClassDefaultObject);
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = false
		},

		/* non-static inline functions */
		PredefinedFunction{
			.CustomComment = "Unreal Function to process all UFunction-calls",
			.ReturnType = "void", .NameWithParams = "ProcessEvent(class UFunction* Function, void* Parms)", .Body = fmt::format(
R"({{
	InSDKUtils::CallGameFunction(InSDKUtils::GetVirtualFunction<void({}*)(const UObject*, class UFunction*, void*)>(this, Offsets::ProcessEventIdx), this, Function, Parms);
}})", Platform::Is32Bit() ? "__thiscall" : ""),
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
	};

	UEClass Struct = ObjectArray::FindClassFast("Struct");

	const int32 UStructIdx = Struct ? Struct.GetIndex() : ObjectArray::FindClassFast("struct").GetIndex(); // misspelled on some UE versions.

	PredefinedElements& UStructPredefs = PredefinedMembers[UStructIdx];

	const char* IsStructOfTypeCode =
R"({
	if (!Base)
		return false;

	for (const UStruct* Struct = this; Struct; Struct = Struct->SuperStruct)
	{
		if (Struct == Base)
			return true;
	}

	return false;
})";

	if (Off::UStruct::StructBaseChain != -1)
	{
		IsStructOfTypeCode =
R"({
	if (!Base)
		return false;

	const int32 NumParentStructBasesInChainMinusOne = Base->BaseChain.NumStructBasesInChainMinusOne;
	return NumParentStructBasesInChainMinusOne <= BaseChain.NumStructBasesInChainMinusOne && BaseChain.StructBaseChainArray[NumParentStructBasesInChainMinusOne] == &Base->BaseChain;
})";
	}

	UStructPredefs.Functions =
	{
		PredefinedFunction {
			.CustomComment = "Checks if this class has a certain base",
			.ReturnType = "bool", .NameWithParams = "IsSubclassOf(const UStruct* Base)", .Body = IsStructOfTypeCode,
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = false
		},
		PredefinedFunction {
			.CustomComment = "Checks if this class has a certain base",
			.ReturnType = "bool", .NameWithParams = "IsSubclassOf(const FName& BaseClassName)", .Body =
R"({
	if (BaseClassName.IsNone())
		return false;

	for (const UStruct* Struct = this; Struct; Struct = Struct->SuperStruct)
	{
		if (Struct->Name == BaseClassName)
			return true;
	}

	return false;
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = false
		},
	};

	PredefinedElements& UClassPredefs = PredefinedMembers[ObjectArray::FindClassFast("Class").GetIndex()];

	UClassPredefs.Functions =
	{
		/* non-static non-inline functions */
		PredefinedFunction {
			.CustomComment = "Gets a UFunction from this UClasses' 'Children' list",
			.ReturnType = "class UFunction*", .NameWithParams = "GetFunction(const char* ClassName, const char* FuncName)", .Body =
R"({
	for(const UStruct* Clss = this; Clss; Clss = Clss->SuperStruct)
	{
		if (Clss->GetName() != ClassName)
			continue;
			
		for (UField* Field = Clss->Children; Field; Field = Field->Next)
		{
			if(Field->HasTypeFlag(EClassCastFlags::Function) && Field->GetName() == FuncName)
				return static_cast<class UFunction*>(Field);
		}
	}

	return nullptr;
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = false
		},
	};


	PredefinedElements& UEnginePredefs = PredefinedMembers[ObjectArray::FindClassFast("Engine").GetIndex()];

	UEnginePredefs.Functions =
	{
		/* static non-inline functions */
		PredefinedFunction {
			.CustomComment = "Gets a pointer to a valid UObject of type UEngine",
			.ReturnType = "class UEngine*", .NameWithParams = "GetEngine()", .Body =
R"({
	static UEngine* GEngine = nullptr;

	if (GEngine)
		return GEngine;
	
	/* (Re-)Initialize if GEngine is nullptr */
	for (int i = 0; i < UObject::GObjects->Num(); i++)
	{
		UObject* Obj = UObject::GObjects->GetByIndex(i);

		if (!Obj)
			continue;

		if (Obj->IsA(UEngine::StaticClass()) && !Obj->IsDefaultObject())
		{
			GEngine = static_cast<UEngine*>(Obj);
			break;
		}
	}

	return GEngine; 
})",
			.bIsStatic = true, .bIsConst = false, .bIsBodyInline = false
		},
	};


	PredefinedElements& UGameEnginePredefs = PredefinedMembers[ObjectArray::FindClassFast("GameEngine").GetIndex()];

	UGameEnginePredefs.Functions =
	{
		/* static non-inline functions */
		PredefinedFunction {
			.CustomComment = "Returns the result of UEngine::GetEngine() without a type-check, might be dangerous",
			.ReturnType = "class UGameEngine*", .NameWithParams = "GetEngine()", .Body =
R"({
	return static_cast<UGameEngine*>(UEngine::GetEngine());
})",
			.bIsStatic = true, .bIsConst = false, .bIsBodyInline = false
		},
	};


	PredefinedElements& UWorldPredefs = PredefinedMembers[ObjectArray::FindClassFast("World").GetIndex()];

	constexpr const char* GetWorldThroughGWorldCode = R"(
	if constexpr (Offsets::GWorld != 0)
		return *reinterpret_cast<UWorld**>(InSDKUtils::GetImageBase() + Offsets::GWorld);
)";

	UWorldPredefs.Functions =
	{
		/* static non-inline functions */
		PredefinedFunction {
			.CustomComment = "Gets a pointer to the current World of the GameViewport",
			.ReturnType = "class UWorld*", .NameWithParams = "GetWorld()", .Body =
fmt::format(R"({{{}
	if (UEngine* Engine = UEngine::GetEngine())
	{{
		if (!Engine->GameViewport)
			return nullptr;

		return Engine->GameViewport->World;
	}}

	return nullptr;
}})", !Settings::CppGenerator::bForceNoGWorldInSDK ?  GetWorldThroughGWorldCode : ""),
			.bIsStatic = true, .bIsConst = false, .bIsBodyInline = false
		},
	};

	UEStruct Vector = ObjectArray::FindObjectFast<UEStruct>("Vector");

	PredefinedElements& FVectorPredefs = PredefinedMembers[Vector.GetIndex()];

	FVectorPredefs.Members.push_back(PredefinedMember{
		PredefinedMember{
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = fmt::format("using UnderlayingType = {}", (Settings::Internal::bUseLargeWorldCoordinates ? "double" : "float")), .Name = "", .Offset = 0x0, .Size = 0x08, .ArrayDim = 0x1, .Alignment = 0x8,
			.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF,
		}
	});

	FVectorPredefs.Functions =
	{
		/* constructors */
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "constexpr", .NameWithParams = "FVector(UnderlayingType X = 0, UnderlayingType Y = 0, UnderlayingType Z = 0)", .Body =
R"(	: X(X), Y(Y), Z(Z)
{
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "constexpr", .NameWithParams = "FVector(const FVector& other)", .Body =
R"(	: X(other.X), Y(other.Y), Z(other.Z)
{
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},

		/* const operators */
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector", .NameWithParams = "operator+(const FVector& Other)", .Body =
R"({
	return { X + Other.X, Y + Other.Y, Z + Other.Z };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector", .NameWithParams = "operator-(const FVector& Other)", .Body =
R"({
	return { X - Other.X, Y - Other.Y, Z - Other.Z };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector", .NameWithParams = "operator*(UnderlayingType Scalar)", .Body =
R"({
	return { X * Scalar, Y * Scalar, Z * Scalar };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector", .NameWithParams = "operator*(const FVector& Other)", .Body =
R"({
	return { X * Other.X, Y * Other.Y, Z * Other.Z };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector", .NameWithParams = "operator/(UnderlayingType Scalar)", .Body =
R"({
	if (Scalar == 0)
		return *this;

	return { X / Scalar, Y / Scalar, Z / Scalar };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector", .NameWithParams = "operator/(const FVector& Other)", .Body =
R"({
	if (Other.X == 0 || Other.Y == 0 || Other.Z == 0)
		return *this;

	return { X / Other.X, Y / Other.Y, Z / Other.Z };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "bool", .NameWithParams = "operator==(const FVector& Other)", .Body =
R"({
	return X == Other.X && Y == Other.Y && Z == Other.Z;
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "bool", .NameWithParams = "operator!=(const FVector& Other)", .Body =
R"({
	return X != Other.X || Y != Other.Y || Z != Other.Z;
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},

		/* Non-const operators */
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector&", .NameWithParams = "operator=(const FVector& other)", .Body =
R"({
	X = other.X;
	Y = other.Y;
	Z = other.Z;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector&", .NameWithParams = "operator+=(const FVector& Other)", .Body =
R"({
	*this = *this + Other;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector&", .NameWithParams = "operator-=(const FVector& Other)", .Body =
R"({
	*this = *this - Other;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector&", .NameWithParams = "operator*=(UnderlayingType Scalar)", .Body =
R"({
	*this = *this * Scalar;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector&", .NameWithParams = "operator*=(const FVector& Other)", .Body =
R"({
	*this = *this * Other;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector&", .NameWithParams = "operator/=(UnderlayingType Scalar)", .Body =
R"({
	*this = *this / Scalar;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector&", .NameWithParams = "operator/=(const FVector& Other)", .Body =
R"({
	*this = *this / Other;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},

		/* Const functions */
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "bool", .NameWithParams = "IsZero()", .Body =
R"({
	return X == 0 && Y == 0 && Z == 0;
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "UnderlayingType", .NameWithParams = "Dot(const FVector& Other)", .Body =
R"({
	return (X * Other.X) + (Y * Other.Y) + (Z * Other.Z);
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "UnderlayingType", .NameWithParams = "Magnitude()", .Body =
R"({
	return std::sqrt((X * X) + (Y * Y) + (Z * Z));
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "FVector", .NameWithParams = "GetNormalized()", .Body =
R"({
	return *this / Magnitude();
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "UnderlayingType", .NameWithParams = "GetDistanceTo(const FVector& Other)", .Body =
R"({
	FVector DiffVector = Other - *this;

	return DiffVector.Magnitude();
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "UnderlayingType", .NameWithParams = "GetDistanceToInMeters(const FVector& Other)", .Body =
R"({
	return GetDistanceTo(Other) * static_cast<UnderlayingType>(0.01);
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},


		/* Non-const functions */
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "FVector&", .NameWithParams = "Normalize()", .Body =
R"({
	*this /= Magnitude();

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
	};

	UEStruct Vector2D = ObjectArray::FindObjectFast<UEStruct>("Vector2D");

	PredefinedElements& FVector2DPredefs = PredefinedMembers[Vector2D.GetIndex()];
	FVector2DPredefs.Members.push_back(PredefinedMember{
		PredefinedMember{
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = fmt::format("using UnderlayingType = {}", (Settings::Internal::bUseLargeWorldCoordinates ? "double" : "float")), .Name = "", .Offset = 0x0, .Size = 0x08, .ArrayDim = 0x1, .Alignment = 0x8,
			.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF,
		}
	});

	FVector2DPredefs.Functions =
	{
		/* constructors */
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "constexpr", .NameWithParams = "FVector2D(UnderlayingType X = 0, UnderlayingType Y = 0)", .Body =
R"(	: X(X), Y(Y)
{
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "constexpr", .NameWithParams = "FVector2D(const FVector2D& other)", .Body =
R"(	: X(other.X), Y(other.Y)
{
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},

		/* const operators */
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector2D", .NameWithParams = "operator+(const FVector2D& Other)", .Body =
R"({
	return { X + Other.X, Y + Other.Y };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector2D", .NameWithParams = "operator-(const FVector2D& Other)", .Body =
R"({
	return { X - Other.X, Y - Other.Y  };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector2D", .NameWithParams = "operator*(UnderlayingType Scalar)", .Body =
R"({
	return { X * Scalar, Y * Scalar };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector2D", .NameWithParams = "operator*(const FVector2D& Other)", .Body =
R"({
	return { X * Other.X, Y * Other.Y };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector2D", .NameWithParams = "operator/(UnderlayingType Scalar)", .Body =
R"({
	if (Scalar == 0)
		return *this;

	return { X / Scalar, Y / Scalar };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector2D", .NameWithParams = "operator/(const FVector2D& Other)", .Body =
R"({
	if (Other.X == 0 || Other.Y == 0)
		return *this;

	return { X / Other.X, Y / Other.Y };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "bool", .NameWithParams = "operator==(const FVector2D& Other)", .Body =
R"({
	return X == Other.X && Y == Other.Y;
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "bool", .NameWithParams = "operator!=(const FVector2D& Other)", .Body =
R"({
	return X != Other.X || Y != Other.Y;
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},

		/* Non-const operators */
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector2D&", .NameWithParams = "operator=(const FVector2D& other)", .Body =
R"({
	X = other.X;
	Y = other.Y;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector2D&", .NameWithParams = "operator+=(const FVector2D& Other)", .Body =
R"({
	*this = *this + Other;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector2D&", .NameWithParams = "operator-=(const FVector2D& Other)", .Body =
R"({
	*this = *this - Other;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector2D&", .NameWithParams = "operator*=(UnderlayingType Scalar)", .Body =
R"({
	*this = *this * Scalar;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector2D&", .NameWithParams = "operator*=(const FVector2D& Other)", .Body =
R"({
	*this = *this * Other;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector2D&", .NameWithParams = "operator/=(UnderlayingType Scalar)", .Body =
R"({
	*this = *this / Scalar;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FVector2D&", .NameWithParams = "operator/=(const FVector2D& Other)", .Body =
R"({
	*this = *this / Other;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},

		/* Const functions */
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "bool", .NameWithParams = "IsZero()", .Body =
R"({
	return X == 0 && Y == 0;
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "UnderlayingType", .NameWithParams = "Dot(const FVector2D& Other)", .Body =
R"({
	return (X * Other.X) + (Y * Other.Y);
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "UnderlayingType", .NameWithParams = "Magnitude()", .Body =
R"({
	return std::sqrt((X * X) + (Y * Y));
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "FVector2D", .NameWithParams = "GetNormalized()", .Body =
R"({
	return *this / Magnitude();
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "UnderlayingType", .NameWithParams = "GetDistanceTo(const FVector2D& Other)", .Body =
R"({
	FVector2D DiffVector = Other - *this;

	return DiffVector.Magnitude();
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},


		/* Non-const functions */
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "FVector2D&", .NameWithParams = "Normalize()", .Body =
R"({
	*this /= Magnitude();

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
	};

	UEStruct Rotator = ObjectArray::FindObjectFast<UEStruct>("Rotator");

	PredefinedElements& FRotatorPredefs = PredefinedMembers[Rotator.GetIndex()];

	FRotatorPredefs.Members.push_back(PredefinedMember{
		PredefinedMember{
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = fmt::format("using UnderlayingType = {}", (Settings::Internal::bUseLargeWorldCoordinates ? "double" : "float")), .Name = "", .Offset = 0x0, .Size = 0x08, .ArrayDim = 0x1, .Alignment = 0x8,
			.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF,
		}
		});

	FRotatorPredefs.Functions =
	{
		/* constructors */
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "constexpr", .NameWithParams = "FRotator(UnderlayingType Pitch = 0, UnderlayingType Yaw = 0, UnderlayingType Roll = 0)", .Body =
R"(	: Pitch(Pitch), Yaw(Yaw), Roll(Roll)
{
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "constexpr", .NameWithParams = "FRotator(const FRotator& other)", .Body =
R"(	: Pitch(other.Pitch), Yaw(other.Yaw), Roll(other.Roll)
{
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		
		/* static functions */
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "UnderlayingType", .NameWithParams = "ClampAxis(UnderlayingType Angle)", .Body =
R"({
	Angle = std::fmod(Angle, static_cast<UnderlayingType>(360));
	if (Angle < static_cast<UnderlayingType>(0))
		Angle += static_cast<UnderlayingType>(360);

	return Angle;
})",
			.bIsStatic = true, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "UnderlayingType", .NameWithParams = "NormalizeAxis(UnderlayingType Angle)", .Body =
R"({
	Angle = ClampAxis(Angle);
	if (Angle > static_cast<UnderlayingType>(180))
		Angle -= static_cast<UnderlayingType>(360);

	return Angle;
})",
			.bIsStatic = true, .bIsConst = false, .bIsBodyInline = true
		},

		/* const operators */
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FRotator", .NameWithParams = "operator+(const FRotator& Other)", .Body =
R"({
	return { Pitch + Other.Pitch, Yaw + Other.Yaw, Roll + Other.Roll };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FRotator", .NameWithParams = "operator-(const FRotator& Other)", .Body =
R"({
	return { Pitch - Other.Pitch, Yaw - Other.Yaw, Roll - Other.Roll };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FRotator", .NameWithParams = "operator*(UnderlayingType Scalar)", .Body =
R"({
	return { Pitch * Scalar, Yaw * Scalar, Roll * Scalar };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FRotator", .NameWithParams = "operator*(const FRotator& Other)", .Body =
R"({
	return { Pitch * Other.Pitch, Yaw * Other.Yaw, Roll * Other.Roll };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FRotator", .NameWithParams = "operator/(UnderlayingType Scalar)", .Body =
R"({
	if (Scalar == 0)
		return *this;

	return { Pitch / Scalar, Yaw / Scalar, Roll / Scalar };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FRotator", .NameWithParams = "operator/(const FRotator& Other)", .Body =
R"({
	if (Other.Pitch == 0 || Other.Yaw == 0 || Other.Roll == 0)
		return *this;

	return { Pitch / Other.Pitch, Yaw / Other.Yaw, Roll / Other.Roll };
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "bool", .NameWithParams = "operator==(const FRotator& Other)", .Body =
R"({
	return Pitch == Other.Pitch && Yaw == Other.Yaw && Roll == Other.Roll;
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "bool", .NameWithParams = "operator!=(const FRotator& Other)", .Body =
R"({
	return Pitch != Other.Pitch || Yaw != Other.Yaw || Roll != Other.Roll;
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},

		/* Non-const operators */
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "FRotator&", .NameWithParams = "operator=(const FRotator& other)", .Body =
R"({
	Pitch = other.Pitch;
	Yaw = other.Yaw;
	Roll = other.Roll;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FRotator&", .NameWithParams = "operator+=(const FRotator& Other)", .Body =
R"({
	*this = *this + Other;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FRotator&", .NameWithParams = "operator-=(const FRotator& Other)", .Body =
R"({
	*this = *this - Other;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FRotator&", .NameWithParams = "operator*=(UnderlayingType Scalar)", .Body =
R"({
	*this = *this * Scalar;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FRotator&", .NameWithParams = "operator*=(const FRotator& Other)", .Body =
R"({
	*this = *this * Other;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FRotator&", .NameWithParams = "operator/=(UnderlayingType Scalar)", .Body =
R"({
	*this = *this / Scalar;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "FRotator&", .NameWithParams = "operator/=(const FRotator& Other)", .Body =
R"({
	*this = *this / Other;

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},

		/* Const functions */
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "FRotator", .NameWithParams = "GetNormalized()", .Body =
R"({
	FRotator rotator = *this;
	rotator.Normalize();

	return rotator;
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "bool", .NameWithParams = "IsZero()", .Body =
R"({
	return ClampAxis(Pitch) == 0 && ClampAxis(Yaw) == 0 && ClampAxis(Roll) == 0;
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},

		/* Non-const functions */
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "FRotator&", .NameWithParams = "Normalize()", .Body =
R"({
	Pitch = NormalizeAxis(Pitch);
	Yaw = NormalizeAxis(Yaw);
	Roll = NormalizeAxis(Roll);

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "FRotator&", .NameWithParams = "Clamp()", .Body =
R"({
	Pitch = ClampAxis(Pitch);
	Yaw = ClampAxis(Yaw);
	Roll = ClampAxis(Roll);

	return *this;
})",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
	};

	SortFunctions(UObjectPredefs.Functions);
	SortFunctions(UClassPredefs.Functions);
	SortFunctions(UEnginePredefs.Functions);
	SortFunctions(UGameEnginePredefs.Functions);
	SortFunctions(UWorldPredefs.Functions);
	SortFunctions(FVectorPredefs.Functions);
	SortFunctions(FVector2DPredefs.Functions);
	SortFunctions(FRotatorPredefs.Functions);
}


/* Reformat a captured-via-stringification lambda source so it doesn't emit as a
 * single mega-line. Macro stringification collapses all inter-token whitespace
 * to single spaces, so the captured text has no newlines.
 *
 * Heuristic: open-brace -> newline+indent, close-brace -> newline (dedent),
 * semicolon (at paren-depth 0) -> newline. String/char literals are passed
 * through verbatim so payload contents aren't reflowed. */
static std::string PrettyPrintCapturedLambda(std::string Raw)
{
	std::string Out;
	Out.reserve(Raw.size() * 2);

	int Depth = 0;
	int ParenDepth = 0;
	bool InString = false;
	char StringDelim = 0;

	auto NewlineIndent = [&](int D)
	{
		Out += '\n';
		Out += '\t';                       // base indent (struct member line)
		for (int i = 0; i < D; ++i) Out += '\t';
	};

	for (size_t i = 0; i < Raw.size(); ++i)
	{
		const char c = Raw[i];

		if (InString)
		{
			Out += c;
			if (c == StringDelim && (i == 0 || Raw[i-1] != '\\')) InString = false;
			continue;
		}
		if (c == '"' || c == '\'')
		{
			InString = true;
			StringDelim = c;
			Out += c;
			continue;
		}

		if (c == '(') { ++ParenDepth; Out += c; continue; }
		if (c == ')') { --ParenDepth; Out += c; continue; }

		if (c == '{')
		{
			size_t j = i + 1;
			while (j < Raw.size() && std::isspace(static_cast<unsigned char>(Raw[j]))) ++j;
			if (j < Raw.size() && Raw[j] == '}') { Out += "{}"; i = j; continue; }
			Out += c;
			++Depth;
			NewlineIndent(Depth);
			while (i + 1 < Raw.size() && std::isspace(static_cast<unsigned char>(Raw[i+1]))) ++i;
			continue;
		}
		if (c == '}')
		{
			--Depth;
			NewlineIndent(Depth);
			Out += c;
			continue;
		}
		if (c == ';' && ParenDepth == 0)
		{
			Out += c;
			while (i + 1 < Raw.size() && std::isspace(static_cast<unsigned char>(Raw[i+1]))) ++i;
			if (i + 1 < Raw.size() && Raw[i+1] != '}') NewlineIndent(Depth);
			continue;
		}

		if (std::isspace(static_cast<unsigned char>(c)))
		{
			if (Out.empty() || Out.back() == ' ' || Out.back() == '\t' || Out.back() == '\n') continue;
			Out += ' ';
			continue;
		}

		Out += c;
	}
	return Out;
}

void CppGenerator::GenerateBasicFiles(StreamType& BasicHpp, StreamType& BasicCpp, StreamType& AssertionsFile)
{
	namespace CppSettings = Settings::CppGenerator;

	static auto SortMembers = [](std::vector<PredefinedMember>& Members) -> void
	{
		std::sort(Members.begin(), Members.end(), ComparePredefinedMembers);
	};

	const std::string SDKMacroDefinitions = fmt::format(R"(

/*
* Macros for opening and closing namespaces, in order to allow to remove the SDK namespace when importing the SDK into IDA.
*
* In IDA under "Options>Compiler" set "SourceParser" to "clang" and add the following arguments 
* 
*	-std=c++20 -Wno-invalid-offsetof -Wno-c++11-narrowing -D IMPORT_CPP_SDK_INTO_IDA=1 
* 
* Omit the '-D IMPORT_CPP_SDK_INTO_IDA=1' if you want to keep the SDK namespace in IDA
*/
#ifndef IMPORT_CPP_SDK_INTO_IDA
	#define SDK_NAMESPACE_START namespace {} {{
	#define SDK_NAMESPACE_END }}
	#define SDK_ALIGN(x) alignas(x)
#else
	#define SDK_NAMESPACE_START
	#define SDK_NAMESPACE_END
	#define SDK_ALIGN(x)
#endif

#define SDK_PARAM_NAMESPACE_START namespace {} {{
#define SDK_PARAM_NAMESPACE_END }}

)", Settings::Config::SDKNamespaceName, CppSettings::ParamNamespaceName);

	const std::string CustomIncludes = fmt::format(R"(#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
{}
#include <string>
#include <functional>
#include <type_traits>
#include <mach/mach.h>
)", (!Settings::Config::SDKNamespaceName.empty() ? SDKMacroDefinitions : ""));

	WriteFileHead(BasicHpp, nullptr, EFileType::BasicHpp, "Basic file containing structs required by the SDK", CustomIncludes);
	WriteFileHead(BasicCpp, nullptr, EFileType::BasicCpp, "Basic file containing function-implementations from Basic.hpp", "#include <mach-o/dyld.h>");


	/* use namespace of UnrealContainers (brings UC::TCHAR alias into scope; defined in UnrealContainers.hpp) */
	BasicHpp <<
		R"(
using namespace UC;

/* Mach-backed IsBadReadPtr — emitted so generated decryption lambdas
 * (FNameEntry::DecryptName, TNameEntryArray::DecryptNameArray, FNamePool::DecryptNamePool)
 * can probe pointers safely without the consumer providing one. */
#ifndef DUMPER7_SDK_HAS_ISBADREADPTR
#define DUMPER7_SDK_HAS_ISBADREADPTR
inline bool IsBadReadPtr(const void* Ptr)
{
	if (!Ptr) return true;
	uint8_t Probe = 0;
	vm_size_t Got = 0;
	kern_return_t KR = vm_read_overwrite(mach_task_self(),
		reinterpret_cast<vm_address_t>(Ptr), 1,
		reinterpret_cast<vm_address_t>(&Probe), &Got);
	return (KR == KERN_INVALID_ADDRESS || KR == KERN_MEMORY_FAILURE
	     || KR == KERN_MEMORY_ERROR    || KR == KERN_PROTECTION_FAILURE);
}
inline bool IsBadReadPtr(uintptr_t Addr) { return IsBadReadPtr(reinterpret_cast<const void*>(Addr)); }
#endif

)";

	BasicHpp << "\n#include \"../NameCollisions.inl\"\n";

	std::string GetNameEntryFromNameOffsetText;

	if (Off::InSDK::Name::bIsAppendStringInlinedAndUsed)
		GetNameEntryFromNameOffsetText = fmt::format("\n	constexpr int32 GetNameEntry      = 0x{:08X};", Off::InSDK::Name::GetNameEntryFromName);

	/* Offsets and disclaimer */
	BasicHpp << fmt::format(R"(
/*
* Disclaimer:
*	- The 'GNames' is only a fallback and null by default, FName::AppendString is used
*	- THe 'GWorld' offset is not used by the SDK, it's just there for "decoration", use the provided 'UWorld::GetWorld()' function instead
*/
namespace Offsets
{{
	constexpr int32 GObjects          = 0x{:08X};
	constexpr int32 AppendString      = 0x{:08X};{}
	constexpr int32 GNames            = 0x{:08X};
	constexpr int32 GWorld            = 0x{:08X};
	constexpr int32 ProcessEvent      = 0x{:08X};
	constexpr int32 ProcessEventIdx   = 0x{:08X};
}}
)", std::max(Off::InSDK::ObjArray::GObjects, 0x0),
	std::max(Off::InSDK::Name::AppendNameToString, 0x0),
	GetNameEntryFromNameOffsetText,
	std::max(Off::InSDK::NameArray::GNames, 0x0),
	std::max(Off::InSDK::World::GWorld, 0x0),
	std::max(Off::InSDK::ProcessEvent::PEOffset, 0x0),
	Off::InSDK::ProcessEvent::PEIndex);


	// Start Namespace 'InSDKUtils'
	BasicHpp <<
		R"(
namespace InSDKUtils
{
)";


	/* Custom 'GetImageBase' function */
	BasicHpp << "\tuintptr_t GetImageBase();\n\n";


	/* GetVirtualFunction(const void* ObjectInstance, int32 Index) function */
	BasicHpp << R"(	template<typename FuncType>
	inline FuncType GetVirtualFunction(const void* ObjectInstance, int32 Index)
	{
		void** VTable = *reinterpret_cast<void***>(const_cast<void*>(ObjectInstance));

		return reinterpret_cast<FuncType>(VTable[Index]);
	}
)";

	//Customizable part of Cpp code to allow for a custom 'CallGameFunction' function
	BasicHpp << CppSettings::CallGameFunction;

	BasicHpp << "}\n\n";
	// End Namespace 'InSDKUtils'

	/* Custom 'GetImageBase' function */
	BasicCpp << fmt::format(R"(uintptr_t InSDKUtils::GetImageBase()
{})", Settings::CppGenerator::GetImageBaseFuncBody);

	BasicHpp << R"(
// Forward declarations because in-line forward declarations make the compiler think 'GetStaticClass()' is a class template
class UClass;
class UObject;
class UFunction;

class FName;
)";

	BasicHpp << R"(
namespace BasicFilesImplUtils
{
	// Helper functions for GetStaticClass and GetStaticBPGeneratedClass
	UClass* FindClassByName(const std::string& Name, bool bByFullName = false);
	UClass* FindClassByFullName(const std::string& Name);

	std::string GetObjectName(class UClass* Class);
	int32 GetObjectIndex(class UClass* Class);

	/* FName represented as a uint64. */
	uint64 GetObjFNameAsUInt64(class UClass* Class);

	UObject* GetObjectByIndex(int32 Index);

	UFunction* FindFunctionByFName(const FName* Name);

	FName StringToName(const TCHAR* Name);

	UObject* GetDefaultObjectImpl(UClass* ClassInstance);
}
)";

	BasicCpp << R"(
class UClass* BasicFilesImplUtils::FindClassByName(const std::string& Name, bool bByFullName)
{
	return bByFullName ? UObject::FindClass(Name) : UObject::FindClassFast(Name);
}

class UClass* BasicFilesImplUtils::FindClassByFullName(const std::string& Name)
{
	return UObject::FindClass(Name);
}

std::string BasicFilesImplUtils::GetObjectName(class UClass* Class)
{
	return Class->GetName();
}

int32 BasicFilesImplUtils::GetObjectIndex(class UClass* Class)
{
	return Class->Index;
}

uint64 BasicFilesImplUtils::GetObjFNameAsUInt64(class UClass* Class)
{
	return *reinterpret_cast<uint64*>(&Class->Name);
}

class UObject* BasicFilesImplUtils::GetObjectByIndex(int32 Index)
{
	return UObject::GObjects->GetByIndex(Index);
}

UFunction* BasicFilesImplUtils::FindFunctionByFName(const FName* Name)
{
	for (int i = 0; i < UObject::GObjects->Num(); ++i)
	{
		UObject* Object = UObject::GObjects->GetByIndex(i);

		if (!Object)
			continue;

		if (Object->Name == *Name)
			return static_cast<UFunction*>(Object);
	}

	return nullptr;
}

FName BasicFilesImplUtils::StringToName(const TCHAR* Name)
{
	return UKismetStringLibrary::Conv_StringToName(FString(Name));
}

UObject* BasicFilesImplUtils::GetDefaultObjectImpl(UClass* Class)
{
	if (Class)
		return Class->ClassDefaultObject;

	return nullptr;
}
)";

	BasicHpp << R"(
const FName& GetStaticName(const TCHAR* Name, FName& StaticName);
)";

	BasicCpp << R"(
const FName& GetStaticName(const TCHAR* Name, FName& StaticName)
{
	if (StaticName.IsNone())
	{
		StaticName = BasicFilesImplUtils::StringToName(Name);
	}

	return StaticName;
}
)";

	/* Implementation of 'UObject::StaticClass()', templated to allow for a per-class local static class-pointer */
	BasicHpp << R"(
template<bool bIsFullName = false>
class UClass* GetStaticClassImpl(const char* Name, class UClass*& StaticClass)
{
	if (StaticClass == nullptr)
	{
		if constexpr (bIsFullName) {
			StaticClass = BasicFilesImplUtils::FindClassByFullName(Name);
		}
		else /* default */ {
			StaticClass = BasicFilesImplUtils::FindClassByName(Name);
		}
	}

	return StaticClass;
}
)";

	/* Implementation of 'UObject::StaticClass()' for 'BlueprintGeneratedClass', templated to allow for a per-class local static class-index */
	BasicHpp << R"(
template<bool bIsFullName = false>
class UClass* GetStaticBPGeneratedClass(const char* Name, int32& ClassIdx, uint64& ClassNameIdx)
{
	/* Could be external function, not really unique to this StaticClass functon */
	static auto SetClassIndex = [](UClass* Class, int32& Index, uint64& ClassName) -> UClass*
		{
			if (Class)
			{
				Index = BasicFilesImplUtils::GetObjectIndex(Class);
				ClassName = BasicFilesImplUtils::GetObjFNameAsUInt64(Class);
			}

			return Class;
		};

	/* Use the full name to find an object */
	if constexpr (bIsFullName)
	{
		if (ClassIdx == 0x0) [[unlikely]]
			return SetClassIndex(BasicFilesImplUtils::FindClassByFullName(Name), ClassIdx, ClassNameIdx);

		UClass* ClassObj = reinterpret_cast<UClass*>(BasicFilesImplUtils::GetObjectByIndex(ClassIdx));

		/* Could use cast flags too to save some string comparisons */
		if (!ClassObj || BasicFilesImplUtils::GetObjFNameAsUInt64(ClassObj) != ClassNameIdx)
			return SetClassIndex(BasicFilesImplUtils::FindClassByFullName(Name), ClassIdx, ClassNameIdx);

		return ClassObj;
	}
	else /* Default, use just the name to find an object*/
	{
		if (ClassIdx == 0x0) [[unlikely]]
			return SetClassIndex(BasicFilesImplUtils::FindClassByName(Name), ClassIdx, ClassNameIdx);

		UClass* ClassObj = reinterpret_cast<UClass*>(BasicFilesImplUtils::GetObjectByIndex(ClassIdx));

		/* Could use cast flags too to save some string comparisons */
		if (!ClassObj || BasicFilesImplUtils::GetObjFNameAsUInt64(ClassObj) != ClassNameIdx)
			return SetClassIndex(BasicFilesImplUtils::FindClassByName(Name), ClassIdx, ClassNameIdx);

		return ClassObj;
	}
}
)";

	/* Implementation of 'UObject::StaticClass()', templated to allow for a per-object local static */
	BasicHpp << R"(
template<class ClassType>
ClassType* GetDefaultObjImpl()
{
	return reinterpret_cast<ClassType*>(BasicFilesImplUtils::GetDefaultObjectImpl(ClassType::StaticClass()));
}
)";

	BasicHpp << R"(
#define STATIC_CLASS_IMPL(NameString) \
{ \
    static UClass* Clss = nullptr; \
    return GetStaticClassImpl(NameString, Clss); \
}

#define STATIC_CLASS_IMPL_FULLNAME(FullNameString) \
{ \
    static UClass* Clss = nullptr; \
    return GetStaticClassImpl<true>(FullNameString, Clss); \
}

#define BP_STATIC_CLASS_IMPL(NameString) \
{ \
    static int32 ClassIdx = 0;   \
    static uint64 ClassName = 0; \
    return GetStaticBPGeneratedClass(NameString, ClassIdx, ClassName); \
}

#define BP_STATIC_CLASS_IMPL_FULLNAME(FullNameString) \
{ \
    static int32 ClassIdx = 0;   \
    static uint64 ClassName = 0; \
    return GetStaticBPGeneratedClass<true>(FullNameString, ClassIdx, ClassName); \
}

#define STATIC_NAME_IMPL(NameString) \
{ \
    static FName Name = FName(); \
    return GetStaticName(NameString, Name); \
}
)";

	// Start class 'FUObjectItem'
	PredefinedStruct FUObjectItem = PredefinedStruct{
		.UniqueName = "FUObjectItem", .Size = Off::InSDK::ObjArray::FUObjectItemSize, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = false, .bIsUnion = false, .Super = nullptr
	};

	FUObjectItem.Properties =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "class UObject*", .Name = "Object", .Offset = Off::InSDK::ObjArray::FUObjectItemInitialOffset, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	GenerateStruct(&FUObjectItem, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);

	constexpr const char* DefaultDecryption = R"([](void* ObjPtr) -> uint8*
	{
		return reinterpret_cast<uint8*>(ObjPtr);
	})";

	std::string DecryptionStrToUse = ObjectArray::DecryptionLambdaStr.empty() ? DefaultDecryption : PrettyPrintCapturedLambda(std::move(ObjectArray::DecryptionLambdaStr));
//#error Fix this and fix alignof(UObject) == 0x1
	if (Off::InSDK::ObjArray::ChunkSize <= 0)
	{
#undef max
		const auto& ObjectsArrayLayout = Off::FUObjectArray::FixedLayout;
		const int32 ObjectArraySize = std::max({ ObjectsArrayLayout.ObjectsOffset + sizeof(void*), ObjectsArrayLayout.NumObjectsOffset + sizeof(int), ObjectsArrayLayout.MaxObjectsOffset + sizeof(int),});
#define max(A, B) (A > B ? A : B)

		// Start class 'TUObjectArray'
		PredefinedStruct TUObjectArray = PredefinedStruct{
			.UniqueName = "TUObjectArray", .Size = ObjectArraySize, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .bIsUnion = false, .Super = nullptr
		};

		TUObjectArray.Properties =
		{
			/* Static members of TUObjectArray */
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = ("static constexpr auto DecryptPtr = " + DecryptionStrToUse), .Offset = 0x0, .Size = 0x0, .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
			},

			/* Non-static members of TUObjectArray */
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "struct FUObjectItem*", .Name = "Objects", .Offset = ObjectsArrayLayout.ObjectsOffset, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "int32", .Name = "MaxElements", .Offset = ObjectsArrayLayout.MaxObjectsOffset, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "int32", .Name = "NumElements", .Offset = ObjectsArrayLayout.NumObjectsOffset, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
		};

		TUObjectArray.Functions =
		{
			PredefinedFunction {
				.CustomComment = "",
				.ReturnType = "inline int32", .NameWithParams = "Num()", .Body =
R"({
	return NumElements;
}
)",				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
			},
			PredefinedFunction {
				.CustomComment = "",
				.ReturnType = "FUObjectItem*", .NameWithParams = "GetDecrytedObjPtr()", .Body =
R"({
	return reinterpret_cast<FUObjectItem*>(DecryptPtr(Objects));
}
)",				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
			},
			PredefinedFunction {
				.CustomComment = "",
				.ReturnType = "inline class UObject*", .NameWithParams = "GetByIndex(const int32 Index)", .Body =
R"({
	if (Index < 0 || Index > NumElements)
		return nullptr;

	return GetDecrytedObjPtr()[Index].Object;
})",
				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
			},
		};

		SortMembers(TUObjectArray.Properties);
		GenerateStruct(&TUObjectArray, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);
	}
	else
	{

#undef max
		const auto& ObjectsArrayLayout = Off::FUObjectArray::ChunkedFixedLayout;
		const int32 ObjectArraySize = std::max({
			ObjectsArrayLayout.ObjectsOffset + sizeof(void*),
			ObjectsArrayLayout.MaxElementsOffset + sizeof(void*),
			ObjectsArrayLayout.NumElementsOffset + sizeof(int),
			ObjectsArrayLayout.MaxChunksOffset + sizeof(int),
			ObjectsArrayLayout.NumChunksOffset + sizeof(int)
		});
#define max(A, B) (A > B ? A : B)

		// Start class 'TUObjectArray'
		PredefinedStruct TUObjectArray = PredefinedStruct{
			.UniqueName = "TUObjectArray", .Size = ObjectArraySize, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .bIsUnion = false, .Super = nullptr
		};

		TUObjectArray.Properties =
		{
			/* Static members of TUObjectArray */
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = ("static constexpr auto DecryptPtr = " + DecryptionStrToUse), .Offset = 0x0, .Size = 0x0, .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "constexpr int32", .Name = "ElementsPerChunk", .Offset = 0x0, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
				.bIsStatic = true, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF, .DefaultValue = fmt::format("0x{:X}", Off::InSDK::ObjArray::ChunkSize)
			},

			/* Non-static members of TUObjectArray */
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "struct FUObjectItem**", .Name = "Objects", .Offset = ObjectsArrayLayout.ObjectsOffset, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "int32", .Name = "MaxElements", .Offset = ObjectsArrayLayout.MaxElementsOffset, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "int32", .Name = "NumElements", .Offset = ObjectsArrayLayout.NumElementsOffset, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "int32", .Name = "MaxChunks", .Offset = ObjectsArrayLayout.MaxChunksOffset, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "int32", .Name = "NumChunks", .Offset = ObjectsArrayLayout.NumChunksOffset, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
		};

		TUObjectArray.Functions =
		{
			PredefinedFunction {
				.CustomComment = "",
				.ReturnType = "inline int32", .NameWithParams = "Num()", .Body =
R"({
	return NumElements;
}
)",				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
			},
			PredefinedFunction {
				.CustomComment = "",
				.ReturnType = "FUObjectItem**", .NameWithParams = "GetDecrytedObjPtr()", .Body =
R"({
	return reinterpret_cast<FUObjectItem**>(DecryptPtr(Objects));
}
)",				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
			},
			PredefinedFunction {
				.CustomComment = "",
				.ReturnType = "inline class UObject*", .NameWithParams = "GetByIndex(const int32 Index)", .Body =
R"({
	const int32 ChunkIndex = Index / ElementsPerChunk;
	const int32 InChunkIdx = Index % ElementsPerChunk;
	
	if (Index < 0 || ChunkIndex >= NumChunks || Index >= NumElements)
	    return nullptr;
	
	FUObjectItem* ChunkPtr = GetDecrytedObjPtr()[ChunkIndex];
	if (!ChunkPtr) return nullptr;
	
	return ChunkPtr[InChunkIdx].Object;
})",
				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
			},
		};

		SortMembers(TUObjectArray.Properties);
		GenerateStruct(&TUObjectArray, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);
	}
	// End class 'TUObjectArray'



	/* TUObjectArrayWrapper so InitGObjects() doesn't need to be called manually anymore */
	// Start class 'TUObjectArrayWrapper'
	BasicHpp << R"(
class TUObjectArrayWrapper
{
private:
	friend class UObject;

private:
	void* GObjectsAddress = nullptr;

private:
	TUObjectArrayWrapper() = default;

public:
	TUObjectArrayWrapper(TUObjectArrayWrapper&&) = delete;
	TUObjectArrayWrapper(const TUObjectArrayWrapper&) = delete;

	TUObjectArrayWrapper& operator=(TUObjectArrayWrapper&&) = delete;
	TUObjectArrayWrapper& operator=(const TUObjectArrayWrapper&) = delete;

private:
	inline void InitGObjects()
	{
		GObjectsAddress = reinterpret_cast<void*>(InSDKUtils::GetImageBase() + Offsets::GObjects);
	}

public:)";
	if constexpr (Settings::CppGenerator::bAddManualOverrideOptions)
	{
	BasicHpp << R"(
	inline void InitManually(void* GObjectsAddressParameter)
	{
		GObjectsAddress = GObjectsAddressParameter;
	}
)"; }
	BasicHpp << R"(
	inline class TUObjectArray* operator->()
	{
		if (!GObjectsAddress) [[unlikely]]
			InitGObjects();

		return reinterpret_cast<class TUObjectArray*>(GObjectsAddress);
	}

	inline TUObjectArray& operator*() const
	{
		return *reinterpret_cast<class TUObjectArray*>(GObjectsAddress);
	}

	inline operator const void* ()
	{
		if (!GObjectsAddress) [[unlikely]]
			InitGObjects();

		return GObjectsAddress;
	}

	inline class TUObjectArray* GetTypedPtr()
	{
		if (!GObjectsAddress) [[unlikely]]
			InitGObjects();

		return reinterpret_cast<class TUObjectArray*>(GObjectsAddress);
	}
};
)";
	// End class 'TUObjectArrayWrapper'



	/* struct FStringData */
	PredefinedStruct FStringData = PredefinedStruct{
		.UniqueName = "FStringData", .Size = 0x800, .Alignment = 0x2, .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = false, .bIsUnion = true, .Super = nullptr
	};

	FStringData.Properties =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "char", .Name = "AnsiName", .Offset = 0x00, .Size = sizeof(char), .ArrayDim = 0x400, .Alignment = alignof(char),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "TCHAR", .Name = "WideName", .Offset = 0x0, .Size = sizeof(TCHAR), .ArrayDim = 0x400, .Alignment = alignof(TCHAR),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	constexpr const char* DefaultNameEntryDecryption = R"([](uint8* RawEntryPtr) -> uint8*
	{
		return RawEntryPtr;
	})";

	constexpr const char* DefaultNameStringDecryption = R"([](std::string Decoded) -> std::string
	{
		return Decoded;
	})";

	constexpr const char* DefaultNameArrayDecryption = R"([](uintptr_t EncryptedNameArrayData) -> uintptr_t
	{
		return EncryptedNameArrayData;
	})";

	constexpr const char* DefaultNamePoolDecryption = R"([](uintptr_t EncryptedNamePoolData) -> uintptr_t
	{
		return EncryptedNamePoolData;
	})";

	std::string NameEntryDecryptionStrToUse  = NameArray::NameEntryDecryptionLambdaStr.empty()  ? DefaultNameEntryDecryption  : PrettyPrintCapturedLambda(std::move(NameArray::NameEntryDecryptionLambdaStr));
	std::string NameStringDecryptionStrToUse = NameArray::NameStringDecryptionLambdaStr.empty() ? DefaultNameStringDecryption : PrettyPrintCapturedLambda(std::move(NameArray::NameStringDecryptionLambdaStr));
	std::string NameArrayDecryptionStrToUse  = NameArray::NameArrayDecryptionLambdaStr.empty()  ? DefaultNameArrayDecryption  : PrettyPrintCapturedLambda(std::move(NameArray::NameArrayDecryptionLambdaStr));
	std::string NamePoolDecryptionStrToUse   = NameArray::NamePoolDecryptionLambdaStr.empty()   ? DefaultNamePoolDecryption   : PrettyPrintCapturedLambda(std::move(NameArray::NamePoolDecryptionLambdaStr));

	if (Off::InSDK::Name::AppendNameToString == 0x0 && !Settings::Internal::bUseNamePool)
	{
		/* struct FNameEntry */
		PredefinedStruct FNameEntry = PredefinedStruct{
			.UniqueName = "FNameEntry", .Size = Off::FNameEntry::NameArray::StringOffset + 0x800, .Alignment = 0x4, .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = false, .bIsUnion = false, .Super = nullptr
		};

		FNameEntry.Properties =
		{
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = ("static constexpr auto DecryptName = " + NameEntryDecryptionStrToUse), .Offset = 0x0, .Size = 0x0, .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = ("static constexpr auto DecryptString = " + NameStringDecryptionStrToUse), .Offset = 0x0, .Size = 0x0, .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "constexpr int32", .Name = "NameWideMask", .Offset = 0x0, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
				.bIsStatic = true, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF, .DefaultValue = "0x1"
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "int32", .Name = "NameIndex", .Offset = Off::FNameEntry::NameArray::IndexOffset, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "union FStringData", .Name = "Name", .Offset = Off::FNameEntry::NameArray::StringOffset, .Size = 0x800, .ArrayDim = 0x1, .Alignment = 0x2,
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
		};

		SortMembers(FNameEntry.Properties);

		FNameEntry.Functions =
		{
			PredefinedFunction {
				.CustomComment = "",
				.ReturnType = "bool", .NameWithParams = "IsWide()", .Body =
R"({
	return (NameIndex & NameWideMask);
})",
				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
			},
			PredefinedFunction {
				.CustomComment = "",
				.ReturnType = "std::string", .NameWithParams = "GetString()", .Body =
R"({
	const auto* Self = reinterpret_cast<const FNameEntry*>(DecryptName(reinterpret_cast<uint8*>(const_cast<FNameEntry*>(this))));
	if (Self->NameIndex & NameWideMask)
		return UC::TCharToUtf8(Self->Name.WideName, std::char_traits<TCHAR>::length(Self->Name.WideName));
	// Decrypt narrow chars at raw-bytes level (legacy entries are null-terminated,
	// so length comes from strlen — works when encrypted output has no embedded 0x00).
	std::string Out = Self->Name.AnsiName;
	return DecryptString(std::move(Out));
})",
				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
			},
		};

		const int32 ChunkTableSize = Off::NameArray::NumElements / sizeof(void*);
		const int32 ChunkTableSizeBytes = ChunkTableSize * sizeof(void*);

		PredefinedStruct TNameEntryArray = PredefinedStruct{
			.UniqueName = "TNameEntryArray", .Size = (ChunkTableSizeBytes + (int)sizeof(void*)), .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .bIsUnion = false, .Super = nullptr
		};

		/* class TNameEntryArray */
		TNameEntryArray.Properties =
		{
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = ("static constexpr auto DecryptNameArray = " + NameArrayDecryptionStrToUse), .Offset = 0x0, .Size = 0x0, .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "constexpr uint32", .Name = "ChunkTableSize", .Offset = 0x0, .Size = sizeof(uint32), .ArrayDim = 0x1, .Alignment = alignof(uint32),
				.bIsStatic = true, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF, .DefaultValue = fmt::format("0x{:04X}", ChunkTableSize)
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "constexpr uint32", .Name = "NumElementsPerChunk", .Offset = 0x0, .Size = sizeof(uint32), .ArrayDim = 0x1, .Alignment = alignof(uint32),
				.bIsStatic = true, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF, .DefaultValue = "0x4000"
			},

			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "FNameEntry**", .Name = "Chunks", .Offset = 0x0, .Size = ChunkTableSizeBytes, .ArrayDim = ChunkTableSize, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF,
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "int32", .Name = "NumElements", .Offset = (ChunkTableSizeBytes + 0x0), .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "int32", .Name = "NumChunks", .Offset = (ChunkTableSizeBytes + 0x4), .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
		};

		TNameEntryArray.Functions =
		{
			PredefinedFunction {
				.CustomComment = "",
				.ReturnType = "bool", .NameWithParams = "IsValidIndex(int32 Index, int32 ChunkIdx, int32 InChunkIdx)", .Body =
R"({
	return Index >= 0 && Index < NumElements;
}
)",
				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
			},
			PredefinedFunction {
				.CustomComment = "",
				.ReturnType = "FNameEntry*", .NameWithParams = "GetEntryByIndex(int32 Index)", .Body =
R"({
	const int32 ChunkIdx = Index / NumElementsPerChunk;
	const int32 InChunk  = Index % NumElementsPerChunk;

	if (!IsValidIndex(Index, ChunkIdx, InChunk))
		return nullptr;

	return Chunks[ChunkIdx][InChunk];
})",
				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
			},
		};

		GenerateStruct(&FStringData, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);
		GenerateStruct(&FNameEntry, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);
		GenerateStruct(&TNameEntryArray, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);
	}
	else if (Off::InSDK::Name::AppendNameToString == 0x0 && Settings::Internal::bUseNamePool)
	{
		/* struct FNumberedData */
		const int32 FNumberedDataSize = Settings::Internal::bUseCasePreservingName ? 0xA : 0x8;

		PredefinedStruct FNumberedData = PredefinedStruct{
			.UniqueName = "FNumberedData", .Size = FNumberedDataSize, .Alignment = 0x1, .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = false, .bIsUnion = false, .Super = nullptr
		};

		const int32 FNumberedDataInitialOffset = Settings::Internal::bUseCasePreservingName ? 0x2 : 0x0;

		/* Use int32/uint32 directly (was uint8[4] + reinterpret_cast). The previous form did
		 * pointer-to-int truncation which clang rejects; this also avoids strict-aliasing UB. */
		FNumberedData.Properties =
		{
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "int32", .Name = "Id", .Offset = FNumberedDataInitialOffset, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember{
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "uint32", .Name = "Number", .Offset = FNumberedDataInitialOffset + 0x4, .Size = sizeof(uint32), .ArrayDim = 0x1, .Alignment = alignof(uint32),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			}
		};

		FNumberedData.Functions =
		{
			PredefinedFunction {
				.CustomComment = "",
				.ReturnType = "int32", .NameWithParams = "GetTypedId()", .Body =
R"({
	return Id;
})",
				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
			},
			PredefinedFunction {
				.CustomComment = "",
				.ReturnType = "uint32", .NameWithParams = "GetNumber()", .Body =
R"({
	return Number;
})",
				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
			},
		};


		if (Settings::Internal::bUseOutlineNumberName)
		{
			FStringData.Properties.push_back(
				PredefinedMember{
					.Comment = "NOT AUTO-GENERATED PROPERTY",
					.Type = "FNumberedData", .Name = "AnsiName", .Offset = 0x10, .Size = FNumberedDataSize, .ArrayDim = 0x400, .Alignment = alignof(char),
					.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
				}
			);
		}


		/* struct FNameEntryHeader */
		const int32 FNameEntryHeaderSize = Off::FNameEntry::NamePool::StringOffset - Off::FNameEntry::NamePool::HeaderOffset;

		PredefinedStruct FNameEntryHeader = PredefinedStruct{
			.UniqueName = "FNameEntryHeader", .Size = FNameEntryHeaderSize, .Alignment = 0x2, .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = false, .bIsUnion = false, .Super = nullptr
		};

		const uint8 LenBitCount = Settings::Internal::bUseCasePreservingName ? 15 : 10;
		const uint8 LenBitOffset = Settings::Internal::bUseCasePreservingName ? 1 : 6;

		FNameEntryHeader.Properties =
		{
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "uint16", .Name = "bIsWide", .Offset = 0x0, .Size = sizeof(uint16), .ArrayDim = 0x1, .Alignment = alignof(uint16),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = true, .BitIndex = 0x0, .BitCount = 1
			},
			PredefinedMember{
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "uint16", .Name = "Len", .Offset = 0x0, .Size = sizeof(uint16), .ArrayDim = 0x1, .Alignment = alignof(uint16),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = true, .BitIndex = LenBitOffset, .BitCount = LenBitCount
			}
		};


		/* struct FNameEntry */
		PredefinedStruct FNameEntry = PredefinedStruct{
			.UniqueName = "FNameEntry", .Size = Off::FNameEntry::NamePool::StringOffset + 0x800, .Alignment = 0x2, .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = false, .bIsUnion = false, .Super = nullptr
		};

		FNameEntry.Properties =
		{
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = ("static constexpr auto DecryptName = " + NameEntryDecryptionStrToUse), .Offset = 0x0, .Size = 0x0, .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = ("static constexpr auto DecryptString = " + NameStringDecryptionStrToUse), .Offset = 0x0, .Size = 0x0, .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "struct FNameEntryHeader", .Name = "Header", .Offset = Off::FNameEntry::NamePool::HeaderOffset, .Size = FNameEntryHeaderSize, .ArrayDim = 0x1, .Alignment = 0x2,
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "union FStringData", .Name = "Name", .Offset = Off::FNameEntry::NamePool::HeaderOffset + FNameEntryHeaderSize, .Size = 0x800, .ArrayDim = 0x1, .Alignment = 0x2,
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
		};

		FNameEntry.Functions =
		{
			PredefinedFunction {
				.CustomComment = "",
				.ReturnType = "bool", .NameWithParams = "IsWide()", .Body =
R"({
	return Header.bIsWide;
})",
				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
			},
			PredefinedFunction {
				.CustomComment = "",
				.ReturnType = "std::string", .NameWithParams = "GetString()", .Body =
R"({
	const auto* Self = reinterpret_cast<const FNameEntry*>(DecryptName(reinterpret_cast<uint8*>(const_cast<FNameEntry*>(this))));
	// Wide path: no DecryptString. The hook is for narrow byte XOR; wide encryption
	// (no known shipping game does this) would need UTF-16-aware math, not a byte hook.
	if (Self->Header.bIsWide)
		return UC::TCharToUtf8(Self->Name.WideName, Self->Header.Len);
	// Decrypt narrow chars at raw-bytes level with the entry's true Len, BEFORE any
	// further conversion — UTF-8 inflation of XOR'd bytes would change the string's
	// size and break per-Len key derivation in the hook.
	std::string Out(Self->Name.AnsiName, Self->Header.Len);
	return DecryptString(std::move(Out));
})",
				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
			},
		};

		constexpr int32 SizeOfChunkPtrs = 0x2000 * sizeof(void*);

		/* class FNamePool */
		PredefinedStruct FNamePool = PredefinedStruct{
			.UniqueName = "FNamePool", .Size = Off::NameArray::ChunksStart + SizeOfChunkPtrs, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .bIsUnion = false, .Super = nullptr
		};

		FNamePool.Properties =
		{
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = ("static constexpr auto DecryptNamePool = " + NamePoolDecryptionStrToUse), .Offset = 0x0, .Size = 0x0, .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "constexpr uint32", .Name = "FNameEntryStride", .Offset = 0x0, .Size = sizeof(uint32), .ArrayDim = 0x1, .Alignment = alignof(uint32),
				.bIsStatic = true, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF, .DefaultValue = fmt::format("0x{:04X}", Off::InSDK::NameArray::FNameEntryStride)
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "constexpr uint32", .Name = "FNameBlockOffsetBits", .Offset = 0x0, .Size = sizeof(uint32), .ArrayDim = 0x1, .Alignment = alignof(uint32),
				.bIsStatic = true, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF, .DefaultValue = fmt::format("0x{:04X}", Off::InSDK::NameArray::FNamePoolBlockOffsetBits)
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "constexpr uint32", .Name = "FNameBlockOffsets", .Offset = 0x0, .Size = sizeof(uint32), .ArrayDim = 0x1, .Alignment = alignof(uint32),
				.bIsStatic = true, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF, .DefaultValue = "1 << FNameBlockOffsetBits"
			},

			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "uint32", .Name = "CurrentBlock", .Offset = Off::NameArray::MaxChunkIndex, .Size = sizeof(uint32), .ArrayDim = 0x1, .Alignment = alignof(uint32),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF,
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "uint32", .Name = "CurrentByteCursor", .Offset = Off::NameArray::ByteCursor, .Size = sizeof(uint32), .ArrayDim = 0x1, .Alignment = alignof(uint32),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "uint8*", .Name = "Blocks", .Offset = Off::NameArray::ChunksStart, .Size = SizeOfChunkPtrs, .ArrayDim = 0x2000, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
		};

		FNamePool.Functions =
		{
			PredefinedFunction {
				.CustomComment = "",
				.ReturnType = "bool", .NameWithParams = "IsValidIndex(int32 Index, int32 ChunkIdx, int32 InChunkIdx)", .Body =
R"({
	return ChunkIdx <= CurrentBlock && !(ChunkIdx == CurrentBlock && InChunkIdx > CurrentByteCursor);
}
)",
				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
			},
			PredefinedFunction {
				.CustomComment = "",
				.ReturnType = "FNameEntry*", .NameWithParams = "GetEntryByIndex(int32 Index)", .Body =
R"({
	const int32 ChunkIdx = Index >> FNameBlockOffsetBits;
	const int32 InChunk = (Index & (FNameBlockOffsets - 1));

	if (!IsValidIndex(Index, ChunkIdx, InChunk))
		return nullptr;

	return reinterpret_cast<FNameEntry*>(Blocks[ChunkIdx] + (InChunk * FNameEntryStride));
})",
				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
			},
		};

		GenerateStruct(&FNumberedData, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);
		GenerateStruct(&FNameEntryHeader, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);
		GenerateStruct(&FStringData, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);
		GenerateStruct(&FNameEntry, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);
		GenerateStruct(&FNamePool, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);
	}


	/* class FName */
	PredefinedStruct FName = PredefinedStruct{
		.UniqueName = "FName", .Size = Off::InSDK::Name::FNameSize, .Alignment = alignof(int32), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .bIsUnion = false, .Super = nullptr
	};

	std::string NameArrayType = Off::InSDK::Name::AppendNameToString == 0 ? Settings::Internal::bUseNamePool ? "FNamePool*" : "TNameEntryArray*" : "void*";
	std::string NameArrayName = Off::InSDK::Name::AppendNameToString == 0 ? "GNames" : "AppendString";

	FName.Properties =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "inline " + NameArrayType, .Name = NameArrayName, .Offset = 0x0, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
			.bIsStatic = true, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF, .DefaultValue = "nullptr"
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "int32", .Name = "ComparisonIndex", .Offset = 0x0, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF, .DefaultValue = "0x0"
		},
	};

	if (Off::InSDK::Name::bIsAppendStringInlinedAndUsed)
	{
		FName.Properties.push_back(
			PredefinedMember{
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "inline void*", .Name = "GetNameEntryFromName", .Offset = 0x0, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = 0x4,
			.bIsStatic = true, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF, .DefaultValue = "nullptr"
			}
		);
	}

	/* Add an error message to FName if ToString is used */
	if (!Off::InSDK::Name::bIsUsingAppendStringOverToString)
	{
		FName.Properties.insert(FName.Properties.begin(), {
			PredefinedMember {
				.Comment = "",
				.Type = "/*  Unlike FMemory::AppendString, FName::ToString allocates memory every time it is used.  */", .Name = NameArrayName, .Offset = 0x0, .Size = 0x0, .ArrayDim = 0x1, .Alignment = 0x4,
				.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "",
				.Type = "/*  This allocation needs to be freed using UE's FMemory::Free function.                   */", .Name = NameArrayName, .Offset = 0x0, .Size = 0x0, .ArrayDim = 0x1, .Alignment = 0x4,
				.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "",
				.Type = "/*  Inside of 'FName::GetRawString':                                                       */", .Name = NameArrayName, .Offset = 0x0, .Size = 0x0, .ArrayDim = 0x1, .Alignment = 0x4,
				.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "",
				.Type = "/*  1. Change \"thread_local FAllocatedString TempString(1024);\" to \"FString TempString;\"   */", .Name = NameArrayName, .Offset = 0x0, .Size = 0x0, .ArrayDim = 0x1, .Alignment = 0x4,
				.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "",
				.Type = "/*  2. Add a \"Free\" function to FString that calls FMemory::Free(Data);                    */", .Name = NameArrayName, .Offset = 0x0, .Size = 0x0, .ArrayDim = 0x1, .Alignment = 0x4,
				.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "",
				.Type = "/*  3. Replace \"TempString.Clear();\" with \"TempString.Free();\"                             */", .Name = NameArrayName, .Offset = 0x0, .Size = 0x0, .ArrayDim = 0x1, .Alignment = 0x4,
				.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
			},
			PredefinedMember {
				.Comment = "Free your allocation with FMemory::Free!",
				.Type = "static_assert(false, \"FName::ToString causes memory-leak. Read comment above!\")", .Name = NameArrayName, .Offset = 0x0, .Size = 0x4, .ArrayDim = 0x0, .Alignment = 0x4,
				.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF, .DefaultValue = "nullptr"
			},
		});
	}

	if (!Settings::Internal::bUseOutlineNumberName)
	{
		FName.Properties.push_back(PredefinedMember{
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = (Settings::Internal::bUseNamePool ? "uint32" : "int32"), .Name = "Number", .Offset = Off::FName::Number, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF, .DefaultValue = "0x0"
			}
		);
	}

	if (Settings::Internal::bUseCasePreservingName)
	{
		const int32 DisplayIndexOffset = Off::FName::Number == 4 ? 0x8 : 0x4;

		FName.Properties.push_back(PredefinedMember{
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "int32", .Name = "DisplayIndex", .Offset = DisplayIndexOffset, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF, .DefaultValue = "0x0"
			}
		);
	}

	SortMembers(FName.Properties);

	std::string GetRawStringWithAppendString = fmt::format(
		R"({{
	TCHAR buffer[1024];
    FString TempString(buffer, 0, 1024);

	if (!AppendString)
		InitInternal();

	InSDKUtils::CallGameFunction(reinterpret_cast<void({}*)(const FName*, FString&)>(AppendString), this, TempString);

	return TempString.ToString();
}}
)", Platform::Is32Bit() ? "__thiscall" : "");

	constexpr const char* GetRawStringWithInlinedAppendString =
		R"({
	TCHAR buffer[1024];
    FString TempString(buffer, 0, 1024);

	if (!AppendString)
		InitInternal();

	const void* NameEntry = InSDKUtils::CallGameFunction(reinterpret_cast<const void*(*)(uint32 CmpIdx)>(GetNameEntryFromName), ComparisonIndex);
	InSDKUtils::CallGameFunction(reinterpret_cast<void(*)(const void*, FString&)>(AppendString), NameEntry, TempString);

	std::string OutputString = TempString.ToString();

	if (Number > 0)
		OutputString += ("_" + std::to_string(Number - 1));

	return OutputString;
}
)";

	constexpr const char* GetRawStringWithNameArray =
		R"({
	if (!GNames)
		InitInternal();

	std::string RetStr = FName::GNames->GetEntryByIndex(GetDisplayIndex())->GetString();

	if (Number > 0)
		RetStr += ("_" + std::to_string(Number - 1));

	return RetStr;
}
)";

	constexpr const char* GetRawStringWithNameArrayWithOutlineNumber =
		R"({
	if (!GNames)
		InitInternal();

	const FNameEntry* Entry = FName::GNames->GetEntryByIndex(GetDisplayIndex());

	if (Entry->Header.Length == 0)
	{{
		if (Entry->Number > 0)
			return FName::GNames->GetEntryByIndex(Entry->NumberedName.Id)->GetString() + "_" + std::to_string(Entry->Number - 1);

		return FName::GNames->GetEntryByIndex(Entry->NumberedName.Id)->GetString();
	}}

	return Entry.GetString();
}
)";

	std::string GetRawStringBody;
	if (Off::InSDK::Name::AppendNameToString == 0)
	{
		GetRawStringBody = Settings::Internal::bUseOutlineNumberName ? GetRawStringWithNameArrayWithOutlineNumber : GetRawStringWithNameArray;
	}
	else
	{
		GetRawStringBody = Off::InSDK::Name::bIsAppendStringInlinedAndUsed ? GetRawStringWithInlinedAppendString : GetRawStringWithAppendString;
	}

	std::string GetNameEntryInitializationCode;

	if (Off::InSDK::Name::bIsAppendStringInlinedAndUsed)
		GetNameEntryInitializationCode = "\n\tGetNameEntryFromName = reinterpret_cast<void*>(InSDKUtils::GetImageBase() + Offsets::GetNameEntry);";

	FName.Functions =
	{
		/* constructors */
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "constexpr",
			.NameWithParams = fmt::format("explicit FName(int32 ComparisonIndex{}{})",
				!Settings::Internal::bUseOutlineNumberName ? ", uint32 Number = 0" : "",
				Settings::Internal::bUseCasePreservingName ? ", int32 DisplayIndex = 0" : ""),
			.Body =
fmt::format(R"(	: ComparisonIndex(ComparisonIndex){}{}
{{
}})",
!Settings::Internal::bUseOutlineNumberName ? ", Number(Number)" : "",
Settings::Internal::bUseCasePreservingName ? ", DisplayIndex(DisplayIndex)" : ""),
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "constexpr", .NameWithParams = "FName() = default;",
			.Body = "",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "constexpr", .NameWithParams = "FName(const FName&) = default;",
			.Body = "",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "constexpr", .NameWithParams = "FName(FName&&) = default;",
			.Body = "",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "constexpr FName&", .NameWithParams = "operator=(const FName&) = default;",
			.Body = "",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "constexpr  FName&", .NameWithParams = "operator=(FName&&) = default;",
			.Body = "",
			.bIsStatic = false, .bIsConst = false, .bIsBodyInline = true
		},
		/* static functions */
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "void", .NameWithParams = "InitInternal()", .Body =
fmt::format(R"({{
	{0} = {2}reinterpret_cast<{1}{2}>(InSDKUtils::GetImageBase() + Offsets::{0});{3}
}})", NameArrayName, NameArrayType, (Off::InSDK::Name::AppendNameToString == 0 && !Settings::Internal::bUseNamePool ? "*" : ""), GetNameEntryInitializationCode),
			.bIsStatic = true, .bIsConst = false, .bIsBodyInline = true
		},
		/* const functions */
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "bool", .NameWithParams = "IsNone()", .Body =
fmt::format(R"({{
	return !{}{};
}}
)",
Settings::Internal::bUseCasePreservingName ? "DisplayIndex" : "ComparisonIndex",
!Settings::Internal::bUseOutlineNumberName ? "&& !Number" : ""),
				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "int32", .NameWithParams = "GetDisplayIndex()", .Body =
fmt::format(R"({{
	return {};
}}
)", Settings::Internal::bUseCasePreservingName ? "DisplayIndex" : "ComparisonIndex"),
				.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "std::string", .NameWithParams = "GetRawString()", .Body = GetRawStringBody,
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "std::string", .NameWithParams = "ToString()", .Body = R"({
	std::string OutputString = GetRawString();

	size_t pos = OutputString.rfind('/');

	if (pos == std::string::npos)
		return OutputString;

	return OutputString.substr(pos + 1);
}
)",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		/* operators */
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "bool", .NameWithParams = "operator==(const FName& Other)", .Body =
fmt::format(R"({{
	return ComparisonIndex == Other.ComparisonIndex{};
}})", !Settings::Internal::bUseOutlineNumberName ? " && Number == Other.Number" : ""),
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "bool", .NameWithParams = "operator!=(const FName& Other)", .Body =
fmt::format(R"({{
	return ComparisonIndex != Other.ComparisonIndex{};
}})", !Settings::Internal::bUseOutlineNumberName ? " || Number != Other.Number" : ""),
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
	};

	if constexpr (Settings::CppGenerator::bAddManualOverrideOptions)
	{
		FName.Functions.insert(
			FName.Functions.begin() + 1,
			PredefinedFunction{
			.CustomComment = "",
			.ReturnType = "void", .NameWithParams = "InitManually(void* Location)", .Body =
fmt::format(R"({{
	{0} = {2}reinterpret_cast<{1}{2}>(Location);
}})", NameArrayName, NameArrayType, (Off::InSDK::Name::AppendNameToString == 0 && !Settings::Internal::bUseNamePool ? "*" : "")),
			.bIsStatic = true, .bIsConst = false, .bIsBodyInline = true
			});
	}

	/* Static `Init()` — resolves AppendString or GNames from imagebase and
	 * applies the matching per-game decryption hook. The consumer calls
	 * `FName::Init()` once at startup and never touches the static directly:
	 *
	 *   SDK::FName::Init();  // populates AppendString or GNames
	 *
	 * For TNameEntryArray games (PUBG): runs DecryptNameArray on the raw
	 * `ImageBase + Offsets::GNames`, then derefs once to get the live array.
	 * For FNamePool games (Valorant): runs DecryptNamePool on the raw address.
	 * For AppendString games: just casts `ImageBase + Offsets::AppendString`. */
	{
		std::string InitBody;
		if (Off::InSDK::Name::AppendNameToString != 0)
		{
			InitBody = R"({
	AppendString = reinterpret_cast<void*>(InSDKUtils::GetImageBase() + Offsets::AppendString);
})";
		}
		else if (Settings::Internal::bUseNamePool)
		{
			InitBody = R"({
	const uintptr_t Raw = InSDKUtils::GetImageBase() + Offsets::GNames;
	GNames = reinterpret_cast<FNamePool*>(FNamePool::DecryptNamePool(Raw));
})";
		}
		else
		{
			InitBody = R"({
	const uintptr_t Raw = InSDKUtils::GetImageBase() + Offsets::GNames;
	const uintptr_t PtrToPtr = TNameEntryArray::DecryptNameArray(Raw);
	GNames = *reinterpret_cast<TNameEntryArray**>(PtrToPtr);
})";
		}

		FName.Functions.insert(
			FName.Functions.begin() + 1,
			PredefinedFunction{
				.CustomComment = "",
				.ReturnType = "void", .NameWithParams = "Init()", .Body = InitBody,
				.bIsStatic = true, .bIsConst = false, .bIsBodyInline = true
			});
	}

	GenerateStruct(&FName, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);


	BasicHpp <<
		R"(
template<typename ClassType>
class TSubclassOf
{
	class UClass* ClassPtr;

public:
	TSubclassOf() = default;

	inline TSubclassOf(UClass* Class)
		: ClassPtr(Class)
	{
	}

	inline UClass* Get()
	{
		return ClassPtr;
	}

	inline operator UClass*() const
	{
		return ClassPtr;
	}

	template<typename Target, typename = std::enable_if<std::is_base_of_v<Target, ClassType>, bool>::type>
	inline operator TSubclassOf<Target>() const
	{
		return ClassPtr;
	}

	inline UClass* operator->()
	{
		return ClassPtr;
	}

	inline TSubclassOf& operator=(UClass* Class)
	{
		ClassPtr = Class;

		return *this;
	}

	inline bool operator==(const TSubclassOf& Other) const
	{
		return ClassPtr == Other.ClassPtr;
	}

	inline bool operator!=(const TSubclassOf& Other) const
	{
		return ClassPtr != Other.ClassPtr;
	}

	inline bool operator==(UClass* Other) const
	{
		return ClassPtr == Other;
	}

	inline bool operator!=(UClass* Other) const
	{
		return ClassPtr != Other;
	}
};
)";

	/* struct FStructBaseChain */
	PredefinedStruct FStructBaseChain = PredefinedStruct{
		.UniqueName = "FStructBaseChain", .Size = sizeof(void*) + sizeof(int32), .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = false, .bIsClass = false, .bIsUnion = false, .Super = nullptr
	};

	FStructBaseChain.Properties =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "FStructBaseChain**", .Name = "StructBaseChainArray", .Offset = 0x0, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "int32", .Name = "NumStructBasesInChainMinusOne", .Offset = sizeof(void*), .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	GenerateStruct(&FStructBaseChain, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);


	const int32 TextDataSize = (Off::InSDK::Text::InTextDataStringOffset + sizeof(FString));

	/* class FTextData */
	PredefinedStruct FTextData = PredefinedStruct{
		.UniqueName = "FTextData", .Size = TextDataSize, .Alignment = alignof(FString), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .bIsUnion = false, .Super = nullptr
	};

	FTextData.Properties =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "class FString", .Name = "TextSource", .Offset = Off::InSDK::Text::InTextDataStringOffset, .Size = sizeof(FString), .ArrayDim = 0x1, .Alignment = alignof(FString),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	BasicHpp << R"(
namespace FTextImpl
{)";
	GenerateStruct(&FTextData, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);
	BasicHpp << "}\n";

	/* class FText */
	PredefinedStruct FText = PredefinedStruct{
		.UniqueName = "FText", .Size = Off::InSDK::Text::TextSize, .Alignment = sizeof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .bIsUnion = false, .Super = nullptr
	};

	FText.Properties =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "class FTextImpl::FTextData*", .Name = "TextData", .Offset = Off::InSDK::Text::TextDatOffset, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = 0x1,
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	FText.Functions =
	{
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "const class FString&", .NameWithParams = "GetStringRef()", .Body =
R"({
	return TextData->TextSource;
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "std::string", .NameWithParams = "ToString()", .Body =
R"({
	return TextData->TextSource.ToString();
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
	};

	GenerateStruct(&FText, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);


	constexpr int32 FWeakObjectPtrSize = 0x08;

	/* class FWeakObjectPtr */
	PredefinedStruct FWeakObjectPtr = PredefinedStruct{
		.UniqueName = "FWeakObjectPtr", .Size = FWeakObjectPtrSize, .Alignment = alignof(int32), .bUseExplictAlignment = false, .bIsFinal = false, .bIsClass = true, .bIsUnion = false, .Super = nullptr
	};

	FWeakObjectPtr.Properties =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "int32", .Name = "ObjectIndex", .Offset = 0x0, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "int32", .Name = "ObjectSerialNumber", .Offset = 0x4, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	FWeakObjectPtr.Functions =
	{
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "class UObject*", .NameWithParams = "Get()", .Body =
R"({
	return UObject::GObjects->GetByIndex(ObjectIndex);
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = false
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "class UObject*", .NameWithParams = "operator->()", .Body =
R"({
	return UObject::GObjects->GetByIndex(ObjectIndex);
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = false
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "bool", .NameWithParams = "operator==(const FWeakObjectPtr& Other)", .Body =
R"({
	return ObjectIndex == Other.ObjectIndex;
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = false
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "bool", .NameWithParams = "operator!=(const FWeakObjectPtr& Other)", .Body =
R"({
	return ObjectIndex != Other.ObjectIndex;
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = false
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "bool", .NameWithParams = "operator==(const class UObject* Other)", .Body =
R"({
	return ObjectIndex == Other->Index;
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = false
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "bool", .NameWithParams = "operator!=(const class UObject* Other)", .Body =
R"({
	return ObjectIndex != Other->Index;
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = false
		},
	};

	GenerateStruct(&FWeakObjectPtr, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);


	BasicHpp <<
		R"(
template<typename UEType>
class TWeakObjectPtr : public FWeakObjectPtr
{
public:
	UEType* Get() const
	{
		return static_cast<UEType*>(FWeakObjectPtr::Get());
	}

	UEType* operator->() const
	{
		return static_cast<UEType*>(FWeakObjectPtr::Get());
	}
};
)";


	/* class FUniqueObjectGuid */
	PredefinedStruct FUniqueObjectGuid = PredefinedStruct{
		.UniqueName = "FUniqueObjectGuid", .Size = 0x10, .Alignment = alignof(uint32), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .bIsUnion = false, .Super = nullptr
	};

	FUniqueObjectGuid.Properties =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "uint32", .Name = "A", .Offset = 0x0, .Size = sizeof(uint32), .ArrayDim = 0x1, .Alignment = alignof(uint32),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "uint32", .Name = "B", .Offset = 0x4, .Size = sizeof(uint32), .ArrayDim = 0x1, .Alignment = alignof(uint32),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "uint32", .Name = "C", .Offset = 0x8, .Size = sizeof(uint32), .ArrayDim = 0x1, .Alignment = alignof(uint32),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "uint32", .Name = "D", .Offset = 0xC, .Size = sizeof(uint32), .ArrayDim = 0x1, .Alignment = alignof(uint32),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	GenerateStruct(&FUniqueObjectGuid, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);


	/* class TPersistentObjectPtr */
	PredefinedStruct TPersistentObjectPtr = PredefinedStruct{
		.CustomTemplateText = "template<typename TObjectID>",
		.UniqueName = "TPersistentObjectPtr", .Size = 0x0, .Alignment = sizeof(void*), .bUseExplictAlignment = false, .bIsFinal = false, .bIsClass = true, .bIsUnion = false, .Super = nullptr
	};

	const int32 ObjectIDOffset = Settings::Internal::bIsWeakObjectPtrWithoutTag ? 0x8 : 0xC;

	TPersistentObjectPtr.Properties =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "FWeakObjectPtr", .Name = "WeakPtr", .Offset = 0x0, .Size = FWeakObjectPtrSize, .ArrayDim = 0x1, .Alignment = alignof(int32),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "int32", .Name = "TagAtLastTest", .Offset = sizeof(void*), .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = sizeof(int32),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "TObjectID", .Name = "ObjectID", .Offset = ObjectIDOffset, .Size = 0x00, .ArrayDim = 0x1, .Alignment = 0x1,
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	if (Settings::Internal::bIsWeakObjectPtrWithoutTag)
		TPersistentObjectPtr.Properties.erase(TPersistentObjectPtr.Properties.begin() + 1); // TagAtLast

	TPersistentObjectPtr.Functions =
	{
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "class UObject*", .NameWithParams = "Get()", .Body =
R"({
	return WeakPtr.Get();
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "class UObject*", .NameWithParams = "operator->()", .Body =
R"({
	return WeakPtr.Get();
})",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
	};

	GenerateStruct(&TPersistentObjectPtr, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);


	/* class TLazyObjectPtr */
	BasicHpp <<
		R"(
template<typename UEType>
class TLazyObjectPtr : public TPersistentObjectPtr<FUniqueObjectGuid>
{
public:
	UEType* Get() const
	{
		return static_cast<UEType*>(TPersistentObjectPtr::Get());
	}
	UEType* operator->() const
	{
		return static_cast<UEType*>(TPersistentObjectPtr::Get());
	}
};
)";


	// Start Namespace 'FakeSoftObjectPtr'
	BasicHpp <<
		R"(
namespace FakeSoftObjectPtr
{
)";

	UEStruct SoftObjectPath = ObjectArray::FindObjectFast<UEStruct>("SoftObjectPath");

	/* if SoftObjectPath doesn't exist just generate FStringAssetReference and call it SoftObjectPath, it's basically the same thing anyways */
	if (!SoftObjectPath)
	{
		/* struct FSoftObjectPtr */
		PredefinedStruct FSoftObjectPath = PredefinedStruct{
			.UniqueName = "FSoftObjectPath", .Size = 0x10, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = false, .bIsClass = false, .bIsUnion = false, .Super = nullptr
		};

		FSoftObjectPath.Properties =
		{
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "class FString", .Name = "AssetLongPathname", .Offset = 0x0, .Size = sizeof(FString), .ArrayDim = 0x1, .Alignment = alignof(FString),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
		};

		GenerateStruct(&FSoftObjectPath, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);
	}
	else /* if SoftObjectPath exists generate a copy of it within this namespace to allow for TSoftObjectPtr declaration (comes before real SoftObjectPath) */
	{
		UEProperty Assetpath = SoftObjectPath.FindMember("AssetPath");

		if (Assetpath && Assetpath.IsA(EClassCastFlags::StructProperty))
			GenerateStruct(Assetpath.Cast<UEStructProperty>().GetUnderlayingStruct(), BasicHpp, BasicCpp, BasicHpp, AssertionsFile);

		GenerateStruct(SoftObjectPath, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);
	}

	// Start Namespace 'FakeSoftObjectPtr'
	BasicHpp << "\n}\n";


	/* struct FSoftObjectPtr */
	BasicHpp <<
		R"(
class FSoftObjectPtr : public TPersistentObjectPtr<FakeSoftObjectPtr::FSoftObjectPath>
{
};
)";


	/* struct TSoftObjectPtr */
	BasicHpp <<
		R"(
template<typename UEType>
class TSoftObjectPtr : public FSoftObjectPtr
{
public:
	UEType* Get() const
	{
		return static_cast<UEType*>(TPersistentObjectPtr::Get());
	}
	UEType* operator->() const
	{
		return static_cast<UEType*>(TPersistentObjectPtr::Get());
	}
};
)";

	/* struct TSoftClassPtr */
	BasicHpp <<
		R"(
template<typename UEType>
class TSoftClassPtr : public FSoftObjectPtr
{
public:
	UEType* Get() const
	{
		return static_cast<UEType*>(TPersistentObjectPtr::Get());
	}
	UEType* operator->() const
	{
		return static_cast<UEType*>(TPersistentObjectPtr::Get());
	}
};
)";	

	if constexpr (Settings::EngineCore::bEnableEncryptedObjectPropertySupport)
	{
		/* struct FEncryptedObjPtr */
		BasicHpp <<
			R"(
class FEncryptedObjPtr
{
public:
	union
	{
		uint64_t EncryptionIndex : 4;
		uintptr_t Object : 60;
	};
};
)";

		/* struct TEncryptedObjPtr */
		BasicHpp <<
			R"(
template<typename UEType>
class TEncryptedObjPtr : public FEncryptedObjPtr
{
public:

public:
	UEType* Get()
	{
		return reinterpret_cast<UEType*>(Object);
	}
	const UEType* Get() const
	{
		return reinterpret_cast<const UEType*>(Object);
	}

	UEType* operator->()
	{
		return Get();
	}
	const UEType* operator->() const
	{
		return Get();
	}

	inline operator UEType* ()
	{
		return Get();
	}
	inline operator UEType* () const
	{
		return Get();
	}

public:
	inline bool operator==(const FEncryptedObjPtr& Other) const
	{
		return Object == Other.Object;
	}
	inline bool operator!=(const FEncryptedObjPtr& Other) const
	{
		return Object != Other.Object;
	}

	inline explicit operator bool() const
	{
		return Object != NULL;
	}

public:
	inline uint8_t GetEncryptionIndex() const
	{
		return static_cast<uint8_t>(EncryptionIndex);
	}
};
)";
	}


	/* class FScriptInterface */
	PredefinedStruct FScriptInterface = PredefinedStruct{
		.UniqueName = "FScriptInterface", .Size = sizeof(void*) + sizeof(void*), .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = false, .bIsClass = true, .bIsUnion = false, .Super = nullptr
	};

	FScriptInterface.Properties =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "UObject*", .Name = "ObjectPointer", .Offset = 0x0, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "void*", .Name = "InterfacePointer", .Offset = sizeof(void*), .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	FScriptInterface.Functions =
	{
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "class UObject*", .NameWithParams = "GetObjectRef()", .Body =
R"({
	return ObjectPointer;
}
)",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
		PredefinedFunction {
			.CustomComment = "",
			.ReturnType = "void*", .NameWithParams = "GetInterfaceRef()", .Body =
R"({
	return InterfacePointer;
}
)",
			.bIsStatic = false, .bIsConst = true, .bIsBodyInline = true
		},
	};

	GenerateStruct(&FScriptInterface, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);


	/* class TScriptInterface */
	PredefinedStruct TScriptInterface = PredefinedStruct{
		.CustomTemplateText = "template<class InterfaceType>",
		.UniqueName = "TScriptInterface", .Size = sizeof(void*) * 2, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .bIsUnion = false, .Super = &FScriptInterface
	};

	GenerateStruct(&TScriptInterface, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);


	if (Settings::Internal::bUseFProperty)
	{
		/* class FFieldPath */
		PredefinedStruct FFieldPath = PredefinedStruct{
			.UniqueName = "FFieldPath", .Size = PropertySizes::FieldPathProperty, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = false, .bIsClass = true, .bIsUnion = false, .Super = nullptr
		};

		FFieldPath.Properties =
		{
			PredefinedMember {
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "class FField*", .Name = "ResolvedField", .Offset = 0x0, .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			},
		};

		/* #ifdef WITH_EDITORONLY_DATA */
		const bool bIsWithEditorOnlyData = PropertySizes::FieldPathProperty > 0x20;

		if (bIsWithEditorOnlyData)
		{
			FFieldPath.Properties.emplace_back
			(
				PredefinedMember{
					.Comment = "NOT AUTO-GENERATED PROPERTY",
					.Type = "class FFieldClass*", .Name = "InitialFieldClass", .Offset = sizeof(void*), .Size = sizeof(void*), .ArrayDim = 0x1, .Alignment = alignof(void*),
					.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
				}
			);
			FFieldPath.Properties.emplace_back
			(
				PredefinedMember{
					.Comment = "NOT AUTO-GENERATED PROPERTY",
					.Type = "int32", .Name = "FieldPathSerialNumber", .Offset = sizeof(void*) * 2, .Size = sizeof(int32), .ArrayDim = 0x1, .Alignment = alignof(int32),
					.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
				}
			);
		}

		FFieldPath.Properties.push_back
		(
			PredefinedMember{
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "TWeakObjectPtr<class UStruct>", .Name = "ResolvedOwner", .Offset = (bIsWithEditorOnlyData ? (int)sizeof(void*) + (int)sizeof(void*) + (int)sizeof(int32) : (int)sizeof(void*)), .Size = 0x8, .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			}
		);
		FFieldPath.Properties.push_back
		(
			PredefinedMember{
				.Comment = "NOT AUTO-GENERATED PROPERTY",
				.Type = "TArray<FName>", .Name = "Path", .Offset = (bIsWithEditorOnlyData ? (int)sizeof(void*) + (int)sizeof(void*) + (int)sizeof(int32) + 0x08 : (int)sizeof(void*) * 2), .Size = sizeof(TArray<int>), .ArrayDim = 0x1, .Alignment = alignof(void*),
				.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
			}
		);

		GenerateStruct(&FFieldPath, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);

		/* class TFieldPath */
		PredefinedStruct TFieldPath = PredefinedStruct{
			.CustomTemplateText = "template<class PropertyType>",
			.UniqueName = "TFieldPath", .Size = PropertySizes::FieldPathProperty, .Alignment = alignof(void*), .bUseExplictAlignment = false, .bIsFinal = true, .bIsClass = true, .bIsUnion = false, .Super = &FFieldPath
		};

		GenerateStruct(&TFieldPath, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);



		// TOptional
		BasicHpp << R"(

template<typename OptionalType, bool bIsIntrusiveUnsetCheck = false>
class TOptional
{
private:
	template<int32 TypeSize>
	struct OptionalWithBool
	{
		static_assert(TypeSize > 0x0, "TOptional can not store an empty type!");

		uint8 Value[TypeSize];
		bool bIsSet;
	};

private:
	using ValueType = std::conditional_t<bIsIntrusiveUnsetCheck, uint8[sizeof(OptionalType)], OptionalWithBool<sizeof(OptionalType)>>;

private:
	alignas(OptionalType) ValueType StoredValue;

private:
	inline uint8* GetValueBytes()
	{
		if constexpr (!bIsIntrusiveUnsetCheck)
			return StoredValue.Value;

		return StoredValue;
	}

	inline const uint8* GetValueBytes() const
	{
		if constexpr (!bIsIntrusiveUnsetCheck)
			return StoredValue.Value;

		return StoredValue;
	}
public:

	inline OptionalType& GetValueRef()
	{
		return *reinterpret_cast<OptionalType*>(GetValueBytes());
	}

	inline const OptionalType& GetValueRef() const
	{
		return *reinterpret_cast<const OptionalType*>(GetValueBytes());
	}

	inline bool IsSet() const
	{
		if constexpr (!bIsIntrusiveUnsetCheck)
			return StoredValue.bIsSet;

		constexpr char ZeroBytes[sizeof(OptionalType)];

		return memcmp(GetValueBytes(), &ZeroBytes, sizeof(OptionalType)) == 0;
	}

	inline explicit operator bool() const
	{
		return IsSet();
	}
};

)";
	} /* End 'if (Settings::Internal::bUseFProperty)' */

	const auto ScriptDelegateSize = (FWeakObjectPtrSize + Off::InSDK::Name::FNameSize);

	/* FScriptDelegate */
	PredefinedStruct FScriptDelegate = PredefinedStruct{
		.UniqueName = "FScriptDelegate", .Size = ScriptDelegateSize, .Alignment = 0x4, .bUseExplictAlignment = false, .bIsFinal = false, .bIsClass = false, .bIsUnion = false, .Super = nullptr
	};

	FScriptDelegate.Properties =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "FWeakObjectPtr", .Name = "Object", .Offset = 0x0, .Size = FWeakObjectPtrSize, .ArrayDim = 0x1, .Alignment = 0x4,
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "FName", .Name = "FunctionName", .Offset = FWeakObjectPtrSize, .Size = Off::InSDK::Name::FNameSize, .ArrayDim = 0x1, .Alignment = 0x4,
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	GenerateStruct(&FScriptDelegate, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);


	/* TDelegate */
	PredefinedStruct TDelegate = PredefinedStruct{
		.CustomTemplateText = "template<typename FunctionSignature>",
		.UniqueName = "TDelegate", .Size = PropertySizes::DelegateProperty, .Alignment = 0x4, .bUseExplictAlignment = false, .bIsFinal = false, .bIsClass = true, .bIsUnion = false, .Super = nullptr
	};

	TDelegate.Properties =
	{
		PredefinedMember{
			.Comment = "Validity Check",
			.Type = "static_assert(false, \"TDelegate should be used with a function signature. Something might be wrong in the SDK-Generator.\")", .Name = "", .Offset = 0x0, .Size = 0x10, .ArrayDim = 0x0, .Alignment = 0x8,
			.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
		},
	};


	GenerateStruct(&TDelegate, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);

	/* TDelegate<Ret(Args...)> */
	PredefinedStruct TDelegateSpezialiation = PredefinedStruct{
		.CustomTemplateText = "template<typename Ret, typename... Args>",
		.UniqueName = "TDelegate<Ret(Args...)>", .Size = PropertySizes::DelegateProperty, .Alignment = 0x1, .bUseExplictAlignment = false, .bIsFinal = false, .bIsClass = true, .bIsUnion = false, .Super = nullptr
	};

	TDelegateSpezialiation.Properties =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "FScriptDelegate", .Name = "BoundFunction", .Offset = 0x0, .Size = ScriptDelegateSize, .ArrayDim = 0x1, .Alignment = sizeof(void*),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		}
	};

	GenerateStruct(&TDelegateSpezialiation, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);

	/* TMulticastInlineDelegate */
	PredefinedStruct TMulticastInlineDelegate = PredefinedStruct{
		.CustomTemplateText = "template<typename FunctionSignature>",
		.UniqueName = "TMulticastInlineDelegate", .Size = PropertySizes::MulticastInlineDelegateProperty, .Alignment = alignof(TArray<int>), .bUseExplictAlignment = false, .bIsFinal = false, .bIsClass = true, .bIsUnion = false, .Super = nullptr
	};
	
	TMulticastInlineDelegate.Properties =
	{
		PredefinedMember{
			.Comment = "Validity Check",
			.Type = "static_assert(false, \"TMulticastInlineDelegate should be used with a function signature. Something might be wrong in the SDK-Generator.\")", .Name = "", .Offset = 0x0, .Size = 0x10, .ArrayDim = 0x0, .Alignment = 0x8,
			.bIsStatic = true, .bIsZeroSizeMember = true, .bIsBitField = false, .BitIndex = 0xFF
		},
	};

	GenerateStruct(&TMulticastInlineDelegate, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);

	/* TMulticastInlineDelegate<Ret(Args...)> */
	PredefinedStruct TMulticastInlineDelegateSpezialiation = PredefinedStruct{
		.CustomTemplateText = "template<typename Ret, typename... Args>",
		.UniqueName = "TMulticastInlineDelegate<Ret(Args...)>", .Size = 0x0, .Alignment = 0x1, .bUseExplictAlignment = false, .bIsFinal = false, .bIsClass = true, .bIsUnion = false, .Super = nullptr
	};

	TMulticastInlineDelegateSpezialiation.Properties =
	{
		PredefinedMember {
			.Comment = "NOT AUTO-GENERATED PROPERTY",
			.Type = "TArray<FScriptDelegate>", .Name = "InvocationList", .Offset = static_cast<int32>(PropertySizes::MulticastInlineDelegateProperty - sizeof(TArray<int>)), .Size = sizeof(TArray<int>), .ArrayDim = 0x1, .Alignment = alignof(TArray<int>),
			.bIsStatic = false, .bIsZeroSizeMember = false, .bIsBitField = false, .BitIndex = 0xFF
		}
	};

	GenerateStruct(&TMulticastInlineDelegateSpezialiation, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);


	/* UE_ENUM_OPERATORS - enum flag operations */
	BasicHpp <<
		R"(
#define UE_ENUM_OPERATORS(EEnumClassType)																													\
																																							\
inline constexpr EEnumClassType operator|(EEnumClassType Left, EEnumClassType Right)															 			\
{																																							\
	using EnumUnderlayingType = std::underlying_type<EEnumClassType>::type;																					\
																																							\
	return static_cast<EEnumClassType>(static_cast<EnumUnderlayingType>(Left) | static_cast<EnumUnderlayingType>(Right));									\
}																																							\
																																							\
inline EEnumClassType& operator|=(EEnumClassType& Left, EEnumClassType Right)																				\
{																																							\
    using EnumUnderlayingType = std::underlying_type<EEnumClassType>::type;																					\
																																							\
    reinterpret_cast<EnumUnderlayingType&>(Left) |= static_cast<EnumUnderlayingType>(Right);																\
	return Left;																																			\
}																																							\
																																							\
inline bool operator&(EEnumClassType Left, EEnumClassType Right)																							\
{																																							\
	using EnumUnderlayingType = std::underlying_type<EEnumClassType>::type;																					\
																																							\
	return ((static_cast<EnumUnderlayingType>(Left) & static_cast<EnumUnderlayingType>(Right)) == static_cast<EnumUnderlayingType>(Right));					\
}
)";

	/* enum class EObjectFlags */
	BasicHpp <<
		R"(
enum class EObjectFlags : uint32
{
	NoFlags							= 0x00000000,

	Public							= 0x00000001,
	Standalone						= 0x00000002,
	MarkAsNative					= 0x00000004,
	Transactional					= 0x00000008,
	ClassDefaultObject				= 0x00000010,
	ArchetypeObject					= 0x00000020,
	Transient						= 0x00000040,

	MarkAsRootSet					= 0x00000080,
	TagGarbageTemp					= 0x00000100,

	NeedInitialization				= 0x00000200,
	NeedLoad						= 0x00000400,
	KeepForCooker					= 0x00000800,
	NeedPostLoad					= 0x00001000,
	NeedPostLoadSubobjects			= 0x00002000,
	NewerVersionExists				= 0x00004000,
	BeginDestroyed					= 0x00008000,
	FinishDestroyed					= 0x00010000,

	BeingRegenerated				= 0x00020000,
	DefaultSubObject				= 0x00040000,
	WasLoaded						= 0x00080000,
	TextExportTransient				= 0x00100000,
	LoadCompleted					= 0x00200000,
	InheritableComponentTemplate	= 0x00400000,
	DuplicateTransient				= 0x00800000,
	StrongRefOnFrame				= 0x01000000,
	NonPIEDuplicateTransient		= 0x02000000,
	Dynamic							= 0x04000000,
	WillBeLoaded					= 0x08000000,
	HasExternalPackage				= 0x10000000,

	MirroredGarbage					= 0x40000000,
	AllocatedInSharedPage			= 0x80000000,
};
)";


	/* enum class EFunctionFlags */
	BasicHpp <<
		R"(
enum class EFunctionFlags : uint32
{
	None							= 0x00000000,

	Final							= 0x00000001,
	RequiredAPI						= 0x00000002,
	BlueprintAuthorityOnly			= 0x00000004, 
	BlueprintCosmetic				= 0x00000008, 
	Net								= 0x00000040,  
	NetReliable						= 0x00000080, 
	NetRequest						= 0x00000100,	
	Exec							= 0x00000200,	
	Native							= 0x00000400,	
	Event							= 0x00000800,   
	NetResponse						= 0x00001000,  
	Static							= 0x00002000,   
	NetMulticast					= 0x00004000,	
	UbergraphFunction				= 0x00008000,  
	MulticastDelegate				= 0x00010000,
	Public							= 0x00020000,	
	Private							= 0x00040000,	
	Protected						= 0x00080000,
	Delegate						= 0x00100000,	
	NetServer						= 0x00200000,	
	HasOutParms						= 0x00400000,	
	HasDefaults						= 0x00800000,
	NetClient						= 0x01000000,
	DLLImport						= 0x02000000,
	BlueprintCallable				= 0x04000000,
	BlueprintEvent					= 0x08000000,
	BlueprintPure					= 0x10000000,	
	EditorOnly						= 0x20000000,	
	Const							= 0x40000000,
	NetValidate						= 0x80000000,

	AllFlags						= 0xFFFFFFFF,
};
)";

	/* enum class EClassFlags */
	BasicHpp <<
		R"(
enum class EClassFlags : uint32
{
	CLASS_None						= 0x00000000u,
	Abstract						= 0x00000001u,
	DefaultConfig					= 0x00000002u,
	Config							= 0x00000004u,
	Transient						= 0x00000008u,
	Parsed							= 0x00000010u,
	MatchedSerializers				= 0x00000020u,
	ProjectUserConfig				= 0x00000040u,
	Native							= 0x00000080u,
	NoExport						= 0x00000100u,
	NotPlaceable					= 0x00000200u,
	PerObjectConfig					= 0x00000400u,
	ReplicationDataIsSetUp			= 0x00000800u,
	EditInlineNew					= 0x00001000u,
	CollapseCategories				= 0x00002000u,
	Interface						= 0x00004000u,
	CustomConstructor				= 0x00008000u,
	Const							= 0x00010000u,
	LayoutChanging					= 0x00020000u,
	CompiledFromBlueprint			= 0x00040000u,
	MinimalAPI						= 0x00080000u,
	RequiredAPI						= 0x00100000u,
	DefaultToInstanced				= 0x00200000u,
	TokenStreamAssembled			= 0x00400000u,
	HasInstancedReference			= 0x00800000u,
	Hidden							= 0x01000000u,
	Deprecated						= 0x02000000u,
	HideDropDown					= 0x04000000u,
	GlobalUserConfig				= 0x08000000u,
	Intrinsic						= 0x10000000u,
	Constructed						= 0x20000000u,
	ConfigDoNotCheckDefaults		= 0x40000000u,
	NewerVersionExists				= 0x80000000u,
};
)";

	/* enum class EClassCastFlags */
	BasicHpp <<
		R"(
enum class EClassCastFlags : uint64
{
	None = 0x0000000000000000,

	Field								= 0x0000000000000001,
	Int8Property						= 0x0000000000000002,
	Enum								= 0x0000000000000004,
	Struct								= 0x0000000000000008,
	ScriptStruct						= 0x0000000000000010,
	Class								= 0x0000000000000020,
	ByteProperty						= 0x0000000000000040,
	IntProperty							= 0x0000000000000080,
	FloatProperty						= 0x0000000000000100,
	UInt64Property						= 0x0000000000000200,
	ClassProperty						= 0x0000000000000400,
	UInt32Property						= 0x0000000000000800,
	InterfaceProperty					= 0x0000000000001000,
	NameProperty						= 0x0000000000002000,
	StrProperty							= 0x0000000000004000,
	Property							= 0x0000000000008000,
	ObjectProperty						= 0x0000000000010000,
	BoolProperty						= 0x0000000000020000,
	UInt16Property						= 0x0000000000040000,
	Function							= 0x0000000000080000,
	StructProperty						= 0x0000000000100000,
	ArrayProperty						= 0x0000000000200000,
	Int64Property						= 0x0000000000400000,
	DelegateProperty					= 0x0000000000800000,
	NumericProperty						= 0x0000000001000000,
	MulticastDelegateProperty			= 0x0000000002000000,
	ObjectPropertyBase					= 0x0000000004000000,
	WeakObjectProperty					= 0x0000000008000000,
	LazyObjectProperty					= 0x0000000010000000,
	SoftObjectProperty					= 0x0000000020000000,
	TextProperty						= 0x0000000040000000,
	Int16Property						= 0x0000000080000000,
	DoubleProperty						= 0x0000000100000000,
	SoftClassProperty					= 0x0000000200000000,
	Package								= 0x0000000400000000,
	Level								= 0x0000000800000000,
	Actor								= 0x0000001000000000,
	PlayerController					= 0x0000002000000000,
	Pawn								= 0x0000004000000000,
	SceneComponent						= 0x0000008000000000,
	PrimitiveComponent					= 0x0000010000000000,
	SkinnedMeshComponent				= 0x0000020000000000,
	SkeletalMeshComponent				= 0x0000040000000000,
	Blueprint							= 0x0000080000000000,
	DelegateFunction					= 0x0000100000000000,
	StaticMeshComponent					= 0x0000200000000000,
	MapProperty							= 0x0000400000000000,
	SetProperty							= 0x0000800000000000,
	EnumProperty						= 0x0001000000000000,
	USparseDelegateFunction				= 0x0002000000000000,
	MulticastInlineDelegateProperty	    = 0x0004000000000000,
	MulticastSparseDelegateProperty	    = 0x0008000000000000,
	FieldPathProperty					= 0x0010000000000000,
	LargeWorldCoordinatesRealProperty	= 0x0080000000000000,
	OptionalProperty					= 0x0100000000000000,
	VValueProperty						= 0x0200000000000000,
	VerseVMClass						= 0x0400000000000000,
	VRestValueProperty					= 0x0800000000000000,
	Utf8StrProperty						= 0x1000000000000000,
	AnsiStrProperty						= 0x2000000000000000,
	VCellProperty						= 0x4000000000000000,
};
)";

	/* enum class EPropertyFlags */
	BasicHpp <<
		R"(
enum class EPropertyFlags : uint64
{
	None								= 0x0000000000000000,

	Edit								= 0x0000000000000001,
	ConstParm							= 0x0000000000000002,
	BlueprintVisible					= 0x0000000000000004,
	ExportObject						= 0x0000000000000008,
	BlueprintReadOnly					= 0x0000000000000010,
	Net									= 0x0000000000000020,
	EditFixedSize						= 0x0000000000000040,
	Parm								= 0x0000000000000080,
	OutParm								= 0x0000000000000100,
	ZeroConstructor						= 0x0000000000000200,
	ReturnParm							= 0x0000000000000400,
	DisableEditOnTemplate 				= 0x0000000000000800,

	Transient							= 0x0000000000002000,
	Config								= 0x0000000000004000,

	DisableEditOnInstance				= 0x0000000000010000,
	EditConst							= 0x0000000000020000,
	GlobalConfig						= 0x0000000000040000,
	InstancedReference					= 0x0000000000080000,	

	DuplicateTransient					= 0x0000000000200000,	
	SubobjectReference					= 0x0000000000400000,	

	SaveGame							= 0x0000000001000000,
	NoClear								= 0x0000000002000000,

	ReferenceParm						= 0x0000000008000000,
	BlueprintAssignable					= 0x0000000010000000,
	Deprecated							= 0x0000000020000000,
	IsPlainOldData						= 0x0000000040000000,
	RepSkip								= 0x0000000080000000,
	RepNotify							= 0x0000000100000000, 
	Interp								= 0x0000000200000000,
	NonTransactional					= 0x0000000400000000,
	EditorOnly							= 0x0000000800000000,
	NoDestructor						= 0x0000001000000000,

	AutoWeak							= 0x0000004000000000,
	ContainsInstancedReference			= 0x0000008000000000,	
	AssetRegistrySearchable				= 0x0000010000000000,
	SimpleDisplay						= 0x0000020000000000,
	AdvancedDisplay						= 0x0000040000000000,
	Protected							= 0x0000080000000000,
	BlueprintCallable					= 0x0000100000000000,
	BlueprintAuthorityOnly				= 0x0000200000000000,
	TextExportTransient					= 0x0000400000000000,
	NonPIEDuplicateTransient			= 0x0000800000000000,
	ExposeOnSpawn						= 0x0001000000000000,
	PersistentInstance					= 0x0002000000000000,
	UObjectWrapper						= 0x0004000000000000, 
	HasGetValueTypeHash					= 0x0008000000000000, 
	NativeAccessSpecifierPublic			= 0x0010000000000000,	
	NativeAccessSpecifierProtected		= 0x0020000000000000,
	NativeAccessSpecifierPrivate		= 0x0040000000000000,	
	SkipSerialization					= 0x0080000000000000, 
};
)";

	/* enum class EClassCastFlags */
	BasicHpp << R"(
UE_ENUM_OPERATORS(EObjectFlags);
UE_ENUM_OPERATORS(EFunctionFlags);
UE_ENUM_OPERATORS(EClassFlags);
UE_ENUM_OPERATORS(EClassCastFlags);
UE_ENUM_OPERATORS(EPropertyFlags);
)";



	/* Write Predefined Structs into Basic.hpp */
	for (const PredefinedStruct& Predefined : PredefinedStructs)
	{
		GenerateStruct(&Predefined, BasicHpp, BasicCpp, BasicHpp, AssertionsFile);
	}


	/* Cyclic dependencies-fixing helper classes */

	// Start Namespace 'CyclicDependencyFixupImpl'
	BasicHpp <<
		R"(
namespace CyclicDependencyFixupImpl
{
)";

	/*
	* Implemenation node:
	*	Maybe: when inheriting form TCylicStructFixup/TCyclicClassFixup use the aligned size, else use UnalignedSize
	*/

	/* TStructOrderFixup */
	BasicHpp << R"(
/*
* A wrapper for a Byte-Array of padding, that allows for casting to the actual underlaiyng type. Used for undefined structs in cylic headers.
*/
template<typename UnderlayingStructType, int32 Size, int32 Align>
struct alignas(Align) TCylicStructFixup
{
private:
	uint8 Pad[Size];

public:
	      UnderlayingStructType& GetTyped()       { return reinterpret_cast<      UnderlayingStructType&>(*this); }
	const UnderlayingStructType& GetTyped() const { return reinterpret_cast<const UnderlayingStructType&>(*this); }
};
)";
	BasicHpp << R"(
/*
* A wrapper for a Byte-Array of padding, that inherited from UObject allows for casting to the actual underlaiyng type and access to basic UObject functionality. For cyclic classes.
*/
template<typename UnderlayingClassType, int32 Size, int32 Align = sizeof(void*), class BaseClassType = class UObject>
struct alignas(Align) TCyclicClassFixup : public BaseClassType
{
private:
	uint8 Pad[Size];

public:
	UnderlayingClassType*       GetTyped()       { return reinterpret_cast<      UnderlayingClassType*>(this); }
	const UnderlayingClassType* GetTyped() const { return reinterpret_cast<const UnderlayingClassType*>(this); }
};

)";

	BasicHpp << "}\n\n";
	// End Namespace 'CyclicDependencyFixupImpl'


	BasicHpp << R"(
template<typename UnderlayingStructType, int32 Size, int32 Align>
using TStructCycleFixup = CyclicDependencyFixupImpl::TCylicStructFixup<UnderlayingStructType, Size, Align>;


template<typename UnderlayingClassType, int32 Size, int32 Align = 0x8>
using TObjectBasedCycleFixup = CyclicDependencyFixupImpl::TCyclicClassFixup<UnderlayingClassType, Size, Align, class UObject>;

template<typename UnderlayingClassType, int32 Size, int32 Align = 0x8>
using TActorBasedCycleFixup = CyclicDependencyFixupImpl::TCyclicClassFixup<UnderlayingClassType, Size, Align, class AActor>;
)";


	WriteFileEnd(BasicHpp, EFileType::BasicHpp);
	WriteFileEnd(BasicCpp, EFileType::BasicCpp);
}


/* See https://github.com/Fischsalat/UnrealContainers/blob/master/UnrealContainers/UnrealContainersNoAlloc.h */
void CppGenerator::GenerateUnrealContainers(StreamType& UEContainersHeader)
{
	WriteFileHead(UEContainersHeader, nullptr, EFileType::UnrealContainers, 
		"Container implementations with iterators. See https://github.com/Fischsalat/UnrealContainers", "#include <string>\n#include <stdexcept>\n#include <iostream>\n#include <optional>");


	UEContainersHeader << fmt::format(R"(
namespace UC
{{
	typedef int8_t  int8;
	typedef int16_t int16;
	typedef int32_t int32;
	typedef int64_t int64;

	typedef uint8_t  uint8;
	typedef uint16_t uint16;
	typedef uint32_t uint32;
	typedef uint64_t uint64;

	/* TCHAR width set by dumper's UEVERSION (matches the target UE's FString char type). */
	{}

	/* UnrealString = std::basic_string<TCHAR>. Adapts to TCHAR — std::wstring or std::u16string. */
	using UnrealString = std::basic_string<TCHAR>;)", SDKTCharAlias);

	UEContainersHeader << R"(

	/*
	 * UTF-8 → TCHAR-string conversion. Handles 16-bit (UTF-16 + surrogates) and 32-bit (UTF-32) TCHAR.
	 * Templated so `if constexpr` correctly discards the unused branch at instantiation time.
	 */
	template<typename CharT = TCHAR>
	inline std::basic_string<CharT> Utf8ToTChar(const char* Data, size_t Length)
	{
		std::basic_string<CharT> Out;
		Out.reserve(Length);
		for (size_t i = 0; i < Length; )
		{
			uint8_t b = static_cast<uint8_t>(Data[i]);
			uint32_t Code = 0;
			if (b < 0x80) {
				Code = b; i += 1;
			} else if ((b & 0xE0) == 0xC0 && i + 1 < Length) {
				Code = ((b & 0x1F) << 6) | (static_cast<uint8_t>(Data[i+1]) & 0x3F); i += 2;
			} else if ((b & 0xF0) == 0xE0 && i + 2 < Length) {
				Code = ((b & 0x0F) << 12) | ((static_cast<uint8_t>(Data[i+1]) & 0x3F) << 6) | (static_cast<uint8_t>(Data[i+2]) & 0x3F); i += 3;
			} else if ((b & 0xF8) == 0xF0 && i + 3 < Length) {
				Code = ((b & 0x07) << 18) | ((static_cast<uint8_t>(Data[i+1]) & 0x3F) << 12) | ((static_cast<uint8_t>(Data[i+2]) & 0x3F) << 6) | (static_cast<uint8_t>(Data[i+3]) & 0x3F); i += 4;
			} else {
				i += 1; continue;
			}
			if constexpr (sizeof(CharT) == 2) {
				if (Code < 0x10000) {
					Out.push_back(static_cast<CharT>(Code));
				} else {
					Code -= 0x10000;
					Out.push_back(static_cast<CharT>(0xD800 | (Code >> 10)));
					Out.push_back(static_cast<CharT>(0xDC00 | (Code & 0x3FF)));
				}
			} else {
				Out.push_back(static_cast<CharT>(Code));
			}
		}
		return Out;
	}

	/*
	 * TCHAR-string → UTF-8 conversion. Handles 16-bit (UTF-16 + surrogates) and 32-bit (UTF-32) TCHAR.
	 * Templated so `if constexpr` correctly discards the unused branch at instantiation time.
	 */
	template<typename CharT = TCHAR>
	inline std::string TCharToUtf8(const CharT* Data, size_t Length)
	{
		std::string Out;
		Out.reserve(Length);
		for (size_t i = 0; i < Length; ++i)
		{
			uint32_t Code;
			if constexpr (sizeof(CharT) == 2) {
				Code = static_cast<uint16_t>(Data[i]);
				if (Code >= 0xD800 && Code <= 0xDBFF && (i + 1) < Length) {
					uint32_t Low = static_cast<uint16_t>(Data[i + 1]);
					if (Low >= 0xDC00 && Low <= 0xDFFF) {
						Code = 0x10000 + (((Code - 0xD800) << 10) | (Low - 0xDC00));
						++i;
					}
				}
			} else {
				Code = static_cast<uint32_t>(Data[i]);
			}
			if (Code < 0x80) {
				Out.push_back(static_cast<char>(Code));
			} else if (Code < 0x800) {
				Out.push_back(static_cast<char>(0xC0 | (Code >> 6)));
				Out.push_back(static_cast<char>(0x80 | (Code & 0x3F)));
			} else if (Code < 0x10000) {
				Out.push_back(static_cast<char>(0xE0 | (Code >> 12)));
				Out.push_back(static_cast<char>(0x80 | ((Code >> 6) & 0x3F)));
				Out.push_back(static_cast<char>(0x80 | (Code & 0x3F)));
			} else {
				Out.push_back(static_cast<char>(0xF0 | (Code >> 18)));
				Out.push_back(static_cast<char>(0x80 | ((Code >> 12) & 0x3F)));
				Out.push_back(static_cast<char>(0x80 | ((Code >> 6) & 0x3F)));
				Out.push_back(static_cast<char>(0x80 | (Code & 0x3F)));
			}
		}
		return Out;
	}

	template<typename ArrayElementType>
	class TArray;

	template<typename SparseArrayElementType>
	class TSparseArray;

	template<typename SetElementType>
	class TSet;

	template<typename KeyElementType, typename ValueElementType>
	class TMap;

	template<typename KeyElementType, typename ValueElementType>
	class TPair;

	namespace Iterators
	{
		class FSetBitIterator;

		template<typename ArrayType>
		class TArrayIterator;

		template<class ContainerType>
		class TContainerIterator;

		template<typename SparseArrayElementType>
		using TSparseArrayIterator = TContainerIterator<TSparseArray<SparseArrayElementType>>;

		template<typename SetElementType>
		using TSetIterator = TContainerIterator<TSet<SetElementType>>;

		template<typename KeyElementType, typename ValueElementType>
		using TMapIterator = TContainerIterator<TMap<KeyElementType, ValueElementType>>;
	}


	namespace ContainerImpl
	{
		namespace HelperFunctions
		{
			inline uint32 FloorLog2(uint32 Value)
			{
				uint32 pos = 0;
				if (Value >= 1 << 16) { Value >>= 16; pos += 16; }
				if (Value >= 1 << 8) { Value >>= 8; pos += 8; }
				if (Value >= 1 << 4) { Value >>= 4; pos += 4; }
				if (Value >= 1 << 2) { Value >>= 2; pos += 2; }
				if (Value >= 1 << 1) { pos += 1; }
				return pos;
			}

			inline uint32 CountLeadingZeros(uint32 Value)
			{
				if (Value == 0)
					return 32;

				return 31 - FloorLog2(Value);
			}
		}

		template<int32 Size, uint32 Alignment>
		struct TAlignedBytes
		{
			alignas(Alignment) uint8 Pad[Size];
		};

		template<uint32 NumInlineElements>
		class TInlineAllocator
		{
		public:
			template<typename ElementType>
			class ForElementType
			{
			private:
				static constexpr int32 ElementSize = sizeof(ElementType);
				static constexpr int32 ElementAlign = alignof(ElementType);

				static constexpr int32 InlineDataSizeBytes = NumInlineElements * ElementSize;

			private:
				TAlignedBytes<ElementSize, ElementAlign> InlineData[NumInlineElements];
				ElementType* SecondaryData;

			public:
				ForElementType()
					: InlineData{ 0x0 }, SecondaryData(nullptr)
				{
				}

				ForElementType(ForElementType&&) = default;
				ForElementType(const ForElementType&) = default;

			public:
				ForElementType& operator=(ForElementType&&) = default;
				ForElementType& operator=(const ForElementType&) = default;

			public:
				inline const ElementType* GetAllocation() const { return SecondaryData ? SecondaryData : reinterpret_cast<const ElementType*>(&InlineData); }

				inline uint32 GetNumInlineBytes() const { return NumInlineElements; }
			};
		};

		class FBitArray
		{
		protected:
			static constexpr int32 NumBitsPerDWORD = 32;
			static constexpr int32 NumBitsPerDWORDLogTwo = 5;

		private:
			TInlineAllocator<4>::ForElementType<int32> Data;
			int32 NumBits;
			int32 MaxBits;

		public:
			FBitArray()
				: NumBits(0), MaxBits(Data.GetNumInlineBytes() * NumBitsPerDWORD)
			{
			}

			FBitArray(const FBitArray&) = default;

			FBitArray(FBitArray&&) = default;

		public:
			FBitArray& operator=(FBitArray&&) = default;

			FBitArray& operator=(const FBitArray& Other) = default;

		private:
			inline void VerifyIndex(int32 Index) const { if (!IsValidIndex(Index)) throw std::out_of_range("Index was out of range!"); }

		public:
			inline int32 Num() const { return NumBits; }
			inline int32 Max() const { return MaxBits; }

			inline const uint32* GetData() const { return reinterpret_cast<const uint32*>(Data.GetAllocation()); }

			inline bool IsValidIndex(int32 Index) const { return Index >= 0 && Index < NumBits; }

			inline bool IsValid() const { return GetData() && NumBits > 0; }

		public:
			inline bool operator[](int32 Index) const { VerifyIndex(Index); return GetData()[Index / NumBitsPerDWORD] & (1 << (Index & (NumBitsPerDWORD - 1))); }

			inline bool operator==(const FBitArray& Other) const { return NumBits == Other.NumBits && GetData() == Other.GetData(); }
			inline bool operator!=(const FBitArray& Other) const { return NumBits != Other.NumBits || GetData() != Other.GetData(); }

		public:
			friend Iterators::FSetBitIterator begin(const FBitArray& Array);
			friend Iterators::FSetBitIterator end  (const FBitArray& Array);
		};

		template<typename SparseArrayType>
		union TSparseArrayElementOrFreeListLink
		{
			SparseArrayType ElementData;

			struct
			{
				int32 PrevFreeIndex;
				int32 NextFreeIndex;
			};
		};

		template<typename SetType>
		class SetElement
		{
		public:
			template<typename SetElementType>
			friend class UC::TSet;

		private:
			SetType Value;
			int32 HashNextId;
			int32 HashIndex;
		};
	}


	template <typename KeyType, typename ValueType>
	class TPair
	{
	public:
		KeyType First;
		ValueType Second;

	public:
		TPair(KeyType Key, ValueType Value)
			: First(Key), Second(Value)
		{
		}

	public:
		inline       KeyType& Key()       { return First; }
		inline const KeyType& Key() const { return First; }

		inline       ValueType& Value()       { return Second; }
		inline const ValueType& Value() const { return Second; }
	};

	template<typename ArrayElementType>
	class TArray
	{
	private:
		template<typename OtherArrayElementType>
		friend class TAllocatedArray;

		template<typename SparseArrayElementType>
		friend class TSparseArray;

	protected:
		static constexpr uint64 ElementAlign = alignof(ArrayElementType);
		static constexpr uint64 ElementSize = sizeof(ArrayElementType);

	protected:
		ArrayElementType* Data;
		int32 NumElements;
		int32 MaxElements;

	public:
		TArray()
			: TArray(nullptr, 0, 0)
		{
		}

		TArray(ArrayElementType* Data, int32 NumElements, int32 MaxElements)
			: Data(Data), NumElements(NumElements), MaxElements(MaxElements)
		{
		}

		TArray(const TArray&) = default;

		TArray(TArray&&) = default;

	public:
		TArray& operator=(TArray&&) = default;
		TArray& operator=(const TArray&) = default;

	private:
		inline int32 GetSlack() const { return MaxElements - NumElements; }

		inline void VerifyIndex(int32 Index) const { if (!IsValidIndex(Index)) throw std::out_of_range("Index was out of range!"); }

		inline       ArrayElementType& GetUnsafe(int32 Index)       { return Data[Index]; }
		inline const ArrayElementType& GetUnsafe(int32 Index) const { return Data[Index]; }

	public:
		/* Adds to the array if there is still space for one more element */
		inline bool Add(const ArrayElementType& Element)
		{
			if (GetSlack() <= 0)
				return false;

			Data[NumElements] = Element;
			NumElements++;

			return true;
		}

		inline bool Remove(int32 Index)
		{
			if (!IsValidIndex(Index))
				return false;

			NumElements--;

			for (int i = Index; i < NumElements; i++)
			{
				/* NumElements was decremented, acessing i + 1 is safe */
				Data[i] = Data[i + 1];
			}

			return true;
		}

		inline void Clear()
		{
			NumElements = 0;

			if (Data)
				memset(Data, 0, NumElements * ElementSize);
		}

    public:
        template<typename OtherType>
        inline std::optional<ArrayElementType> Find(const OtherType& ElementToSearch, bool(*IsEqual)(const ArrayElementType&, const OtherType&)) const
        {
            for (const auto& Element : *this)
            {
                if (IsEqual(Element, ElementToSearch))
                    return Element;
            }

            return {};
        }

        inline std::optional<ArrayElementType> Find(const ArrayElementType& ElementToSearch) const
            requires std::equality_comparable<ArrayElementType>
        {
            for (const auto& Element : *this)
            {
                if (Element == ElementToSearch)
                    return Element;
            }

            return {};
        }

        template<typename OtherType>
        inline bool Contains(const OtherType& ElementToSearch,     bool(*IsEqual)(const ArrayElementType&, const OtherType&)) const
        {
            return Find<OtherType>(ElementToSearch, IsEqual).has_value();
        }

        inline bool Contains(const ArrayElementType& ElementToSearch) const
            requires std::equality_comparable<ArrayElementType>
        {
            return Find(ElementToSearch).has_value();
        }

	public:
		inline int32 Num() const { return NumElements; }
		inline int32 Max() const { return MaxElements; }

		inline const ArrayElementType* GetDataPtr() const { return Data; }

		inline bool IsValidIndex(int32 Index) const { return Data && Index >= 0 && Index < NumElements; }

		inline bool IsValid() const { return Data && NumElements > 0 && MaxElements >= NumElements; }

	public:
		inline       ArrayElementType& operator[](int32 Index)       { VerifyIndex(Index); return Data[Index]; }
		inline const ArrayElementType& operator[](int32 Index) const { VerifyIndex(Index); return Data[Index]; }

		inline bool operator==(const TArray<ArrayElementType>& Other) const { return Data == Other.Data; }
		inline bool operator!=(const TArray<ArrayElementType>& Other) const { return Data != Other.Data; }

		inline explicit operator bool() const { return IsValid(); };

	public:
		template<typename T> friend Iterators::TArrayIterator<T> begin(const TArray& Array);
		template<typename T> friend Iterators::TArrayIterator<T> end  (const TArray& Array);
	};

	class FString : public TArray<TCHAR>
	{
	public:
		friend std::ostream& operator<<(std::ostream& Stream, const UC::FString& Str) { return Stream << Str.ToString(); }

	public:
		using TArray::TArray;

		FString(const TCHAR* Str)
		{
			const uint32 NullTerminatedLength = static_cast<uint32>(std::char_traits<TCHAR>::length(Str) + 0x1);

			Data = const_cast<TCHAR*>(Str);
			NumElements = NullTerminatedLength;
			MaxElements = NullTerminatedLength;
		}

		FString(TCHAR* Str, int32 Num, int32 Max)
		{
			Data = Str;
			NumElements = Num;
			MaxElements = Max;
		}

	public:
		inline std::string ToString() const
		{
			if (*this)
			{
				return TCharToUtf8(Data, NumElements - 1); // Exclude null-terminator
			}

			return "";
		}

		inline UnrealString ToWString() const
		{
			if (*this)
				return UnrealString(Data);

			return UnrealString();
		}

	public:
		inline       TCHAR* CStr()       { return Data; }
		inline const TCHAR* CStr() const { return Data; }

	public:
		inline bool operator==(const FString& Other) const { return Other ? NumElements == Other.NumElements && std::char_traits<TCHAR>::compare(Data, Other.Data, NumElements) == 0 : false; }
		inline bool operator!=(const FString& Other) const { return Other ? NumElements != Other.NumElements || std::char_traits<TCHAR>::compare(Data, Other.Data, NumElements) != 0 : true; }
	};

	// Utf8String that assumes C-APIs (strlen, strcmp) behaviour works for char8_t like Ansi strings, execept it's counting/comparing bytes not characters.
	class FUtf8String : public TArray<char8_t>
	{
	public:
		friend std::ostream& operator<<(std::ostream& Stream, const UC::FUtf8String& Str) { return Stream << Str.ToString(); }

	private:
		inline const char* GetDataAsConstCharPtr() const
		{
			return reinterpret_cast<const char*>(Data);
		}

	public:
		using TArray::TArray;

		FUtf8String(const char8_t* Str)
		{
			Data = const_cast<char8_t*>(Str);

			const uint32 NullTerminatedLength = static_cast<uint32>(strlen(GetDataAsConstCharPtr()) + 0x1);

			NumElements = NullTerminatedLength;
			MaxElements = NullTerminatedLength;
		}

		FUtf8String(char8_t* Str, int32 Num, int32 Max)
		{
			Data = Str;
			NumElements = Num;
			MaxElements = Max;
		}

	public:
		inline std::string ToString() const
		{
			if (*this)
			{
				return std::string(GetDataAsConstCharPtr(), NumElements - 1); // Exclude null-terminator
			}

			return "";
		}

		inline UnrealString ToWString() const
		{
			if (*this)
				return Utf8ToTChar(reinterpret_cast<const char*>(Data), NumElements - 1); // Exclude null-terminator

			return UnrealString();
		}

	public:
		inline       char8_t* CStr()       { return Data; }
		inline const char8_t* CStr() const { return Data; }

	public:
		inline bool operator==(const FUtf8String& Other) const { return Other ? NumElements == Other.NumElements && strcmp(GetDataAsConstCharPtr(), Other.GetDataAsConstCharPtr()) == 0 : false; }
		inline bool operator!=(const FUtf8String& Other) const { return Other ? NumElements != Other.NumElements || strcmp(GetDataAsConstCharPtr(), Other.GetDataAsConstCharPtr()) != 0 : true; }
	};

	class FAnsiString : public TArray<char>
	{
	public:
		friend std::ostream& operator<<(std::ostream& Stream, const UC::FAnsiString& Str) { return Stream << Str.ToString(); }

	public:
		using TArray::TArray;

		FAnsiString(const char* Str)
		{
			const uint32 NullTerminatedLength = static_cast<uint32>(strlen(Str) + 0x1);

			Data = const_cast<char*>(Str);
			NumElements = NullTerminatedLength;
			MaxElements = NullTerminatedLength;
		}

		FAnsiString(char* Str, int32 Num, int32 Max)
		{
			Data = Str;
			NumElements = Num;
			MaxElements = Max;
		}

	public:
		inline std::string ToString() const
		{
			if (*this)
			{
				return std::string(Data, NumElements - 1); // Exclude null-terminator
			}

			return "";
		}

		inline UnrealString ToWString() const
		{
			if (*this)
				return Utf8ToTChar(Data, NumElements - 1); // Exclude null-terminator

			return UnrealString();
		}

	public:
		inline       char* CStr() { return Data; }
		inline const char* CStr() const { return Data; }

	public:
		inline bool operator==(const FAnsiString& Other) const { return Other ? NumElements == Other.NumElements && strcmp(Data, Other.Data) == 0 : false; }
		inline bool operator!=(const FAnsiString& Other) const { return Other ? NumElements != Other.NumElements || strcmp(Data, Other.Data) != 0 : true; }
	};


	/*
	* Class to allow construction of a TArray, that uses c-style standard-library memory allocation.
	* 
	* Useful for calling functions that expect a buffer of a certain size and do not reallocate that buffer.
	* This avoids leaking memory, if the array would otherwise be allocated by the engine, and couldn't be freed without FMemory-functions.
	*/
	template<typename ArrayElementType>
	class TAllocatedArray : public TArray<ArrayElementType>
	{
	public:
		TAllocatedArray() = delete;

	public:
		TAllocatedArray(int32 Size)
		{
			this->Data = static_cast<ArrayElementType*>(malloc(Size * sizeof(ArrayElementType)));
			this->NumElements = 0x0;
			this->MaxElements = Size;
		}

		~TAllocatedArray()
		{
			if (this->Data)
				free(this->Data);

			this->NumElements = 0x0;
			this->MaxElements = 0x0;
		}

	public:
		inline operator       TArray<ArrayElementType>()       { return *reinterpret_cast<      TArray<ArrayElementType>*>(this); }
		inline operator const TArray<ArrayElementType>() const { return *reinterpret_cast<const TArray<ArrayElementType>*>(this); }
	};

	/*
	* Class to allow construction of an FString, that uses c-style standard-library memory allocation.
	*
	* Useful for calling functions that expect a buffer of a certain size and do not reallocate that buffer.
	* This avoids leaking memory, if the array would otherwise be allocated by the engine, and couldn't be freed without FMemory-functions.
	*/
	class FAllocatedString : public FString
	{
	public:
		FAllocatedString() = delete;

	public:
		FAllocatedString(int32 Size)
		{
			Data = static_cast<TCHAR*>(malloc(Size * sizeof(TCHAR)));
			NumElements = 0x0;
			MaxElements = Size;
		}

		~FAllocatedString()
		{
			if (Data)
				free(Data);

			NumElements = 0x0;
			MaxElements = 0x0;
		}

	public:
		inline operator       FString()       { return *reinterpret_cast<      FString*>(this); }
		inline operator const FString() const { return *reinterpret_cast<const FString*>(this); }
	};)";

	UEContainersHeader << R"(
	template<typename SparseArrayElementType>
	class TSparseArray
	{
	private:
		static constexpr uint32 ElementAlign = alignof(SparseArrayElementType);
		static constexpr uint32 ElementSize = sizeof(SparseArrayElementType);

	private:
		using FElementOrFreeListLink = ContainerImpl::TSparseArrayElementOrFreeListLink<ContainerImpl::TAlignedBytes<ElementSize, ElementAlign>>;

	private:
		TArray<FElementOrFreeListLink> Data;
		ContainerImpl::FBitArray AllocationFlags;
		int32 FirstFreeIndex;
		int32 NumFreeIndices;

	public:
		TSparseArray()
			: FirstFreeIndex(-1), NumFreeIndices(0)
		{
		}

		TSparseArray(TSparseArray&&) = default;
		TSparseArray(const TSparseArray&) = default;

	public:
		TSparseArray& operator=(TSparseArray&&) = default;
		TSparseArray& operator=(const TSparseArray&) = default;

	private:
		inline void VerifyIndex(int32 Index) const { if (!IsValidIndex(Index)) throw std::out_of_range("Index was out of range!"); }

	public:
		inline int32 NumAllocated() const { return Data.Num(); }

		inline int32 Num() const { return NumAllocated() - NumFreeIndices; }
		inline int32 Max() const { return Data.Max(); }

		inline bool IsValidIndex(int32 Index) const { return Data.IsValidIndex(Index) && AllocationFlags[Index]; }

		inline bool IsValid() const { return Data.IsValid() && AllocationFlags.IsValid(); }

	public:
		const ContainerImpl::FBitArray& GetAllocationFlags() const { return AllocationFlags; }

	public:
		inline       SparseArrayElementType& operator[](int32 Index)       { VerifyIndex(Index); return *reinterpret_cast<SparseArrayElementType*>(&Data.GetUnsafe(Index).ElementData); }
		inline const SparseArrayElementType& operator[](int32 Index) const { VerifyIndex(Index); return *reinterpret_cast<SparseArrayElementType*>(&Data.GetUnsafe(Index).ElementData); }

		inline bool operator==(const TSparseArray<SparseArrayElementType>& Other) const { return Data == Other.Data; }
		inline bool operator!=(const TSparseArray<SparseArrayElementType>& Other) const { return Data != Other.Data; }

	public:
		template<typename T> friend Iterators::TSparseArrayIterator<T> begin(const TSparseArray& Array);
		template<typename T> friend Iterators::TSparseArrayIterator<T> end  (const TSparseArray& Array);
	};

	template<typename SetElementType>
	class TSet
	{
	private:
		static constexpr uint32 ElementAlign = alignof(SetElementType);
		static constexpr uint32 ElementSize = sizeof(SetElementType);

	private:
		using SetDataType = ContainerImpl::SetElement<SetElementType>;
		using HashType = ContainerImpl::TInlineAllocator<1>::ForElementType<int32>;

	private:
		TSparseArray<SetDataType> Elements;
		HashType Hash;
		int32 HashSize;

	public:
		TSet()
			: HashSize(0)
		{
		}

		TSet(TSet&&) = default;
		TSet(const TSet&) = default;

	public:
		TSet& operator=(TSet&&) = default;
		TSet& operator=(const TSet&) = default;

	private:
		inline void VerifyIndex(int32 Index) const { if (!IsValidIndex(Index)) throw std::out_of_range("Index was out of range!"); }

	public:
		inline int32 NumAllocated() const { return Elements.NumAllocated(); }

		inline int32 Num() const { return Elements.Num(); }
		inline int32 Max() const { return Elements.Max(); }

		inline bool IsValidIndex(int32 Index) const { return Elements.IsValidIndex(Index); }

		inline bool IsValid() const { return Elements.IsValid(); }

	public:
		const ContainerImpl::FBitArray& GetAllocationFlags() const { return Elements.GetAllocationFlags(); }

	public:
		inline       SetElementType& operator[] (int32 Index)       { return Elements[Index].Value; }
		inline const SetElementType& operator[] (int32 Index) const { return Elements[Index].Value; }

		inline bool operator==(const TSet<SetElementType>& Other) const { return Elements == Other.Elements; }
		inline bool operator!=(const TSet<SetElementType>& Other) const { return Elements != Other.Elements; }

	public:
		template<typename T> friend Iterators::TSetIterator<T> begin(const TSet& Set);
		template<typename T> friend Iterators::TSetIterator<T> end  (const TSet& Set);
	};

	template<typename KeyElementType, typename ValueElementType>
	class TMap
	{
	public:
		using ElementType = TPair<KeyElementType, ValueElementType>;

	private:
		TSet<ElementType> Elements;

	private:
		inline void VerifyIndex(int32 Index) const { if (!IsValidIndex(Index)) throw std::out_of_range("Index was out of range!"); }

	public:
		inline int32 NumAllocated() const { return Elements.NumAllocated(); }

		inline int32 Num() const { return Elements.Num(); }
		inline int32 Max() const { return Elements.Max(); }

		inline bool IsValidIndex(int32 Index) const { return Elements.IsValidIndex(Index); }

		inline bool IsValid() const { return Elements.IsValid(); }

	public:
		const ContainerImpl::FBitArray& GetAllocationFlags() const { return Elements.GetAllocationFlags(); }

	public:
		inline decltype(auto) Find(const KeyElementType& Key, bool(*Equals)(const KeyElementType& LeftKey, const KeyElementType& RightKey))
		{
			for (auto It = begin(*this); It != end(*this); ++It)
			{
				if (Equals(It->Key(), Key))
					return It;
			}
		
			return end(*this);
		}

	public:
		inline       ElementType& operator[] (int32 Index)       { return Elements[Index]; }
		inline const ElementType& operator[] (int32 Index) const { return Elements[Index]; }

		inline bool operator==(const TMap<KeyElementType, ValueElementType>& Other) const { return Elements == Other.Elements; }
		inline bool operator!=(const TMap<KeyElementType, ValueElementType>& Other) const { return Elements != Other.Elements; }

	public:
		template<typename KeyType, typename ValueType> friend Iterators::TMapIterator<KeyType, ValueType> begin(const TMap& Map);
		template<typename KeyType, typename ValueType> friend Iterators::TMapIterator<KeyType, ValueType> end  (const TMap& Map);
	};

	namespace Iterators
	{
		class FRelativeBitReference
		{
		protected:
			static constexpr int32 NumBitsPerDWORD = 32;
			static constexpr int32 NumBitsPerDWORDLogTwo = 5;

		public:
			inline explicit FRelativeBitReference(int32 BitIndex)
				: WordIndex(BitIndex >> NumBitsPerDWORDLogTwo)
				, Mask(1 << (BitIndex & (NumBitsPerDWORD - 1)))
			{
			}

			int32  WordIndex;
			uint32 Mask;
		};

		class FSetBitIterator : public FRelativeBitReference
		{
		private:
			const ContainerImpl::FBitArray& Array;

			uint32 UnvisitedBitMask;
			int32 CurrentBitIndex;
			int32 BaseBitIndex;

		public:
			explicit FSetBitIterator(const ContainerImpl::FBitArray& InArray, int32 StartIndex = 0)
				: FRelativeBitReference(StartIndex)
				, Array(InArray)
				, UnvisitedBitMask((~0U) << (StartIndex & (NumBitsPerDWORD - 1)))
				, CurrentBitIndex(StartIndex)
				, BaseBitIndex(StartIndex & ~(NumBitsPerDWORD - 1))
			{
				if (StartIndex != Array.Num())
					FindFirstSetBit();
			}

		public:
			inline FSetBitIterator& operator++()
			{
				UnvisitedBitMask &= ~this->Mask;

				FindFirstSetBit();

				return *this;
			}

			inline explicit operator bool() const { return CurrentBitIndex < Array.Num(); }

			inline bool operator==(const FSetBitIterator& Rhs) const { return CurrentBitIndex == Rhs.CurrentBitIndex && &Array == &Rhs.Array; }
			inline bool operator!=(const FSetBitIterator& Rhs) const { return CurrentBitIndex != Rhs.CurrentBitIndex || &Array != &Rhs.Array; }

		public:
			inline int32 GetIndex() { return CurrentBitIndex; }

			void FindFirstSetBit()
			{
				const uint32* ArrayData = Array.GetData();
				const int32   ArrayNum = Array.Num();
				const int32   LastWordIndex = (ArrayNum - 1) / NumBitsPerDWORD;

				uint32 RemainingBitMask = ArrayData[this->WordIndex] & UnvisitedBitMask;
				while (!RemainingBitMask)
				{
					++this->WordIndex;
					BaseBitIndex += NumBitsPerDWORD;
					if (this->WordIndex > LastWordIndex)
					{
						CurrentBitIndex = ArrayNum;
						return;
					}

					RemainingBitMask = ArrayData[this->WordIndex];
					UnvisitedBitMask = ~0;
				}

				const uint32 NewRemainingBitMask = RemainingBitMask & (RemainingBitMask - 1);

				this->Mask = NewRemainingBitMask ^ RemainingBitMask;

				CurrentBitIndex = BaseBitIndex + NumBitsPerDWORD - 1 - ContainerImpl::HelperFunctions::CountLeadingZeros(this->Mask);

				if (CurrentBitIndex > ArrayNum)
					CurrentBitIndex = ArrayNum;
			}
		};

		template<typename ArrayType>
		class TArrayIterator
		{
		private:
			TArray<ArrayType>& IteratedArray;
			int32 Index;

		public:
			TArrayIterator(const TArray<ArrayType>& Array, int32 StartIndex = 0x0)
				: IteratedArray(const_cast<TArray<ArrayType>&>(Array)), Index(StartIndex)
			{
			}

		public:
			inline int32 GetIndex() { return Index; }

			inline int32 IsValid() { return IteratedArray.IsValidIndex(GetIndex()); }

		public:
			inline TArrayIterator& operator++() { ++Index; return *this; }
			inline TArrayIterator& operator--() { --Index; return *this; }

			inline       ArrayType& operator*()       { return IteratedArray[GetIndex()]; }
			inline const ArrayType& operator*() const { return IteratedArray[GetIndex()]; }

			inline       ArrayType* operator->()       { return &IteratedArray[GetIndex()]; }
			inline const ArrayType* operator->() const { return &IteratedArray[GetIndex()]; }

			inline bool operator==(const TArrayIterator& Other) const { return &IteratedArray == &Other.IteratedArray && Index == Other.Index; }
			inline bool operator!=(const TArrayIterator& Other) const { return &IteratedArray != &Other.IteratedArray || Index != Other.Index; }
		};

		template<class ContainerType>
		class TContainerIterator
		{
		private:
			ContainerType& IteratedContainer;
			FSetBitIterator BitIterator;

		public:
			TContainerIterator(const ContainerType& Container, const ContainerImpl::FBitArray& BitArray, int32 StartIndex = 0x0)
				: IteratedContainer(const_cast<ContainerType&>(Container)), BitIterator(BitArray, StartIndex)
			{
			}

		public:
			inline int32 GetIndex() { return BitIterator.GetIndex(); }

			inline int32 IsValid() { return IteratedContainer.IsValidIndex(GetIndex()); }

		public:
			inline TContainerIterator& operator++() { ++BitIterator; return *this; }

			inline       auto& operator*()       { return IteratedContainer[GetIndex()]; }
			inline const auto& operator*() const { return IteratedContainer[GetIndex()]; }

			inline       auto* operator->()       { return &IteratedContainer[GetIndex()]; }
			inline const auto* operator->() const { return &IteratedContainer[GetIndex()]; }

			inline bool operator==(const TContainerIterator& Other) const { return &IteratedContainer == &Other.IteratedContainer && BitIterator == Other.BitIterator; }
			inline bool operator!=(const TContainerIterator& Other) const { return &IteratedContainer != &Other.IteratedContainer || BitIterator != Other.BitIterator; }
		};
	}

	inline Iterators::FSetBitIterator begin(const ContainerImpl::FBitArray& Array) { return Iterators::FSetBitIterator(Array, 0); }
	inline Iterators::FSetBitIterator end  (const ContainerImpl::FBitArray& Array) { return Iterators::FSetBitIterator(Array, Array.Num()); }

	template<typename T> inline Iterators::TArrayIterator<T> begin(const TArray<T>& Array) { return Iterators::TArrayIterator<T>(Array, 0); }
	template<typename T> inline Iterators::TArrayIterator<T> end  (const TArray<T>& Array) { return Iterators::TArrayIterator<T>(Array, Array.Num()); }

	template<typename T> inline Iterators::TSparseArrayIterator<T> begin(const TSparseArray<T>& Array) { return Iterators::TSparseArrayIterator<T>(Array, Array.GetAllocationFlags(), 0); }
	template<typename T> inline Iterators::TSparseArrayIterator<T> end  (const TSparseArray<T>& Array) { return Iterators::TSparseArrayIterator<T>(Array, Array.GetAllocationFlags(), Array.NumAllocated()); }

	template<typename T> inline Iterators::TSetIterator<T> begin(const TSet<T>& Set) { return Iterators::TSetIterator<T>(Set, Set.GetAllocationFlags(), 0); }
	template<typename T> inline Iterators::TSetIterator<T> end  (const TSet<T>& Set) { return Iterators::TSetIterator<T>(Set, Set.GetAllocationFlags(), Set.NumAllocated()); }

	template<typename T0, typename T1> inline Iterators::TMapIterator<T0, T1> begin(const TMap<T0, T1>& Map) { return Iterators::TMapIterator<T0, T1>(Map, Map.GetAllocationFlags(), 0); }
	template<typename T0, typename T1> inline Iterators::TMapIterator<T0, T1> end  (const TMap<T0, T1>& Map) { return Iterators::TMapIterator<T0, T1>(Map, Map.GetAllocationFlags(), Map.NumAllocated()); }

#if defined(_WIN64)
	static_assert(sizeof(TArray<int32>) == 0x10, "TArray has a wrong size!");
	static_assert(sizeof(TSet<int32>) == 0x50, "TSet has a wrong size!");
	static_assert(sizeof(TMap<int32, int32>) == 0x50, "TMap has a wrong size!");
#elif defined(_WIN32)
	static_assert(sizeof(TArray<int32>) == 0x0C, "TArray has a wrong size!");
	static_assert(sizeof(TSet<int32>) == 0x3C, "TSet has a wrong size!");
	static_assert(sizeof(TMap<int32, int32>) == 0x3C, "TMap has a wrong size!");
#endif
}
)";

	WriteFileEnd(UEContainersHeader, EFileType::UnrealContainers);
}

