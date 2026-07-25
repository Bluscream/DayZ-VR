#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    struct PatternByte { std::uint8_t value{}; std::uint8_t mask{}; };
    struct Signature
    {
        const char* field;
        const char* pattern;
        std::optional<std::size_t> displacementOffset;
        std::ptrdiff_t resultOffset{};
        std::optional<std::size_t> immediateOffset;
        std::size_t immediateSize{};
    };

    constexpr Signature kClientSignatures[]{
        {"prepareViewRva", "48 85 D2 0F ?? ?? ?? ?? ?? 48 8B C4 55 56 57 41 56 41 57 48 8D A8 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 89 58 08", {}},
        {"executeViewRva", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC ?? 48 8B 02 48 8B F9 48 8B CA 41 0F B6 E8 4C", {}},
        {"finalizeViewRva", "48 89 5C 24 08 57 48 83 EC 20 41 0F B6 F8 48 8B DA 45 84 C0 75 09 48 8B 02 48 8B CA FF 50 28", {}},
        {"projectionDispatchRva", "40 53 48 83 EC 20 48 8B D9 48 8B 89 ?? ?? ?? ?? 48 85 C9 74 13 84 D2 74 0F", {}},
        {"hudLayoutRva", "40 53 48 83 EC 60 48 8B 01 48 8B D9 0F 29 74 24 50 0F 29 7C 24 40 44 0F", {}},
        {"guiInputMessageRva", "40 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 30 48 8D 6C 24 30 4C 8B 7D 60 4C 8D A1 E8 00 00 00", {}},
        {"guiScaleRva", "F3 0F 11 35 ?? ?? ?? ?? 0F 28 CF 41 0F 28 D8 F3 0F 59 0D", 4},
        {"engineSingletonRva", "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 0C 48 81 C1 30 02 00 00 E8 ?? ?? ?? ?? 33 C9 E8 ?? ?? ?? ?? E?", 3},
        {"inventoryPreviewPrepareCallerRva", "44 8B 0D ?? ?? ?? ?? 45 33 FF 0F 28 BC 24 40 04 00 00 41 8B DF", {}},
        {"dynamicBlurRva", "48 8B C4 4C 89 48 20 55 48 8D 68 C8 48 81 EC 30 01 00 00 48 89 58 18 48 89 70 F0 48 89 78 E8", {}},
        {"dynamicBlurParameterIndexRva", "44 8B 05 ?? ?? ?? ?? 48 8B D8 48 8B 77 20 44 8B 77 54", 3},
        {"profileFovRva", "C7 43 48 00 00 80 3F F3 0F 10 05 ?? ?? ?? ?? 48 8D 8B 90 00 00 00 F3 0F 11 43 4C", 11},
        {"cameraManagerRva", "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 03 48 8B CB FF 50 60", 3},
        {"getActiveCameraStateRva", "83 B9 ?? ?? ?? ?? FF 74 08 48 8B 81 ?? ?? ?? ?? C3 48 8B 81 ?? ?? ?? ??", {}},
        {"cameraFovUpdateRva", "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 56 41 57 48 81 EC F0 00 00 00 0F 29 70 D8 48 8B E9", {}},
        {"finalPlayerSimulationRva", "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 81 EC ?? 00 00 00 0F 10 05 ?? ?? ?? ?? 48 8D 15", {}},
        {"humanAnimationUpdateRva", "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D AC 24 ?? ?? FF FF B8 ?? ?? 00 00 E8 ?? ?? ?? ?? 48 2B E0 0F 29 B4 24 ?? ?? 00 00 41 0F B6 D9 0F 28 F1 48 8B F9", {}},
        {"setEntityTransformRva", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 60 48 8B 71 68 48 8B EA 48 8B F9 48 85 F6 74 ?? 48 83 7E 18 00", {}},
        {"entityAttachmentTransformRva", "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 41 54 41 55 41 56 41 57 48 81 EC 90 00 00 00 4C 8B FA", {}},
        {"playerProxyTransformRva", "48 89 5C 24 20 56 57 41 54 41 56 41 57 48 83 EC 20 45 33 FF 48 8B F9 4C 89 3A 45 0F B6 E1 4C 89 7A 08", {}},
        {"playerProxyLocalTransformRva", "48 89 5C 24 08 57 48 83 EC 20 48 8B 01 49 8B D9 44 8B 4C 24 50 48 8B FA 48 8B D3 FF 90 E8 05 00 00 8B 03 89", {}},
        {"skinningExportRva", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 49 8B F9 49 8B D8 4C 8B F2 48 8B F1", {}},
        {"poseProviderVtableRva", "48 8D 05 ?? ?? ?? ?? 48 89 01 4C 89 B1 18 0C 00 00 4C 89 B1 20 0C 00 00", 3},
        {"getLocalTransformRva", "40 53 48 83 EC 50 8B C2 49 8B D8 8B 94 81 ?? ?? ?? ?? 83 FA FF 75 0D", {}},
        {"setLocalTransformRva", "40 53 48 83 EC 20 41 8B 40 24 48 8B D9 44 8B D2 48 8B 91 ?? ?? ?? ??", {}},
        {"rebuildModelTransformsRva", "48 8B C4 55 41 56 48 8D 68 A1 48 81 EC A8 00 00 00 80 B9 ?? ?? ?? ??", {}},
        {"inventoryLayerDrawCallerRva", "44 8B 44 24 70 44 8B CE FF 50 60 48 8D 0D ?? ?? ?? ?? 48 8B 5C 24 40", {}, 11},
        {"hudLayerDrawCaller0Rva", "44 8B C6 FF 50 68 48 8D 0D ?? ?? ?? ?? 48 8B 5C 24 40", {}, 6},
        {"hudLayerDrawCaller1Rva", "F7 F7 4C 8B 09 8B D3 44 8B C0 41 FF 51 68 48 8D 0D ?? ?? ?? ?? FF 15", {}, 14},
        {"contextCameraOffset", "40 53 48 83 EC 20 48 8B D9 48 8B 89 ?? ?? ?? ?? 48 85 C9 74 13 84 D2 74 0F", {}, 0, 12, 4},
        {"preparedContextCameraOffset", "4C 8B 95 ?? ?? ?? ?? 4C 8D A1 ?? ?? ?? ?? 4C 89 91 ?? ?? ?? ?? 4C 8B F2", {}, 0, 10, 4},
        {"contextDescriptorOffset", "8B D5 48 8B CF 8B F0 E8 ?? ?? ?? ?? 4D 8B 86 ?? ?? ?? ?? 0F 57 C9 F3 0F 10 05", {}, 0, 15, 4},
        {"contextArenaCursorOffset", "48 8B 01 FF 90 E0 00 00 00 8B 83 ?? ?? ?? ?? 48 8D 8B ?? ?? ?? ?? 48 C7 83", {}, 0, 11, 4},
        {"providerBoneMapOffset", "40 53 48 83 EC 50 8B C2 49 8B D8 8B 94 81 ?? ?? ?? ?? 83 FA FF 75 0D", {}, 0, 14, 4},
        {"providerParentMapOffset", "48 C1 E2 05 49 03 96 ?? ?? ?? ?? 4C 8D 42 10 E8 ?? ?? ?? ?? 43 8B 84 BE ?? ?? ?? ?? 4C 8D 45 F7 49 8B BE ?? ?? ?? ??", {}, 0, 24, 4},
        {"providerModelTransformsOffset", "4C 89 B1 ?? ?? ?? ?? 4C 89 B1 ?? ?? ?? ?? 4C 89 B1 ?? ?? ?? ?? C7 81 ?? ?? ?? ?? FF FF FF FF 44 89 B1 ?? ?? ?? ?? 44 89 89 ?? ?? ?? ??", {}, 0, 10, 4},
        {"providerSkinningTransformsOffset", "4C 89 B1 ?? ?? ?? ?? 4C 89 B1 ?? ?? ?? ?? 4C 89 B1 ?? ?? ?? ?? C7 81 ?? ?? ?? ?? FF FF FF FF 44 89 B1 ?? ?? ?? ?? 44 89 89 ?? ?? ?? ??", {}, 0, 17, 4},
        {"providerBoneCountOffset", "4C 89 B1 ?? ?? ?? ?? 4C 89 B1 ?? ?? ?? ?? 4C 89 B1 ?? ?? ?? ?? C7 81 ?? ?? ?? ?? FF FF FF FF 44 89 B1 ?? ?? ?? ?? 44 89 89 ?? ?? ?? ??", {}, 0, 34, 4},
        {"providerExtraBoneCountOffset", "4C 89 B1 ?? ?? ?? ?? 4C 89 B1 ?? ?? ?? ?? 4C 89 B1 ?? ?? ?? ?? C7 81 ?? ?? ?? ?? FF FF FF FF 44 89 B1 ?? ?? ?? ?? 44 89 89 ?? ?? ?? ??", {}, 0, 41, 4},
        {"providerPoseDirtyOffset", "48 8B C4 55 41 56 48 8D 68 A1 48 81 EC A8 00 00 00 80 B9 ?? ?? ?? ?? 00 4C 8B F1", {}, 0, 19, 4},
    };

    constexpr Signature kServerSignatures[]{
        {"tryFireWeaponRva", "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 60 8B FA 48 8B D9 48 85 C9 0F 84 ?? ?? ?? ?? 80 B9 E3 00 00 00 00 0F 85 ?? ?? ?? ??", {}},
        {"fireParameterBuilderRva", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 60 48 8B 99 ?? ?? ?? ?? 49 8B F1 4C 8B CA 44 89 44 24 20 45 8B F0", {}},
        {"shootFromCameraAdjustmentRva", "48 85 D2 0F 84 ?? ?? ?? ?? 48 8B C4 48 89 70 20 55 57 41 56 48 8D 68 B1 48 81 EC C0 00 00 00 0F 29 78 C8 0F 28 FB 41 8B F0", {}},
        {"cameraMuzzleConvergenceRva", "40 55 53 56 57 41 56 48 8D AC 24 80 FE FF FF 48 81 EC 80 02 00 00 48 8B FA 49 8B D9 48 8D 55 58 41 8B F0", {}},
        {"finalShotCreationRva", "4C 89 4C 24 20 4C 89 44 24 18 89 54 24 10 55 41 55 41 56 41 57 48 81 EC 98 00 00 00 49 8B E9 44 8B FA 4C 8B F1", {}},
        {"scriptRpcSendRva", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 41 0F B6 F9 48 8D 59 40 41 8B F0 48 8B EA", {}},
        {"onRpcDispatchRva", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 50 8B 05 ?? ?? ?? ?? 41 8B F9 49 8B F0 48 8B EA 48 8B D9 83 F8 FF", {}},
    };

    enum class ProfileKind
    {
        Client,
        Server,
    };

    struct Section { std::uint32_t rva{}, rawOffset{}, rawSize{}, characteristics{}; };
    struct Result
    {
        ProfileKind kind{};
        std::string name;
        std::uint32_t timestamp{}, imageSize{};
        std::vector<std::uint32_t> values;
    };

    std::uint32_t valueOf(const Result& result, std::span<const Signature> signatures,
        std::string_view field)
    {
        for (std::size_t index = 0; index < signatures.size(); ++index)
            if (field == signatures[index].field)
                return result.values.at(index);
        throw std::runtime_error("internal error: unknown generated field");
    }

    void validateExtractedLayout(const Result& result, std::span<const Signature> signatures)
    {
        for (std::size_t index = 0; index < signatures.size(); ++index)
        {
            const std::string_view field(signatures[index].field);
            const auto value = result.values[index];
            if (field.ends_with("Rva") && value >= result.imageSize)
                throw std::runtime_error(std::string(field) + ": RVA is outside SizeOfImage");
            if (field.ends_with("Offset") && value >= 0x10000)
                throw std::runtime_error(std::string(field) + ": implausibly large structure offset");
        }

        if (result.kind == ProfileKind::Server)
        {
            const auto adjustment = valueOf(result, signatures, "shootFromCameraAdjustmentRva");
            const auto convergence = valueOf(result, signatures, "cameraMuzzleConvergenceRva");
            if (convergence <= adjustment || convergence - adjustment > 0x1000)
                throw std::runtime_error("server aiming functions failed relationship validation");
            return;
        }

        const auto contextCamera = valueOf(result, signatures, "contextCameraOffset");
        const auto preparedCamera = valueOf(result, signatures, "preparedContextCameraOffset");
        const auto descriptor = valueOf(result, signatures, "contextDescriptorOffset");
        const auto arena = valueOf(result, signatures, "contextArenaCursorOffset");
        if ((contextCamera & 7) || (preparedCamera & 3) || (descriptor & 7) ||
            (arena & 7) || !(contextCamera < descriptor && descriptor < preparedCamera &&
                preparedCamera < arena) || arena - descriptor > 0x200)
            throw std::runtime_error("context layout offsets failed relationship validation");

        const auto parentMap = valueOf(result, signatures, "providerParentMapOffset");
        const auto boneMap = valueOf(result, signatures, "providerBoneMapOffset");
        const auto modelTransforms = valueOf(result, signatures, "providerModelTransformsOffset");
        const auto skinningTransforms = valueOf(result, signatures, "providerSkinningTransformsOffset");
        const auto boneCount = valueOf(result, signatures, "providerBoneCountOffset");
        const auto extraBoneCount = valueOf(result, signatures, "providerExtraBoneCountOffset");
        const auto poseDirty = valueOf(result, signatures, "providerPoseDirtyOffset");
        if ((parentMap | boneMap | modelTransforms | skinningTransforms | boneCount |
                extraBoneCount) & 3 ||
            !(parentMap < boneMap && boneMap < modelTransforms &&
                modelTransforms < skinningTransforms && skinningTransforms < boneCount &&
                boneCount < extraBoneCount && extraBoneCount < poseDirty) ||
            skinningTransforms - modelTransforms > 0x40 ||
            extraBoneCount - boneCount > 0x10 || poseDirty - extraBoneCount > 0x100)
            throw std::runtime_error("pose-provider offsets failed relationship validation");
    }

    class PeImage
    {
    public:
        explicit PeImage(const fs::path& path) : path_(path)
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream) throw std::runtime_error("cannot open " + path.string());
            const auto length = stream.tellg();
            if (length <= 0) throw std::runtime_error("empty input file");
            data_.resize(static_cast<std::size_t>(length));
            stream.seekg(0);
            stream.read(reinterpret_cast<char*>(data_.data()), length);
            if (!stream) throw std::runtime_error("cannot read " + path.string());
            parse();
        }

        Result scan(ProfileKind kind, std::span<const Signature> signatures) const
        {
            Result result{kind, path_.stem().string(), timestamp_, imageSize_, {}};
            for (const auto& signature : signatures)
            {
                const auto pattern = parsePattern(signature.pattern);
                std::vector<std::uint32_t> hits;
                for (const auto& section : sections_)
                {
                    if (!(section.characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
                    for (std::size_t offset = 0; offset + pattern.size() <= section.rawSize; ++offset)
                    {
                        bool equal = true;
                        for (std::size_t index = 0; index < pattern.size(); ++index)
                        {
                            const auto actual = data_[section.rawOffset + offset + index];
                            if ((actual & pattern[index].mask) != (pattern[index].value & pattern[index].mask))
                            { equal = false; break; }
                        }
                        if (equal) hits.push_back(section.rva + static_cast<std::uint32_t>(offset));
                    }
                }
                if (hits.empty()) throw std::runtime_error(std::string(signature.field) + ": signature not found");
                if (hits.size() != 1)
                {
                    std::ostringstream message;
                    message << signature.field << ": signature is ambiguous";
                    throw std::runtime_error(message.str());
                }
                const auto adjusted = static_cast<std::int64_t>(hits.front()) +
                    signature.resultOffset;
                if (adjusted < 0 || adjusted > UINT32_MAX)
                    throw std::runtime_error(std::string(signature.field) +
                        ": adjusted RVA is outside the PE32+ address range");
                auto value = static_cast<std::uint32_t>(adjusted);
                if (signature.immediateOffset)
                {
                    if (!signature.immediateSize || signature.immediateSize > sizeof(value))
                        throw std::runtime_error(std::string(signature.field) +
                            ": unsupported immediate size");
                    value = 0;
                    const auto bytes = readRva(hits.front() +
                        static_cast<std::uint32_t>(*signature.immediateOffset),
                        signature.immediateSize);
                    for (std::size_t index = 0; index < signature.immediateSize; ++index)
                        value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
                }
                if (signature.displacementOffset)
                {
                    const auto displacementRva = value + static_cast<std::uint32_t>(*signature.displacementOffset);
                    std::int32_t displacement{};
                    const auto bytes = readRva(displacementRva, sizeof(displacement));
                    std::memcpy(&displacement, bytes, sizeof(displacement));
                    const auto resolved = static_cast<std::int64_t>(displacementRva) + 4 +
                        displacement;
                    if (resolved < 0 || resolved > UINT32_MAX)
                        throw std::runtime_error(std::string(signature.field) +
                            ": resolved RVA is outside the PE32+ address range");
                    value = static_cast<std::uint32_t>(resolved);
                }
                result.values.push_back(value);
                std::cout << "  " << std::left << std::setw(38) << signature.field
                          << " 0x" << std::right << std::hex << std::uppercase << value << std::dec << '\n';
            }
            validateExtractedLayout(result, signatures);
            return result;
        }

    private:
        static int nibble(char value)
        {
            value = static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            throw std::runtime_error("invalid pattern digit");
        }

        static std::vector<PatternByte> parsePattern(std::string_view text)
        {
            std::vector<PatternByte> result;
            for (std::size_t pos = 0; pos < text.size();)
            {
                while (pos < text.size() && text[pos] == ' ') ++pos;
                if (pos == text.size()) break;
                const auto end = text.find(' ', pos);
                const auto token = text.substr(pos, end == std::string_view::npos ? text.size() - pos : end - pos);
                if (token.size() != 2) throw std::runtime_error("invalid pattern token");
                PatternByte item;
                if (token[0] != '?') { item.value |= static_cast<std::uint8_t>(nibble(token[0]) << 4); item.mask |= 0xF0; }
                if (token[1] != '?') { item.value |= static_cast<std::uint8_t>(nibble(token[1])); item.mask |= 0x0F; }
                result.push_back(item);
                pos = end == std::string_view::npos ? text.size() : end + 1;
            }
            return result;
        }

        const std::uint8_t* readRva(std::uint32_t rva, std::size_t size) const
        {
            for (const auto& section : sections_)
                if (rva >= section.rva && static_cast<std::uint64_t>(rva) + size <= static_cast<std::uint64_t>(section.rva) + section.rawSize)
                    return data_.data() + section.rawOffset + rva - section.rva;
            throw std::runtime_error("resolved RVA is outside raw section data");
        }

        template<class T> const T& at(std::size_t offset) const
        {
            if (offset + sizeof(T) > data_.size()) throw std::runtime_error("truncated PE file");
            return *reinterpret_cast<const T*>(data_.data() + offset);
        }

        void parse()
        {
            const auto& dos = at<IMAGE_DOS_HEADER>(0);
            if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) throw std::runtime_error("not a PE file");
            const auto ntOffset = static_cast<std::size_t>(dos.e_lfanew);
            const auto& nt = at<IMAGE_NT_HEADERS64>(ntOffset);
            if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
                nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
                throw std::runtime_error("expected an x64 PE32+ executable");
            timestamp_ = nt.FileHeader.TimeDateStamp;
            imageSize_ = nt.OptionalHeader.SizeOfImage;
            const auto sectionOffset = ntOffset + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
            for (std::size_t index = 0; index < nt.FileHeader.NumberOfSections; ++index)
            {
                const auto& section = at<IMAGE_SECTION_HEADER>(sectionOffset + index * sizeof(IMAGE_SECTION_HEADER));
                if (static_cast<std::uint64_t>(section.PointerToRawData) + section.SizeOfRawData > data_.size())
                    throw std::runtime_error("invalid PE section bounds");
                sections_.push_back({section.VirtualAddress, section.PointerToRawData,
                    section.SizeOfRawData, section.Characteristics});
            }
        }

        fs::path path_;
        std::vector<std::uint8_t> data_;
        std::vector<Section> sections_;
        std::uint32_t timestamp_{}, imageSize_{};
    };

    void writeProfileType(std::ofstream& out, std::string_view typeName,
        std::string_view arrayName, std::span<const Signature> signatures,
        const std::vector<const Result*>& results)
    {
        out << "    struct " << typeName << "\n    {\n        const char* name;\n"
               "        std::uint32_t peTimestamp;\n        std::uint32_t imageSize;\n";
        for (const auto& signature : signatures)
            out << "        std::uintptr_t " << signature.field << ";\n";
        out << "    };\n\n    inline constexpr std::array<" << typeName << ", "
            << results.size() << "> " << arrayName << "{{\n";
        for (const auto* result : results)
        {
            out << "        " << typeName << "{\"" << result->name << "\", 0x"
                << std::hex << std::uppercase << result->timestamp << "u, 0x"
                << result->imageSize << "u";
            for (const auto value : result->values) out << ", 0x" << value;
            out << "},\n";
        }
        out << "    }};\n\n";
    }

    void writeHeader(const fs::path& path, const std::vector<Result>& results)
    {
        if (path.has_parent_path()) fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot create " + path.string());
        std::vector<const Result*> clientResults;
        std::vector<const Result*> serverResults;
        for (const auto& result : results)
        {
            if (result.kind == ProfileKind::Client) clientResults.push_back(&result);
            else serverResults.push_back(&result);
        }
        out << "// Generated by dayz_offset_updater. Do not edit by hand.\n#pragma once\n\n"
               "#include <array>\n#include <cstdint>\n\nnamespace dayz::offsets\n{\n";
        writeProfileType(out, "BuildProfile", "kBuildProfiles", kClientSignatures,
            clientResults);
        writeProfileType(out, "ServerBuildProfile", "kServerBuildProfiles",
            kServerSignatures, serverResults);
        out << "}\n";
        if (!out) throw std::runtime_error("failed while writing " + path.string());
    }

    std::wstring lowercase(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character)
        {
            return static_cast<wchar_t>(std::towlower(character));
        });
        return value;
    }

    void usage()
    {
        std::cout << "Usage: dayz_offset_updater [--output <header>] <DayZ_x64.exe> "
                     "[DayZDiag_x64.exe] [DayZServer_x64.exe]\n"
                     "  DayZ_x64.exe       generates a client profile\n"
                     "  DayZDiag_x64.exe   generates both client and server profiles\n"
                     "  DayZServer_x64.exe generates a dedicated-server profile\n";
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        fs::path output = "dayz_offsets.generated.hpp";
        std::vector<fs::path> inputs;
        for (int index = 1; index < argc; ++index)
        {
            const std::wstring_view argument(argv[index]);
            if (argument == L"--help" || argument == L"-h") { usage(); return 0; }
            if (argument == L"--output" || argument == L"-o")
            {
                if (++index >= argc) throw std::runtime_error("--output requires a path");
                output = argv[index];
            }
            else if (!argument.empty() && argument.front() == L'-')
                throw std::runtime_error("unknown option");
            else inputs.emplace_back(argv[index]);
        }
        if (inputs.empty()) { usage(); return 2; }
        std::vector<Result> results;
        for (const auto& input : inputs)
        {
            const auto stem = lowercase(input.stem().wstring());
            const bool dedicatedServer = stem == L"dayzserver_x64";
            const bool diagnostic = stem == L"dayzdiag_x64";
            const bool client = stem == L"dayz_x64" || diagnostic;
            if (!dedicatedServer && !client)
                throw std::runtime_error("unsupported executable name: " + input.filename().string());

            const PeImage image(input);
            if (client)
            {
                std::cout << "Scanning client profile " << input.string() << '\n';
                results.push_back(image.scan(ProfileKind::Client, kClientSignatures));
            }
            if (dedicatedServer || diagnostic)
            {
                std::cout << "Scanning server profile " << input.string() << '\n';
                results.push_back(image.scan(ProfileKind::Server, kServerSignatures));
            }
        }
        writeHeader(output, results);
        std::cout << "Wrote " << output.string() << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "dayz_offset_updater: " << error.what() << '\n';
        return 1;
    }
}
