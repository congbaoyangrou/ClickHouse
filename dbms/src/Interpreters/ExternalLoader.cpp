#include "ExternalLoader.h"

#include <mutex>
#include <pcg_random.hpp>
#include <Common/Config/AbstractConfigurationComparison.h>
#include <Common/Exception.h>
#include <Common/StringUtils/StringUtils.h>
#include <Common/ThreadPool.h>
#include <Common/randomSeed.h>
#include <Common/setThreadName.h>
#include <ext/chrono_io.h>
#include <ext/scope_guard.h>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/copy.hpp>
#include <unordered_set>


namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int BAD_ARGUMENTS;
}


namespace
{
    template <typename ReturnType>
    ReturnType convertTo(ExternalLoader::LoadResult result)
    {
        if constexpr (std::is_same_v<ReturnType, ExternalLoader::LoadResult>)
            return result;
        else
        {
            static_assert(std::is_same_v<ReturnType, ExternalLoader::LoadablePtr>);
            return std::move(result.object);
        }
    }

    template <typename ReturnType>
    ReturnType convertTo(ExternalLoader::LoadResults results)
    {
        if constexpr (std::is_same_v<ReturnType, ExternalLoader::LoadResults>)
            return results;
        else
        {
            static_assert(std::is_same_v<ReturnType, ExternalLoader::Loadables>);
            ExternalLoader::Loadables objects;
            objects.reserve(results.size());
            for (const auto & result : results)
            {
                if (auto object = std::move(result.object))
                    objects.push_back(std::move(object));
            }
            return objects;
        }
    }

    template <typename ReturnType>
    ReturnType notExists(const String & name)
    {
        if constexpr (std::is_same_v<ReturnType, ExternalLoader::LoadResult>)
        {
            ExternalLoader::LoadResult res;
            res.name = name;
            return res;
        }
        else
        {
            static_assert(std::is_same_v<ReturnType, ExternalLoader::LoadablePtr>);
            return nullptr;
        }
    }


    /// Lock mutex only in async mode
    /// In other case does nothing
    struct LoadingGuardForAsyncLoad
    {
        std::unique_lock<std::mutex> lock;
        LoadingGuardForAsyncLoad(bool async, std::mutex & mutex)
        {
            if (async)
                lock = std::unique_lock(mutex);
        }
    };
}

struct ExternalLoader::ObjectConfig
{
    Poco::AutoPtr<Poco::Util::AbstractConfiguration> config;
    String key_in_config;
    String repository_name;
    String path;
};


/** Reads configurations from configuration repository and parses it.
  */
class ExternalLoader::LoadablesConfigReader : private boost::noncopyable
{
public:
    LoadablesConfigReader(const String & type_name_, Logger * log_)
        : type_name(type_name_), log(log_)
    {
    }
    ~LoadablesConfigReader() = default;

    using RepositoryPtr = std::unique_ptr<IExternalLoaderConfigRepository>;

    void addConfigRepository(const String & repository_name, RepositoryPtr repository, const ExternalLoaderConfigSettings & settings)
    {
        std::lock_guard lock{mutex};
        RepositoryInfo repository_info{std::move(repository), settings, {}};
        repositories.emplace(repository_name, std::move(repository_info));
        need_collect_object_configs = true;
    }

    RepositoryPtr removeConfigRepository(const String & repository_name)
    {
        std::lock_guard lock{mutex};
        auto it = repositories.find(repository_name);
        if (it == repositories.end())
            return nullptr;
        auto repository = std::move(it->second.repository);
        repositories.erase(it);
        need_collect_object_configs = true;
        return repository;
    }

    using ObjectConfigsPtr = std::shared_ptr<const std::unordered_map<String /* object's name */, ObjectConfig>>;

    /// Reads all repositories.
    ObjectConfigsPtr read()
    {
        std::lock_guard lock(mutex);
        readRepositories();
        collectObjectConfigs();
        return object_configs;
    }

    /// Reads only a specified repository.
    /// This functions checks only a specified repository but returns configs from all repositories.
    ObjectConfigsPtr read(const String & repository_name)
    {
        std::lock_guard lock(mutex);
        readRepositories(repository_name);
        collectObjectConfigs();
        return object_configs;
    }

    /// Reads only a specified path from a specified repository.
    /// This functions checks only a specified repository but returns configs from all repositories.
    ObjectConfigsPtr read(const String & repository_name, const String & path)
    {
        std::lock_guard lock(mutex);
        readRepositories(repository_name, path);
        collectObjectConfigs();
        return object_configs;
    }

private:
    struct FileInfo
    {
        Poco::Timestamp last_update_time = 0;
        std::vector<std::pair<String, ObjectConfig>> objects; // Parsed contents of the file.
        bool in_use = true; // Whether the `FileInfo` should be destroyed because the correspondent file is deleted.
    };

    struct RepositoryInfo
    {
        RepositoryPtr repository;
        ExternalLoaderConfigSettings settings;
        std::unordered_map<String /* path */, FileInfo> files;
    };

    /// Reads the repositories.
    /// Checks last modification times of files and read those files which are new or changed.
    void readRepositories(const std::optional<String> & only_repository_name = {}, const std::optional<String> & only_path = {})
    {
        Strings repository_names;
        if (only_repository_name)
        {
            if (repositories.count(*only_repository_name))
                repository_names.push_back(*only_repository_name);
        }
        else
            boost::copy(repositories | boost::adaptors::map_keys, std::back_inserter(repository_names));

        for (const auto & repository_name : repository_names)
        {
            auto & repository_info = repositories[repository_name];

            for (auto & file_info : repository_info.files | boost::adaptors::map_values)
                file_info.in_use = false;

            Strings existing_paths;
            if (only_path)
            {
                if (repository_info.repository->exists(*only_path))
                    existing_paths.push_back(*only_path);
            }
            else
                boost::copy(repository_info.repository->getAllLoadablesDefinitionNames(), std::back_inserter(existing_paths));

            for (const auto & path : existing_paths)
            {
                auto it = repository_info.files.find(path);
                if (it != repository_info.files.end())
                {
                    FileInfo & file_info = it->second;
                    if (readFileInfo(file_info, *repository_info.repository, path, repository_info.settings))
                        need_collect_object_configs = true;
                }
                else
                {
                    FileInfo file_info;
                    if (readFileInfo(file_info, *repository_info.repository, path, repository_info.settings))
                    {
                        repository_info.files.emplace(path, std::move(file_info));
                        need_collect_object_configs = true;
                    }
                }
            }

            Strings deleted_paths;
            for (auto & [path, file_info] : repository_info.files)
            {
                if (file_info.in_use)
                    continue;

                if (only_path && (*only_path != path))
                    continue;

                deleted_paths.emplace_back(path);
            }

            if (!deleted_paths.empty())
            {
                for (const String & deleted_path : deleted_paths)
                    repository_info.files.erase(deleted_path);
                need_collect_object_configs = true;
            }
        }
    }

    /// Reads a file, returns true if the file is new or changed.
    bool readFileInfo(
        FileInfo & file_info,
        IExternalLoaderConfigRepository & repository,
        const String & path,
        const ExternalLoaderConfigSettings & settings) const
    {
        try
        {
            if (path.empty() || !repository.exists(path))
            {
                LOG_WARNING(log, "Config file '" + path + "' does not exist");
                return false;
            }

            auto update_time_from_repository = repository.getUpdateTime(path);

            /// Actually it can't be less, but for sure we check less or equal
            if (update_time_from_repository <= file_info.last_update_time)
            {
                file_info.in_use = true;
                return false;
            }

            auto file_contents = repository.load(path);

            /// get all objects' definitions
            Poco::Util::AbstractConfiguration::Keys keys;
            file_contents->keys(keys);

            /// for each object defined in repositories
            std::vector<std::pair<String, ObjectConfig>> object_configs_from_file;
            for (const auto & key : keys)
            {
                if (!startsWith(key, settings.external_config))
                {
                    if (!startsWith(key, "comment") && !startsWith(key, "include_from"))
                        LOG_WARNING(log, path << ": file contains unknown node '" << key << "', expected '" << settings.external_config << "'");
                    continue;
                }

                String object_name = file_contents->getString(key + "." + settings.external_name);
                if (object_name.empty())
                {
                    LOG_WARNING(log, path << ": node '" << key << "' defines " << type_name << " with an empty name. It's not allowed");
                    continue;
                }

                object_configs_from_file.emplace_back(object_name, ObjectConfig{file_contents, key, {}, {}});
            }

            file_info.objects = std::move(object_configs_from_file);
            file_info.last_update_time = update_time_from_repository;
            file_info.in_use = true;
            return true;
        }
        catch (...)
        {
            tryLogCurrentException(log, "Failed to load config file '" + path + "'");
            return false;
        }
    }

    /// Builds a map of current configurations of objects.
    void collectObjectConfigs()
    {
        if (!need_collect_object_configs)
            return;
        need_collect_object_configs = false;

        // Generate new result.
        auto new_configs = std::make_shared<std::unordered_map<String /* object's name */, ObjectConfig>>();

        for (const auto & [repository_name, repository_info] : repositories)
        {
            for (const auto & [path, file_info] : repository_info.files)
            {
                for (const auto & [object_name, object_config] : file_info.objects)
                {
                    auto already_added_it = new_configs->find(object_name);
                    if (already_added_it == new_configs->end())
                    {
                        auto & new_config = new_configs->emplace(object_name, object_config).first->second;
                        new_config.repository_name = repository_name;
                        new_config.path = path;
                    }
                    else
                    {
                        const auto & already_added = already_added_it->second;
                        if (!startsWith(repository_name, IExternalLoaderConfigRepository::INTERNAL_REPOSITORY_NAME_PREFIX) &&
                            !startsWith(already_added.repository_name, IExternalLoaderConfigRepository::INTERNAL_REPOSITORY_NAME_PREFIX))
                        {
                            LOG_WARNING(
                                log,
                                type_name << " '" << object_name << "' is found "
                                          << (((path == already_added.path) && repository_name == already_added.repository_name)
                                                  ? ("twice in the same file '" + path + "'")
                                                  : ("both in file '" + already_added.path + "' and '" + path + "'")));
                        }
                    }
                }
            }
        }

        object_configs = new_configs;
    }

    const String type_name;
    Logger * log;

    std::mutex mutex;
    std::unordered_map<String, RepositoryInfo> repositories;
    ObjectConfigsPtr object_configs;
    bool need_collect_object_configs = false;
};


/** Manages loading and reloading objects. Uses configurations from the class LoadablesConfigReader.
  * Supports parallel loading.
  */
class ExternalLoader::LoadingDispatcher : private boost::noncopyable
{
public:
    /// Called to load or reload an object.
    using CreateObjectFunction = std::function<LoadablePtr(
        const String & /* name */, const ObjectConfig & /* config */, const LoadablePtr & /* previous_version */)>;

    LoadingDispatcher(
        const CreateObjectFunction & create_object_function_,
        const String & type_name_,
        Logger * log_)
        : create_object(create_object_function_)
        , type_name(type_name_)
        , log(log_)
    {
    }

    ~LoadingDispatcher()
    {
        std::unique_lock lock{mutex};
        infos.clear(); /// We clear this map to tell the threads that we don't want any load results anymore.

        /// Wait for all the threads to finish.
        while (!loading_ids.empty())
        {
            auto it = loading_ids.begin();
            auto thread = std::move(it->second);
            loading_ids.erase(it);
            lock.unlock();
            event.notify_all();
            thread.join();
            lock.lock();
        }
    }

    using ObjectConfigsPtr = LoadablesConfigReader::ObjectConfigsPtr;

    /// Sets new configurations for all the objects.
    void setConfiguration(const ObjectConfigsPtr & new_configs)
    {
        std::lock_guard lock{mutex};
        if (configs == new_configs)
            return;

        configs = new_configs;

        std::vector<String> removed_names;
        for (auto & [name, info] : infos)
        {
            auto new_config_it = new_configs->find(name);
            if (new_config_it == new_configs->end())
                removed_names.emplace_back(name);
            else
            {
                const auto & new_config = new_config_it->second;
                bool config_is_same = isSameConfiguration(*info.object_config.config, info.object_config.key_in_config, *new_config.config, new_config.key_in_config);
                info.object_config = new_config;
                if (!config_is_same)
                {
                    /// Configuration has been changed.
                    info.config_changed = true;

                    if (info.triedToLoad())
                    {
                        /// The object has been tried to load before, so it is currently in use or was in use
                        /// and we should try to reload it with the new config.
                        cancelLoading(info);
                        startLoading(name, info);
                    }
                }
            }
        }

        /// Insert to the map those objects which added to the new configuration.
        for (const auto & [name, config] : *new_configs)
        {
            if (infos.find(name) == infos.end())
            {
                Info & info = infos.emplace(name, Info{name, config}).first->second;
                if (always_load_everything)
                    startLoading(name, info);
            }
        }

        /// Remove from the map those objects which were removed from the configuration.
        for (const String & name : removed_names)
            infos.erase(name);

        /// Maybe we have just added new objects which require to be loaded
        /// or maybe we have just removed object which were been loaded,
        /// so we should notify `event` to recheck conditions in load() and loadAll() now.
        event.notify_all();
    }

    /// Sets whether all the objects from the configuration should be always loaded (even if they aren't used).
    void enableAlwaysLoadEverything(bool enable)
    {
        std::lock_guard lock{mutex};
        if (always_load_everything == enable)
            return;

        always_load_everything = enable;

        if (enable)
        {
            /// Start loading all the objects which were not loaded yet.
            for (auto & [name, info] : infos)
                if (!info.triedToLoad())
                    startLoading(name, info);
        }
    }

    /// Sets whether the objects should be loaded asynchronously, each loading in a new thread (from the thread pool).
    void enableAsyncLoading(bool enable)
    {
        enable_async_loading = enable;
    }

    /// Returns the status of the object.
    /// If the object has not been loaded yet then the function returns Status::NOT_LOADED.
    /// If the specified name isn't found in the configuration then the function returns Status::NOT_EXIST.
    Status getCurrentStatus(const String & name) const
    {
        std::lock_guard lock{mutex};
        const Info * info = getInfo(name);
        if (!info)
            return Status::NOT_EXIST;
        return info->status();
    }

    /// Returns the load result of the object.
    template <typename ReturnType>
    ReturnType getCurrentLoadResult(const String & name) const
    {
        std::lock_guard lock{mutex};
        const Info * info = getInfo(name);
        if (!info)
            return notExists<ReturnType>(name);
        return info->getLoadResult<ReturnType>();
    }

    /// Returns all the load results as a map.
    /// The function doesn't load anything, it just returns the current load results as is.
    template <typename ReturnType>
    ReturnType getCurrentLoadResults(const FilterByNameFunction & filter) const
    {
        std::lock_guard lock{mutex};
        return collectLoadResults<ReturnType>(filter);
    }

    size_t getNumberOfCurrentlyLoadedObjects() const
    {
        std::lock_guard lock{mutex};
        size_t count = 0;
        for (const auto & name_and_info : infos)
        {
            const auto & info = name_and_info.second;
            if (info.loaded())
                ++count;
        }
        return count;
    }

    bool hasCurrentlyLoadedObjects() const
    {
        std::lock_guard lock{mutex};
        for (auto & name_info : infos)
            if (name_info.second.loaded())
                return true;
        return false;
    }

    Strings getAllTriedToLoadNames() const
    {
        Strings names;
        for (auto & [name, info] : infos)
            if (info.triedToLoad())
                names.push_back(name);
        return names;
    }

    /// Tries to load a specified object during the timeout.
    template <typename ReturnType>
    ReturnType tryLoad(const String & name, Duration timeout)
    {
        std::unique_lock lock{mutex};
        Info * info = loadImpl(name, timeout, lock);
        if (!info)
            return notExists<ReturnType>(name);
        return info->getLoadResult<ReturnType>();
    }

    template <typename ReturnType>
    ReturnType tryLoad(const FilterByNameFunction & filter, Duration timeout)
    {
        std::unique_lock lock{mutex};
        loadImpl(filter, timeout, lock);
        return collectLoadResults<ReturnType>(filter);
    }

    /// Tries to load or reload a specified object.
    template <typename ReturnType>
    ReturnType tryLoadOrReload(const String & name, Duration timeout)
    {
        std::unique_lock lock{mutex};
        Info * info = getInfo(name);
        if (!info)
            return notExists<ReturnType>(name);
        cancelLoading(*info);
        info->forced_to_reload = true;

        info = loadImpl(name, timeout, lock);
        if (!info)
            return notExists<ReturnType>(name);
        return info->getLoadResult<ReturnType>();
    }

    template <typename ReturnType>
    ReturnType tryLoadOrReload(const FilterByNameFunction & filter, Duration timeout)
    {
        std::unique_lock lock{mutex};
        for (auto & [name, info] : infos)
        {
            if (filter(name))
            {
                cancelLoading(info);
                info.forced_to_reload = true;
            }
        }

        loadImpl(filter, timeout, lock);
        return collectLoadResults<ReturnType>(filter);
    }

    /// Starts reloading all the object which update time is earlier than now.
    /// The function doesn't touch the objects which were never tried to load.
    void reloadOutdated()
    {
        /// Iterate through all the objects and find loaded ones which should be checked if they need update.
        std::unordered_map<LoadablePtr, bool> should_update_map;
        {
            std::lock_guard lock{mutex};
            TimePoint now = std::chrono::system_clock::now();
            for (const auto & name_and_info : infos)
            {
                const auto & info = name_and_info.second;
                if ((now >= info.next_update_time) && !info.loading() && info.loaded())
                    should_update_map.emplace(info.object, info.failedToReload());
            }
        }

        /// Find out which of the loaded objects were modified.
        /// We couldn't perform these checks while we were building `should_update_map` because
        /// the `mutex` should be unlocked while we're calling the function object->isModified()
        for (auto & [object, should_update_flag] : should_update_map)
        {
            try
            {
                /// Maybe alredy true, if we have an exception
                if (!should_update_flag)
                    should_update_flag = object->isModified();
            }
            catch (...)
            {
                tryLogCurrentException(log, "Could not check if " + type_name + " '" + object->getName() + "' was modified");
                /// Cannot check isModified, so update
                should_update_flag = true;
            }
        }

        /// Iterate through all the objects again and either start loading or just set `next_update_time`.
        {
            std::lock_guard lock{mutex};
            TimePoint now = std::chrono::system_clock::now();
            for (auto & [name, info] : infos)
            {
                if ((now >= info.next_update_time) && !info.loading())
                {
                    if (info.loaded())
                    {
                        auto it = should_update_map.find(info.object);
                        if (it == should_update_map.end())
                            continue; /// Object has been just loaded (it wasn't loaded while we were building the map `should_update_map`), so we don't have to reload it right now.

                        bool should_update_flag = it->second;
                        if (!should_update_flag)
                        {
                            info.next_update_time = calculateNextUpdateTime(info.object, info.error_count);
                            continue;
                        }

                        /// Object was modified or it was failed to reload last time, so it should be reloaded.
                        startLoading(name, info);
                    }
                    else if (info.failed())
                    {
                        /// Object was never loaded successfully and should be reloaded.
                        startLoading(name, info);
                    }
                }
            }
        }
    }

private:
    struct Info
    {
        Info(const String & name_, const ObjectConfig & object_config_) : name(name_), object_config(object_config_) {}

        bool loaded() const { return object != nullptr; }
        bool failed() const { return !object && exception; }
        bool loading() const { return loading_id != 0; }
        bool triedToLoad() const { return loaded() || failed() || loading(); }
        bool ready() const { return (loaded() || failed()) && !forced_to_reload; }
        bool failedToReload() const { return loaded() && exception != nullptr; }

        Status status() const
        {
            if (object)
                return loading() ? Status::LOADED_AND_RELOADING : Status::LOADED;
            else if (exception)
                return loading() ? Status::FAILED_AND_RELOADING : Status::FAILED;
            else
                return loading() ? Status::LOADING : Status::NOT_LOADED;
        }

        Duration loadingDuration() const
        {
            if (loading())
                return std::chrono::duration_cast<Duration>(std::chrono::system_clock::now() - loading_start_time);
            return std::chrono::duration_cast<Duration>(loading_end_time - loading_start_time);
        }

        template <typename ReturnType>
        ReturnType getLoadResult() const
        {
            if constexpr (std::is_same_v<ReturnType, LoadResult>)
            {
                LoadResult result;
                result.name = name;
                result.status = status();
                result.object = object;
                result.exception = exception;
                result.loading_start_time = loading_start_time;
                result.loading_duration = loadingDuration();
                result.origin = object_config.path;
                result.repository_name = object_config.repository_name;
                return result;
            }
            else
            {
                static_assert(std::is_same_v<ReturnType, ExternalLoader::LoadablePtr>);
                return object;
            }
        }

        String name;
        LoadablePtr object;
        ObjectConfig object_config;
        TimePoint loading_start_time;
        TimePoint loading_end_time;
        size_t loading_id = 0; /// Non-zero if it's loading right now.
        size_t error_count = 0; /// Numbers of errors since last successful loading.
        std::exception_ptr exception; /// Last error occurred.
        TimePoint next_update_time = TimePoint::max(); /// Time of the next update, `TimePoint::max()` means "never".
    };

    Info * getInfo(const String & name)
    {
        auto it = infos.find(name);
        if (it == infos.end())
            return nullptr;
        return &it->second;
    }

    const Info * getInfo(const String & name) const
    {
        auto it = infos.find(name);
        if (it == infos.end())
            return nullptr;
        return &it->second;
    }

    template <typename ReturnType>
    ReturnType collectLoadResults(const FilterByNameFunction & filter) const
    {
        ReturnType results;
        results.reserve(infos.size());
        for (const auto & [name, info] : infos)
        {
            if (filter(name))
            {
                auto result = info.template getLoadResult<typename ReturnType::value_type>();
                if constexpr (std::is_same_v<typename ReturnType::value_type, LoadablePtr>)
                {
                    if (!result)
                        continue;
                }
                results.emplace_back(std::move(result));
            }
        }
        return results;
    }

    Info * loadImpl(const String & name, Duration timeout, std::unique_lock<std::mutex> & lock)
    {
        Info * info;
        auto pred = [&]()
        {
            info = getInfo(name);
            if (!info || info->ready())
                return true;
            if (!info->loading())
                startLoading(name, *info);
            return info->ready();
        };

        if (timeout == WAIT)
            event.wait(lock, pred);
        else
            event.wait_for(lock, timeout, pred);

        return info;
    }

    void loadImpl(const FilterByNameFunction & filter, Duration timeout, std::unique_lock<std::mutex> & lock)
    {
        auto pred = [&]()
        {
            bool all_ready = true;
            for (auto & [name, info] : infos)
            {
                if (info.ready() || !filter(name))
                    continue;
                if (!info.loading())
                    startLoading(name, info);
                if (!info.ready())
                    all_ready = false;
            }
            return all_ready;
        };

        if (timeout == WAIT)
            event.wait(lock, pred);
        else
            event.wait_for(lock, timeout, pred);
    }

    void startLoading(const String & name, Info & info)
    {
        if (info.loading())
            return;

        /// All loadings have unique loading IDs.
        size_t loading_id = next_loading_id++;
        info.loading_id = loading_id;
        info.loading_start_time = std::chrono::system_clock::now();
        info.loading_end_time = TimePoint{};

        if (enable_async_loading)
        {
            /// Put a job to the thread pool for the loading.
            auto thread = ThreadFromGlobalPool{&LoadingDispatcher::doLoading, this, name, loading_id, true};
            loading_ids.try_emplace(loading_id, std::move(thread));
        }
        else
        {
            /// Perform the loading immediately.
            doLoading(name, loading_id, false);
        }
    }

    /// Load one object, returns object ptr or exception
    /// Do not require locking

    std::pair<LoadablePtr, std::exception_ptr> loadOneObject(
        const String & name,
        const ObjectConfig & config,
        LoadablePtr previous_version)
    {
        LoadablePtr new_object;
        std::exception_ptr new_exception;
        try
        {
            new_object = create_object(name, config, previous_version);
        }
        catch (...)
        {
            new_exception = std::current_exception();
        }
        return std::make_pair(new_object, new_exception);

    }

    /// Return single object info, checks loading_id and name
    std::optional<Info> getSingleObjectInfo(const String & name, size_t loading_id, bool async)
    {
        LoadingGuardForAsyncLoad lock(async, mutex);
        Info * info = getInfo(name);
        if (!info || !info->loading() || (info->loading_id != loading_id))
            return {};

        return *info;
    }

    /// Removes object loading_id from loading_ids if it present
    /// in other case do nothin should by done with lock
    void finishObjectLoading(size_t loading_id, const LoadingGuardForAsyncLoad &)
    {
        auto it = loading_ids.find(loading_id);
        if (it != loading_ids.end())
        {
            it->second.detach();
            loading_ids.erase(it);
        }
    }

    /// Process loading result
    /// Calculates next update time and process errors
    void processLoadResult(
        const String & name,
        size_t loading_id,
        LoadablePtr previous_version,
        LoadablePtr new_object,
        std::exception_ptr new_exception,
        size_t error_count,
        bool async)
    {
        LoadingGuardForAsyncLoad lock(async, mutex);
        /// Calculate a new update time.
        TimePoint next_update_time;
        try
        {
            if (new_exception)
                ++error_count;
            else
                error_count = 0;

            LoadablePtr object = previous_version;
            if (new_object)
                object = new_object;

            next_update_time = calculateNextUpdateTime(object, error_count);
        }
        catch (...)
        {
            tryLogCurrentException(log, "Cannot find out when the " + type_name + " '" + name + "' should be updated");
            next_update_time = TimePoint::max();
        }


        Info * info = getInfo(name);

        /// And again we should check if this is still the same loading as we were doing.
        /// This is necessary because the object could be removed or load with another config while the `mutex` was unlocked.
        if (!info || !info->loading() || (info->loading_id != loading_id))
            return;

        if (new_exception)
        {
            auto next_update_time_description = [next_update_time]
            {
                if (next_update_time == TimePoint::max())
                    return String();
                return ", next update is scheduled at " + ext::to_string(next_update_time);
            };
            if (previous_version)
                tryLogException(new_exception, log, "Could not update " + type_name + " '" + name + "'"
                                ", leaving the previous version" + next_update_time_description());
            else
                tryLogException(new_exception, log, "Could not load " + type_name + " '" + name + "'" + next_update_time_description());
        }

        if (new_object)
            info->object = new_object;

        info->exception = new_exception;
        info->error_count = error_count;
        info->loading_end_time = std::chrono::system_clock::now();
        info->loading_id = 0;
        info->next_update_time = next_update_time;

        info->forced_to_reload = false;
        if (new_object)
            info->config_changed = false;

        finishObjectLoading(loading_id, lock);
    }


    /// Does the loading, possibly in the separate thread.
    void doLoading(const String & name, size_t loading_id, bool async)
    {
        try
        {
            /// We check here if this is exactly the same loading as we planned to perform.
            /// This check is necessary because the object could be removed or load with another config before this thread even starts.
            std::optional<Info> info = getSingleObjectInfo(name, loading_id, async);
            if (!info)
                return;

            /// Use `create_function` to perform the actual loading.
            /// It's much better to do it with `mutex` unlocked because the loading can take a lot of time
            /// and require access to other objects.
            auto previous_version_to_use = info->object;
            bool need_complete_reloading = !info->object || info->config_changed || info->forced_to_reload;
            if (need_complete_reloading)
                previous_version_to_use = nullptr; /// Need complete reloading, cannot use the previous version.
            auto [new_object, new_exception] = loadOneObject(name, info->object_config, previous_version_to_use);
            if (!new_object && !new_exception)
                throw Exception("No object created and no exception raised for " + type_name, ErrorCodes::LOGICAL_ERROR);

            processLoadResult(name, loading_id, info->object, new_object, new_exception, info->error_count, async);
            event.notify_all();
        }
        catch (...)
        {
            LoadingGuardForAsyncLoad lock(async, mutex);
            finishObjectLoading(loading_id, lock);
            throw;
        }
    }

    void cancelLoading(const String & name)
    {
        Info * info = getInfo(name);
        if (info)
            cancelLoading(*info);
    }

    void cancelLoading(Info & info)
    {
        if (!info.loading())
            return;

        /// In fact we cannot actually CANCEL the loading (because it's possibly already being performed in another thread).
        /// But we can reset the `loading_id` and doLoading() will understand it as a signal to stop loading.
        info.loading_id = 0;
        info.loading_end_time = std::chrono::system_clock::now();
    }

    /// Calculate next update time for loaded_object. Can be called without mutex locking,
    /// because single loadable can be loaded in single thread only.
    TimePoint calculateNextUpdateTime(const LoadablePtr & loaded_object, size_t error_count) const
    {
        static constexpr auto never = TimePoint::max();

        if (loaded_object)
        {
            if (!loaded_object->supportUpdates())
                return never;

            /// do not update loadable objects with zero as lifetime
            const auto & lifetime = loaded_object->getLifetime();
            if (lifetime.min_sec == 0 && lifetime.max_sec == 0)
                return never;

            if (!error_count)
            {
                std::uniform_int_distribution<UInt64> distribution{lifetime.min_sec, lifetime.max_sec};
                return std::chrono::system_clock::now() + std::chrono::seconds{distribution(rnd_engine)};
            }
        }

        return std::chrono::system_clock::now() + std::chrono::seconds(calculateDurationWithBackoff(rnd_engine, error_count));
    }

    const CreateObjectFunction create_object;
    const String type_name;
    Logger * log;

    mutable std::mutex mutex;
    std::condition_variable event;
    ObjectConfigsPtr configs;
    std::unordered_map<String, Info> infos;
    bool always_load_everything = false;
    std::atomic<bool> enable_async_loading = false;
    std::unordered_map<size_t, ThreadFromGlobalPool> loading_ids;
    size_t next_loading_id = 1; /// should always be > 0
    mutable pcg64 rnd_engine{randomSeed()};
};


class ExternalLoader::PeriodicUpdater : private boost::noncopyable
{
public:
    static constexpr UInt64 check_period_sec = 5;

    PeriodicUpdater(LoadablesConfigReader & config_files_reader_, LoadingDispatcher & loading_dispatcher_)
        : config_files_reader(config_files_reader_), loading_dispatcher(loading_dispatcher_)
    {
    }

    ~PeriodicUpdater() { enable(false); }

    void enable(bool enable_)
    {
        std::unique_lock lock{mutex};
        enabled = enable_;

        if (enable_)
        {
            if (!thread.joinable())
            {
                /// Starts the thread which will do periodic updates.
                thread = ThreadFromGlobalPool{&PeriodicUpdater::doPeriodicUpdates, this};
            }
        }
        else
        {
            if (thread.joinable())
            {
                /// Wait for the thread to finish.
                auto temp_thread = std::move(thread);
                lock.unlock();
                event.notify_one();
                temp_thread.join();
            }
        }
    }


private:
    void doPeriodicUpdates()
    {
        setThreadName("ExterLdrReload");

        std::unique_lock lock{mutex};
        auto pred = [this] { return !enabled; };
        while (!event.wait_for(lock, std::chrono::seconds(check_period_sec), pred))
        {
            lock.unlock();
            loading_dispatcher.setConfiguration(config_files_reader.read());
            loading_dispatcher.reloadOutdated();
            lock.lock();
        }
    }

    LoadablesConfigReader & config_files_reader;
    LoadingDispatcher & loading_dispatcher;

    mutable std::mutex mutex;
    bool enabled = false;
    ThreadFromGlobalPool thread;
    std::condition_variable event;
};


ExternalLoader::ExternalLoader(const String & type_name_, Logger * log_)
    : config_files_reader(std::make_unique<LoadablesConfigReader>(type_name_, log_))
    , loading_dispatcher(std::make_unique<LoadingDispatcher>(
          std::bind(&ExternalLoader::createObject, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3),
          type_name_,
          log_))
    , periodic_updater(std::make_unique<PeriodicUpdater>(*config_files_reader, *loading_dispatcher))
    , type_name(type_name_)
    , log(log_)
{
}

ExternalLoader::~ExternalLoader() = default;

void ExternalLoader::addConfigRepository(
    const std::string & repository_name,
    std::unique_ptr<IExternalLoaderConfigRepository> config_repository,
    const ExternalLoaderConfigSettings & config_settings)
{
    config_files_reader->addConfigRepository(repository_name, std::move(config_repository), config_settings);
    reloadConfig(repository_name);
}

std::unique_ptr<IExternalLoaderConfigRepository> ExternalLoader::removeConfigRepository(const std::string & repository_name)
{
    auto repository = config_files_reader->removeConfigRepository(repository_name);
    reloadConfig(repository_name);
    return repository;
}

void ExternalLoader::enableAlwaysLoadEverything(bool enable)
{
    loading_dispatcher->enableAlwaysLoadEverything(enable);
}

void ExternalLoader::enableAsyncLoading(bool enable)
{
    loading_dispatcher->enableAsyncLoading(enable);
}

void ExternalLoader::enablePeriodicUpdates(bool enable_)
{
    periodic_updater->enable(enable_);
}

bool ExternalLoader::hasCurrentlyLoadedObjects() const
{
    return loading_dispatcher->hasCurrentlyLoadedObjects();
}

ExternalLoader::Status ExternalLoader::getCurrentStatus(const String & name) const
{
    return loading_dispatcher->getCurrentStatus(name);
}

template <typename ReturnType, typename>
ReturnType ExternalLoader::getCurrentLoadResult(const String & name) const
{
    return loading_dispatcher->getCurrentLoadResult<ReturnType>(name);
}

template <typename ReturnType, typename>
ReturnType ExternalLoader::getCurrentLoadResults(const FilterByNameFunction & filter) const
{
    return loading_dispatcher->getCurrentLoadResults<ReturnType>(filter);
}

ExternalLoader::Loadables ExternalLoader::getCurrentlyLoadedObjects() const
{
    return getCurrentLoadResults<Loadables>();
}

ExternalLoader::Loadables ExternalLoader::getCurrentlyLoadedObjects(const FilterByNameFunction & filter) const
{
    return getCurrentLoadResults<Loadables>(filter);
}

size_t ExternalLoader::getNumberOfCurrentlyLoadedObjects() const
{
    return loading_dispatcher->getNumberOfCurrentlyLoadedObjects();
}

template <typename ReturnType, typename>
ReturnType ExternalLoader::tryLoad(const String & name, Duration timeout) const
{
    return loading_dispatcher->tryLoad<ReturnType>(name, timeout);
}

template <typename ReturnType, typename>
ReturnType ExternalLoader::tryLoad(const FilterByNameFunction & filter, Duration timeout) const
{
    return loading_dispatcher->tryLoad<ReturnType>(filter, timeout);
}

template <typename ReturnType, typename>
ReturnType ExternalLoader::load(const String & name) const
{
    auto result = tryLoad<LoadResult>(name);
    checkLoaded(result, false);
    return convertTo<ReturnType>(result);
}

template <typename ReturnType, typename>
ReturnType ExternalLoader::load(const FilterByNameFunction & filter) const
{
    auto results = tryLoad<LoadResults>(filter);
    checkLoaded(results, false);
    return convertTo<ReturnType>(results);
}

template <typename ReturnType, typename>
ReturnType ExternalLoader::loadOrReload(const String & name) const
{
    loading_dispatcher->setConfiguration(config_files_reader->read());
    auto result = loading_dispatcher->tryLoadOrReload<LoadResult>(name, WAIT);
    checkLoaded(result, true);
    return convertTo<ReturnType>(result);
}

template <typename ReturnType, typename>
ReturnType ExternalLoader::loadOrReload(const FilterByNameFunction & filter) const
{
    loading_dispatcher->setConfiguration(config_files_reader->read());
    auto results = loading_dispatcher->tryLoadOrReload<LoadResults>(filter, WAIT);
    checkLoaded(results, true);
    return convertTo<ReturnType>(results);
}

template <typename ReturnType, typename>
ReturnType ExternalLoader::reloadAllTriedToLoad() const
{
    std::unordered_set<String> names;
    boost::range::copy(getAllTriedToLoadNames(), std::inserter(names, names.end()));
    return loadOrReload<ReturnType>([&names](const String & name) { return names.count(name); });
}

Strings ExternalLoader::getAllTriedToLoadNames() const
{
    return loading_dispatcher->getAllTriedToLoadNames();
}


void ExternalLoader::checkLoaded(const ExternalLoader::LoadResult & result,
                                 bool check_no_errors) const
{
    if (result.object && (!check_no_errors || !result.exception))
        return;
    if (result.status == ExternalLoader::Status::LOADING)
        throw Exception(type_name + " '" + result.name + "' is still loading", ErrorCodes::BAD_ARGUMENTS);
    if (result.exception)
        std::rethrow_exceptiozn(result.exception);
    if (result.status == ExternalLoader::Status::NOT_EXIST)
        throw Exception(type_name + " '" + result.name + "' not found", ErrorCodes::BAD_ARGUMENTS);
    if (result.status == ExternalLoader::Status::NOT_LOADED)
        throw Exception(type_name + " '" + result.name + "' not tried to load", ErrorCodes::BAD_ARGUMENTS);
}

void ExternalLoader::checkLoaded(const ExternalLoader::LoadResults & results,
                                 bool check_no_errors) const
{
    std::exception_ptr exception;
    for (const auto & result : results)
    {
        try
        {
            checkLoaded(result, check_no_errors);
        }
        catch (...)
        {
            if (!exception)
                exception = std::current_exception();
            else
                tryLogCurrentException(log);
        }
    }

    if (exception)
        std::rethrow_exception(exception);
}


void ExternalLoader::reloadConfig() const
{
    loading_dispatcher->setConfiguration(config_files_reader->read());
}

void ExternalLoader::reloadConfig(const String & repository_name) const
{
    loading_dispatcher->setConfiguration(config_files_reader->read(repository_name));
}

void ExternalLoader::reloadConfig(const String & repository_name, const String & path) const
{
    loading_dispatcher->setConfiguration(config_files_reader->read(repository_name, path));
}

ExternalLoader::LoadablePtr ExternalLoader::createObject(
    const String & name, const ObjectConfig & config, const LoadablePtr & previous_version) const
{
    if (previous_version)
        return previous_version->clone();

    return create(name, *config.config, config.key_in_config);
}

std::vector<std::pair<String, Int8>> ExternalLoader::getStatusEnumAllPossibleValues()
{
    return std::vector<std::pair<String, Int8>>{
        {toString(Status::NOT_LOADED), static_cast<Int8>(Status::NOT_LOADED)},
        {toString(Status::LOADED), static_cast<Int8>(Status::LOADED)},
        {toString(Status::FAILED), static_cast<Int8>(Status::FAILED)},
        {toString(Status::LOADING), static_cast<Int8>(Status::LOADING)},
        {toString(Status::LOADED_AND_RELOADING), static_cast<Int8>(Status::LOADED_AND_RELOADING)},
        {toString(Status::FAILED_AND_RELOADING), static_cast<Int8>(Status::FAILED_AND_RELOADING)},
        {toString(Status::NOT_EXIST), static_cast<Int8>(Status::NOT_EXIST)},
    };
}


String toString(ExternalLoader::Status status)
{
    using Status = ExternalLoader::Status;
    switch (status)
    {
        case Status::NOT_LOADED: return "NOT_LOADED";
        case Status::LOADED: return "LOADED";
        case Status::FAILED: return "FAILED";
        case Status::LOADING: return "LOADING";
        case Status::FAILED_AND_RELOADING: return "FAILED_AND_RELOADING";
        case Status::LOADED_AND_RELOADING: return "LOADED_AND_RELOADING";
        case Status::NOT_EXIST: return "NOT_EXIST";
    }
    __builtin_unreachable();
}


std::ostream & operator<<(std::ostream & out, ExternalLoader::Status status)
{
    return out << toString(status);
}


template ExternalLoader::LoadablePtr ExternalLoader::getCurrentLoadResult<ExternalLoader::LoadablePtr>(const String &) const;
template ExternalLoader::LoadResult ExternalLoader::getCurrentLoadResult<ExternalLoader::LoadResult>(const String &) const;
template ExternalLoader::Loadables ExternalLoader::getCurrentLoadResults<ExternalLoader::Loadables>(const FilterByNameFunction &) const;
template ExternalLoader::LoadResults ExternalLoader::getCurrentLoadResults<ExternalLoader::LoadResults>(const FilterByNameFunction &) const;

template ExternalLoader::LoadablePtr ExternalLoader::tryLoad<ExternalLoader::LoadablePtr>(const String &, Duration) const;
template ExternalLoader::LoadResult ExternalLoader::tryLoad<ExternalLoader::LoadResult>(const String &, Duration) const;
template ExternalLoader::Loadables ExternalLoader::tryLoad<ExternalLoader::Loadables>(const FilterByNameFunction &, Duration) const;
template ExternalLoader::LoadResults ExternalLoader::tryLoad<ExternalLoader::LoadResults>(const FilterByNameFunction &, Duration) const;

template ExternalLoader::LoadablePtr ExternalLoader::load<ExternalLoader::LoadablePtr>(const String &) const;
template ExternalLoader::LoadResult ExternalLoader::load<ExternalLoader::LoadResult>(const String &) const;
template ExternalLoader::Loadables ExternalLoader::load<ExternalLoader::Loadables>(const FilterByNameFunction &) const;
template ExternalLoader::LoadResults ExternalLoader::load<ExternalLoader::LoadResults>(const FilterByNameFunction &) const;

template ExternalLoader::LoadablePtr ExternalLoader::loadOrReload<ExternalLoader::LoadablePtr>(const String &) const;
template ExternalLoader::LoadResult ExternalLoader::loadOrReload<ExternalLoader::LoadResult>(const String &) const;
template ExternalLoader::Loadables ExternalLoader::loadOrReload<ExternalLoader::Loadables>(const FilterByNameFunction &) const;
template ExternalLoader::LoadResults ExternalLoader::loadOrReload<ExternalLoader::LoadResults>(const FilterByNameFunction &) const;

template ExternalLoader::Loadables ExternalLoader::reloadAllTriedToLoad<ExternalLoader::Loadables>() const;
template ExternalLoader::LoadResults ExternalLoader::reloadAllTriedToLoad<ExternalLoader::LoadResults>() const;
}
