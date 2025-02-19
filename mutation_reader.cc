/*
 * Copyright (C) 2015 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <boost/range/algorithm/heap_algorithm.hpp>
#include <boost/range/algorithm/reverse.hpp>
#include <boost/move/iterator.hpp>
#include <variant>

#include "mutation_reader.hh"
#include <seastar/core/future-util.hh>
#include "flat_mutation_reader.hh"
#include "schema_registry.hh"


static constexpr size_t merger_small_vector_size = 4;

template<typename T>
using merger_vector = utils::small_vector<T, merger_small_vector_size>;

GCC6_CONCEPT(
    template<typename Producer>
    concept bool FragmentProducer = requires(Producer p, dht::partition_range part_range, position_range pos_range,
            db::timeout_clock::time_point timeout) {
        // The returned fragments are expected to have the same
        // position_in_partition. Iterators and references are expected
        // to be valid until the next call to operator()().
        { p(timeout) } -> future<boost::iterator_range<merger_vector<mutation_fragment>::iterator>>;
        // These have the same semantics as their
        // flat_mutation_reader counterparts.
        { p.next_partition() };
        { p.fast_forward_to(part_range, timeout) } -> future<>;
        { p.fast_forward_to(pos_range, timeout) } -> future<>;
        { p.buffer_size() } -> size_t;
    };
)

/**
 * Merge mutation-fragments produced by producer.
 *
 * Merge a non-decreasing stream of mutation-fragments into strictly
 * increasing stream. The merger is stateful, it's intended to be kept
 * around *at least* for merging an entire partition. That is, creating
 * a new instance for each batch of fragments will produce incorrect
 * results.
 *
 * Call operator() to get the next mutation fragment. operator() will
 * consume fragments from the producer using operator().
 * Any fast-forwarding has to be communicated to the merger object using
 * fast_forward_to() and next_partition(), as appropriate.
 */
template<class Producer>
GCC6_CONCEPT(
    requires FragmentProducer<Producer>
)
class mutation_fragment_merger {
    using iterator = merger_vector<mutation_fragment>::iterator;

    const schema_ptr _schema;
    Producer _producer;
    iterator _it{};
    iterator _end{};

    future<> fetch(db::timeout_clock::time_point timeout) {
        if (!empty()) {
            return make_ready_future<>();
        }

        return _producer(timeout).then([this] (boost::iterator_range<iterator> fragments) {
            _it = fragments.begin();
            _end = fragments.end();
        });
    }

    bool empty() const {
        return _it == _end;
    }

    const mutation_fragment& top() const {
        return *_it;
    }

    mutation_fragment pop() {
        return std::move(*_it++);
    }

public:
    mutation_fragment_merger(schema_ptr schema, Producer&& producer)
        : _schema(std::move(schema))
        , _producer(std::move(producer)) {
    }

    future<mutation_fragment_opt> operator()(db::timeout_clock::time_point timeout) {
        return fetch(timeout).then([this] () -> mutation_fragment_opt {
            if (empty()) {
                return mutation_fragment_opt();
            }
            auto current = pop();
            while (!empty() && current.mergeable_with(top())) {
                current.apply(*_schema, pop());
            }
            return current;
        });
    }

    void next_partition() {
        _producer.next_partition();
    }

    future<> fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout) {
        return _producer.fast_forward_to(pr, timeout);
    }

    future<> fast_forward_to(position_range pr, db::timeout_clock::time_point timeout) {
        return _producer.fast_forward_to(std::move(pr), timeout);
    }

    size_t buffer_size() const {
        return _producer.buffer_size();
    }
};

// Merges the output of the sub-readers into a single non-decreasing
// stream of mutation-fragments.
class mutation_reader_merger {
public:
    using reader_iterator = std::list<flat_mutation_reader>::iterator;

    struct reader_and_fragment {
        reader_iterator reader{};
        mutation_fragment fragment;

        reader_and_fragment(reader_iterator r, mutation_fragment f)
            : reader(r)
            , fragment(std::move(f)) {
        }
    };

    struct reader_and_last_fragment_kind {
        reader_iterator reader{};
        mutation_fragment::kind last_kind = mutation_fragment::kind::partition_end;

        reader_and_last_fragment_kind() = default;

        reader_and_last_fragment_kind(reader_iterator r, mutation_fragment::kind k)
            : reader(r)
            , last_kind(k) {
        }
    };

    using mutation_fragment_batch = boost::iterator_range<merger_vector<mutation_fragment>::iterator>;
private:
    struct reader_heap_compare;
    struct fragment_heap_compare;

    std::unique_ptr<reader_selector> _selector;
    // We need a list because we need stable addresses across additions
    // and removals.
    std::list<flat_mutation_reader> _all_readers;
    // We remove unneeded readers in batches. Until it is their time they
    // are kept in _to_remove.
    std::list<flat_mutation_reader> _to_remove;
    // Readers positioned at a partition, different from the one we are
    // reading from now. For these readers the attached fragment is
    // always partition_start. Used to pick the next partition.
    merger_vector<reader_and_fragment> _reader_heap;
    // Readers and their current fragments, belonging to the current
    // partition.
    merger_vector<reader_and_fragment> _fragment_heap;
    merger_vector<reader_and_last_fragment_kind> _next;
    // Readers that reached EOS.
    merger_vector<reader_and_last_fragment_kind> _halted_readers;
    merger_vector<mutation_fragment> _current;
    // Optimisation for cases where only a single reader emits a particular
    // partition. If _single_reader.reader is not null that reader is
    // guaranteed to be the only one having relevant data until the partition
    // end, a call to next_partition() or a call to
    // fast_forward_to(dht::partition_range).
    reader_and_last_fragment_kind _single_reader;
    const schema_ptr _schema;
    streamed_mutation::forwarding _fwd_sm;
    mutation_reader::forwarding _fwd_mr;
private:
    void maybe_add_readers(const std::optional<dht::ring_position_view>& pos);
    void add_readers(std::vector<flat_mutation_reader> new_readers);
    future<> prepare_next(db::timeout_clock::time_point timeout);
    // Collect all forwardable readers into _next, and remove them from
    // their previous containers (_halted_readers and _fragment_heap).
    void prepare_forwardable_readers();
public:
    mutation_reader_merger(schema_ptr schema,
            std::unique_ptr<reader_selector> selector,
            streamed_mutation::forwarding fwd_sm,
            mutation_reader::forwarding fwd_mr);
    // Produces the next batch of mutation-fragments of the same
    // position.
    future<mutation_fragment_batch> operator()(db::timeout_clock::time_point timeout);
    void next_partition();
    future<> fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout);
    future<> fast_forward_to(position_range pr, db::timeout_clock::time_point timeout);
    size_t buffer_size() const;
};

// Combines multiple mutation_readers into one.
class combined_mutation_reader : public flat_mutation_reader::impl {
    mutation_fragment_merger<mutation_reader_merger> _producer;
    streamed_mutation::forwarding _fwd_sm;
public:
    // The specified streamed_mutation::forwarding and
    // mutation_reader::forwarding tag must be the same for all included
    // readers.
    combined_mutation_reader(schema_ptr schema,
            std::unique_ptr<reader_selector> selector,
            streamed_mutation::forwarding fwd_sm,
            mutation_reader::forwarding fwd_mr);
    virtual future<> fill_buffer(db::timeout_clock::time_point timeout) override;
    virtual void next_partition() override;
    virtual future<> fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout) override;
    virtual future<> fast_forward_to(position_range pr, db::timeout_clock::time_point timeout) override;
    virtual size_t buffer_size() const override;
};

// Dumb selector implementation for combined_mutation_reader that simply
// forwards it's list of readers.
class list_reader_selector : public reader_selector {
    std::vector<flat_mutation_reader> _readers;

public:
    explicit list_reader_selector(schema_ptr s, std::vector<flat_mutation_reader> readers)
        : reader_selector(s, dht::ring_position_view::min())
        , _readers(std::move(readers)) {
    }

    list_reader_selector(const list_reader_selector&) = delete;
    list_reader_selector& operator=(const list_reader_selector&) = delete;

    list_reader_selector(list_reader_selector&&) = default;
    list_reader_selector& operator=(list_reader_selector&&) = default;

    virtual std::vector<flat_mutation_reader> create_new_readers(const std::optional<dht::ring_position_view>&) override {
        _selector_position = dht::ring_position_view::max();
        return std::exchange(_readers, {});
    }

    virtual std::vector<flat_mutation_reader> fast_forward_to(const dht::partition_range&, db::timeout_clock::time_point timeout) override {
        return {};
    }
};

void mutation_reader_merger::maybe_add_readers(const std::optional<dht::ring_position_view>& pos) {
    if (_selector->has_new_readers(pos)) {
        add_readers(_selector->create_new_readers(pos));
    }
}

void mutation_reader_merger::add_readers(std::vector<flat_mutation_reader> new_readers) {
    for (auto&& new_reader : new_readers) {
        _all_readers.emplace_back(std::move(new_reader));
        _next.emplace_back(std::prev(_all_readers.end()), mutation_fragment::kind::partition_end);
    }
}

struct mutation_reader_merger::reader_heap_compare {
    const schema& s;

    explicit reader_heap_compare(const schema& s)
        : s(s) {
    }

    bool operator()(const mutation_reader_merger::reader_and_fragment& a, const mutation_reader_merger::reader_and_fragment& b) {
        // Invert comparison as this is a max-heap.
        return b.fragment.as_partition_start().key().less_compare(s, a.fragment.as_partition_start().key());
    }
};

struct mutation_reader_merger::fragment_heap_compare {
    position_in_partition::less_compare cmp;

    explicit fragment_heap_compare(const schema& s)
        : cmp(s) {
    }

    bool operator()(const mutation_reader_merger::reader_and_fragment& a, const mutation_reader_merger::reader_and_fragment& b) {
        // Invert comparison as this is a max-heap.
        return cmp(b.fragment.position(), a.fragment.position());
    }
};

future<> mutation_reader_merger::prepare_next(db::timeout_clock::time_point timeout) {
    return parallel_for_each(_next, [this, timeout] (reader_and_last_fragment_kind rk) {
        return (*rk.reader)(timeout).then([this, rk] (mutation_fragment_opt mfo) {
            if (mfo) {
                if (mfo->is_partition_start()) {
                    _reader_heap.emplace_back(rk.reader, std::move(*mfo));
                    boost::push_heap(_reader_heap, reader_heap_compare(*_schema));
                } else {
                    _fragment_heap.emplace_back(rk.reader, std::move(*mfo));
                    boost::range::push_heap(_fragment_heap, fragment_heap_compare(*_schema));
                }
            } else if (_fwd_sm == streamed_mutation::forwarding::yes && rk.last_kind != mutation_fragment::kind::partition_end) {
                // When in streamed_mutation::forwarding mode we need
                // to keep track of readers that returned
                // end-of-stream to know what readers to ff. We can't
                // just ff all readers as we might drop fragments from
                // partitions we haven't even read yet.
                // Readers whoose last emitted fragment was a partition
                // end are out of data for good for the current range.
                _halted_readers.push_back(rk);
            } else if (_fwd_mr == mutation_reader::forwarding::no) {
                _to_remove.splice(_to_remove.end(), _all_readers, rk.reader);
                if (_to_remove.size() >= 4) {
                    _to_remove.clear();
                }
            }
        });
    }).then([this] {
        _next.clear();

        // We are either crossing partition boundary or ran out of
        // readers. If there are halted readers then we are just
        // waiting for a fast-forward so there is nothing to do.
        if (_fragment_heap.empty() && _halted_readers.empty()) {
            if (_reader_heap.empty()) {
                maybe_add_readers(std::nullopt);
            } else {
                maybe_add_readers(_reader_heap.front().fragment.as_partition_start().key());
            }
        }
    });
}

void mutation_reader_merger::prepare_forwardable_readers() {
    _next.reserve(_halted_readers.size() + _fragment_heap.size() + _next.size());

    std::move(_halted_readers.begin(), _halted_readers.end(), std::back_inserter(_next));
    if (_single_reader.reader != reader_iterator{}) {
        _next.emplace_back(std::exchange(_single_reader.reader, {}), _single_reader.last_kind);
    }
    for (auto& df : _fragment_heap) {
        _next.emplace_back(df.reader, df.fragment.mutation_fragment_kind());
    }

    _halted_readers.clear();
    _fragment_heap.clear();
}

mutation_reader_merger::mutation_reader_merger(schema_ptr schema,
        std::unique_ptr<reader_selector> selector,
        streamed_mutation::forwarding fwd_sm,
        mutation_reader::forwarding fwd_mr)
    : _selector(std::move(selector))
    , _schema(std::move(schema))
    , _fwd_sm(fwd_sm)
    , _fwd_mr(fwd_mr) {
    maybe_add_readers(std::nullopt);
}

future<mutation_reader_merger::mutation_fragment_batch> mutation_reader_merger::operator()(db::timeout_clock::time_point timeout) {
    // Avoid merging-related logic if we know that only a single reader owns
    // current partition.
    if (_single_reader.reader != reader_iterator{}) {
        if (_single_reader.reader->is_buffer_empty()) {
            if (_single_reader.reader->is_end_of_stream()) {
                _current.clear();
                return make_ready_future<mutation_fragment_batch>(_current);
            }
            return _single_reader.reader->fill_buffer(timeout).then([this, timeout] { return operator()(timeout); });
        }
        _current.clear();
        _current.emplace_back(_single_reader.reader->pop_mutation_fragment());
        _single_reader.last_kind = _current.back().mutation_fragment_kind();
        if (_current.back().is_end_of_partition()) {
            _next.emplace_back(std::exchange(_single_reader.reader, {}), mutation_fragment::kind::partition_end);
        }
        return make_ready_future<mutation_fragment_batch>(_current);
    }

    if (!_next.empty()) {
        return prepare_next(timeout).then([this, timeout] { return (*this)(timeout); });
    }

    _current.clear();

    // If we ran out of fragments for the current partition, select the
    // readers for the next one.
    if (_fragment_heap.empty()) {
        if (!_halted_readers.empty() || _reader_heap.empty()) {
            return make_ready_future<mutation_fragment_batch>(_current);
        }

        auto key = [] (const merger_vector<reader_and_fragment>& heap) -> const dht::decorated_key& {
            return heap.front().fragment.as_partition_start().key();
        };

        do {
            boost::range::pop_heap(_reader_heap, reader_heap_compare(*_schema));
            // All fragments here are partition_start so no need to
            // heap-sort them.
            _fragment_heap.emplace_back(std::move(_reader_heap.back()));
            _reader_heap.pop_back();
        }
        while (!_reader_heap.empty() && key(_fragment_heap).equal(*_schema, key(_reader_heap)));
        if (_fragment_heap.size() == 1) {
            _single_reader = { _fragment_heap.back().reader, mutation_fragment::kind::partition_start };
            _current.emplace_back(std::move(_fragment_heap.back().fragment));
            _fragment_heap.clear();
            return make_ready_future<mutation_fragment_batch>(_current);
        }
    }

    const auto equal = position_in_partition::equal_compare(*_schema);
    do {
        boost::range::pop_heap(_fragment_heap, fragment_heap_compare(*_schema));
        auto& n = _fragment_heap.back();
        const auto kind = n.fragment.mutation_fragment_kind();
        _current.emplace_back(std::move(n.fragment));
        _next.emplace_back(n.reader, kind);
        _fragment_heap.pop_back();
    }
    while (!_fragment_heap.empty() && equal(_current.back().position(), _fragment_heap.front().fragment.position()));

    return make_ready_future<mutation_fragment_batch>(_current);
}

void mutation_reader_merger::next_partition() {
    prepare_forwardable_readers();
    for (auto& rk : _next) {
        rk.last_kind = mutation_fragment::kind::partition_end;
        rk.reader->next_partition();
    }
}

future<> mutation_reader_merger::fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout) {
    _single_reader = { };
    _next.clear();
    _halted_readers.clear();
    _fragment_heap.clear();
    _reader_heap.clear();

    for (auto it = _all_readers.begin(); it != _all_readers.end(); ++it) {
        _next.emplace_back(it, mutation_fragment::kind::partition_end);
    }
    return parallel_for_each(_all_readers, [this, &pr, timeout] (flat_mutation_reader& mr) {
        return mr.fast_forward_to(pr, timeout);
    }).then([this, &pr, timeout] {
        add_readers(_selector->fast_forward_to(pr, timeout));
    });
}

future<> mutation_reader_merger::fast_forward_to(position_range pr, db::timeout_clock::time_point timeout) {
    prepare_forwardable_readers();
    return parallel_for_each(_next, [this, pr = std::move(pr), timeout] (reader_and_last_fragment_kind rk) {
        return rk.reader->fast_forward_to(pr, timeout);
    });
}

size_t mutation_reader_merger::buffer_size() const {
    return boost::accumulate(_all_readers | boost::adaptors::transformed(std::mem_fn(&flat_mutation_reader::buffer_size)), size_t(0));
}

combined_mutation_reader::combined_mutation_reader(schema_ptr schema,
        std::unique_ptr<reader_selector> selector,
        streamed_mutation::forwarding fwd_sm,
        mutation_reader::forwarding fwd_mr)
    : impl(std::move(schema))
    , _producer(_schema, mutation_reader_merger(_schema, std::move(selector), fwd_sm, fwd_mr))
    , _fwd_sm(fwd_sm) {
}

future<> combined_mutation_reader::fill_buffer(db::timeout_clock::time_point timeout) {
    return repeat([this, timeout] {
        return _producer(timeout).then([this] (mutation_fragment_opt mfo) {
            if (!mfo) {
                _end_of_stream = true;
                return stop_iteration::yes;
            }
            push_mutation_fragment(std::move(*mfo));
            if (is_buffer_full()) {
                return stop_iteration::yes;
            }
            return stop_iteration::no;
        });
    });
}

void combined_mutation_reader::next_partition() {
    if (_fwd_sm == streamed_mutation::forwarding::yes) {
        clear_buffer();
        _end_of_stream = false;
        _producer.next_partition();
    } else {
        clear_buffer_to_next_partition();
        // If the buffer is empty at this point then all fragments in it
        // belonged to the current partition, so either:
        // * All (forwardable) readers are still positioned in the
        // inside of the current partition, or
        // * They are between the current one and the next one.
        // Either way we need to call next_partition on them.
        if (is_buffer_empty()) {
            _producer.next_partition();
        }
    }
}

future<> combined_mutation_reader::fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout) {
    clear_buffer();
    _end_of_stream = false;
    return _producer.fast_forward_to(pr, timeout);
}

future<> combined_mutation_reader::fast_forward_to(position_range pr, db::timeout_clock::time_point timeout) {
    forward_buffer_to(pr.start());
    _end_of_stream = false;
    return _producer.fast_forward_to(std::move(pr), timeout);
}

size_t combined_mutation_reader::buffer_size() const {
    return flat_mutation_reader::impl::buffer_size() + _producer.buffer_size();
}

flat_mutation_reader make_combined_reader(schema_ptr schema,
        std::unique_ptr<reader_selector> selectors,
        streamed_mutation::forwarding fwd_sm,
        mutation_reader::forwarding fwd_mr) {
    return make_flat_mutation_reader<combined_mutation_reader>(schema,
            std::move(selectors),
            fwd_sm,
            fwd_mr);
}

flat_mutation_reader make_combined_reader(schema_ptr schema,
        std::vector<flat_mutation_reader> readers,
        streamed_mutation::forwarding fwd_sm,
        mutation_reader::forwarding fwd_mr) {
    if (readers.size() == 1) {
        return std::move(readers.front());
    }
    return make_flat_mutation_reader<combined_mutation_reader>(schema,
            std::make_unique<list_reader_selector>(schema, std::move(readers)),
            fwd_sm,
            fwd_mr);
}

flat_mutation_reader make_combined_reader(schema_ptr schema,
        flat_mutation_reader&& a,
        flat_mutation_reader&& b,
        streamed_mutation::forwarding fwd_sm,
        mutation_reader::forwarding fwd_mr) {
    std::vector<flat_mutation_reader> v;
    v.reserve(2);
    v.push_back(std::move(a));
    v.push_back(std::move(b));
    return make_combined_reader(std::move(schema), std::move(v), fwd_sm, fwd_mr);
}

class restricting_mutation_reader : public flat_mutation_reader::impl {
    struct mutation_source_and_params {
        mutation_source _ms;
        schema_ptr _s;
        std::reference_wrapper<const dht::partition_range> _range;
        std::reference_wrapper<const query::partition_slice> _slice;
        std::reference_wrapper<const io_priority_class> _pc;
        tracing::trace_state_ptr _trace_state;
        streamed_mutation::forwarding _fwd;
        mutation_reader::forwarding _fwd_mr;

        flat_mutation_reader operator()(reader_resource_tracker tracker) {
            return _ms.make_reader(std::move(_s), _range.get(), _slice.get(), _pc.get(), std::move(_trace_state), _fwd, _fwd_mr, tracker);
        }
    };

    struct pending_state {
        reader_concurrency_semaphore& semaphore;
        mutation_source_and_params reader_factory;
    };
    struct admitted_state {
        lw_shared_ptr<reader_concurrency_semaphore::reader_permit> permit;
        flat_mutation_reader reader;
    };
    std::variant<pending_state, admitted_state> _state;

    static const ssize_t new_reader_base_cost{16 * 1024};

    template<typename Function>
    GCC6_CONCEPT(
        requires std::is_move_constructible<Function>::value
            && requires(Function fn, flat_mutation_reader& reader) {
                fn(reader);
            }
    )
    decltype(auto) with_reader(Function fn, db::timeout_clock::time_point timeout) {
        if (auto* state = std::get_if<admitted_state>(&_state)) {
            return fn(state->reader);
        }

        return std::get<pending_state>(_state).semaphore.wait_admission(new_reader_base_cost,
                timeout).then([this, fn = std::move(fn)] (lw_shared_ptr<reader_concurrency_semaphore::reader_permit> permit) mutable {
            auto reader_factory = std::move(std::get<pending_state>(_state).reader_factory);
            _state.emplace<admitted_state>(admitted_state{permit, reader_factory(reader_resource_tracker(permit))});
            return fn(std::get<admitted_state>(_state).reader);
        });
    }
public:
    restricting_mutation_reader(reader_concurrency_semaphore& semaphore,
            mutation_source ms,
            schema_ptr s,
            const dht::partition_range& range,
            const query::partition_slice& slice,
            const io_priority_class& pc,
            tracing::trace_state_ptr trace_state,
            streamed_mutation::forwarding fwd,
            mutation_reader::forwarding fwd_mr)
        : impl(s)
        , _state(pending_state{semaphore,
                mutation_source_and_params{std::move(ms), std::move(s), range, slice, pc, std::move(trace_state), fwd, fwd_mr}}) {
    }

    virtual future<> fill_buffer(db::timeout_clock::time_point timeout) override {
        return with_reader([this, timeout] (flat_mutation_reader& reader) {
            return reader.fill_buffer(timeout).then([this, &reader] {
                _end_of_stream = reader.is_end_of_stream();
                reader.move_buffer_content_to(*this);
            });
        }, timeout);
    }
    virtual void next_partition() override {
        clear_buffer_to_next_partition();
        if (!is_buffer_empty()) {
            return;
        }
        _end_of_stream = false;
        if (auto* state = std::get_if<admitted_state>(&_state)) {
            return state->reader.next_partition();
        }
    }
    virtual future<> fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout) override {
        clear_buffer();
        _end_of_stream = false;
        return with_reader([&pr, timeout] (flat_mutation_reader& reader) {
            return reader.fast_forward_to(pr, timeout);
        }, timeout);
    }
    virtual future<> fast_forward_to(position_range pr, db::timeout_clock::time_point timeout) override {
        forward_buffer_to(pr.start());
        _end_of_stream = false;
        return with_reader([pr = std::move(pr), timeout] (flat_mutation_reader& reader) mutable {
            return reader.fast_forward_to(std::move(pr), timeout);
        }, timeout);
    }
    virtual size_t buffer_size() const override {
        if (auto* state = std::get_if<admitted_state>(&_state)) {
            return state->reader.buffer_size();
        }
        return 0;
    }
};

flat_mutation_reader
make_restricted_flat_reader(reader_concurrency_semaphore& semaphore,
                       mutation_source ms,
                       schema_ptr s,
                       const dht::partition_range& range,
                       const query::partition_slice& slice,
                       const io_priority_class& pc,
                       tracing::trace_state_ptr trace_state,
                       streamed_mutation::forwarding fwd,
                       mutation_reader::forwarding fwd_mr) {
    return make_flat_mutation_reader<restricting_mutation_reader>(semaphore, std::move(ms), std::move(s), range, slice, pc, std::move(trace_state), fwd, fwd_mr);
}


snapshot_source make_empty_snapshot_source() {
    return snapshot_source([] {
        return make_empty_mutation_source();
    });
}

mutation_source make_empty_mutation_source() {
    return mutation_source([](schema_ptr s,
            const dht::partition_range& pr,
            const query::partition_slice& slice,
            const io_priority_class& pc,
            tracing::trace_state_ptr tr,
            streamed_mutation::forwarding fwd,
            mutation_reader::forwarding,
            reader_resource_tracker) {
        return make_empty_flat_reader(s);
    }, [] {
        return [] (const dht::decorated_key& key) {
            return partition_presence_checker_result::definitely_doesnt_exist;
        };
    });
}

mutation_source make_combined_mutation_source(std::vector<mutation_source> addends) {
    return mutation_source([addends = std::move(addends)] (schema_ptr s,
            const dht::partition_range& pr,
            const query::partition_slice& slice,
            const io_priority_class& pc,
            tracing::trace_state_ptr tr,
            streamed_mutation::forwarding fwd) {
        std::vector<flat_mutation_reader> rd;
        rd.reserve(addends.size());
        for (auto&& ms : addends) {
            rd.emplace_back(ms.make_reader(s, pr, slice, pc, tr, fwd));
        }
        return make_combined_reader(s, std::move(rd), fwd);
    });
}

/// See make_foreign_reader() for description.
class foreign_reader : public flat_mutation_reader::impl {
    template <typename T>
    using foreign_unique_ptr = foreign_ptr<std::unique_ptr<T>>;

    using fragment_buffer = circular_buffer<mutation_fragment>;

    foreign_unique_ptr<flat_mutation_reader> _reader;
    foreign_unique_ptr<future<>> _read_ahead_future;
    // Set this flag when next_partition() is called.
    // This pending call will be executed the next time we go to the remote
    // reader (a fill_buffer() or a fast_forward_to() call).
    bool _pending_next_partition = false;
    streamed_mutation::forwarding _fwd_sm;

    // Forward an operation to the reader on the remote shard.
    // If the remote reader has an ongoing read-ahead, bring it to the
    // foreground (wait on it) and execute the operation after.
    // After the operation completes, kick off a new read-ahead (fill_buffer())
    // and move it to the background (save it's future but don't wait on it
    // now). If all works well read-aheads complete by the next operation and
    // we don't have to wait on the remote reader filling its buffer.
    template <typename Operation, typename Result = futurize_t<std::result_of_t<Operation()>>>
    Result forward_operation(db::timeout_clock::time_point timeout, Operation op) {
        return smp::submit_to(_reader.get_owner_shard(), [reader = _reader.get(),
                read_ahead_future = std::exchange(_read_ahead_future, nullptr),
                pending_next_partition = std::exchange(_pending_next_partition, false),
                timeout,
                op = std::move(op)] () mutable {
            auto exec_op_and_read_ahead = [=] () mutable {
                if (pending_next_partition) {
                    reader->next_partition();
                }
                // Not really variadic, we expect 0 (void) or 1 parameter.
                return op().then([=] (auto... result) {
                    auto f = reader->is_end_of_stream() ? nullptr : std::make_unique<future<>>(reader->fill_buffer(timeout));
                    return make_ready_future<std::tuple<foreign_unique_ptr<future<>>, decltype(result)...>>(
                                std::tuple(make_foreign(std::move(f)), std::move(result)...));
                });
            };
            if (read_ahead_future) {
                return read_ahead_future->then(std::move(exec_op_and_read_ahead));
            } else {
                return exec_op_and_read_ahead();
            }
        }).then([this] (auto fut_and_result) {
            _read_ahead_future = std::get<0>(std::move(fut_and_result));
            static_assert(std::tuple_size<decltype(fut_and_result)>::value <= 2);
            if constexpr (std::tuple_size<decltype(fut_and_result)>::value == 1) {
                return make_ready_future<>();
            } else {
                auto result = std::get<1>(std::move(fut_and_result));
                return make_ready_future<decltype(result)>(std::move(result));
            }
        });
    }
public:
    foreign_reader(schema_ptr schema,
            foreign_unique_ptr<flat_mutation_reader> reader,
            streamed_mutation::forwarding fwd_sm = streamed_mutation::forwarding::no);

    ~foreign_reader();

    // this is captured.
    foreign_reader(const foreign_reader&) = delete;
    foreign_reader& operator=(const foreign_reader&) = delete;
    foreign_reader(foreign_reader&&) = delete;
    foreign_reader& operator=(foreign_reader&&) = delete;

    virtual future<> fill_buffer(db::timeout_clock::time_point timeout) override;
    virtual void next_partition() override;
    virtual future<> fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout) override;
    virtual future<> fast_forward_to(position_range pr, db::timeout_clock::time_point timeout) override;
};

foreign_reader::foreign_reader(schema_ptr schema,
        foreign_unique_ptr<flat_mutation_reader> reader,
        streamed_mutation::forwarding fwd_sm)
    : impl(std::move(schema))
    , _reader(std::move(reader))
    , _fwd_sm(fwd_sm) {
}

foreign_reader::~foreign_reader() {
    if (!_read_ahead_future && !_reader) {
        return;
    }
    smp::submit_to(_reader.get_owner_shard(), [reader = std::move(_reader), read_ahead_future = std::move(_read_ahead_future)] () mutable {
        if (read_ahead_future) {
            return read_ahead_future->finally([r = std::move(reader)] {});
        }
        return make_ready_future<>();
    });
}

future<> foreign_reader::fill_buffer(db::timeout_clock::time_point timeout) {
    if (_end_of_stream || is_buffer_full()) {
        return make_ready_future();
    }

    struct fill_buffer_result {
        foreign_unique_ptr<fragment_buffer> buffer;
        bool end_of_stream;
    };

    return forward_operation(timeout, [reader = _reader.get(), timeout] () {
        auto f = reader->is_buffer_empty() ? reader->fill_buffer(timeout) : make_ready_future<>();
        return f.then([=] {
            return make_ready_future<fill_buffer_result>(fill_buffer_result{
                    std::make_unique<fragment_buffer>(reader->detach_buffer()),
                    reader->is_end_of_stream()});
        });
    }).then([this] (fill_buffer_result res) mutable {
        _end_of_stream = res.end_of_stream;
        for (const auto& mf : *res.buffer) {
            // Need a copy since the mf is on the remote shard.
            push_mutation_fragment(mutation_fragment(*_schema, mf));
        }
    });
}

void foreign_reader::next_partition() {
    if (_fwd_sm == streamed_mutation::forwarding::yes) {
        clear_buffer();
        _end_of_stream = false;
        _pending_next_partition = true;
    } else {
        clear_buffer_to_next_partition();
        if (is_buffer_empty()) {
            _end_of_stream = false;
            _pending_next_partition = true;
        }
    }
}

future<> foreign_reader::fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout) {
    clear_buffer();
    _end_of_stream = false;
    return forward_operation(timeout, [reader = _reader.get(), &pr, timeout] () {
        return reader->fast_forward_to(pr, timeout);
    });
}

future<> foreign_reader::fast_forward_to(position_range pr, db::timeout_clock::time_point timeout) {
    forward_buffer_to(pr.start());
    _end_of_stream = false;
    return forward_operation(timeout, [reader = _reader.get(), pr = std::move(pr), timeout] () {
        return reader->fast_forward_to(std::move(pr), timeout);
    });
}

flat_mutation_reader make_foreign_reader(schema_ptr schema,
            foreign_ptr<std::unique_ptr<flat_mutation_reader>> reader,
            streamed_mutation::forwarding fwd_sm) {
    if (reader.get_owner_shard() == engine().cpu_id()) {
        return std::move(*reader);
    }
    return make_flat_mutation_reader<foreign_reader>(std::move(schema), std::move(reader), fwd_sm);
}

namespace {

// A special-purpose shard reader.
//
// Shard reader manages a reader located on a remote shard. It transparently
// supports read-ahead (background fill_buffer() calls).
// This reader is not for general use, it was designed to serve the
// multishard_combining_reader.
// Although it implements the flat_mutation_reader:impl interface it cannot be
// wrapped into a flat_mutation_reader, as it needs to be managed by a shared
// pointer.
class shard_reader : public enable_lw_shared_from_this<shard_reader>, public flat_mutation_reader::impl {
    struct fill_buffer_result {
        foreign_ptr<std::unique_ptr<const circular_buffer<mutation_fragment>>> buffer;
        bool end_of_stream = false;

        fill_buffer_result() = default;
        fill_buffer_result(circular_buffer<mutation_fragment> buffer, bool end_of_stream)
            : buffer(make_foreign(std::make_unique<const circular_buffer<mutation_fragment>>(std::move(buffer))))
            , end_of_stream(end_of_stream) {
        }
    };

    // Encapsulates all data and logic that is local to the remote shard the
    // reader lives on.
    class remote_reader {
        schema_ptr _schema;
        reader_lifecycle_policy& _lifecycle_policy;
        const dht::partition_range* _pr;
        const query::partition_slice& _ps;
        const io_priority_class& _pc;
        tracing::global_trace_state_ptr _trace_state;
        const mutation_reader::forwarding _fwd_mr;
        reader_concurrency_semaphore::inactive_read_handle _irh;
        bool _reader_created = false;
        bool _drop_partition_start = false;
        bool _drop_static_row = false;

        std::optional<dht::decorated_key> _last_pkey;
        std::optional<position_in_partition> _last_position_in_partition;
        // These are used when the reader has to be recreated (after having been
        // evicted while paused) and the range and/or slice it is recreated with
        // differs from the original ones.
        std::optional<dht::partition_range> _range_override;
        std::optional<query::partition_slice> _slice_override;

    private:
        void update_last_position(const circular_buffer<mutation_fragment>& buffer);
        void adjust_partition_slice();
        flat_mutation_reader recreate_reader();
        flat_mutation_reader resume_or_create_reader();
        future<> do_fill_buffer(flat_mutation_reader& reader, db::timeout_clock::time_point timeout);
        future<> ensure_buffer_contains_all_fragments_for_last_pos(flat_mutation_reader& reader, circular_buffer<mutation_fragment>& buffer,
                db::timeout_clock::time_point timeout);

    public:
        remote_reader(
                schema_ptr schema,
                reader_lifecycle_policy& lifecycle_policy,
                const dht::partition_range& pr,
                const query::partition_slice& ps,
                const io_priority_class& pc,
                tracing::trace_state_ptr trace_state,
                mutation_reader::forwarding fwd_mr);
        future<fill_buffer_result> fill_buffer(const dht::partition_range& pr, bool pending_next_partition, db::timeout_clock::time_point timeout);
        future<> fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout);
        reader_concurrency_semaphore::inactive_read_handle inactive_read_handle() && {
            return std::move(_irh);
        }
    };

private:
    shared_ptr<reader_lifecycle_policy> _lifecycle_policy;
    const unsigned _shard;
    const dht::partition_range* _pr;
    const query::partition_slice& _ps;
    const io_priority_class& _pc;
    tracing::global_trace_state_ptr _trace_state;
    const mutation_reader::forwarding _fwd_mr;
    bool _pending_next_partition = false;
    bool _stopped = false;
    std::optional<future<>> _read_ahead;
    foreign_ptr<std::unique_ptr<remote_reader>> _reader;

private:
    future<> do_fill_buffer(db::timeout_clock::time_point timeout);

public:
    shard_reader(
            schema_ptr schema,
            shared_ptr<reader_lifecycle_policy> lifecycle_policy,
            unsigned shard,
            const dht::partition_range& pr,
            const query::partition_slice& ps,
            const io_priority_class& pc,
            tracing::trace_state_ptr trace_state,
            mutation_reader::forwarding fwd_mr)
        : impl(std::move(schema))
        , _lifecycle_policy(std::move(lifecycle_policy))
        , _shard(shard)
        , _pr(&pr)
        , _ps(ps)
        , _pc(pc)
        , _trace_state(std::move(trace_state))
        , _fwd_mr(fwd_mr) {
    }

    shard_reader(shard_reader&&) = delete;
    shard_reader& operator=(shard_reader&&) = delete;

    shard_reader(const shard_reader&) = delete;
    shard_reader& operator=(const shard_reader&) = delete;

    void stop() noexcept;

    const mutation_fragment& peek_buffer() const {
        return buffer().front();
    }
    virtual future<> fill_buffer(db::timeout_clock::time_point timeout) override;
    virtual void next_partition() override;
    virtual future<> fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout) override;
    virtual future<> fast_forward_to(position_range, db::timeout_clock::time_point timeout) override;
    bool done() const {
        return _reader && is_buffer_empty() && is_end_of_stream();
    }
    void read_ahead(db::timeout_clock::time_point timeout);
    bool is_read_ahead_in_progress() const {
        return _read_ahead.has_value();
    }
};

void shard_reader::stop() noexcept {
    // Nothing to do if there was no reader created, nor is there a background
    // read ahead in progress which will create one.
    if (!_reader && !_read_ahead) {
        return;
    }

    _stopped = true;

    auto f = _read_ahead ? *std::exchange(_read_ahead, std::nullopt) : make_ready_future<>();

    _lifecycle_policy->destroy_reader(_shard, f.then([this] {
        return smp::submit_to(_shard, [this] {
            return make_foreign(std::make_unique<reader_concurrency_semaphore::inactive_read_handle>(std::move(*_reader).inactive_read_handle()));
        }).then([this] (foreign_ptr<std::unique_ptr<reader_concurrency_semaphore::inactive_read_handle>> irh) {
            return reader_lifecycle_policy::stopped_reader{std::move(irh), detach_buffer(), _pending_next_partition};
        });
    }).finally([zis = shared_from_this()] {}));
}

void shard_reader::remote_reader::update_last_position(const circular_buffer<mutation_fragment>& buffer) {
    if (buffer.empty()) {
        return;
    }

    auto rbegin = std::reverse_iterator(buffer.end());
    auto rend = std::reverse_iterator(buffer.begin());
    if (auto pk_it = std::find_if(rbegin, rend, std::mem_fn(&mutation_fragment::is_partition_start)); pk_it != rend) {
        _last_pkey = pk_it->as_partition_start().key();
    }

    _last_position_in_partition.emplace(buffer.back().position());
}

void shard_reader::remote_reader::adjust_partition_slice() {
    if (!_slice_override) {
        _slice_override = _ps;
    }

    auto& last_ckey = _last_position_in_partition->key();
    auto ranges = _slice_override->default_row_ranges();
    query::trim_clustering_row_ranges_to(*_schema, ranges, last_ckey);

    _slice_override->clear_ranges();
    _slice_override->set_range(*_schema, _last_pkey->key(), std::move(ranges));
}

flat_mutation_reader shard_reader::remote_reader::recreate_reader() {
    const dht::partition_range* range = _pr;
    const query::partition_slice* slice = &_ps;

    if (_last_pkey) {
        bool partition_range_is_inclusive = true;

        if (_last_position_in_partition) {
            switch (_last_position_in_partition->region()) {
            case partition_region::partition_start:
                _drop_partition_start = true;
                break;
            case partition_region::static_row:
                _drop_partition_start = true;
                _drop_static_row = true;
                break;
            case partition_region::clustered:
                _drop_partition_start = true;
                _drop_static_row = true;
                adjust_partition_slice();
                slice = &*_slice_override;
                break;
            case partition_region::partition_end:
                partition_range_is_inclusive = false;
                break;
            }
        }

        // The original range contained a single partition and we've read it
        // all. We'd have to create a reader with an empty range that would
        // immediately be at EOS. This is not possible so just create an empty
        // reader instead.
        // This should be extremely rare (who'd create a multishard reader to
        // read a single partition) but still, let's make sure we handle it
        // correctly.
        if (_pr->is_singular() && !partition_range_is_inclusive) {
            return make_empty_flat_reader(_schema);
        }

        _range_override = dht::partition_range({dht::partition_range::bound(*_last_pkey, partition_range_is_inclusive)}, _pr->end());
        range = &*_range_override;
    }

    return _lifecycle_policy.create_reader(
            _schema,
            *range,
            *slice,
            _pc,
            _trace_state,
            _fwd_mr);
}

flat_mutation_reader shard_reader::remote_reader::resume_or_create_reader() {
    if (!_reader_created) {
        auto reader = _lifecycle_policy.create_reader(_schema, *_pr, _ps, _pc, _trace_state, _fwd_mr);
        _reader_created = true;
        return reader;
    }
    if (auto reader_opt = _lifecycle_policy.try_resume(std::move(_irh))) {
        return std::move(*reader_opt);
    }
    return recreate_reader();
}

future<> shard_reader::remote_reader::do_fill_buffer(flat_mutation_reader& reader, db::timeout_clock::time_point timeout) {
    if (!_drop_partition_start && !_drop_static_row) {
        return reader.fill_buffer(timeout);
    }
    return repeat([this, &reader, timeout] {
        return reader.fill_buffer(timeout).then([this, &reader] {
            const auto eos = reader.is_end_of_stream();

            if (reader.is_buffer_empty()) {
                return stop_iteration(eos);
            }
            if (_drop_partition_start) {
                _drop_partition_start = false;
                if (reader.peek_buffer().is_partition_start()) {
                    reader.pop_mutation_fragment();
                }
            }

            if (reader.is_buffer_empty()) {
                return stop_iteration(eos);
            }
            if (_drop_static_row) {
                _drop_static_row = false;
                if (reader.peek_buffer().is_static_row()) {
                    reader.pop_mutation_fragment();
                }
            }

            return stop_iteration(reader.is_buffer_full() || eos);
        });
    });
}

future<> shard_reader::remote_reader::ensure_buffer_contains_all_fragments_for_last_pos(flat_mutation_reader& reader,
        circular_buffer<mutation_fragment>& buffer, db::timeout_clock::time_point timeout) {
    if (buffer.empty() || !buffer.back().is_range_tombstone()) {
        return make_ready_future<>();
    }

    auto stop = [this, &reader, &buffer] {
        if (reader.is_buffer_empty()) {
            return reader.is_end_of_stream();
        }
        const auto& next_pos = reader.peek_buffer().position();
        if (next_pos.region() != partition_region::clustered) {
            return true;
        }
        return !next_pos.key().equal(*_schema, buffer.back().position().key());
    };

    return do_until(stop, [this, &reader, &buffer, timeout] {
        if (reader.is_buffer_empty()) {
            return do_fill_buffer(reader, timeout);
        }
        buffer.emplace_back(reader.pop_mutation_fragment());
        return make_ready_future<>();
    });
}

shard_reader::remote_reader::remote_reader(
        schema_ptr schema,
        reader_lifecycle_policy& lifecycle_policy,
        const dht::partition_range& pr,
        const query::partition_slice& ps,
        const io_priority_class& pc,
        tracing::trace_state_ptr trace_state,
        mutation_reader::forwarding fwd_mr)
    : _schema(std::move(schema))
    , _lifecycle_policy(lifecycle_policy)
    , _pr(&pr)
    , _ps(ps)
    , _pc(pc)
    , _trace_state(std::move(trace_state))
    , _fwd_mr(fwd_mr) {
}

future<shard_reader::fill_buffer_result> shard_reader::remote_reader::fill_buffer(const dht::partition_range& pr, bool pending_next_partition,
        db::timeout_clock::time_point timeout) {
    // We could have missed a `fast_forward_to()` if the reader wasn't created yet.
    _pr = &pr;
    if (pending_next_partition) {
        _last_position_in_partition = position_in_partition(position_in_partition::end_of_partition_tag_t{});
    }
    return do_with(resume_or_create_reader(), circular_buffer<mutation_fragment>{},
            [this, pending_next_partition, timeout] (flat_mutation_reader& reader, circular_buffer<mutation_fragment>& buffer) mutable {
        if (pending_next_partition) {
            reader.next_partition();
        }

        return do_fill_buffer(reader, timeout).then([this, &reader, &buffer, timeout] {
            buffer = reader.detach_buffer();
            // When the reader is recreated (after having been evicted) we
            // recreate it such that it starts reading from *after* the last
            // seen fragment's position. If the last seen fragment is a range
            // tombstone it is *not* guaranteed that the next fragments in the
            // data stream have positions strictly greater than the range
            // tombstone's. If the reader is evicted and has to be recreated,
            // these fragments would be then skipped as the read would continue
            // after their position.
            // To avoid this ensure that the buffer contains *all* fragments for
            // the last seen position.
            return ensure_buffer_contains_all_fragments_for_last_pos(reader, buffer, timeout);
        }).then([this, &reader, &buffer] {
            const auto eos = reader.is_end_of_stream() && reader.is_buffer_empty();
            update_last_position(buffer);
            _irh = _lifecycle_policy.pause(std::move(reader));
            return fill_buffer_result(std::move(buffer), eos);
        });
    });
}

future<> shard_reader::remote_reader::fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout) {
    _pr = &pr;
    _last_pkey.reset();
    _last_position_in_partition.reset();

    if (!_reader_created || !_irh) {
        return make_ready_future<>();
    }
    if (auto reader_opt = _lifecycle_policy.try_resume(std::move(_irh))) {
        auto f = reader_opt->fast_forward_to(pr, timeout);
        return f.then([this, reader = std::move(*reader_opt)] () mutable {
            _irh = _lifecycle_policy.pause(std::move(reader));
        });
    }
    return make_ready_future<>();
}


future<> shard_reader::do_fill_buffer(db::timeout_clock::time_point timeout) {
    auto fill_buf_fut = make_ready_future<fill_buffer_result>();
    const auto pending_next_partition = std::exchange(_pending_next_partition, false);

    struct reader_and_buffer_fill_result {
        foreign_ptr<std::unique_ptr<remote_reader>> reader;
        fill_buffer_result result;
    };

    if (!_reader) {
        fill_buf_fut = smp::submit_to(_shard, [this, gs = global_schema_ptr(_schema), pending_next_partition, timeout] {
            auto rreader = make_foreign(std::make_unique<remote_reader>(gs.get(), *_lifecycle_policy, *_pr, _ps, _pc, _trace_state, _fwd_mr));
            auto f = rreader->fill_buffer(*_pr, pending_next_partition, timeout);
            return f.then([rreader = std::move(rreader)] (fill_buffer_result res) mutable {
                return make_ready_future<reader_and_buffer_fill_result>(reader_and_buffer_fill_result{std::move(rreader), std::move(res)});
            });
        }).then([this, timeout] (reader_and_buffer_fill_result res) {
            _reader = std::move(res.reader);
            return std::move(res.result);
        });
    } else {
        fill_buf_fut = smp::submit_to(_shard, [this, pending_next_partition, timeout] () mutable {
            return _reader->fill_buffer(*_pr, pending_next_partition, timeout);
        });
    }

    return fill_buf_fut.then([this, zis = shared_from_this()] (fill_buffer_result res) mutable {
        _end_of_stream = res.end_of_stream;
        for (const auto& mf : *res.buffer) {
            push_mutation_fragment(mutation_fragment(*_schema, mf));
        }
    });
}

future<> shard_reader::fill_buffer(db::timeout_clock::time_point timeout) {
    if (_read_ahead) {
        return *std::exchange(_read_ahead, std::nullopt);
    }
    if (!is_buffer_empty()) {
        return make_ready_future<>();
    }
    return do_fill_buffer(timeout);
}

void shard_reader::next_partition() {
    if (!_reader) {
        return;
    }
    clear_buffer_to_next_partition();
    _pending_next_partition = is_buffer_empty();
}

future<> shard_reader::fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout) {
    _pr = &pr;

    if (!_reader) {
        // No need to fast-forward uncreated readers, they will be passed the new
        // range when created.
        return make_ready_future<>();
    }

    _end_of_stream = false;
    clear_buffer();

    auto f = _read_ahead ? *std::exchange(_read_ahead, std::nullopt) : make_ready_future<>();
    return f.then([this, &pr, timeout] {
        return smp::submit_to(_shard, [this, &pr, timeout] {
            return _reader->fast_forward_to(pr, timeout);
        });
    });
}

future<> shard_reader::fast_forward_to(position_range, db::timeout_clock::time_point timeout) {
    return make_exception_future<>(std::bad_function_call());
}

void shard_reader::read_ahead(db::timeout_clock::time_point timeout) {
    if (_read_ahead || is_end_of_stream() || !is_buffer_empty()) {
        return;
    }

    _read_ahead.emplace(do_fill_buffer(timeout));
}

} // anonymous namespace

// See make_multishard_combining_reader() for description.
class multishard_combining_reader : public flat_mutation_reader::impl {
    struct shard_and_token {
        shard_id shard;
        dht::token token;

        bool operator<(const shard_and_token& o) const {
            // Reversed, as we want a min-heap.
            return token > o.token;
        }
    };

    const dht::i_partitioner& _partitioner;
    std::vector<lw_shared_ptr<shard_reader>> _shard_readers;
    // Contains the position of each shard with token granularity, organized
    // into a min-heap. Used to select the shard with the smallest token each
    // time a shard reader produces a new partition.
    std::vector<shard_and_token> _shard_selection_min_heap;
    unsigned _current_shard;
    bool _crossed_shards;
    unsigned _concurrency = 1;

    void on_partition_range_change(const dht::partition_range& pr);
    bool maybe_move_to_next_shard(const dht::token* const t = nullptr);
    future<> handle_empty_reader_buffer(db::timeout_clock::time_point timeout);

public:
    multishard_combining_reader(
            shared_ptr<reader_lifecycle_policy> lifecycle_policy,
            const dht::i_partitioner& partitioner,
            schema_ptr s,
            const dht::partition_range& pr,
            const query::partition_slice& ps,
            const io_priority_class& pc,
            tracing::trace_state_ptr trace_state,
            mutation_reader::forwarding fwd_mr);

    ~multishard_combining_reader();

    // this is captured.
    multishard_combining_reader(const multishard_combining_reader&) = delete;
    multishard_combining_reader& operator=(const multishard_combining_reader&) = delete;
    multishard_combining_reader(multishard_combining_reader&&) = delete;
    multishard_combining_reader& operator=(multishard_combining_reader&&) = delete;

    virtual future<> fill_buffer(db::timeout_clock::time_point timeout) override;
    virtual void next_partition() override;
    virtual future<> fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout) override;
    virtual future<> fast_forward_to(position_range pr, db::timeout_clock::time_point timeout) override;
};

void multishard_combining_reader::on_partition_range_change(const dht::partition_range& pr) {
    _shard_selection_min_heap.clear();
    _shard_selection_min_heap.reserve(_partitioner.shard_count());

    auto token = pr.start() ? pr.start()->value().token() : dht::minimum_token();
    _current_shard = _partitioner.shard_of(token);

    const auto update_and_push_token_for_shard = [this, &token] (shard_id shard) {
        token = _partitioner.token_for_next_shard(token, shard);
        _shard_selection_min_heap.push_back(shard_and_token{shard, token});
        boost::push_heap(_shard_selection_min_heap);
    };

    for (auto shard = _current_shard + 1; shard < smp::count; ++shard) {
        update_and_push_token_for_shard(shard);
    }
    for (auto shard = 0u; shard < _current_shard; ++shard) {
        update_and_push_token_for_shard(shard);
    }
}

bool multishard_combining_reader::maybe_move_to_next_shard(const dht::token* const t) {
    if (_shard_selection_min_heap.empty() || (t && *t < _shard_selection_min_heap.front().token)) {
        return false;
    }

    boost::pop_heap(_shard_selection_min_heap);
    const auto next_shard = _shard_selection_min_heap.back().shard;
    _shard_selection_min_heap.pop_back();

    if (t) {
        _shard_selection_min_heap.push_back(shard_and_token{_current_shard, *t});
        boost::push_heap(_shard_selection_min_heap);
    }

    _crossed_shards = true;
    _current_shard = next_shard;
    return true;
}

future<> multishard_combining_reader::handle_empty_reader_buffer(db::timeout_clock::time_point timeout) {
    auto& reader = *_shard_readers[_current_shard];

    if (reader.is_end_of_stream()) {
        if (_shard_selection_min_heap.empty()) {
            _end_of_stream = true;
        } else {
            maybe_move_to_next_shard();
        }
        return make_ready_future<>();
    } else if (reader.is_read_ahead_in_progress()) {
        return reader.fill_buffer(timeout);
    } else {
        // If we crossed shards and the next reader has an empty buffer we
        // double concurrency so the next time we cross shards we will have
        // more chances of hitting the reader's buffer.
        if (_crossed_shards) {
            _concurrency = std::min(_concurrency * 2, _partitioner.shard_count());

            // If concurrency > 1 we kick-off concurrency-1 read-aheads in the
            // background. They will be brought to the foreground when we move
            // to their respective shard.
            for (unsigned i = 1; i < _concurrency; ++i) {
                _shard_readers[(_current_shard + i) % _partitioner.shard_count()]->read_ahead(timeout);
            }
        }
        return reader.fill_buffer(timeout);
    }
}

multishard_combining_reader::multishard_combining_reader(
        shared_ptr<reader_lifecycle_policy> lifecycle_policy,
        const dht::i_partitioner& partitioner,
        schema_ptr s,
        const dht::partition_range& pr,
        const query::partition_slice& ps,
        const io_priority_class& pc,
        tracing::trace_state_ptr trace_state,
        mutation_reader::forwarding fwd_mr)
    : impl(std::move(s))
    , _partitioner(partitioner) {

    on_partition_range_change(pr);

    _shard_readers.reserve(_partitioner.shard_count());
    for (unsigned i = 0; i < _partitioner.shard_count(); ++i) {
        _shard_readers.emplace_back(make_lw_shared<shard_reader>(_schema, lifecycle_policy, i, pr, ps, pc, trace_state, fwd_mr));
    }
}

multishard_combining_reader::~multishard_combining_reader() {
    for (auto& sr : _shard_readers) {
        sr->stop();
    }
}

future<> multishard_combining_reader::fill_buffer(db::timeout_clock::time_point timeout) {
    _crossed_shards = false;
    return do_until([this] { return is_buffer_full() || is_end_of_stream(); }, [this, timeout] {
        auto& reader = *_shard_readers[_current_shard];

        if (reader.is_buffer_empty()) {
            return handle_empty_reader_buffer(timeout);
        }

        while (!reader.is_buffer_empty() && !is_buffer_full()) {
            if (const auto& mf = reader.peek_buffer(); mf.is_partition_start() && maybe_move_to_next_shard(&mf.as_partition_start().key().token())) {
                return make_ready_future<>();
            }
            push_mutation_fragment(reader.pop_mutation_fragment());
        }
        return make_ready_future<>();
    });
}

void multishard_combining_reader::next_partition() {
    clear_buffer_to_next_partition();
    if (is_buffer_empty()) {
        _shard_readers[_current_shard]->next_partition();
    }
}

future<> multishard_combining_reader::fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout) {
    clear_buffer();
    _end_of_stream = false;
    on_partition_range_change(pr);
    return parallel_for_each(_shard_readers, [&pr, timeout] (lw_shared_ptr<shard_reader>& sr) {
        return sr->fast_forward_to(pr, timeout);
    });
}

future<> multishard_combining_reader::fast_forward_to(position_range pr, db::timeout_clock::time_point timeout) {
    return make_exception_future<>(std::bad_function_call());
}

namespace {

class inactive_shard_read : public reader_concurrency_semaphore::inactive_read {
    flat_mutation_reader_opt _reader;
public:
    inactive_shard_read(flat_mutation_reader reader)
        : _reader(std::move(reader)) {
    }
    flat_mutation_reader reader() && {
        return std::move(*_reader);
    }
    virtual void evict() override {
        _reader = {};
    }
};

}

reader_concurrency_semaphore::inactive_read_handle
reader_lifecycle_policy::pause(reader_concurrency_semaphore& sem, flat_mutation_reader reader) {
    return sem.register_inactive_read(std::make_unique<inactive_shard_read>(std::move(reader)));
}

flat_mutation_reader_opt
reader_lifecycle_policy::try_resume(reader_concurrency_semaphore& sem, reader_concurrency_semaphore::inactive_read_handle irh) {
    auto ir_ptr = sem.unregister_inactive_read(std::move(irh));
    if (!ir_ptr) {
        return {};
    }
    auto& ir = static_cast<inactive_shard_read&>(*ir_ptr);
    return std::move(ir).reader();
}

reader_concurrency_semaphore::inactive_read_handle
reader_lifecycle_policy::pause(flat_mutation_reader reader) {
    return pause(semaphore(), std::move(reader));
}

flat_mutation_reader_opt
reader_lifecycle_policy::try_resume(reader_concurrency_semaphore::inactive_read_handle irh) {
    return try_resume(semaphore(), std::move(irh));
}

flat_mutation_reader make_multishard_combining_reader(
        shared_ptr<reader_lifecycle_policy> lifecycle_policy,
        const dht::i_partitioner& partitioner,
        schema_ptr schema,
        const dht::partition_range& pr,
        const query::partition_slice& ps,
        const io_priority_class& pc,
        tracing::trace_state_ptr trace_state,
        mutation_reader::forwarding fwd_mr) {
    return make_flat_mutation_reader<multishard_combining_reader>(std::move(lifecycle_policy), partitioner, std::move(schema), pr, ps, pc,
            std::move(trace_state), fwd_mr);
}

class queue_reader final : public flat_mutation_reader::impl {
    friend class queue_reader_handle;

private:
    queue_reader_handle* _handle = nullptr;
    std::optional<promise<>> _not_full;
    std::optional<promise<>> _full;
    std::exception_ptr _ex;

private:
    void push_and_maybe_notify(mutation_fragment&& mf) {
        push_mutation_fragment(std::move(mf));
        if (_full && is_buffer_full()) {
            _full->set_value();
            _full.reset();
        }
    }

public:
    explicit queue_reader(schema_ptr s)
        : impl(std::move(s)) {
    }
    ~queue_reader() {
        if (_handle) {
            _handle->_reader = nullptr;
        }
    }
    virtual future<> fill_buffer(db::timeout_clock::time_point) override {
        if (_ex) {
            return make_exception_future<>(_ex);
        }
        if (_end_of_stream || !is_buffer_empty()) {
            return make_ready_future<>();
        }
        if (_not_full) {
            _not_full->set_value();
            _not_full.reset();
        }
        _full.emplace();
        return _full->get_future();
    }
    virtual void next_partition() override {
        throw std::bad_function_call();
    }
    virtual future<> fast_forward_to(const dht::partition_range&, db::timeout_clock::time_point) override {
        throw std::bad_function_call();
    }
    virtual future<> fast_forward_to(position_range, db::timeout_clock::time_point) override {
        throw std::bad_function_call();
    }
    future<> push(mutation_fragment&& mf) {
        if (!is_buffer_full()) {
            push_and_maybe_notify(std::move(mf));
            return make_ready_future<>();
        }
        _not_full.emplace();
        return _not_full->get_future().then([this, mf = std::move(mf)] () mutable {
            push_and_maybe_notify(std::move(mf));
        });
    }
    void push_end_of_stream() {
        _end_of_stream = true;
        if (_full) {
            _full->set_value();
            _full.reset();
        }
    }
    void abort(std::exception_ptr ep) {
        _end_of_stream = true;
        _ex = std::move(ep);
        if (_full) {
            _full->set_exception(_ex);
            _full.reset();
        }
    }
};

void queue_reader_handle::abandon() {
    abort(std::make_exception_ptr<std::runtime_error>(std::runtime_error("Abandoned queue_reader_handle")));
}

queue_reader_handle::queue_reader_handle(queue_reader& reader) : _reader(&reader) {
    _reader->_handle = this;
}

queue_reader_handle::queue_reader_handle(queue_reader_handle&& o) : _reader(std::exchange(o._reader, nullptr)) {
    if (_reader) {
        _reader->_handle = this;
    }
}

queue_reader_handle::~queue_reader_handle() {
    abandon();
}

queue_reader_handle& queue_reader_handle::operator=(queue_reader_handle&& o) {
    abandon();
    _reader = std::exchange(o._reader, nullptr);
    _ex = std::exchange(o._ex, {});
    if (_reader) {
        _reader->_handle = this;
    }
    return *this;
}

future<> queue_reader_handle::push(mutation_fragment mf) {
    if (!_reader) {
        if (_ex) {
            return make_exception_future<>(_ex);
        }
        return make_exception_future<>(std::runtime_error("Dangling queue_reader_handle"));
    }
    return _reader->push(std::move(mf));
}

void queue_reader_handle::push_end_of_stream() {
    if (!_reader) {
        throw std::runtime_error("Dangling queue_reader_handle");
    }
    _reader->push_end_of_stream();
    _reader->_handle = nullptr;
    _reader = nullptr;
}

void queue_reader_handle::abort(std::exception_ptr ep) {
    _ex = std::move(ep);
    if (_reader) {
        _reader->abort(_ex);
        _reader->_handle = nullptr;
        _reader = nullptr;
    }
}

std::pair<flat_mutation_reader, queue_reader_handle> make_queue_reader(schema_ptr s) {
    auto impl = std::make_unique<queue_reader>(std::move(s));
    auto handle = queue_reader_handle(*impl);
    return {flat_mutation_reader(std::move(impl)), std::move(handle)};
}
