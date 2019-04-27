// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glad/glad.h>

#include "common/assert.h"
#include "common/common_types.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_opengl/gl_shader_gen.h"

namespace Core {
class System;
}

namespace FileUtil {
class IOFile;
}

namespace OpenGL {

using ProgramCode = std::vector<u64>;
using Maxwell = Tegra::Engines::Maxwell3D::Regs;

using TextureBufferUsage = std::bitset<64>;

/// Allocated bindings used by an OpenGL shader program.
struct BaseBindings {
    u32 cbuf{};
    u32 gmem{};
    u32 sampler{};
    u32 image{};

    bool operator==(const BaseBindings& rhs) const {
        return std::tie(cbuf, gmem, sampler, image) ==
               std::tie(rhs.cbuf, rhs.gmem, rhs.sampler, rhs.image);
    }

    bool operator!=(const BaseBindings& rhs) const {
        return !operator==(rhs);
    }
};

/// Describes the different variants a single program can be compiled.
struct ProgramVariant {
    BaseBindings base_bindings;
    GLenum primitive_mode{};
    TextureBufferUsage texture_buffer_usage{};

    bool operator==(const ProgramVariant& rhs) const {
        return std::tie(base_bindings, primitive_mode, texture_buffer_usage) ==
               std::tie(rhs.base_bindings, rhs.primitive_mode, rhs.texture_buffer_usage);
    }

    bool operator!=(const ProgramVariant& rhs) const {
        return !operator==(rhs);
    }
};

/// Describes how a shader is used.
struct ShaderDiskCacheUsage {
    u64 unique_identifier{};
    ProgramVariant variant;

    bool operator==(const ShaderDiskCacheUsage& rhs) const {
        return std::tie(unique_identifier, variant) == std::tie(rhs.unique_identifier, rhs.variant);
    }

    bool operator!=(const ShaderDiskCacheUsage& rhs) const {
        return !operator==(rhs);
    }
};

} // namespace OpenGL

namespace std {

template <>
struct hash<OpenGL::BaseBindings> {
    std::size_t operator()(const OpenGL::BaseBindings& bindings) const {
        return static_cast<std::size_t>(bindings.cbuf) ^
               (static_cast<std::size_t>(bindings.gmem) << 8) ^
               (static_cast<std::size_t>(bindings.sampler) << 16) ^
               (static_cast<std::size_t>(bindings.image) << 24);
    }
};

template <>
struct hash<OpenGL::ProgramVariant> {
    std::size_t operator()(const OpenGL::ProgramVariant& variant) const {
        return std::hash<OpenGL::BaseBindings>()(variant.base_bindings) ^
               std::hash<OpenGL::TextureBufferUsage>()(variant.texture_buffer_usage) ^
               (static_cast<std::size_t>(variant.primitive_mode) << 6);
    }
};

template <>
struct hash<OpenGL::ShaderDiskCacheUsage> {
    std::size_t operator()(const OpenGL::ShaderDiskCacheUsage& usage) const {
        return static_cast<std::size_t>(usage.unique_identifier) ^
               std::hash<OpenGL::ProgramVariant>()(usage.variant);
    }
};

} // namespace std

namespace OpenGL {

/// Describes a shader how it's used by the guest GPU
class ShaderDiskCacheRaw {
public:
    explicit ShaderDiskCacheRaw(u64 unique_identifier, Maxwell::ShaderProgram program_type,
                                u32 program_code_size, u32 program_code_size_b,
                                ProgramCode program_code, ProgramCode program_code_b);
    ShaderDiskCacheRaw();
    ~ShaderDiskCacheRaw();

    bool Load(FileUtil::IOFile& file);

    bool Save(FileUtil::IOFile& file) const;

    u64 GetUniqueIdentifier() const {
        return unique_identifier;
    }

    bool HasProgramA() const {
        return program_type == Maxwell::ShaderProgram::VertexA;
    }

    Maxwell::ShaderProgram GetProgramType() const {
        return program_type;
    }

    Maxwell::ShaderStage GetProgramStage() const {
        switch (program_type) {
        case Maxwell::ShaderProgram::VertexA:
        case Maxwell::ShaderProgram::VertexB:
            return Maxwell::ShaderStage::Vertex;
        case Maxwell::ShaderProgram::TesselationControl:
            return Maxwell::ShaderStage::TesselationControl;
        case Maxwell::ShaderProgram::TesselationEval:
            return Maxwell::ShaderStage::TesselationEval;
        case Maxwell::ShaderProgram::Geometry:
            return Maxwell::ShaderStage::Geometry;
        case Maxwell::ShaderProgram::Fragment:
            return Maxwell::ShaderStage::Fragment;
        }
        UNREACHABLE();
    }

    const ProgramCode& GetProgramCode() const {
        return program_code;
    }

    const ProgramCode& GetProgramCodeB() const {
        return program_code_b;
    }

private:
    u64 unique_identifier{};
    Maxwell::ShaderProgram program_type{};
    u32 program_code_size{};
    u32 program_code_size_b{};

    ProgramCode program_code;
    ProgramCode program_code_b;
};

/// Contains decompiled data from a shader
struct ShaderDiskCacheDecompiled {
    std::string code;
    GLShader::ShaderEntries entries;
};

/// Contains an OpenGL dumped binary program
struct ShaderDiskCacheDump {
    GLenum binary_format;
    std::vector<u8> binary;
};

class ShaderDiskCacheOpenGL {
public:
    explicit ShaderDiskCacheOpenGL(Core::System& system);

    /// Loads transferable cache. If file has a old version or on failure, it deletes the file.
    std::optional<std::pair<std::vector<ShaderDiskCacheRaw>, std::vector<ShaderDiskCacheUsage>>>
    LoadTransferable();

    /// Loads current game's precompiled cache. Invalidates on failure.
    std::pair<std::unordered_map<u64, ShaderDiskCacheDecompiled>,
              std::unordered_map<ShaderDiskCacheUsage, ShaderDiskCacheDump>>
    LoadPrecompiled();

    /// Removes the transferable (and precompiled) cache file.
    void InvalidateTransferable() const;

    /// Removes the precompiled cache file.
    void InvalidatePrecompiled() const;

    /// Saves a raw dump to the transferable file. Checks for collisions.
    void SaveRaw(const ShaderDiskCacheRaw& entry);

    /// Saves shader usage to the transferable file. Does not check for collisions.
    void SaveUsage(const ShaderDiskCacheUsage& usage);

    /// Saves a decompiled entry to the precompiled file. Does not check for collisions.
    void SaveDecompiled(u64 unique_identifier, const std::string& code,
                        const GLShader::ShaderEntries& entries);

    /// Saves a dump entry to the precompiled file. Does not check for collisions.
    void SaveDump(const ShaderDiskCacheUsage& usage, GLuint program);

private:
    /// Loads the transferable cache. Returns empty on failure.
    std::optional<std::pair<std::unordered_map<u64, ShaderDiskCacheDecompiled>,
                            std::unordered_map<ShaderDiskCacheUsage, ShaderDiskCacheDump>>>
    LoadPrecompiledFile(FileUtil::IOFile& file);

    /// Loads a decompiled cache entry from the passed file. Returns empty on failure.
    std::optional<ShaderDiskCacheDecompiled> LoadDecompiledEntry(FileUtil::IOFile& file);

    /// Saves a decompiled entry to the passed file. Returns true on success.
    bool SaveDecompiledFile(FileUtil::IOFile& file, u64 unique_identifier, const std::string& code,
                            const std::vector<u8>& compressed_code,
                            const GLShader::ShaderEntries& entries);

    /// Returns if the cache can be used
    bool IsUsable() const;

    /// Opens current game's transferable file and write it's header if it doesn't exist
    FileUtil::IOFile AppendTransferableFile() const;

    /// Opens current game's precompiled file and write it's header if it doesn't exist
    FileUtil::IOFile AppendPrecompiledFile() const;

    /// Create shader disk cache directories. Returns true on success.
    bool EnsureDirectories() const;

    /// Gets current game's transferable file path
    std::string GetTransferablePath() const;

    /// Gets current game's precompiled file path
    std::string GetPrecompiledPath() const;

    /// Get user's transferable directory path
    std::string GetTransferableDir() const;

    /// Get user's precompiled directory path
    std::string GetPrecompiledDir() const;

    /// Get user's shader directory path
    std::string GetBaseDir() const;

    /// Get current game's title id
    std::string GetTitleID() const;

    // Copre system
    Core::System& system;
    // Stored transferable shaders
    std::unordered_map<u64, std::unordered_set<ShaderDiskCacheUsage>> transferable;
    // The cache has been loaded at boot
    bool tried_to_load{};
};

} // namespace OpenGL