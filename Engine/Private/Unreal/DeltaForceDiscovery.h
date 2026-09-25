#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../../Public/Unreal/DeltaForce.h"

/* Internal to the Delta Force profile. Everything in here only reads memory through the kernel (never dereferences). */
namespace DeltaForce::Discovery
{
	using FMemoryRanges = std::vector<std::pair<uint64_t, uint64_t>>; // [Begin, End) of readable, non-executable image segments

	/* Decodes FName 'Id' with the given (possibly unverified) profile. Returns false if the entry isn't valid. */
	bool TryDecodeName(uint64_t ImageBase, const FProfile& Profile, uint32_t Id, std::string& OutName);

	/* Checks the globals of a complete profile: FNamePool decodes "None", GUObjectArray holds the 5 CoreUObject anchors at their InternalIndex. */
	bool ValidateProfile(uint64_t ImageBase, const FProfile& Profile);

	/*
	* Scans 'DataRanges' for FNamePool/FUObjectArray candidates, infers the name encoding and the UObject header/FUObjectItem layout
	* and returns the single profile that validates. Fails (never guesses) on zero or on ambiguous results.
	*/
	bool DiscoverProfile(uint64_t ImageBase, const FMemoryRanges& DataRanges, FProfile& OutProfile, std::string& OutError);

	/* Infers name codec + UObject header/item layout for a profile whose NamesRVA/ObjectsRVA are already set (manual override). */
	bool CompleteProfile(uint64_t ImageBase, FProfile& InOutProfile, std::string& OutError);
}
