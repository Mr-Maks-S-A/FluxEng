#pragma once
/**
 * @file AssetManager.hpp
 * @brief Менеджер ассетов: кеш, асинхронная загрузка на JobSystem, обратные вызовы в главном потоке.
 *
 * @code
 * AssetSystem::VirtualFileSystem vfs;
 * vfs.mount(AssetSystem::PackSource::open_file("base.fxpk").value());
 * AssetSystem::AssetManager assets(vfs, &runtime.jobs());      // nullptr вместо планировщика — загрузка синхронно
 *
 * auto staff = assets.load<AssetSystem::MeshAsset>("models/staff.fmesh");   // вернулся сразу, грузится в фоне
 * assets.on_done(staff, [&](const auto& h) { if (h.ready()) upload_to_gpu(*h.get()); });
 *
 * // каждый кадр, в потоке, где разрешён вызов GPU:
 * assets.pump();                                                // выполнит готовые обратные вызовы
 * @endcode
 *
 * Правила:
 * - **Один путь — одна загрузка.** Повторный `load<T>(path)` пока ассет жив возвращает тот же результат, загрузчик
 *   вызывается один раз, даже если просят из разных потоков одновременно.
 * - **Кеш слабый.** Менеджер держит `weak_ptr`: ассет живёт, пока у кого-то есть Handle (или `shared()`).
 *   `collect()` выбрасывает умершие записи.
 * - **Загрузка в фоне — только CPU** (чтение, разбор). Всё, что трогает GPU, делается в обратном вызове `on_done`,
 *   который выполняет `pump()` в вызывающем потоке.
 * - Менеджер должен пережить свои Handle не обязательно, а вот Scheduler и VFS обязаны пережить менеджер.
 *   Деструктор дожидается задач в полёте.
 * - Типы ассетов из коробки: ImageAsset (.png .jpg .jpeg .bmp .tga .gif), MeshAsset (.gltf .glb .fmesh),
 *   TextAsset (.txt .json .glsl .toml .cfg), BlobAsset (.bin .dat). Свои — register_loader<T>().
 */

#include <AssetSystem/AssetId.hpp>
#include <AssetSystem/Error.hpp>
#include <AssetSystem/Loaders.hpp>
#include <AssetSystem/Types.hpp>
#include <AssetSystem/Vfs.hpp>

#include <JobSystem/Scheduler.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AssetSystem {

enum class LoadState : std::uint8_t { Pending, Ready, Failed };

/// @brief Что видит загрузчик.
struct LoadContext {
    std::string_view path;                ///< Нормализованный путь.
    std::span<const std::byte> bytes;     ///< Содержимое файла.
    const VirtualFileSystem& vfs;         ///< Для зависимых файлов (внешние буферы glTF).
    const AssetLimits& limits;
};

namespace detail {

class SlotBase {
public:
    virtual ~SlotBase() = default;
    AssetId id{};
    std::string path;
    std::atomic<LoadState> state{LoadState::Pending};
    std::optional<AssetError> error; ///< Пишется до release-записи state.
    JobSystem::JobCounter counter;   ///< Обнуляется, когда задача загрузки закончена.
    std::mutex mutex;                ///< Защищает callbacks.
    std::vector<std::function<void()>> callbacks;
};

template<typename T>
class Slot final : public SlotBase {
public:
    std::shared_ptr<const T> value; ///< Пишется до release-записи state.
};

} // namespace detail

/// @brief Ссылка на загружаемый или загруженный ассет. Копируется дёшево (счётчик ссылок). Потокобезопасна для чтения.
template<typename T>
class Handle {
public:
    Handle() = default;

    [[nodiscard]] bool valid() const noexcept { return m_slot != nullptr; }
    [[nodiscard]] LoadState state() const noexcept {
        return m_slot ? m_slot->state.load(std::memory_order_acquire) : LoadState::Failed;
    }
    [[nodiscard]] bool pending() const noexcept { return state() == LoadState::Pending; }
    [[nodiscard]] bool ready() const noexcept { return state() == LoadState::Ready; }
    [[nodiscard]] bool failed() const noexcept { return state() == LoadState::Failed; }

    /// @brief Ассет или nullptr, пока не Ready.
    [[nodiscard]] const T* get() const noexcept { return ready() ? m_slot->value.get() : nullptr; }
    [[nodiscard]] std::shared_ptr<const T> shared() const noexcept { return ready() ? m_slot->value : nullptr; }
    /// @brief Причина отказа или nullptr, если не Failed.
    [[nodiscard]] const AssetError* error() const noexcept {
        return failed() && m_slot->error ? &*m_slot->error : nullptr;
    }
    [[nodiscard]] AssetId id() const noexcept { return m_slot ? m_slot->id : AssetId{}; }
    [[nodiscard]] const std::string& path() const noexcept {
        static const std::string empty;
        return m_slot ? m_slot->path : empty;
    }

    friend bool operator==(const Handle& a, const Handle& b) noexcept { return a.m_slot == b.m_slot; }

private:
    friend class AssetManager;
    explicit Handle(std::shared_ptr<detail::Slot<T>> slot) noexcept : m_slot(std::move(slot)) {}
    std::shared_ptr<detail::Slot<T>> m_slot;
};

struct AssetManagerConfig {
    AssetLimits limits{};
    bool default_loaders = true; ///< Зарегистрировать загрузчики из коробки.
};

struct AssetStats {
    std::uint64_t loads_started = 0;  ///< Реальных загрузок (промахи кеша).
    std::uint64_t cache_hits = 0;     ///< load() вернул уже существующий слот.
    std::uint64_t loads_ok = 0;
    std::uint64_t loads_failed = 0;
    std::uint64_t bytes_read = 0;     ///< Прочитано из VFS.
    std::uint64_t in_flight = 0;      ///< Задач в полёте прямо сейчас.
};

class AssetManager {
public:
    template<typename T>
    using LoaderFn = std::function<Result<T>(const LoadContext&)>;

    /// @param jobs Планировщик для фоновой загрузки; nullptr — каждый load() выполняется синхронно в вызывающем потоке.
    explicit AssetManager(const VirtualFileSystem& vfs, JobSystem::Scheduler* jobs = nullptr, AssetManagerConfig config = {});
    ~AssetManager();

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    /// @brief Загрузчик типа `T` для расширения (с точкой, любой регистр). Регистрировать до первых load().
    template<typename T>
    void register_loader(std::string_view extension, LoaderFn<T> loader) {
        ErasedLoader erased = [fn = std::move(loader)](const LoadContext& context) -> Result<std::shared_ptr<const void>> {
            Result<T> result = fn(context);
            if (!result) return std::unexpected(std::move(result.error()));
            return std::shared_ptr<const void>(std::make_shared<const T>(std::move(*result)));
        };
        add_loader(std::type_index(typeid(T)), extension, std::move(erased));
    }

    /// @brief Запускает загрузку (или возвращает уже идущую/готовую). Не блокирует, если есть планировщик.
    template<typename T>
    [[nodiscard]] Handle<T> load(std::string_view path) {
        auto slot = std::static_pointer_cast<detail::Slot<T>>(
            acquire(std::type_index(typeid(T)), path, [] { return std::make_shared<detail::Slot<T>>(); },
                    [](detail::SlotBase& base, std::shared_ptr<const void> value) {
                        static_cast<detail::Slot<T>&>(base).value = std::static_pointer_cast<const T>(std::move(value));
                    }));
        return Handle<T>(std::move(slot));
    }

    /// @brief Загружает и ждёт (ожидающий поток помогает выполнять задачи).
    template<typename T>
    [[nodiscard]] Result<std::shared_ptr<const T>> load_sync(std::string_view path) {
        return wait(load<T>(path));
    }

    /// @brief Ждёт завершения одной загрузки.
    template<typename T>
    [[nodiscard]] Result<std::shared_ptr<const T>> wait(const Handle<T>& handle) {
        if (!handle.valid()) return fail(ErrorCode::NotFound, "invalid handle");
        if (m_jobs != nullptr) m_jobs->wait(handle.m_slot->counter);
        if (handle.ready()) return handle.shared();
        return std::unexpected(handle.error() ? *handle.error() : AssetError{ErrorCode::Io, "load did not finish"});
    }

    /// @brief Ждёт все начатые загрузки.
    void wait_all();

    /// @brief Вызвать `fn(handle)` в потоке, который вызывает pump(), когда загрузка закончится (успехом или ошибкой).
    /// Если уже закончилась — на ближайшем pump().
    template<typename T>
    void on_done(const Handle<T>& handle, std::function<void(const Handle<T>&)> fn) {
        if (!handle.valid() || !fn) return;
        auto slot = handle.m_slot;
        add_callback(*slot, [handle, fn = std::move(fn)] { fn(handle); });
    }

    /// @brief Выполняет накопленные обратные вызовы. Звать из потока, которому разрешён GPU. @return сколько выполнено.
    std::size_t pump();

    /// @brief Забывает записи кеша, на которые никто не держит Handle.
    std::size_t collect();

    /// @brief Убирает путь из кеша: следующий load() прочитает файл заново (горячая перезагрузка).
    template<typename T>
    bool evict(std::string_view path) { return evict_impl(std::type_index(typeid(T)), path); }

    [[nodiscard]] AssetStats stats() const noexcept;
    [[nodiscard]] const VirtualFileSystem& vfs() const noexcept { return m_vfs; }
    [[nodiscard]] const AssetLimits& limits() const noexcept { return m_config.limits; }

private:
    using ErasedLoader = std::function<Result<std::shared_ptr<const void>>(const LoadContext&)>;
    using SlotFactory = std::function<std::shared_ptr<detail::SlotBase>()>;
    using ValueSetter = std::function<void(detail::SlotBase&, std::shared_ptr<const void>)>;

    struct Key {
        std::type_index type;
        AssetId id;
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            return static_cast<std::size_t>(key.id.value) ^ (key.type.hash_code() * 0x9E3779B97F4A7C15ull);
        }
    };

    void add_loader(std::type_index type, std::string_view extension, ErasedLoader loader);
    std::shared_ptr<detail::SlotBase> acquire(std::type_index type, std::string_view path, const SlotFactory& make, const ValueSetter& set);
    void run(const std::shared_ptr<detail::SlotBase>& slot, const ErasedLoader& loader, const ValueSetter& set);
    void finish(detail::SlotBase& slot);
    void add_callback(detail::SlotBase& slot, std::function<void()> callback);
    void post_main(std::function<void()> fn);
    bool evict_impl(std::type_index type, std::string_view path);

    const VirtualFileSystem& m_vfs;
    JobSystem::Scheduler* m_jobs;
    AssetManagerConfig m_config;

    mutable std::mutex m_mutex; // кеш и реестр загрузчиков
    std::unordered_map<Key, std::weak_ptr<detail::SlotBase>, KeyHash> m_cache;
    std::map<std::pair<std::type_index, std::string>, ErasedLoader> m_loaders;

    std::mutex m_main_mutex;
    std::vector<std::function<void()>> m_main_queue;

    std::atomic<std::uint64_t> m_started{0}, m_hits{0}, m_ok{0}, m_failed{0}, m_bytes{0}, m_in_flight{0};
};

} // namespace AssetSystem
