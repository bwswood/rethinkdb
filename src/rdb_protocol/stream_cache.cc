// Copyright 2010-2013 RethinkDB, all rights reserved.
#include "rdb_protocol/stream_cache.hpp"

#include "rdb_protocol/env.hpp"

namespace ql {

bool stream_cache2_t::contains(int64_t key) {
    return streams.find(key) != streams.end();
}

void stream_cache2_t::insert(int64_t key,
                             scoped_ptr_t<env_t> &&val_env,
                             counted_t<datum_stream_t> val_stream) {
    maybe_evict();
    std::pair<boost::ptr_map<int64_t, entry_t>::iterator, bool> res =
        streams.insert(key, new entry_t(time(0), std::move(val_env), val_stream));
    guarantee(res.second);
}

void stream_cache2_t::erase(int64_t key) {
    size_t num_erased = streams.erase(key);
    guarantee(num_erased == 1);
}

bool stream_cache2_t::serve(int64_t key, Response *res, signal_t *interruptor) {
    boost::ptr_map<int64_t, entry_t>::iterator it = streams.find(key);
    if (it == streams.end()) return false;
    entry_t *entry = it->second;
    entry->last_activity = time(0);
    bool should_erase = false;
    try {
        // Reset the env_t's interruptor to a good one before we use it.  This may be a
        // hack.  (I'd rather not have env_t be mutable this way -- could we construct
        // a new env_t instead?  Why do we keep env_t's around anymore?)
        entry->env->interruptor = interruptor;

        res->set_type(Response::SUCCESS_PARTIAL);
        {
            profile::sampler_t sampler("Evaluating stream elements.", entry->env->trace);
            for (int chunk_size = 0;
                 chunk_size < entry->max_chunk_size || entry->max_chunk_size == 0;
                 ++chunk_size) {
                counted_t<const datum_t> next_datum = entry->stream->next(entry->env.get());

                if (next_datum.has()) {
                    next_datum->write_to_protobuf(res->add_response());
                } else {
                    should_erase = true;
                    res->set_type(Response::SUCCESS_SEQUENCE);
                    break;
                }
                sampler.new_sample();
            }
        }
    } catch (const std::exception &e) {
        erase(key);
        throw;
    }
    
    if (should_erase) {
        erase(key);
    }
    return true;
}

void stream_cache2_t::maybe_evict() {
    // We never evict right now.
}

stream_cache2_t::entry_t::entry_t(time_t _last_activity, scoped_ptr_t<env_t> &&env_ptr,
                                  counted_t<datum_stream_t> _stream)
    : last_activity(_last_activity), env(std::move(env_ptr)), stream(_stream),
      max_chunk_size(DEFAULT_MAX_CHUNK_SIZE), max_age(DEFAULT_MAX_AGE) { }

stream_cache2_t::entry_t::~entry_t() { }


} // namespace ql
