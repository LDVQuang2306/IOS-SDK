#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "DeltaForceDiscovery.h"
#include "Utils.h"
#include "Menu/Logger.h"

/*
* Port of the discovery/validation logic of the reference DFSDKDumper (Discovery.cpp, Layout.cpp, Names.cpp, Reader.cpp).
* All reads go through the kernel (vm_read_overwrite) and every search has a hard read budget, so a wrong candidate can
* only be rejected, never crash the game.
*/

/* Name codec */
namespace DeltaForce
{
	static uint32_t NameKey(uint32_t N)
	{
		switch (N % 9)
		{
		case 0: return (N & 31) + N;
		case 1: return (N ^ 0xDF) + N;
		case 2: return (N | 0xCF) + N;
		case 3: return 33 * N;
		case 4: return N + (N >> 2);
		case 5: return 3 * N + 5;
		case 6: return ((4 * N) | 5) + N;
		case 7: return ((N >> 4) | 7) + N;
		default: return (N ^ 12) + N;
		}
	}

	void DecryptAnsi(uint8_t* Text, size_t Length)
	{
		if (!Text || Length == 0 || Text[0] == 0)
			return;

		const uint8_t Mask = static_cast<uint8_t>((NameKey(static_cast<uint32_t>(Length)) & 0x80) ^ 0xFF);

		for (size_t i = 0; i < Length; i++)
			Text[i] ^= Mask;
	}

	void DecryptWide(uint16_t* Text, size_t Length)
	{
		if (!Text || Length == 0 || Text[0] == 0)
			return;

		const uint16_t Mask = static_cast<uint16_t>((NameKey(static_cast<uint32_t>(Length)) | 0x7F) + 0x80);

		/* Only every second code unit is obfuscated */
		for (size_t i = 0; i < Length; i += 2)
			Text[i] ^= Mask;
	}

	const char* GetDecryptionSource()
	{
		return R"(namespace DeltaForceNames
{
	inline uint32 Key(uint32 N)
	{
		switch (N % 9)
		{
		case 0: return (N & 31) + N;
		case 1: return (N ^ 0xDF) + N;
		case 2: return (N | 0xCF) + N;
		case 3: return 33 * N;
		case 4: return N + (N >> 2);
		case 5: return 3 * N + 5;
		case 6: return ((4 * N) | 5) + N;
		case 7: return ((N >> 4) | 7) + N;
		default: return (N ^ 12) + N;
		}
	}

	inline void DecryptAnsi(char* Text, int32 Length)
	{
		if (!Text || Length <= 0 || Text[0] == 0)
			return;

		const uint8 Mask = static_cast<uint8>((Key(static_cast<uint32>(Length)) & 0x80) ^ 0xFF);

		for (int32 i = 0; i < Length; i++)
			Text[i] = static_cast<char>(static_cast<uint8>(Text[i]) ^ Mask);
	}

	inline void DecryptWide(char16_t* Text, int32 Length)
	{
		if (!Text || Length <= 0 || Text[0] == 0)
			return;

		const uint16 Mask = static_cast<uint16>((Key(static_cast<uint32>(Length)) | 0x7F) + 0x80);

		for (int32 i = 0; i < Length; i += 2)
			Text[i] = static_cast<char16_t>(static_cast<uint16>(Text[i]) ^ Mask);
	}
}
)";
	}
}

namespace DeltaForce::Discovery
{
namespace
{
	struct ReadError : std::runtime_error
	{
		using std::runtime_error::runtime_error;
	};

	struct FMemory
	{
		virtual ~FMemory() = default;
		virtual bool Read(uint64_t Address, void* Out, size_t Size) = 0;

		template<class T>
		T Get(uint64_t Address)
		{
			T Value{};
			if (!Address || !Read(Address, &Value, sizeof(Value)))
				throw ReadError("unreadable address");

			return Value;
		}
	};

	struct FProcessMemory final : FMemory
	{
		bool Read(uint64_t Address, void* Out, size_t Size) override
		{
			if (Size > 16u * 1024u * 1024u)
				return false;

			return ReadMemory(static_cast<uintptr_t>(Address), Out, Size);
		}
	};

	/* Bounds the total work of a nested search. Exceeding the budget is an error, not a result. */
	struct FBudgetMemory final : FMemory
	{
		FMemory& Source;
		uint64_t MaxBytes;
		uint64_t MaxCalls;
		const char* What;
		uint64_t Bytes = 0;
		uint64_t Calls = 0;

		FBudgetMemory(FMemory& InSource, uint64_t InMaxBytes, uint64_t InMaxCalls, const char* InWhat)
			: Source(InSource), MaxBytes(InMaxBytes), MaxCalls(InMaxCalls), What(InWhat)
		{
		}

		bool Read(uint64_t Address, void* Out, size_t Size) override
		{
			if (Size > MaxBytes - Bytes || ++Calls > MaxCalls)
				throw ReadError(What);

			Bytes += Size;
			return Source.Read(Address, Out, Size);
		}
	};

	inline bool IsPointer(uint64_t P)
	{
		return P >= 4096 && P < 0x0000800000000000ULL && !(P & 7);
	}

	template<class T>
	inline T At(const uint8_t* Data, size_t Pos)
	{
		T Result{};
		memcpy(&Result, Data + Pos, sizeof(Result));
		return Result;
	}

	template<class T>
	inline T At(const std::vector<uint8_t>& Bytes, size_t Pos)
	{
		return At<T>(Bytes.data(), Pos);
	}

	inline std::string Leaf(const std::string& Name)
	{
		const size_t Slash = Name.find_last_of('/');
		return Slash == std::string::npos ? Name : Name.substr(Slash + 1);
	}

	inline void Bound(bool bCondition, const char* Reason)
	{
		if (!bCondition)
			throw ReadError(Reason);
	}

	std::string Utf16ToUtf8(const std::vector<uint16_t>& Text)
	{
		std::string Output;

		for (size_t i = 0; i < Text.size(); ++i)
		{
			uint32_t C = Text[i];

			if (C >= 0xD800 && C <= 0xDBFF)
			{
				Bound(i + 1 < Text.size() && Text[i + 1] >= 0xDC00 && Text[i + 1] <= 0xDFFF, "invalid UTF-16 high surrogate");
				C = 0x10000 + ((C - 0xD800) << 10) + (Text[++i] - 0xDC00);
			}
			else
			{
				Bound(!(C >= 0xDC00 && C <= 0xDFFF), "invalid UTF-16 low surrogate");
			}

			Bound(C != 0, "embedded null in name");

			if (C < 0x80)
			{
				Output += static_cast<char>(C);
			}
			else if (C < 0x800)
			{
				Output += static_cast<char>(0xC0 | (C >> 6));
				Output += static_cast<char>(0x80 | (C & 63));
			}
			else if (C < 0x10000)
			{
				Output += static_cast<char>(0xE0 | (C >> 12));
				Output += static_cast<char>(0x80 | ((C >> 6) & 63));
				Output += static_cast<char>(0x80 | (C & 63));
			}
			else
			{
				Output += static_cast<char>(0xF0 | (C >> 18));
				Output += static_cast<char>(0x80 | ((C >> 12) & 63));
				Output += static_cast<char>(0x80 | ((C >> 6) & 63));
				Output += static_cast<char>(0x80 | (C & 63));
			}
		}

		return Output;
	}

	/* Minimal reflection reader: names, object slots and object identity. Caches are per instance (use a fresh reader after GC). */
	class FReader
	{
	public:
		struct FObjectInfo
		{
			uint64_t Address;
			uint64_t Class;
			uint64_t Outer;
			std::string Name;
		};

	private:
		FMemory& Memory;
		FProfile P;
		uint64_t Names;
		uint64_t Objects;
		std::unordered_map<uint32_t, std::string> NameCache;
		std::unordered_map<uint64_t, FObjectInfo> ObjectCache;

	public:
		FReader(FMemory& InMemory, const FProfile& InProfile, uint64_t ImageBase)
			: Memory(InMemory), P(InProfile), Names(ImageBase + InProfile.NamesRVA), Objects(ImageBase + InProfile.ObjectsRVA)
		{
			Bound(P.NamesRVA && P.ObjectsRVA && P.PoolBlocks, "incomplete profile");
			Bound(P.PoolBlockBits >= 12 && P.PoolBlockBits <= 20 && P.NameStride == 2 && P.ChunkSize == 0x10000 && P.ItemSize >= 8 && P.ItemSize <= 64 &&
				P.ItemSize % 8 == 0 && P.ItemObject <= P.ItemSize - 8, "unsupported object/pool geometry");
			Bound(P.NameLengthShift >= 1 && P.NameLengthShift <= 12 && (P.NameCodec == ENameCodec::DfV1 || P.NameCodec == ENameCodec::Plain), "unsupported name encoding");
		}

	public:
		uint32_t BlockBytes() const
		{
			return P.NameStride << P.PoolBlockBits;
		}

		std::string Name(uint32_t Id, uint32_t Number = 0)
		{
			auto Found = NameCache.find(Id);

			if (Found == NameCache.end())
			{
				const uint32_t Block = Id >> P.PoolBlockBits;
				const uint32_t Current = Memory.Get<uint32_t>(Names + P.PoolCurrentBlock);
				Bound(Current < 8192 && Block <= Current, "name block out of range");

				const uint32_t Offset = (Id & ((1u << P.PoolBlockBits) - 1)) * P.NameStride;
				const uint64_t BlockPtr = Memory.Get<uint64_t>(Names + P.PoolBlocks + uint64_t(Block) * 8);
				const uint32_t Limit = Block == Current ? Memory.Get<uint32_t>(Names + P.PoolCursor) : BlockBytes();
				Bound(Limit <= BlockBytes() && Offset + 2 <= Limit, "name entry outside committed block");

				const uint16_t Header = Memory.Get<uint16_t>(BlockPtr + Offset);
				const uint32_t Length = Header >> P.NameLengthShift;
				const bool bIsWide = Header & 1;
				Bound(Length > 0 && Length <= 1023 && Offset + 2 + Length * (bIsWide ? 2 : 1) <= Limit, "invalid name header");

				std::string Decoded;

				if (bIsWide)
				{
					std::vector<uint16_t> Text(Length);
					Bound(Memory.Read(BlockPtr + Offset + 2, Text.data(), Length * 2), "unreadable wide name");

					if (P.NameCodec == ENameCodec::DfV1)
						DecryptWide(Text.data(), Text.size());

					Decoded = Utf16ToUtf8(Text);
				}
				else
				{
					std::vector<uint8_t> Text(Length);
					Bound(Memory.Read(BlockPtr + Offset + 2, Text.data(), Length), "unreadable ANSI name");

					if (P.NameCodec == ENameCodec::DfV1)
						DecryptAnsi(Text.data(), Text.size());

					Bound(std::none_of(Text.begin(), Text.end(), [](uint8_t C) { return C < 32 || C == 127; }), "invalid ANSI name after decode");
					Decoded.assign(Text.begin(), Text.end());
				}

				Found = NameCache.emplace(Id, std::move(Decoded)).first;
			}

			if (!Number)
				return Found->second;

			/* ToString: 'Number - 1' printed as signed 32-bit */
			const uint32_t Bits = Number - 1u;
			const int64_t Suffix = Bits <= 0x7FFFFFFFu ? static_cast<int64_t>(Bits) : static_cast<int64_t>(Bits) - 0x100000000LL;
			return Found->second + "_" + std::to_string(Suffix);
		}

		int32_t ObjectCount()
		{
			const int32_t Count = Memory.Get<int32_t>(Objects + P.ObjectArray + P.ObjectsCount);
			Bound(Count > 0 && static_cast<uint32_t>(Count) <= P.MaxObjects, "GUObjectArray not initialized or count invalid");
			return Count;
		}

		uint64_t ObjectAt(uint32_t Index)
		{
			Bound(Index < static_cast<uint32_t>(ObjectCount()), "object index out of range");

			const uint64_t Array = Objects + P.ObjectArray;
			const int32_t NumChunks = Memory.Get<int32_t>(Array + P.ObjectsNumChunks);
			const uint32_t Chunk = Index / P.ChunkSize;
			Bound(NumChunks > 0 && NumChunks <= static_cast<int32_t>((P.MaxObjects + P.ChunkSize - 1) / P.ChunkSize) && Chunk < static_cast<uint32_t>(NumChunks), "object chunk out of range");

			const uint64_t Table = Memory.Get<uint64_t>(Array + P.ObjectsChunks);
			const uint64_t ChunkPtr = Memory.Get<uint64_t>(Table + uint64_t(Chunk) * 8);
			Bound(ChunkPtr != 0, "missing committed object chunk");

			return Memory.Get<uint64_t>(ChunkPtr + uint64_t(Index % P.ChunkSize) * P.ItemSize + P.ItemObject);
		}

		FObjectInfo Object(uint64_t Address)
		{
			auto It = ObjectCache.find(Address);
			if (It != ObjectCache.end())
				return It->second;

			Bound(Address != 0, "null UObject");

			const uint32_t Id = Memory.Get<uint32_t>(Address + P.ObjectName);
			const uint32_t Number = Memory.Get<uint32_t>(Address + P.ObjectName + 4);

			FObjectInfo Info{ Address, Memory.Get<uint64_t>(Address + P.ObjectClass), Memory.Get<uint64_t>(Address + P.ObjectOuter), Leaf(Name(Id, Number)) };
			ObjectCache.emplace(Address, Info);

			return Info;
		}

		std::string FullName(uint64_t Address)
		{
			const FObjectInfo Info = Object(Address);
			std::string Result = Info.Name;

			std::unordered_set<uint64_t> Seen{ Address };
			for (uint64_t Outer = Info.Outer; Outer;)
			{
				Bound(Seen.insert(Outer).second && Seen.size() <= 256, "cycle/depth in UObject outer chain");

				const FObjectInfo Parent = Object(Outer);
				Result = Parent.Name + "." + Result;
				Outer = Parent.Outer;
			}

			return Object(Info.Class).Name + " " + Result;
		}

		bool IsA(uint64_t Address, const std::string& ClassName)
		{
			std::unordered_set<uint64_t> Seen;

			for (uint64_t Class = Object(Address).Class; Class; Class = Memory.Get<uint64_t>(Class + P.StructSuper))
			{
				Bound(Seen.insert(Class).second && Seen.size() <= 256, "cycle/depth in class hierarchy");

				if (Object(Class).Name == ClassName)
					return true;
			}

			return false;
		}
	};

	bool NoneBlock(FMemory& Memory, uint64_t Address)
	{
		uint16_t Header = 0;
		if (!Memory.Read(Address, &Header, sizeof(Header)))
			return false;

		bool bLengthOk = false;
		for (unsigned Shift = 1; Shift <= 12; ++Shift)
		{
			if ((Header >> Shift) == 4)
				bLengthOk = true;
		}

		if (!bLengthOk)
			return false;

		if (Header & 1)
		{
			uint16_t Text[4]{};
			const uint16_t Plain[4]{ 'N', 'o', 'n', 'e' };

			if (!Memory.Read(Address + 2, Text, sizeof(Text)))
				return false;

			if (memcmp(Text, Plain, sizeof(Text)) == 0)
				return true;

			DecryptWide(Text, 4);
			return memcmp(Text, Plain, sizeof(Text)) == 0;
		}

		uint8_t Text[4]{};
		const uint8_t Plain[4]{ 'N', 'o', 'n', 'e' };

		if (!Memory.Read(Address + 2, Text, sizeof(Text)))
			return false;

		if (memcmp(Text, Plain, sizeof(Text)) == 0)
			return true;

		DecryptAnsi(Text, 4);
		return memcmp(Text, Plain, sizeof(Text)) == 0;
	}

	bool Pool(FMemory& Memory, uint64_t Base, const FProfile& P)
	{
		try
		{
			const uint64_t A = Base + P.NamesRVA;
			const uint32_t Block = Memory.Get<uint32_t>(A + P.PoolCurrentBlock);
			const uint32_t Cursor = Memory.Get<uint32_t>(A + P.PoolCursor);

			if (Block >= 8192 || Cursor > (P.NameStride << P.PoolBlockBits) || (Cursor & 1) || (!Block && Cursor < 6))
				return false;

			if (!IsPointer(Memory.Get<uint64_t>(A + P.PoolBlocks + uint64_t(Block) * 8)))
				return false;

			FReader Reader(Memory, P, Base);
			return Reader.Name(0) == "None";
		}
		catch (const ReadError&)
		{
			return false;
		}
	}

	/* Cheap screen for a TUObjectArray chunk table: FUObjectItem stride/offset must agree with UObject::InternalIndex for some header offset. */
	bool ObjectTable(FMemory& Memory, uint64_t Table, uint32_t Count)
	{
		uint64_t FirstChunk = 0;
		if (!Memory.Read(Table, &FirstChunk, sizeof(FirstChunk)) || !IsPointer(FirstChunk))
			return false;

		std::vector<uint8_t> Items(2048);
		if (!Memory.Read(FirstChunk, Items.data(), Items.size()))
			return false;

		struct FHeader
		{
			std::array<uint8_t, 64> Bytes{};
			bool bReadable = false;
		};

		std::vector<FHeader> Headers(256);
		for (size_t Word = 0; Word < Headers.size(); ++Word)
		{
			const uint64_t Object = At<uint64_t>(Items, Word * 8);

			if (IsPointer(Object))
				Headers[Word].bReadable = Memory.Read(Object, Headers[Word].Bytes.data(), 64);
		}

		bool bSparsePossible = false;

		auto LaterEvidence = [&](uint64_t Chunk, uint32_t ChunkIndex, uint32_t LocalCount, uint32_t Stride, uint32_t Item) -> bool
		{
			if (!LocalCount)
				return false;

			const uint32_t First = LocalCount > 32 ? LocalCount - 32 : 0;

			std::vector<uint8_t> Tail(32 * Stride);
			if (!Memory.Read(Chunk + uint64_t(First) * Stride, Tail.data(), uint64_t(LocalCount - First) * Stride))
				return false;

			struct FTail
			{
				uint32_t Slot;
				std::array<uint8_t, 64> Header{};
			};

			std::vector<FTail> Rows;
			for (uint32_t i = First; i < LocalCount; ++i)
			{
				const uint64_t Address = At<uint64_t>(Tail, (i - First) * Stride + Item);
				if (!IsPointer(Address))
					continue;

				FTail Row{ ChunkIndex * 65536u + i, {} };
				if (Memory.Read(Address, Row.Header.data(), Row.Header.size()))
					Rows.push_back(Row);
			}

			if (Rows.size() < 3)
				return false;

			for (uint32_t Index = 8; Index + 4 <= 64; ++Index)
			{
				bool bMismatch = false;
				unsigned Agreements = 0;

				for (const FTail& Row : Rows)
				{
					if (At<uint32_t>(Row.Header.data(), Index) != Row.Slot)
					{
						bMismatch = true;
						break;
					}

					++Agreements;
				}

				if (!bMismatch && Agreements >= 3)
					return true;
			}

			return false;
		};

		for (uint32_t Stride = 8; Stride <= 64; Stride += 8)
		{
			for (uint32_t Item = 0; Item + 8 <= Stride; Item += 8)
			{
				const uint32_t Capacity = static_cast<uint32_t>((Items.size() - Item - 8) / Stride + 1);
				const uint32_t Slots = std::min(Count, Capacity);

				bool bEmpty = true;
				for (uint32_t i = 0; i < Slots; ++i)
				{
					if (At<uint64_t>(Items, i * Stride + Item))
					{
						bEmpty = false;
						break;
					}
				}

				if (bEmpty)
				{
					/* Don't eliminate a live registry whose first chunk is empty */
					if (Count > Capacity)
					{
						const uint32_t ChunkIndex = (Count - 1) / 65536u;
						uint64_t Later = 0;

						if (Memory.Read(Table + uint64_t(ChunkIndex) * 8, &Later, sizeof(Later)) && IsPointer(Later))
							bSparsePossible = bSparsePossible || LaterEvidence(Later, ChunkIndex, Count - ChunkIndex * 65536u, Stride, Item);
					}

					continue;
				}

				for (uint32_t Index = 8; Index + 4 <= 64; Index += 4)
				{
					uint32_t Agreements = 0;
					bool bMismatch = false;

					for (uint32_t i = 0; i < Slots; ++i)
					{
						const uint32_t Pos = i * Stride + Item;
						const uint64_t Object = At<uint64_t>(Items, Pos);

						if (!Object)
							continue;

						if (!IsPointer(Object))
						{
							bMismatch = true;
							break;
						}

						const FHeader& Header = Headers[Pos / 8];
						if (!Header.bReadable)
							continue; // A transient slot is not evidence

						if (At<uint32_t>(Header.Bytes.data(), Index) != i)
						{
							bMismatch = true;
							break;
						}

						++Agreements;
					}

					if (!bMismatch && Agreements >= 3)
						return true;
				}
			}
		}

		return bSparsePossible;
	}

	bool ValidateProfileImpl(FMemory& Memory, uint64_t Base, const FProfile& P)
	{
		try
		{
			const uint64_t Limit = std::numeric_limits<uint64_t>::max();
			const uint64_t Rva = std::max(P.NamesRVA, P.ObjectsRVA);

			if (!P.NamesRVA || !P.ObjectsRVA || Rva > Limit - Base || 0x11000 > Limit - (Base + Rva) || !Pool(Memory, Base, P))
				return false;

			FReader Reader(Memory, P, Base);

			const int32_t Count = Reader.ObjectCount();
			const int32_t Chunks = Memory.Get<int32_t>(Base + P.ObjectsRVA + P.ObjectArray + P.ObjectsNumChunks);

			if (Chunks <= 0 || uint64_t(Chunks) * P.ChunkSize < static_cast<uint32_t>(Count) || Chunks > static_cast<int32_t>((P.MaxObjects + P.ChunkSize - 1) / P.ChunkSize))
				return false;

			std::set<std::string> Anchors;

			auto Inspect = [&](uint32_t I, uint64_t Obj)
			{
				if (!IsPointer(Obj))
					return;

				try
				{
					uint32_t NameData[2]{};
					if (!Memory.Read(Obj + P.ObjectName, NameData, sizeof(NameData)))
						return;

					const std::string LeafName = Leaf(Reader.Name(NameData[0], NameData[1]));
					if (LeafName != "CoreUObject" && LeafName != "Object" && LeafName != "Class" && LeafName != "Package" && LeafName != "ScriptStruct")
						return;

					if (Memory.Get<int32_t>(Obj + P.ObjectIndex) != static_cast<int32_t>(I))
						return;

					const std::string Full = Reader.FullName(Obj);

					if (Full == "Package CoreUObject")
					{
						if (Memory.Get<uint64_t>(Obj + P.ObjectOuter))
							return;
					}
					else if (Full == "Class CoreUObject.Object" || Full == "Class CoreUObject.Class" || Full == "Class CoreUObject.Package" || Full == "Class CoreUObject.ScriptStruct")
					{
						const int32_t Size = Memory.Get<int32_t>(Obj + P.StructSize);
						const int32_t Align = Memory.Get<int32_t>(Obj + P.StructAlignment);

						if (Size < 0x28 || Size > 0x100000 || Align <= 0 || Align > 4096 || (Align & (Align - 1)))
							return;

						if (Full == "Class CoreUObject.Object" && Memory.Get<uint64_t>(Obj + P.StructSuper))
							return;

						if (Full != "Class CoreUObject.Object" && !Reader.IsA(Obj, "Class"))
							return;
					}
					else
					{
						return;
					}

					Anchors.insert(Full);
				}
				catch (const ReadError&)
				{
					// GC/transient slots do not validate an anchor
				}
			};

			/* Registration order is not fixed, read slot batches (not a kernel call per empty slot). */
			const uint64_t Table = Memory.Get<uint64_t>(Base + P.ObjectsRVA + P.ObjectArray + P.ObjectsChunks);
			if (!IsPointer(Table))
				return false;

			std::vector<uint8_t> Items(1024 * P.ItemSize);

			for (uint32_t First = 0; First < static_cast<uint32_t>(Count) && Anchors.size() < 5;)
			{
				const uint64_t Chunk = Memory.Get<uint64_t>(Table + uint64_t(First / P.ChunkSize) * 8);
				const uint32_t Offset = First % P.ChunkSize;
				const uint32_t Amount = std::min<uint32_t>({ 1024u, P.ChunkSize - Offset, static_cast<uint32_t>(Count) - First });

				if (!IsPointer(Chunk) || !Memory.Read(Chunk + uint64_t(Offset) * P.ItemSize, Items.data(), uint64_t(Amount) * P.ItemSize))
					return false;

				for (uint32_t J = 0; J < Amount && Anchors.size() < 5; ++J)
					Inspect(First + J, At<uint64_t>(Items, J * P.ItemSize + P.ItemObject));

				First += Amount;
			}

			return Anchors.size() == 5;
		}
		catch (const ReadError&)
		{
		}

		return false;
	}

	bool DetectNameLayout(FMemory& Memory, uint64_t Base, FProfile& Profile)
	{
		try
		{
			const uint64_t Block = Memory.Get<uint64_t>(Base + Profile.NamesRVA + Profile.PoolBlocks);
			if (!IsPointer(Block))
				return false;

			const uint16_t Header = Memory.Get<uint16_t>(Block);

			bool bFound = false;
			FProfile Result;

			for (uint32_t Shift = 1; Shift <= 12; ++Shift)
			{
				if ((Header >> Shift) != 4)
					continue; // FName[0] must be "None"

				for (ENameCodec Codec : { ENameCodec::DfV1, ENameCodec::Plain })
				{
					FProfile P = Profile;
					P.NameCodec = Codec;
					P.NameLengthShift = Shift;

					try
					{
						if (FReader(Memory, P, Base).Name(0) != "None")
							continue;
					}
					catch (const ReadError&)
					{
						continue;
					}

					if (bFound)
						throw ReadError("Layout: ambiguous name encoding");

					bFound = true;
					Result = P;
				}
			}

			if (bFound)
				Profile = Result;

			return bFound;
		}
		catch (const ReadError&)
		{
			return false;
		}
	}

	struct FRow
	{
		uint32_t Slot;
		uint64_t Address;
		std::array<uint8_t, 64> Header;
	};

	struct FPhysicalRow
	{
		uint32_t Chunk;
		uint32_t Byte;
		uint64_t Address;
		std::array<uint8_t, 64> Header;
	};

	bool Disjoint(uint32_t A, uint32_t ASize, uint32_t B, uint32_t BSize)
	{
		return A + ASize <= B || B + BSize <= A;
	}

	bool SeparateFields(const FProfile& P)
	{
		const std::pair<uint32_t, uint32_t> Fields[] = { { P.ObjectName, 8 }, { P.ObjectIndex, 4 }, { P.ObjectClass, 8 }, { P.ObjectOuter, 8 } };

		for (unsigned i = 0; i < 4; ++i)
		{
			for (unsigned j = 0; j < i; ++j)
			{
				if (!Disjoint(Fields[i].first, Fields[i].second, Fields[j].first, Fields[j].second))
					return false;
			}
		}

		return true;
	}

	/* Infers FUObjectItem stride/offset and UObject Name/Class/Outer/Index offsets. Unique result or false/ReadError. */
	bool InferObjectLayout(FMemory& Source, uint64_t Base, FProfile& Profile)
	{
		FBudgetMemory Memory(Source, 48ULL * 1024 * 1024, 500000, "Layout: aggregate inference read budget exceeded");

		FProfile Seed = Profile;
		if (!DetectNameLayout(Memory, Base, Seed))
			return false;

		uint32_t Count = 0;
		uint64_t Table = 0;

		try
		{
			Count = static_cast<uint32_t>(FReader(Memory, Seed, Base).ObjectCount());
			Table = Memory.Get<uint64_t>(Base + Seed.ObjectsRVA + Seed.ObjectArray + Seed.ObjectsChunks);
		}
		catch (const ReadError&)
		{
			return false;
		}

		if (!IsPointer(Table))
			return false;

		/* Sample physical item storage once */
		std::vector<FPhysicalRow> Samples;
		uint64_t BytesRead = 0;

		for (uint32_t Ch = 0; uint64_t(Ch) * Seed.ChunkSize < Count && Samples.size() < 128; ++Ch)
		{
			uint64_t Chunk = 0;
			if (!Memory.Read(Table + uint64_t(Ch) * 8, &Chunk, 8) || !IsPointer(Chunk))
				return false;

			const uint32_t Slots = std::min<uint32_t>(Seed.ChunkSize, Count - Ch * Seed.ChunkSize);

			for (uint32_t Offset = 0; Offset < Slots * 64 && Samples.size() < 128;)
			{
				std::array<uint8_t, 4096> Bytes{};
				const uint32_t Amount = std::min<uint32_t>(static_cast<uint32_t>(Bytes.size()), Slots * 64 - Offset);

				BytesRead += Amount;
				if (BytesRead > 32ULL * 1024 * 1024)
					throw ReadError("Layout: physical item sampling budget exceeded");

				if (!Memory.Read(Chunk + Offset, Bytes.data(), Amount))
					break;

				for (uint32_t J = 0; J + 8 <= Amount && Samples.size() < 128; J += 8)
				{
					FPhysicalRow Row{ Ch, Offset + J, At<uint64_t>(Bytes.data(), J), {} };

					if (IsPointer(Row.Address) && Memory.Read(Row.Address, Row.Header.data(), Row.Header.size()))
						Samples.push_back(Row);
				}

				Offset += Amount;
			}
		}

		if (Samples.size() < 5)
			return false;

		std::vector<FProfile> Matches;
		unsigned CandidateChecks = 0;

		for (uint32_t Stride = 8; Stride <= 64; Stride += 8)
		{
			for (uint32_t Item = 0; Item + 8 <= Stride; Item += 8)
			{
				FProfile P = Seed;
				P.ItemSize = Stride;
				P.ItemObject = Item;

				std::vector<FRow> Rows;
				for (const FPhysicalRow& R : Samples)
				{
					if (R.Byte % Stride != Item || R.Byte / Stride >= P.ChunkSize)
						continue;

					const uint32_t Slot = R.Chunk * P.ChunkSize + R.Byte / Stride;
					if (Slot < Count)
						Rows.push_back({ Slot, R.Address, R.Header });
				}

				if (Rows.size() < 5)
					continue;

				/* Index/slot agreement eliminates wrong strides before any name read */
				std::vector<uint32_t> Indices;
				for (uint32_t Off = 8; Off + 4 <= 64; Off += 4)
				{
					if (std::all_of(Rows.begin(), Rows.end(), [&](const FRow& R) { return At<uint32_t>(R.Header.data(), Off) == R.Slot; }))
						Indices.push_back(Off);
				}

				if (Indices.empty())
					continue;

				FReader Names(Memory, P, Base);

				for (uint32_t NameOff = 8; NameOff + 8 <= 64; NameOff += 4)
				{
					const FRow* Package = nullptr;
					const FRow* Object = nullptr;
					FRow LatePackage{};
					FRow LateObject{};
					size_t Decoded = 0;
					std::set<std::string> Unique;

					for (const FRow& R : Rows)
					{
						try
						{
							const uint32_t Id = At<uint32_t>(R.Header.data(), NameOff);
							const uint32_t Number = At<uint32_t>(R.Header.data(), NameOff + 4);
							const std::string Name = Leaf(Names.Name(Id));

							++Decoded;

							if (!Number && Name == "CoreUObject")
								Package = &R;

							if (!Number && Name == "Object")
								Object = &R;

							if (Name != "None")
								Unique.insert(Name);
						}
						catch (const ReadError&)
						{
						}
					}

					if (Unique.size() < 4 || Decoded * 5 < Rows.size() * 4)
						continue;

					/* The sample establishes consistency, not registration order. Locate missing roots across committed slots. */
					if (!Package || !Object)
					{
						std::vector<uint8_t> Items(1024 * Stride);

						for (uint32_t First = 0; First < Count && (!Package || !Object);)
						{
							const uint32_t Offset = First % P.ChunkSize;
							const uint32_t Amount = std::min<uint32_t>({ 1024u, P.ChunkSize - Offset, Count - First });

							uint64_t Chunk = 0;
							if (!Memory.Read(Table + uint64_t(First / P.ChunkSize) * 8, &Chunk, 8) || !IsPointer(Chunk) ||
								!Memory.Read(Chunk + uint64_t(Offset) * Stride, Items.data(), uint64_t(Amount) * Stride))
								break;

							for (uint32_t J = 0; J < Amount && (!Package || !Object); ++J)
							{
								FRow R{ First + J, At<uint64_t>(Items, J * Stride + Item), {} };

								if (!IsPointer(R.Address) || !Memory.Read(R.Address, R.Header.data(), R.Header.size()))
									continue;

								if (!std::any_of(Indices.begin(), Indices.end(), [&](uint32_t Index) { return At<uint32_t>(R.Header.data(), Index) == R.Slot; }))
									continue;

								try
								{
									const std::string Name = Leaf(Names.Name(At<uint32_t>(R.Header.data(), NameOff), At<uint32_t>(R.Header.data(), NameOff + 4)));

									if (!Package && Name == "CoreUObject")
									{
										LatePackage = R;
										Package = &LatePackage;
									}

									if (!Object && Name == "Object")
									{
										LateObject = R;
										Object = &LateObject;
									}
								}
								catch (const ReadError&)
								{
								}
							}

							First += Amount;
						}
					}

					if (!Package || !Object)
						continue;

					P.ObjectName = NameOff;

					for (uint32_t Cls = 8; Cls + 8 <= 64; Cls += 8)
					{
						const uint64_t PackageClass = At<uint64_t>(Package->Header.data(), Cls);
						const uint64_t ObjectClass = At<uint64_t>(Object->Header.data(), Cls);

						if (!IsPointer(PackageClass) || !IsPointer(ObjectClass))
							continue;

						try
						{
							auto ClassName = [&](uint64_t Ptr) { return Names.Name(Memory.Get<uint32_t>(Ptr + NameOff), Memory.Get<uint32_t>(Ptr + NameOff + 4)); };

							if (ClassName(PackageClass) != "Package" || ClassName(ObjectClass) != "Class")
								continue;
						}
						catch (const ReadError&)
						{
							continue;
						}

						P.ObjectClass = Cls;

						for (uint32_t Outer = 8; Outer + 8 <= 64; Outer += 8)
						{
							if (At<uint64_t>(Package->Header.data(), Outer) != 0 || At<uint64_t>(Object->Header.data(), Outer) != Package->Address)
								continue;

							P.ObjectOuter = Outer;

							for (uint32_t Index : Indices)
							{
								P.ObjectIndex = Index;

								if (!SeparateFields(P))
									continue;

								if (++CandidateChecks > 128)
									throw ReadError("Layout: too many header candidates");

								if (ValidateProfileImpl(Memory, Base, P))
								{
									Matches.push_back(P);

									if (Matches.size() > 1)
										throw ReadError("Layout: ambiguous UObject/item layout; refusing to guess");
								}
							}
						}
					}
				}
			}
		}

		if (Matches.empty())
			return false;

		Profile = Matches.front();
		return true;
	}

	/* Reads a scan window; pages that can't be read (holes) are zero-filled instead of failing the whole window. */
	void ReadWindow(FMemory& Memory, uint64_t Start, std::vector<uint8_t>& Bytes)
	{
		if (Memory.Read(Start, Bytes.data(), Bytes.size()))
			return;

		constexpr size_t Page = 0x4000;

		for (size_t Offset = 0; Offset < Bytes.size(); Offset += Page)
		{
			const size_t Amount = std::min(Page, Bytes.size() - Offset);

			if (!Memory.Read(Start + Offset, Bytes.data() + Offset, Amount))
				memset(Bytes.data() + Offset, 0, Amount);
		}
	}
}

	bool TryDecodeName(uint64_t ImageBase, const FProfile& Profile, uint32_t Id, std::string& OutName)
	{
		try
		{
			FProcessMemory Memory;
			OutName = FReader(Memory, Profile, ImageBase).Name(Id);
			return true;
		}
		catch (const ReadError&)
		{
			return false;
		}
	}

	bool ValidateProfile(uint64_t ImageBase, const FProfile& Profile)
	{
		FProcessMemory Memory;
		return ValidateProfileImpl(Memory, ImageBase, Profile);
	}

	bool CompleteProfile(uint64_t ImageBase, FProfile& InOutProfile, std::string& OutError)
	{
		try
		{
			FProcessMemory Memory;
			FBudgetMemory Bounded(Memory, 128ULL * 1024 * 1024, 1000000, "Discovery: aggregate layout read budget exceeded");

			FProfile P = InOutProfile;
			if (!InferObjectLayout(Bounded, ImageBase, P))
			{
				OutError = "GNames/GObjects do not validate (name encoding, UObject header or items unsupported/not ready)";
				return false;
			}

			InOutProfile = P;
			return true;
		}
		catch (const std::exception& Error)
		{
			OutError = Error.what();
			return false;
		}
	}

	bool DiscoverProfile(uint64_t ImageBase, const FMemoryRanges& DataRanges, FProfile& OutProfile, std::string& OutError)
	{
		try
		{
			FProcessMemory Memory;

			/* Deeper reflection offsets come from the seed; the globals are unknown. */
			FProfile Layout = SeedProfile();
			Layout.NamesRVA = 8;
			Layout.ObjectsRVA = 8;

			constexpr uint64_t Budget = 512ULL * 1024 * 1024;
			uint64_t Total = 0;
			uint64_t Previous = ImageBase;

			for (const auto& Range : DataRanges)
			{
				if (Range.first < Previous || Range.second <= Range.first || Range.second - Range.first > Budget - Total)
					throw ReadError("Discovery: invalid/overlapping data ranges or 512 MiB scan budget exceeded");

				Total += Range.second - Range.first;
				Previous = Range.second;
			}

			LogInfo("[DeltaForce] Scanning %.1f MiB of data segments for GNames/GObjects...", static_cast<double>(Total) / (1024.0 * 1024.0));

			constexpr uint64_t Window = 4ULL * 1024 * 1024;
			constexpr uint64_t Overlap = 0x11000; // > PoolCurrentBlock + 4, so pool candidates near a window end are complete

			std::set<uint64_t> Names;
			std::set<uint64_t> Objects;
			std::unordered_map<uint64_t, bool> NameBlocks;
			std::map<std::pair<uint64_t, uint32_t>, bool> ObjectTables;
			std::vector<uint8_t> Bytes;
			uint64_t Completed = 0;
			uint64_t NextLog = 64ULL * 1024 * 1024;

			for (const auto& Range : DataRanges)
			{
				for (uint64_t Start = Range.first; Start < Range.second;)
				{
					const uint64_t StepBytes = std::min(Window, Range.second - Start);
					Bytes.resize(static_cast<size_t>(std::min(Window + Overlap, Range.second - Start)));
					ReadWindow(Memory, Start, Bytes);

					for (uint64_t Offset = (8 - (Start & 7)) & 7; Offset < StepBytes; Offset += 8)
					{
						const uint64_t Address = Start + Offset;
						if (Address <= ImageBase)
							continue;

						const size_t Size = Bytes.size();
						const size_t I = static_cast<size_t>(Offset);

						/* FNamePool candidate */
						if (I + Layout.PoolCurrentBlock + 4 <= Size)
						{
							const uint32_t Block = At<uint32_t>(Bytes, I + Layout.PoolCurrentBlock);
							const uint32_t Cursor = At<uint32_t>(Bytes, I + Layout.PoolCursor);
							const uint64_t First = At<uint64_t>(Bytes, I + Layout.PoolBlocks);

							if (Block < 8192 && Cursor <= (Layout.NameStride << Layout.PoolBlockBits) && !(Cursor & 1) && (Block || Cursor >= 6) && IsPointer(First))
							{
								auto Found = NameBlocks.find(First);

								if (Found == NameBlocks.end())
								{
									if (NameBlocks.size() >= 4096)
										NameBlocks.clear();

									bool bMatch = false;
									try
									{
										FBudgetMemory Probe(Memory, 1024 * 1024, 64, "probe budget");
										bMatch = NoneBlock(Probe, First);
									}
									catch (const ReadError&)
									{
									}

									Found = NameBlocks.emplace(First, bMatch).first;
								}

								if (Found->second)
								{
									FProfile P = Layout;
									P.NamesRVA = Address - ImageBase;

									try
									{
										FBudgetMemory Probe(Memory, 16ULL * 1024 * 1024, 16384, "probe budget");

										if (DetectNameLayout(Probe, ImageBase, P) && Pool(Probe, ImageBase, P))
											Names.insert(P.NamesRVA);
									}
									catch (const ReadError&)
									{
									}
								}
							}
						}

						/* FUObjectArray candidate */
						const size_t Arr = I + Layout.ObjectArray;
						if (Arr + Layout.ObjectsChunks + 8 <= Size)
						{
							const int32_t Count = At<int32_t>(Bytes, Arr + Layout.ObjectsCount);
							const int32_t Chunks = At<int32_t>(Bytes, Arr + Layout.ObjectsNumChunks);
							const uint64_t Table = At<uint64_t>(Bytes, Arr + Layout.ObjectsChunks);

							if (Count >= 5 && Count <= static_cast<int32_t>(Layout.MaxObjects) && Chunks > 0 &&
								Chunks <= static_cast<int32_t>((Layout.MaxObjects + Layout.ChunkSize - 1) / Layout.ChunkSize) &&
								uint64_t(Chunks) * Layout.ChunkSize >= static_cast<uint32_t>(Count) && IsPointer(Table))
							{
								const auto Key = std::make_pair(Table, static_cast<uint32_t>(Count));
								auto Found = ObjectTables.find(Key);

								if (Found == ObjectTables.end())
								{
									if (ObjectTables.size() >= 4096)
										ObjectTables.clear();

									bool bMatch = false;
									try
									{
										FBudgetMemory Probe(Memory, 16ULL * 1024 * 1024, 16384, "probe budget");
										bMatch = ObjectTable(Probe, Table, static_cast<uint32_t>(Count));
									}
									catch (const ReadError&)
									{
									}

									Found = ObjectTables.emplace(Key, bMatch).first;
								}

								if (Found->second)
									Objects.insert(Address - ImageBase);
							}
						}

						if (Names.size() > 16 || Objects.size() > 64)
							throw ReadError(("Discovery: too many candidate globals names=" + std::to_string(Names.size()) + " objects=" + std::to_string(Objects.size())).c_str());
					}

					Start += StepBytes;
					Completed += StepBytes;

					if (Completed >= NextLog)
					{
						LogInfo("[DeltaForce] Scanned %llu MiB (GNames candidates: %zu, GObjects candidates: %zu)", static_cast<unsigned long long>(Completed >> 20), Names.size(), Objects.size());
						NextLog += 64ULL * 1024 * 1024;
					}
				}
			}

			LogInfo("[DeltaForce] Scan finished (GNames candidates: %zu, GObjects candidates: %zu). Inferring layout...", Names.size(), Objects.size());

			/* Aggregate limit across all candidate pairs */
			FBudgetMemory Bounded(Memory, 128ULL * 1024 * 1024, 1000000, "Discovery: aggregate layout read budget exceeded");

			FProfile Result{};
			unsigned Matches = 0;

			for (const uint64_t N : Names)
			{
				for (const uint64_t O : Objects)
				{
					FProfile P = Layout;
					P.NamesRVA = N;
					P.ObjectsRVA = O;

					if (InferObjectLayout(Bounded, ImageBase, P))
					{
						Result = P;

						if (++Matches > 1)
							throw ReadError("Discovery: ambiguous validated globals; refusing to guess");
					}
				}
			}

			if (!Matches)
			{
				throw ReadError(Names.empty() ? "Discovery: FNamePool not found (game not fully loaded yet?)" :
					Objects.empty() ? "Discovery: FUObjectArray not found (game not fully loaded yet?)" :
					"Discovery: no unique supported layout (name encoding, UObject header, items or deeper reflection unsupported/not ready)");
			}

			OutProfile = Result;
			return true;
		}
		catch (const std::exception& Error)
		{
			OutError = Error.what();
			return false;
		}
	}
}
