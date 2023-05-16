// Copyright 2020 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "cluster/partition.h"

#include "cloud_storage/remote_partition.h"
#include "cluster/logger.h"
#include "config/configuration.h"
#include "model/fundamental.h"
#include "model/metadata.h"
#include "model/namespace.h"
#include "prometheus/prometheus_sanitize.h"
#include "raft/types.h"

namespace cluster {

static bool is_id_allocator_topic(model::ntp ntp) {
    return ntp.ns == model::kafka_internal_namespace
           && ntp.tp.topic == model::id_allocator_topic;
}

static bool is_tx_manager_topic(const model::ntp& ntp) {
    return ntp == model::tx_manager_ntp;
}

partition::partition(
  consensus_ptr r,
  ss::sharded<cluster::tx_gateway_frontend>& tx_gateway_frontend,
  ss::sharded<cloud_storage::remote>& cloud_storage_api,
  ss::sharded<cloud_storage::cache>& cloud_storage_cache,
  ss::sharded<features::feature_table>& feature_table,
  ss::sharded<cluster::tm_stm_cache>& tm_stm_cache,
  config::binding<uint64_t> max_concurrent_producer_ids,
  std::optional<s3::bucket_name> read_replica_bucket)
  : _raft(r)
  , _probe(std::make_unique<replicated_partition_probe>(*this))
  , _tx_gateway_frontend(tx_gateway_frontend)
  , _feature_table(feature_table)
  , _tm_stm_cache(tm_stm_cache)
  , _is_tx_enabled(config::shard_local_cfg().enable_transactions.value())
  , _is_idempotence_enabled(
      config::shard_local_cfg().enable_idempotence.value()) {
    auto stm_manager = _raft->log().stm_manager();

    if (is_id_allocator_topic(_raft->ntp())) {
        _id_allocator_stm = ss::make_shared<cluster::id_allocator_stm>(
          clusterlog, _raft.get());
    } else if (is_tx_manager_topic(_raft->ntp())) {
        if (_raft->log_config().is_collectable()) {
            _log_eviction_stm = ss::make_lw_shared<raft::log_eviction_stm>(
              _raft.get(), clusterlog, stm_manager, _as);
        }

        if (_is_tx_enabled) {
            _tm_stm = ss::make_shared<cluster::tm_stm>(
              clusterlog, _raft.get(), feature_table, _tm_stm_cache);
            stm_manager->add_stm(_tm_stm);
        }
    } else {
        if (_raft->log_config().is_collectable()) {
            _log_eviction_stm = ss::make_lw_shared<raft::log_eviction_stm>(
              _raft.get(), clusterlog, stm_manager, _as);
        }
        const model::topic_namespace tp_ns(
          _raft->ntp().ns, _raft->ntp().tp.topic);
        bool is_group_ntp = tp_ns == model::kafka_group_nt
                            || tp_ns == model::kafka_consumer_offsets_nt;

        bool has_rm_stm = (_is_tx_enabled || _is_idempotence_enabled)
                          && model::controller_ntp != _raft->ntp()
                          && !is_group_ntp;

        if (has_rm_stm) {
            _rm_stm = ss::make_shared<cluster::rm_stm>(
              clusterlog,
              _raft.get(),
              _tx_gateway_frontend,
              _feature_table,
              max_concurrent_producer_ids);
            stm_manager->add_stm(_rm_stm);
        }

        if (
          config::shard_local_cfg().cloud_storage_enabled()
          && cloud_storage_api.local_is_initialized()
          && _raft->ntp().ns == model::kafka_namespace) {
            _archival_meta_stm
              = ss::make_shared<cluster::archival_metadata_stm>(
                _raft.get(),
                cloud_storage_api.local(),
                _feature_table.local(),
                clusterlog);
            stm_manager->add_stm(_archival_meta_stm);

            if (cloud_storage_cache.local_is_initialized()) {
                auto bucket
                  = config::shard_local_cfg().cloud_storage_bucket.value();
                if (
                  read_replica_bucket
                  && _raft->log_config().is_read_replica_mode_enabled()) {
                    vlog(
                      clusterlog.info,
                      "{} Remote topic bucket is {}",
                      _raft->ntp(),
                      read_replica_bucket);
                    // Override the bucket for read replicas
                    _read_replica_bucket = read_replica_bucket;
                    bucket = read_replica_bucket;
                }
                if (!bucket) {
                    throw std::runtime_error{
                      "configuration property cloud_storage_bucket is not set"};
                }
                _cloud_storage_partition
                  = ss::make_shared<cloud_storage::remote_partition>(
                    _archival_meta_stm->manifest(),
                    cloud_storage_api.local(),
                    cloud_storage_cache.local(),
                    s3::bucket_name{*bucket});
            }
        }
    }
}

ss::future<std::vector<rm_stm::tx_range>> partition::aborted_transactions_cloud(
  const cloud_storage::offset_range& offsets) {
    return _cloud_storage_partition->aborted_transactions(offsets);
}

bool partition::is_remote_fetch_enabled() const {
    const auto& cfg = _raft->log_config();
    if (_feature_table.local().is_active(features::feature::cloud_retention)) {
        // Since 22.3, the ntp_config is authoritative.
        return cfg.is_remote_fetch_enabled();
    } else {
        // We are in the process of an upgrade: apply <22.3 behavior of acting
        // as if every partition has remote read enabled if the cluster
        // default is true.
        return cfg.is_remote_fetch_enabled()
               || config::shard_local_cfg().cloud_storage_enable_remote_read();
    }
}

bool partition::cloud_data_available() const {
    return static_cast<bool>(_cloud_storage_partition)
           && _cloud_storage_partition->is_data_available();
}

model::offset partition::start_cloud_offset() const {
    vassert(
      cloud_data_available(),
      "Method can only be called if cloud data is available, ntp: {}",
      _raft->ntp());
    return kafka::offset_cast(
      _cloud_storage_partition->first_uploaded_offset());
}

model::offset partition::next_cloud_offset() const {
    vassert(
      cloud_data_available(),
      "Method can only be called if cloud data is available, ntp: {}",
      _raft->ntp());
    return kafka::offset_cast(_cloud_storage_partition->next_kafka_offset());
}

ss::future<storage::translating_reader> partition::make_cloud_reader(
  storage::log_reader_config config,
  std::optional<model::timeout_clock::time_point> deadline) {
    vassert(
      cloud_data_available(),
      "Method can only be called if cloud data is available, ntp: {}",
      _raft->ntp());
    return _cloud_storage_partition->make_reader(config, deadline);
}

ss::future<result<kafka_result>> partition::replicate(
  model::record_batch_reader&& r, raft::replicate_options opts) {
    using ret_t = result<kafka_result>;
    auto res = co_await _raft->replicate(std::move(r), opts);
    if (!res) {
        co_return ret_t(res.error());
    }
    co_return ret_t(
      kafka_result{kafka::offset(get_offset_translator_state()->from_log_offset(
        res.value().last_offset)())});
}

ss::shared_ptr<cluster::rm_stm> partition::rm_stm() {
    if (!_rm_stm) {
        if (!_is_tx_enabled && !_is_idempotence_enabled) {
            vlog(
              clusterlog.error,
              "Can't process transactional and idempotent requests to {}. The "
              "feature is disabled.",
              _raft->ntp());
        } else {
            vlog(
              clusterlog.error,
              "Topic {} doesn't support idempotency and transactional "
              "processing.",
              _raft->ntp());
        }
    }
    return _rm_stm;
}

kafka_stages partition::replicate_in_stages(
  model::batch_identity bid,
  model::record_batch_reader&& r,
  raft::replicate_options opts) {
    using ret_t = result<kafka_result>;
    if (bid.is_transactional) {
        if (!_is_tx_enabled) {
            vlog(
              clusterlog.error,
              "Can't process a transactional request to {}. Transactional "
              "processing isn't enabled.",
              _raft->ntp());
            return kafka_stages(raft::errc::timeout);
        }

        if (!_rm_stm) {
            vlog(
              clusterlog.error,
              "Topic {} doesn't support transactional processing.",
              _raft->ntp());
            return kafka_stages(raft::errc::timeout);
        }
    }

    if (bid.has_idempotent()) {
        if (!_is_idempotence_enabled) {
            vlog(
              clusterlog.error,
              "Can't process an idempotent request to {}. Idempotency isn't "
              "enabled.",
              _raft->ntp());
            return kafka_stages(raft::errc::timeout);
        }

        if (!_rm_stm) {
            vlog(
              clusterlog.error,
              "Topic {} doesn't support idempotency.",
              _raft->ntp());
            return kafka_stages(raft::errc::timeout);
        }
    }

    if (_rm_stm) {
        return _rm_stm->replicate_in_stages(bid, std::move(r), opts);
    }

    auto res = _raft->replicate_in_stages(std::move(r), opts);
    auto replicate_finished = res.replicate_finished.then(
      [this](result<raft::replicate_result> r) {
          if (!r) {
              return ret_t(r.error());
          }
          auto old_offset = r.value().last_offset;
          auto new_offset = kafka::offset(
            get_offset_translator_state()->from_log_offset(old_offset)());
          return ret_t(kafka_result{new_offset});
      });
    return kafka_stages(
      std::move(res.request_enqueued), std::move(replicate_finished));
}

ss::future<> partition::start() {
    auto ntp = _raft->ntp();

    _probe.setup_metrics(ntp);

    auto f = _raft->start();

    if (is_id_allocator_topic(ntp)) {
        return f.then([this] { return _id_allocator_stm->start(); });
    } else if (_log_eviction_stm) {
        f = f.then([this] { return _log_eviction_stm->start(); });
    }

    if (_rm_stm) {
        f = f.then([this] { return _rm_stm->start(); });
    }

    if (_tm_stm) {
        f = f.then([this] { return _tm_stm->start(); });
    }

    if (_archival_meta_stm) {
        f = f.then([this] { return _archival_meta_stm->start(); });
    }

    if (_cloud_storage_partition) {
        f = f.then([this] { return _cloud_storage_partition->start(); });
    }

    return f;
}

ss::future<> partition::stop() {
    auto partition_ntp = ntp();
    vlog(clusterlog.debug, "Stopping partition: {}", partition_ntp);
    _as.request_abort();

    if (_id_allocator_stm) {
        vlog(
          clusterlog.debug,
          "Stopping id_allocator_stm on partition: {}",
          partition_ntp);
        co_await _id_allocator_stm->stop();
    }

    if (_log_eviction_stm) {
        vlog(
          clusterlog.debug,
          "Stopping log_eviction_stm on partition: {}",
          partition_ntp);
        co_await _log_eviction_stm->stop();
    }

    if (_rm_stm) {
        vlog(
          clusterlog.debug, "Stopping rm_stm on partition: {}", partition_ntp);
        co_await _rm_stm->stop();
    }

    if (_tm_stm) {
        vlog(
          clusterlog.debug, "Stopping tm_stm on partition: {}", partition_ntp);
        co_await _tm_stm->stop();
    }
    if (_archival_meta_stm) {
        vlog(
          clusterlog.debug,
          "Stopping archival_meta_stm on partition: {}",
          partition_ntp);
        co_await _archival_meta_stm->stop();
    }

    if (_cloud_storage_partition) {
        vlog(
          clusterlog.debug,
          "Stopping cloud_storage_partition on partition: {}",
          partition_ntp);
        co_await _cloud_storage_partition->stop();
    }

    vlog(clusterlog.debug, "Stopped partition {}", partition_ntp);
}

ss::future<std::optional<storage::timequery_result>>
partition::timequery(storage::timequery_config cfg) {
    // Read replicas never consider local raft data
    if (_raft->log_config().is_read_replica_mode_enabled()) {
        co_return co_await cloud_storage_timequery(cfg);
    }

    if (_raft->log().start_timestamp() <= cfg.time) {
        // The query is ahead of the local data's start_timestamp: this means
        // it _might_ hit on local data: start_timestamp is not precise, so
        // once we query we might still fall back to cloud storage
        auto result = co_await local_timequery(cfg);
        if (!result.has_value()) {
            // The local storage hit a case where it needs to fall back
            // to querying cloud storage.
            co_return co_await cloud_storage_timequery(cfg);
        } else {
            co_return result;
        }
    } else {
        if (
          may_read_from_cloud()
          && _cloud_storage_partition->bounds_timestamp(cfg.time)) {
            // Timestamp is before local storage but within cloud storage
            co_return co_await cloud_storage_timequery(cfg);
        } else {
            // No cloud data: queries earlier than the start of the log
            // will hit on the start of the log.
            co_return co_await local_timequery(cfg);
        }
    }
}

bool partition::may_read_from_cloud() const {
    return _cloud_storage_partition
           && _cloud_storage_partition->is_data_available();
}

ss::future<std::optional<storage::timequery_result>>
partition::cloud_storage_timequery(storage::timequery_config cfg) {
    if (may_read_from_cloud()) {
        // We have data in the remote partition, and all the data in the raft
        // log is ahead of the query timestamp or the topic is a read replica,
        // so proceed to query the remote partition to try and find the earliest
        // data that has timestamp >= the query time.
        vlog(
          clusterlog.debug,
          "timequery (cloud) {} t={} max_offset(k)={}",
          _raft->ntp(),
          cfg.time,
          cfg.max_offset);

        // remote_partition pre-translates offsets for us, so no call into
        // the offset translator here
        auto result = co_await _cloud_storage_partition->timequery(cfg);
        if (result) {
            vlog(
              clusterlog.debug,
              "timequery (cloud) {} t={} max_offset(r)={} result(r)={}",
              _raft->ntp(),
              cfg.time,
              cfg.max_offset,
              result->offset);
        }

        co_return result;
    }

    co_return std::nullopt;
}

ss::future<std::optional<storage::timequery_result>>
partition::local_timequery(storage::timequery_config cfg) {
    vlog(
      clusterlog.debug,
      "timequery (raft) {} t={} max_offset(k)={}",
      _raft->ntp(),
      cfg.time,
      cfg.max_offset);

    cfg.max_offset = _raft->get_offset_translator_state()->to_log_offset(
      cfg.max_offset);

    auto result = co_await _raft->timequery(cfg);

    bool may_answer_from_cloud = may_read_from_cloud()
                                 && _cloud_storage_partition->bounds_timestamp(
                                   cfg.time);

    if (result) {
        if (
          _raft->log().start_timestamp() > cfg.time && may_answer_from_cloud) {
            // Query raced with prefix truncation
            vlog(
              clusterlog.debug,
              "timequery (raft) ts={} raced with truncation (start_timestamp "
              "{}, "
              "result {})",
              cfg.time,
              _raft->log().start_timestamp(),
              result->time);
            co_return std::nullopt;
        }

        if (
          _raft->log().start_timestamp() <= cfg.time && result->time > cfg.time
          && may_answer_from_cloud) {
            // start_timestamp() points to the beginning of the oldest segment,
            // but start_offset points to somewhere within a segment.  If our
            // timequery hits the range between the start of segment and
            // the start_offset, consensus::timequery may answer with
            // the start offset rather than the pre-start-offset location
            // where the timestamp is actually found.
            // Ref https://github.com/redpanda-data/redpanda/issues/9669
            vlog(
              clusterlog.debug,
              "Timequery (raft) ts={} miss on local log (start_timestamp {}, "
              "result {})",
              cfg.time,
              _raft->log().start_timestamp(),
              result->time);
            co_return std::nullopt;
        }

        if (result->offset == _raft->log().offsets().start_offset) {
            // If we hit at the start of the local log, this is ambiguous:
            // there could be earlier batches prior to start_offset which
            // have the same timestamp and are present in cloud storage.
            vlog(
              clusterlog.debug,
              "Timequery (raft) ts={} hit start_offset in local log "
              "(start_offset {} start_timestamp {}, result {})",
              _raft->log().offsets().start_offset,
              cfg.time,
              _raft->log().start_timestamp(),
              cfg.time);
            if (
              _cloud_storage_partition
              && _cloud_storage_partition->is_data_available()
              && may_answer_from_cloud) {
                // Even though we hit data with the desired timestamp, we cannot
                // be certain that this is the _first_ batch with the desired
                // timestamp: return null so that the caller will fall back
                // to cloud storage.
                co_return std::nullopt;
            }
        }

        vlog(
          clusterlog.debug,
          "timequery (raft) {} t={} max_offset(r)={} result(r)={}",
          _raft->ntp(),
          cfg.time,
          cfg.max_offset,
          result->offset);
        result->offset = _raft->get_offset_translator_state()->from_log_offset(
          result->offset);
    }

    co_return result;
}

ss::future<> partition::update_configuration(topic_properties properties) {
    co_await _raft->log().update_configuration(
      properties.get_ntp_cfg_overrides());
}

std::optional<model::offset>
partition::get_term_last_offset(model::term_id term) const {
    auto o = _raft->log().get_term_last_offset(term);
    if (!o) {
        return std::nullopt;
    }
    // Kafka defines leader epoch last offset as a first offset of next
    // leader epoch
    return model::next_offset(*o);
}

std::optional<model::offset>
partition::get_cloud_term_last_offset(model::term_id term) const {
    auto o = _cloud_storage_partition->get_term_last_offset(term);
    if (!o) {
        return std::nullopt;
    }
    // Kafka defines leader epoch last offset as a first offset of next
    // leader epoch
    return model::next_offset(kafka::offset_cast(*o));
}

ss::future<> partition::remove_persistent_state() {
    if (_rm_stm) {
        co_await _rm_stm->remove_persistent_state();
    }
    if (_tm_stm) {
        co_await _tm_stm->remove_persistent_state();
    }
    if (_archival_meta_stm) {
        co_await _archival_meta_stm->remove_persistent_state();
    }
    if (_id_allocator_stm) {
        co_await _id_allocator_stm->remove_persistent_state();
    }
}

ss::future<> partition::remove_remote_persistent_state(ss::abort_source& as) {
    // Backward compatibility: even if remote.delete is true, only do
    // deletion if the partition is in full tiered storage mode (this
    // excludes read replica clusters from deleting data in S3)
    bool tiered_storage = get_ntp_config().is_tiered_storage();

    if (
      _cloud_storage_partition && tiered_storage
      && get_ntp_config().remote_delete()) {
        vlog(
          clusterlog.debug,
          "Erasing S3 objects for partition {} ({} {} {})",
          ntp(),
          get_ntp_config(),
          get_ntp_config().is_archival_enabled(),
          get_ntp_config().is_read_replica_mode_enabled());
        co_await _cloud_storage_partition->erase(as);
    } else if (_cloud_storage_partition && tiered_storage) {
        vlog(
          clusterlog.info,
          "Leaving tiered storage objects behind for partition {}",
          ntp());
    }
}

ss::future<> partition::unsafe_reset_remote_partition_manifest(iobuf buf) {
    vlog(clusterlog.info, "[{}] Unsafe manifest reset requested", ntp());

    if (!(config::shard_local_cfg().cloud_storage_enabled()
          && _archival_meta_stm)) {
        vlog(
          clusterlog.warn,
          "[{}] Archival STM not present. Skipping unsafe reset ...",
          ntp());
        throw std::runtime_error("Archival STM not present");
    }

    // Deserialise provided manifest
    cloud_storage::partition_manifest req_m{
      _raft->ntp(), _raft->log_config().get_initial_revision()};
    co_await req_m.update(std::move(buf));

    // A generous timeout of 60 seconds is used as it applies
    // for the replication multiple batches.
    auto deadline = ss::lowres_clock::now() + 60s;
    std::vector<cluster::command_batch_builder> builders;

    auto reset_builder = _archival_meta_stm->batch_start(deadline, _as);

    // Add command to drop manifest. When applied, the current manifest
    // will be replaced with a default constructed one.
    reset_builder.reset_metadata();
    builders.push_back(std::move(reset_builder));

    // Add segments. Note that we only add, and omit the replaced segments.
    // This should only be done in the context of a last-ditch effort to
    // restore functionality to a partition. Replaced segments are likely
    // unimportant.
    constexpr size_t segments_per_batch = 256;
    std::vector<cloud_storage::segment_meta> segments;
    segments.reserve(segments_per_batch);

    for (const auto& [_, s] : req_m) {
        segments.emplace_back(s);
        if (segments.size() == segments_per_batch) {
            auto segments_builder = _archival_meta_stm->batch_start(
              deadline, _as);
            segments_builder.add_segments(std::move(segments));
            builders.push_back(std::move(segments_builder));

            segments.clear();
        }
    }

    if (segments.size() > 0) {
        auto segments_builder = _archival_meta_stm->batch_start(deadline, _as);
        segments_builder.add_segments(std::move(segments));
        builders.push_back(std::move(segments_builder));
    }

    size_t idx = 0;
    for (auto& builder : builders) {
        vlog(
          clusterlog.info,
          "Unsafe reset replicating batch {}/{}",
          idx + 1,
          builders.size());

        auto errc = co_await builder.replicate();
        if (errc) {
            if (errc == raft::errc::shutting_down) {
                // During shutdown, act like we hit an abort source rather
                // than trying to log+handle this like a write error.
                throw ss::abort_requested_exception();
            }

            vlog(
              clusterlog.warn,
              "[{}] Unsafe reset failed to update archival STM: {}",
              ntp(),
              errc.message());
            throw std::runtime_error(
              fmt::format("Failed to update archival STM: {}", errc.message()));
        }

        ++idx;
    }

    vlog(
      clusterlog.info,
      "[{}] Unsafe reset replicated STM commands successfully",
      ntp());
}

std::ostream& operator<<(std::ostream& o, const partition& x) {
    return o << x._raft;
}
} // namespace cluster
