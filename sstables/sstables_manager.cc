/*
 * Copyright (C) 2019-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "log.hh"
#include "sstables/sstables_manager.hh"
#include "sstables/partition_index_cache.hh"
#include "sstables/sstables.hh"
#include "db/config.hh"
#include "gms/feature.hh"
#include "gms/feature_service.hh"
#include "db/system_keyspace.hh"

namespace sstables {

logging::logger smlogger("sstables_manager");

sstables_manager::sstables_manager(
    db::large_data_handler& large_data_handler, const db::config& dbcfg, gms::feature_service& feat, cache_tracker& ct, size_t available_memory, directory_semaphore& dir_sem, object_storage_config oscfg)
    : _large_data_handler(large_data_handler), _db_config(dbcfg), _features(feat), _cache_tracker(ct)
    , _sstable_metadata_concurrency_sem(
        max_count_sstable_metadata_concurrent_reads,
        max_memory_sstable_metadata_concurrent_reads(available_memory),
        "sstable_metadata_concurrency_sem",
        std::numeric_limits<size_t>::max(),
        utils::updateable_value(std::numeric_limits<uint32_t>::max()),
        utils::updateable_value(std::numeric_limits<uint32_t>::max()))
    , _dir_semaphore(dir_sem)
    , _object_storage_config(std::move(oscfg))
{
}

sstables_manager::~sstables_manager() {
    assert(_closing);
    assert(_active.empty());
    assert(_undergoing_close.empty());
}

void sstables_manager::update_object_storage_config(object_storage_config oscfg) {
    // FIXME -- entries grabbed by sstables' storages are not yet updated
    _object_storage_config = std::move(oscfg);
}

const locator::host_id& sstables_manager::get_local_host_id() const {
    return _db_config.host_id;
}

shared_sstable sstables_manager::make_sstable(schema_ptr schema,
        const data_dictionary::storage_options& storage,
        sstring dir,
        generation_type generation,
        sstable_version_types v,
        sstable_format_types f,
        gc_clock::time_point now,
        io_error_handler_gen error_handler_gen,
        size_t buffer_size) {
    return make_lw_shared<sstable>(std::move(schema), storage, std::move(dir), generation, v, f, get_large_data_handler(), *this, now, std::move(error_handler_gen), buffer_size);
}

sstable_writer_config sstables_manager::configure_writer(sstring origin) const {
    sstable_writer_config cfg;

    cfg.promoted_index_block_size = _db_config.column_index_size_in_kb() * 1024;
    cfg.promoted_index_auto_scale_threshold = (size_t)_db_config.column_index_auto_scale_threshold_in_kb() * 1024;
    if (!cfg.promoted_index_auto_scale_threshold) {
        cfg.promoted_index_auto_scale_threshold = std::numeric_limits<size_t>::max();
    }
    cfg.validation_level = _db_config.enable_sstable_key_validation()
            ? mutation_fragment_stream_validation_level::clustering_key
            : mutation_fragment_stream_validation_level::token;
    cfg.summary_byte_cost = summary_byte_cost(_db_config.sstable_summary_ratio());

    cfg.origin = std::move(origin);

    return cfg;
}

void sstables_manager::add(sstable* sst) {
    _active.push_back(*sst);
}

void sstables_manager::deactivate(sstable* sst) {
    // At this point, sst has a reference count of zero, since we got here from
    // lw_shared_ptr_deleter<sstables::sstable>::dispose().
    _active.erase(_active.iterator_to(*sst));
    _undergoing_close.push_back(*sst);
    // guard against sstable::close_files() calling shared_from_this() and immediately destroying
    // the result, which will dispose of the sstable recursively
    auto ptr = sst->shared_from_this();
    (void)sst->destroy().finally([ptr] {
        // destruction of ptr will call maybe_done() and release close()
    });
}

void sstables_manager::remove(sstable* sst) {
    _undergoing_close.erase(_undergoing_close.iterator_to(*sst));
    delete sst;
    maybe_done();
}

void sstables_manager::maybe_done() {
    if (_closing && _active.empty() && _undergoing_close.empty()) {
        _done.set_value();
    }
}

future<> sstables_manager::close() {
    _closing = true;
    maybe_done();
    co_await _done.get_future();
    co_await _sstable_metadata_concurrency_sem.stop();
}

std::unique_ptr<sstable_directory::components_lister> sstables_manager::get_components_lister(const data_dictionary::storage_options& storage, std::filesystem::path dir) {
    return std::visit(overloaded_functor {
        [dir] (const data_dictionary::storage_options::local& loc) mutable -> std::unique_ptr<sstable_directory::components_lister> {
            return std::make_unique<sstable_directory::filesystem_components_lister>(std::move(dir));
        },
        [this, dir] (const data_dictionary::storage_options::s3& os) mutable -> std::unique_ptr<sstable_directory::components_lister> {
            return std::make_unique<sstable_directory::system_keyspace_components_lister>(system_keyspace(), dir.native());
        }
    }, storage.value);
}

void sstables_manager::plug_system_keyspace(db::system_keyspace& sys_ks) noexcept {
    _sys_ks = sys_ks.shared_from_this();
}

void sstables_manager::unplug_system_keyspace() noexcept {
    _sys_ks = nullptr;
}

}   // namespace sstables
