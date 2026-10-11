#include <AssetSystem/AssetManager.hpp>

#include <algorithm>
#include <exception>
#include <thread>

namespace AssetSystem {

namespace {

std::vector<std::string> split_extensions(std::initializer_list<const char*> list) { return {list.begin(), list.end()}; }

} // namespace

AssetManager::AssetManager(const VirtualFileSystem& vfs, JobSystem::Scheduler* jobs, AssetManagerConfig config)
    : m_vfs(vfs), m_jobs(jobs), m_config(std::move(config)) {
    if (!m_config.default_loaders) return;

    for (const std::string& ext : split_extensions({".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif"})) {
        register_loader<ImageAsset>(ext, [](const LoadContext& c) { return decode_image(c.bytes, c.limits); });
    }
    for (const std::string& ext : split_extensions({".gltf", ".glb"})) {
        register_loader<MeshAsset>(ext, [](const LoadContext& c) { return parse_gltf(c.bytes, c.path, &c.vfs, c.limits); });
    }
    register_loader<MeshAsset>(".fmesh", [](const LoadContext& c) { return parse_mesh_binary(c.bytes, c.limits); });
    for (const std::string& ext : split_extensions({".txt", ".json", ".glsl", ".toml", ".cfg"})) {
        register_loader<TextAsset>(ext, [](const LoadContext& c) -> Result<TextAsset> {
            return TextAsset{std::string(reinterpret_cast<const char*>(c.bytes.data()), c.bytes.size())};
        });
    }
    for (const std::string& ext : split_extensions({".bin", ".dat"})) {
        register_loader<BlobAsset>(ext, [](const LoadContext& c) -> Result<BlobAsset> {
            return BlobAsset{Bytes(c.bytes.begin(), c.bytes.end())};
        });
    }
}

AssetManager::~AssetManager() {
    // Задачи держат `this`: менеджер не уходит, пока хоть одна не закончила.
    while (m_in_flight.load(std::memory_order_acquire) > 0) std::this_thread::yield();
}

void AssetManager::add_loader(std::type_index type, std::string_view extension, ErasedLoader loader) {
    std::string ext(extension);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (!ext.empty() && ext.front() != '.') ext.insert(ext.begin(), '.');
    std::lock_guard lock(m_mutex);
    m_loaders.insert_or_assign({type, std::move(ext)}, std::move(loader));
}

std::shared_ptr<detail::SlotBase> AssetManager::acquire(std::type_index type, std::string_view path, const SlotFactory& make,
                                                        const ValueSetter& set) {
    const auto reject = [&](std::shared_ptr<detail::SlotBase> slot, AssetError error) {
        slot->error = std::move(error);
        slot->state.store(LoadState::Failed, std::memory_order_release);
        m_failed.fetch_add(1, std::memory_order_relaxed);
        return slot;
    };

    auto normalized = normalize_path(path);
    if (!normalized) {
        auto slot = make();
        slot->path = std::string(path);
        return reject(std::move(slot), std::move(normalized.error()));
    }
    const AssetId id = asset_id_of_normalized(*normalized);
    const Key key{type, id};

    std::shared_ptr<detail::SlotBase> slot;
    const ErasedLoader* loader = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_cache.find(key); it != m_cache.end()) {
            if (auto existing = it->second.lock()) { // попадание: ничего не выделяем
                m_hits.fetch_add(1, std::memory_order_relaxed);
                return existing;
            }
        }
        slot = make();
        slot->id = id;
        slot->path = std::move(*normalized);
        const auto found = m_loaders.find({type, extension_of(slot->path)});
        if (found == m_loaders.end()) {
            std::string message = "no loader for '" + slot->path + "' with this asset type";
            return reject(std::move(slot), AssetError{ErrorCode::Unsupported, std::move(message)});
        }
        loader = &found->second; // узлы std::map не переезжают: указатель живёт, пока жив менеджер
        m_cache[key] = slot;
        m_started.fetch_add(1, std::memory_order_relaxed);
        m_in_flight.fetch_add(1, std::memory_order_relaxed);
    }

    auto work = [this, slot, loader, set] { run(slot, *loader, set); };
    if (m_jobs != nullptr) {
        m_jobs->run(slot->counter, std::move(work));
    } else {
        work();
    }
    return slot;
}

void AssetManager::run(const std::shared_ptr<detail::SlotBase>& slot, const ErasedLoader& loader, const ValueSetter& set) {
    try {
        auto bytes = m_vfs.read(slot->path);
        if (!bytes) {
            slot->error = std::move(bytes.error());
        } else if (bytes->size() > m_config.limits.max_file_bytes) {
            slot->error = AssetError{ErrorCode::TooLarge, slot->path + " is larger than the file limit"};
        } else {
            m_bytes.fetch_add(bytes->size(), std::memory_order_relaxed);
            const LoadContext context{slot->path, *bytes, m_vfs, m_config.limits};
            auto result = loader(context);
            if (result) {
                set(*slot, std::move(*result));
            } else {
                slot->error = std::move(result.error());
            }
        }
    } catch (const std::exception& e) {
        slot->error = AssetError{ErrorCode::Decode, "loader threw: " + std::string(e.what())};
    } catch (...) {
        slot->error = AssetError{ErrorCode::Decode, "loader threw an unknown exception"};
    }
    finish(*slot);
}

void AssetManager::finish(detail::SlotBase& slot) {
    const bool ok = !slot.error.has_value();
    (ok ? m_ok : m_failed).fetch_add(1, std::memory_order_relaxed);
    slot.state.store(ok ? LoadState::Ready : LoadState::Failed, std::memory_order_release);

    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard lock(slot.mutex);
        callbacks.swap(slot.callbacks);
    }
    for (auto& callback : callbacks) post_main(std::move(callback));
    m_in_flight.fetch_sub(1, std::memory_order_release); // последним: после него менеджер может быть уничтожен
}

void AssetManager::add_callback(detail::SlotBase& slot, std::function<void()> callback) {
    {
        std::lock_guard lock(slot.mutex);
        // Состояние проверяется под тем же мьютексом, под которым finish() забирает список: гонки нет ни в одну сторону.
        if (slot.state.load(std::memory_order_acquire) == LoadState::Pending) {
            slot.callbacks.push_back(std::move(callback));
            return;
        }
    }
    post_main(std::move(callback));
}

void AssetManager::post_main(std::function<void()> fn) {
    std::lock_guard lock(m_main_mutex);
    m_main_queue.push_back(std::move(fn));
}

std::size_t AssetManager::pump() {
    std::vector<std::function<void()>> batch;
    {
        std::lock_guard lock(m_main_mutex);
        batch.swap(m_main_queue);
    }
    std::exception_ptr first_error;
    for (auto& fn : batch) {
        try {
            fn();
        } catch (...) {
            if (!first_error) first_error = std::current_exception();
        }
    }
    if (first_error) std::rethrow_exception(first_error);
    return batch.size();
}

void AssetManager::wait_all() {
    while (m_in_flight.load(std::memory_order_acquire) > 0) {
        std::vector<std::shared_ptr<detail::SlotBase>> live;
        {
            std::lock_guard lock(m_mutex);
            for (const auto& [_, weak] : m_cache) {
                if (auto slot = weak.lock()) live.push_back(std::move(slot));
            }
        }
        if (m_jobs != nullptr) {
            for (const auto& slot : live) m_jobs->wait(slot->counter);
        }
        std::this_thread::yield();
    }
}

std::size_t AssetManager::collect() {
    std::lock_guard lock(m_mutex);
    return std::erase_if(m_cache, [](const auto& entry) { return entry.second.expired(); });
}

bool AssetManager::evict_impl(std::type_index type, std::string_view path) {
    const auto id = asset_id(path);
    if (!id) return false;
    std::lock_guard lock(m_mutex);
    return m_cache.erase(Key{type, *id}) > 0;
}

AssetStats AssetManager::stats() const noexcept {
    return {m_started.load(std::memory_order_relaxed), m_hits.load(std::memory_order_relaxed),
            m_ok.load(std::memory_order_relaxed),      m_failed.load(std::memory_order_relaxed),
            m_bytes.load(std::memory_order_relaxed),   m_in_flight.load(std::memory_order_relaxed)};
}

} // namespace AssetSystem
