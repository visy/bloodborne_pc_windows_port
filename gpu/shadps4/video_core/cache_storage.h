// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <functional>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
    /// bbport: inputs of one shader compilation, to translate it again for another GPU
    /// (vk_pipeline_serialization.cpp). Kept when the cache is rebuilt.
    ShaderSource,
};

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    void Open();
    void Close();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    void FinishPreload();
    /// bbport: removes every cached blob except those of the `keep` types (an incompatible cache
    /// is rebuilt, not ignored). Directory caches only.
    void Clear(std::initializer_list<BlobType> keep = {});
    /// bbport: helpers of the shader cache rebuild; directory caches only (false/empty for an
    /// archived cache).
    [[nodiscard]] bool SupportsFiles() const;
    [[nodiscard]] bool Exists(BlobType type, const std::string& name) const;
    void Remove(BlobType type, const std::string& name);
    /// Names (without extension) of the blobs of a type.
    [[nodiscard]] std::vector<std::string> ListNames(BlobType type) const;
    /// Waits until every queued write is on disk.
    void Flush();

    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);

    void ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func);
    /// bbport: as ForEachBlob, with the blob name (file name without extension).
    void ForEachNamedBlob(
        BlobType type,
        const std::function<void(const std::string& name, std::vector<u8>&& data)>& func);

private:
    std::jthread io_worker{};
    std::filesystem::path cache_path{};
    bool opened{};
};

} // namespace Storage
