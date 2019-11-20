#ifndef hrd_hash_set_h_
#define hrd_hash_set_h_

#include <functional>
#include <stdexcept>
#include <cstdint>
#include <string.h> //memcpy

//version 1.2.6

#ifdef _MSC_VER
#  include <pmmintrin.h>
#  define HRD_ALWAYS_INLINE __forceinline
#  define HRD_LIKELY(condition) condition
#  define HRD_UNLIKELY(condition) condition
#else
#  include <x86intrin.h>
#  define HRD_ALWAYS_INLINE __attribute__((always_inline)) inline
#  define HRD_LIKELY(condition) __builtin_expect(condition, 1)
#  define HRD_UNLIKELY(condition) __builtin_expect(condition, 0)
#endif

namespace hrd {

class hash_base
{
public:
    typedef size_t size_type;

    template<typename T>
    struct hash_ : public std::hash<T> {
        HRD_ALWAYS_INLINE size_t operator()(const T& val) const noexcept {
            return hash_base::hash_1<sizeof(T)>(&val);
        }
    };

    template<size_t SIZE>
    static uint32_t hash(const void* ptr) noexcept {
        return hash_1<SIZE>(ptr);
    }

    size_type size() const noexcept { return _size; }
    size_type capacity() const noexcept { return _capacity; }

    static constexpr size_type max_size() noexcept {
        return (size_type(1) << (sizeof(size_type) * 8 - 2)) - 1;
    }

    bool empty() const noexcept { return !_size; }

protected:
    //2 bits used as data-marker
    enum { ACTIVE_MARK = 0x2, USED_MARK = 0x1 | ACTIVE_MARK, DELETED_MARK = (USED_MARK & ~ACTIVE_MARK) };

#pragma pack(push, 4)
    template<class T>
    struct StorageItem
    {
        StorageItem(StorageItem&& r) : data(std::move(r.data)) { mark = r.mark; /*set mark after data-ctor call, if any exception happens nothing break old mark value - new-in-place*/ }
        StorageItem(const StorageItem& r) : data(r.data) { mark = r.mark; }

        uint32_t mark;
        T data;
    };
#pragma pack(pop)

    constexpr static const uint32_t OFFSET_BASIS = 2166136261;

    template<size_t SIZE>
    static uint32_t hash_1(const void* ptr) noexcept {
        return hash_base::fnv_1a((const char*)ptr, SIZE);
    }

    constexpr static HRD_ALWAYS_INLINE uint32_t fnv_1a(const char* key, size_t len, uint32_t hash32 = OFFSET_BASIS) noexcept
    {
        constexpr const uint32_t PRIME = 1607;

        for (size_t cnt = len / sizeof(uint32_t); cnt--; key += sizeof(uint32_t))
            hash32 = (hash32 ^ (*(uint32_t*)key)) * PRIME;

        if (len & sizeof(uint16_t)) {
            hash32 = (hash32 ^ (*(uint16_t*)key)) * PRIME;
            key += sizeof(uint16_t);
        }
        if (len & 1)
            hash32 = (hash32 ^ (*key)) * PRIME;

        return hash32 ^ (hash32 >> 16);
    }

    HRD_ALWAYS_INLINE static uint32_t make_mark(size_t h) noexcept {
        return static_cast<uint32_t>(h | USED_MARK);
    }

    //space must be allocated before
    template<class storage_type>
    HRD_ALWAYS_INLINE void insert_unique(const storage_type& st, std::true_type /*trivial data*/)
    {
        _size++;
        for (size_t i = st.mark;; ++i)
        {
            i &= _capacity;
            auto& r = reinterpret_cast<storage_type*>(_elements)[i];
            if (!r.mark) {
                memcpy(&r, &st, sizeof(st));
                return;
            }
        }
    }

    //space must be allocated before
    template<typename V>
    HRD_ALWAYS_INLINE void insert_unique(V&& st, std::false_type /*non-trivial data*/)
    {
        typedef typename std::remove_reference<V>::type storage_type;

        for (size_t i = st.mark;; ++i)
        {
            i &= _capacity;
            auto& r = reinterpret_cast<storage_type*>(_elements)[i];
            if (!r.mark) {
                new ((void*)&r) storage_type(std::forward<V>(st));
                _size++;
                return;
            }
        }
    }

    template<typename this_type>
    void resize_pow2(size_t pow2, std::true_type /*trivial data*/)
    {
        typename this_type::storage_type* elements = (typename this_type::storage_type*)calloc(pow2--, sizeof(typename this_type::storage_type));
        if (!elements)
            throw_bad_alloc();

        if (size_t cnt = _size)
        {
            for (typename this_type::storage_type* p = reinterpret_cast<typename this_type::storage_type*>(_elements);; ++p)
            {
                if (HRD_UNLIKELY(p->mark >= ACTIVE_MARK))
                {
                    for (size_t i = p->mark;; ++i)
                    {
                        i &= pow2;
                        auto& r = elements[i];
                        if (HRD_LIKELY(!r.mark)) {
                            memcpy(&r, p, sizeof(typename this_type::storage_type));
                            break;
                        }
                    }
                    if (!--cnt)
                        break;
                }
            }
        }

        if (_capacity)
            free(_elements);
        _capacity = pow2;
        _elements = elements;
        _erased = 0;
    }

    template<typename this_type>
    void resize_pow2(size_t pow2, std::false_type /*non-trivial data*/)
    {
        this_type tmp(pow2, false);
        if (HRD_LIKELY(_size)) //rehash
        {
            typedef typename this_type::storage_type StorageType;
            for (StorageType* p = reinterpret_cast<StorageType*>(_elements);; ++p)
            {
                if (p->mark >= ACTIVE_MARK) {
                    typedef typename this_type::value_type VT;

                    VT& r = p->data;
                    tmp.insert_unique(std::move(*p), std::false_type());
                    r.~VT();

                    //next 2 lines to cover any exception that occurs during next tmp.insert_unique(std::move(r));
                    p->mark = DELETED_MARK;
                    _erased++;

                    if (!--_size)
                        break;
                }
            }
        }
        static_assert(sizeof(*this) == sizeof(hash_base), "hash_base::swap should be used");
        swap(tmp); //swap base members only
    }

    template<typename this_type>
    HRD_ALWAYS_INLINE void resize_pow2(size_t pow2) {
        resize_pow2<this_type>(pow2, typename this_type::IS_TRIVIALLY_COPYABLE());
    }

    HRD_ALWAYS_INLINE static size_t roundup(size_t sz) noexcept
    {
#ifdef _MSC_VER
        unsigned long idx;
#  if defined(_WIN64) || defined(__LP64__)
        _BitScanReverse64(&idx, sz - 1);
#  else
        _BitScanReverse(&idx, sz - 1);
#  endif
#else
#  ifdef __LP64__
        int idx = __bsrq(sz - 1);
#  else
        int idx = __bsrd(sz - 1);
#  endif
#endif
        return size_t(1) << (idx + 1);
    }

#ifdef _MSC_VER
    __declspec(noreturn, noinline)
#else
    __attribute__((noinline, noreturn))
#endif
    static void throw_bad_alloc() {
        throw std::bad_alloc();
    }

#ifdef _MSC_VER
    __declspec(noreturn, noinline)
#else
    __attribute__((noinline, noreturn))
#endif
    static void throw_length_error() {
        throw std::length_error("size exceeded");
    }

    template<typename base>
    struct iterator_base
    {
        class const_iterator
        {
        public:
            typedef std::forward_iterator_tag iterator_category;
            typedef typename base::value_type value_type;
            typedef typename base::value_type* pointer;
            typedef typename base::value_type& reference;
            typedef std::ptrdiff_t difference_type;

            const_iterator() noexcept : _ptr(nullptr), _cnt(0) {}

            HRD_ALWAYS_INLINE const_iterator& operator++() noexcept
            {
                typename base::storage_type* p = _ptr;
                if (_cnt)
                {
                    _cnt--;
                    while ((++p)->mark < base::ACTIVE_MARK)
                        ;
                    _ptr = p;
                    return *this;
                }
                _ptr = nullptr;
                return *this;
            }

            HRD_ALWAYS_INLINE const_iterator operator++(int) noexcept
            {
                const_iterator ret(*this);
                ++(*this);
                return ret;
            }

            bool operator== (const const_iterator& r) const noexcept { return _ptr == r._ptr; }
            bool operator!= (const const_iterator& r) const noexcept { return _ptr != r._ptr; }

            const typename base::value_type& operator*() const noexcept { return _ptr->data; }
            const typename base::value_type* operator->() const noexcept { return &_ptr->data; }

        protected:
            friend base;
            friend hash_base;
            const_iterator(typename base::storage_type* p, typename base::size_type cnt) noexcept : _ptr(p), _cnt(cnt) {}

            typename base::storage_type* _ptr;
            typename base::size_type _cnt;
        };

        class iterator : public const_iterator
        {
        public:
            using const_iterator::iterator_category;
            using const_iterator::value_type;
            using const_iterator::pointer;
            using const_iterator::reference;
            using const_iterator::difference_type;
            using const_iterator::operator*;
            using const_iterator::operator->;

            iterator() noexcept {}

            typename base::value_type& operator*() noexcept { return const_iterator::_ptr->data; }
            typename base::value_type* operator->() noexcept { return &const_iterator::_ptr->data; }

        private:
            friend base;
            friend hash_base;
            iterator(typename base::storage_type* p, typename base::size_type cnt = 0) : const_iterator(p, cnt) {}
        };
    };

    template<typename this_type>
    HRD_ALWAYS_INLINE void ctor_copy(const this_type& r)
    {
        typedef typename this_type::storage_type StorageType;

        size_t cnt = r._size;
        if (HRD_LIKELY(cnt)) {
            ctor_pow2(r._capacity + 1, sizeof(StorageType));
            for (const StorageType* p = reinterpret_cast<StorageType*>(r._elements);; ++p)
            {
                if (p->mark >= ACTIVE_MARK) {
                    insert_unique(*p, typename this_type::IS_TRIVIALLY_COPYABLE());
                    if (!--cnt)
                        break;
                }
            }
        }
        else
            ctor_empty();
    }

    HRD_ALWAYS_INLINE void ctor_move(hash_base&& r)
    {
        memcpy(this, &r, sizeof(hash_base));
        if (HRD_LIKELY(r._capacity))
            r.ctor_empty();
        else
            _elements = &_size; //0-hash indicates empty element - use this trick to prevent redundant "is empty" check in find-function
    }

#if (__cplusplus >= 201402L || _MSC_VER > 1600 || __clang__)
    template<typename this_type>
    HRD_ALWAYS_INLINE void ctor_init_list(std::initializer_list<typename this_type::value_type> lst, this_type& ref)
    {
        ctor_pow2(roundup((lst.size() | 1) * 2), sizeof(typename this_type::storage_type));
        ctor_insert_(lst.begin(), lst.end(), ref, std::true_type());
    }
#endif

    template<typename V, class this_type>
    HRD_ALWAYS_INLINE void ctor_insert_(V&& val, this_type& ref, std::true_type /*resized*/)
    {
        const uint32_t mark = make_mark(ref._hf(this_type::key_getter::get_key(val)));
        for (size_t i = mark;; ++i)
        {
            i &= _capacity;
            typename this_type::storage_type* r = reinterpret_cast<typename this_type::storage_type*>(_elements) + i;
            auto h = r->mark;
            if (!h)
            {
                typedef typename this_type::value_type value_type;

                new ((void*)&r->data) value_type(std::forward<V>(val));
                r->mark = mark;
                _size++;
                return;
            }
            if (h == mark && HRD_LIKELY(ref._eql(this_type::key_getter::get_key(r->data), this_type::key_getter::get_key(val)))) //identical found
                return;
        }
    }

    template<typename V, class this_type>
    HRD_ALWAYS_INLINE void ctor_insert_(V&& val, this_type& ref, std::false_type /*not resized yet*/)
    {
        if (HRD_UNLIKELY((_capacity - _size) <= _size))
            resize_pow2<this_type>(2 * (_capacity + 1));

        ctor_insert_(std::forward<V>(val), ref, std::true_type());
    }

    template <typename Iter, class this_type, typename SIZE_PREPARED>
    void ctor_insert_(Iter first, Iter last, this_type& ref, SIZE_PREPARED) {
        for (; first != last; ++first)
            ctor_insert_(*first, ref, SIZE_PREPARED());
    }

    template<typename Iter, class this_type>
    HRD_ALWAYS_INLINE void ctor_iters(Iter first, Iter last, this_type& ref, std::random_access_iterator_tag)
    {
        ctor_pow2(roundup((std::distance(first, last) | 1) * 2), sizeof(typename this_type::storage_type));
        ctor_insert_(first, last, ref, std::true_type());
    }

    template<typename Iter, class this_type, typename XXX>
    HRD_ALWAYS_INLINE void ctor_iters(Iter first, Iter last, this_type& ref, XXX)
    {
        ctor_empty();
        ctor_insert_(first, last, ref, std::false_type());
    }

    HRD_ALWAYS_INLINE void ctor_empty() noexcept
    {
        _size = 0;
        _capacity = 0;
        _elements = &_size; //0-hash indicates empty element - use this trick to prevent redundant "is empty" check in find-function
        _erased = 0;
    }

    HRD_ALWAYS_INLINE void ctor_pow2(size_t pow2, size_t element_size)
    {
        _size = 0;
        _capacity = pow2 - 1;
        _elements = calloc(pow2, element_size); //pos2-- for performance in lookup-function
        _erased = 0;
        if (HRD_UNLIKELY(!_elements))
            throw_bad_alloc();
    }

    template<typename storage_type, typename data_type>
    HRD_ALWAYS_INLINE void deleteElement(storage_type* ptr) noexcept
    {
        ptr->data.~data_type();
        _size--;

        //set DELETED_MARK only if next element not 0
        const uint32_t next_mark = (HRD_LIKELY(ptr != (reinterpret_cast<storage_type*>(_elements) + _capacity)) ? ptr + 1 : reinterpret_cast<storage_type*>(_elements))->mark;
        if (HRD_LIKELY(!next_mark))
            ptr->mark = 0;
        else {
            ptr->mark = DELETED_MARK;
            _erased++;
        }
    }

    template<class this_type>
    HRD_ALWAYS_INLINE void clear(std::true_type) noexcept
    {
        if (_capacity) {
            free(_elements);
            ctor_empty();
        }
    }

    template<class this_type>
    HRD_ALWAYS_INLINE void clear(std::false_type) noexcept
    {
        if (auto cnt = _size)
        {
            typedef typename this_type::storage_type storage_type;
            typedef typename this_type::value_type data_type;

            for (storage_type* p = reinterpret_cast<storage_type*>(_elements);; ++p)
            {
                if (p->mark >= ACTIVE_MARK) {
                    cnt--;
                    p->data.~data_type();
                    if (!cnt)
                        break;
                }
            }
        }
        else if (!_capacity)
            return;

        free(_elements);
        ctor_empty();
    }

    template<typename V, class this_type>
    HRD_ALWAYS_INLINE std::pair<typename this_type::iterator, bool> insert_(V&& val, this_type& ref)
    {
        size_t used = _erased + _size;
        if (HRD_UNLIKELY(_capacity - used <= used))
            resize_pow2<this_type>(2 * (_capacity + 1));

        typename this_type::storage_type* empty_spot = nullptr;
        uint32_t deleted_mark = DELETED_MARK;
        const uint32_t mark = make_mark(ref._hf(this_type::key_getter::get_key(val)));

        for (size_t i = mark;; ++i)
        {
            i &= _capacity;
            typename this_type::storage_type* r = reinterpret_cast<typename this_type::storage_type*>(_elements) + i;
            auto h = r->mark;
            if (!h)
            {
                if (HRD_UNLIKELY(!!empty_spot)) r = empty_spot;

                typedef typename this_type::value_type value_type;

                std::pair<typename this_type::iterator, bool> ret(r, true);
                new ((void*)&r->data) value_type(std::forward<V>(val));
                r->mark = mark;
                _size++;
                if (HRD_UNLIKELY(!!empty_spot)) _erased--;
                return ret;
            }
            if (h == mark)
            {
                if (HRD_LIKELY(ref._eql(this_type::key_getter::get_key(r->data), this_type::key_getter::get_key(val)))) //identical found
                    return std::pair<typename this_type::iterator, bool>(r, false);
            }
            else if (h == deleted_mark)
            {
                deleted_mark = 0; //prevent additional empty_spot == null_ptr comparison
                empty_spot = r;
            }
        }
    }

    template<typename key_type, class this_type>
    HRD_ALWAYS_INLINE typename this_type::storage_type* find_(const key_type& k, this_type& ref) const noexcept
    {
        const uint32_t mark = make_mark(ref._hf(k));
        for (size_t i = mark;; ++i)
        {
            i &= _capacity;
            auto& r = reinterpret_cast<typename this_type::storage_type*>(_elements)[i];
            auto h = r.mark;
            if (HRD_LIKELY(h == mark))
            {
                if (HRD_LIKELY(ref._eql(this_type::key_getter::get_key(r.data), k))) //identical found
                    return &r;
            }
            else if (!h)
                return nullptr;
        }
    }

    template<class this_type>
    void shrink_to_fit()
    {
        if (HRD_LIKELY(_size)) {
            size_t pow2 = roundup(_size * 2);
            if (HRD_LIKELY(_erased || (_capacity + 1) != pow2))
                resize_pow2<this_type>(pow2, typename this_type::IS_TRIVIALLY_COPYABLE());
        }
        else {
            clear<this_type>(std::true_type());
        }
    }

    HRD_ALWAYS_INLINE void swap(hash_base& r)
    {
        __m128i mm0 = _mm_loadu_si128((__m128i*)this);
        __m128i r_mm0 = _mm_loadu_si128((__m128i*) & r);

#if defined(_WIN64) || defined(__LP64__)
        static_assert(sizeof(r) == 32, "must be sizeof(hash_base)==32");

        __m128i mm1 = _mm_loadu_si128((__m128i*)this + 1);
        __m128i r_mm1 = _mm_loadu_si128((__m128i*) & r + 1);

        _mm_storeu_si128((__m128i*)this, r_mm0);
        _mm_storeu_si128((__m128i*)this + 1, r_mm1);
        _mm_storeu_si128((__m128i*) & r, mm0);
        _mm_storeu_si128((__m128i*) & r + 1, mm1);
#else
        static_assert(sizeof(r) == 16, "must be sizeof(hash_base)==16");

        _mm_storeu_si128((__m128i*)this, r_mm0);
        _mm_storeu_si128((__m128i*) & r, mm0);
#endif

        if (!_capacity)
            _elements = &_size;
        if (!r._capacity)
            r._elements = &r._size;
    }

#ifdef _MSC_VER
    HRD_ALWAYS_INLINE static uint64_t umul128(uint64_t a, uint64_t b) noexcept {
        uint64_t l = _umul128(a, b, &a);
        return l + a;
    }
#else
    HRD_ALWAYS_INLINE static uint64_t umul128(uint64_t a, uint64_t b) noexcept {
        typedef unsigned __int128 uint128_t;

        auto result = static_cast<uint128_t>(a) * static_cast<uint128_t>(b);
        return static_cast<uint64_t>(result) + static_cast<uint64_t>(result >> 64U);
    }
#endif

    size_type _size;
    size_type _capacity;
    void* _elements;
    size_type _erased;
};

template<>
HRD_ALWAYS_INLINE uint32_t hash_base::hash_1<1>(const void* ptr) noexcept {
    uint32_t hash32 = (OFFSET_BASIS ^ (*(uint8_t*)ptr)) * 1607;
    return hash32 ^ (hash32 >> 16);
}

template<>
HRD_ALWAYS_INLINE uint32_t hash_base::hash_1<2>(const void* ptr) noexcept {
    uint32_t hash32 = (OFFSET_BASIS ^ (*(uint16_t*)ptr)) * 1607;
    return hash32 ^ (hash32 >> 16);
}

template<>
HRD_ALWAYS_INLINE uint32_t hash_base::hash_1<4>(const void* ptr) noexcept {
    return static_cast<uint32_t>(umul128(*(uint32_t*)ptr, 0xde5fb9d2630458e9ull));
    /*
        uint32_t hash32 = (OFFSET_BASIS ^ (*(uint32_t*)ptr)) * 1607;
        return hash32 ^ (hash32 >> 16);
    */
}

template<>
HRD_ALWAYS_INLINE uint32_t hash_base::hash_1<8>(const void* ptr) noexcept {
    return static_cast<uint32_t>(umul128(*(uint64_t*)ptr, 0xde5fb9d2630458e9ull));
    /*
        uint32_t* key = (uint32_t*)ptr;
        uint32_t hash32 = (((OFFSET_BASIS ^ key[0]) * 1607) ^ key[1]) * 1607;
        return hash32 ^ (hash32 >> 16);
    */
}

template<>
HRD_ALWAYS_INLINE uint32_t hash_base::hash_1<12>(const void* ptr) noexcept
{
    const uint32_t* key = reinterpret_cast<const uint32_t*>(ptr);

    const uint32_t PRIME = 1607;

    uint32_t hash32 = (OFFSET_BASIS ^ key[0]) * PRIME;
    hash32 = (hash32 ^ key[1]) * PRIME;
    hash32 = (hash32 ^ key[2]) * PRIME;

    return hash32 ^ (hash32 >> 16);
}

template<>
HRD_ALWAYS_INLINE uint32_t hash_base::hash_1<16>(const void* ptr) noexcept
{
    const uint32_t* key = reinterpret_cast<const uint32_t*>(ptr);

    const uint32_t PRIME = 1607;

    uint32_t hash32 = (OFFSET_BASIS ^ key[0]) * PRIME;
    hash32 = (hash32 ^ key[1]) * PRIME;
    hash32 = (hash32 ^ key[2]) * PRIME;
    hash32 = (hash32 ^ key[3]) * PRIME;

    return hash32 ^ (hash32 >> 16);
}

template<>
struct hash_base::hash_<std::string> {
    HRD_ALWAYS_INLINE size_t operator()(const std::string& val) const noexcept {
        return hash_base::fnv_1a(val.c_str(), val.size());
    }
};

//----------------------------------------- hash_set -----------------------------------------

template<class Key, class Hash = hash_base::hash_<Key>, class Pred = std::equal_to<Key>>
class hash_set : public hash_base
{
public:
    typedef hash_set<Key, Hash, Pred>   this_type;
    typedef Key                         key_type;
    typedef Hash                        hasher;
    typedef Pred                        key_equal;
    typedef const key_type              value_type;
    typedef value_type& reference;
    typedef const value_type& const_reference;

private:
    friend iterator_base<this_type>;
    friend hash_base;
    typedef StorageItem<key_type> storage_type;
#if (__cplusplus >= 201402L || _MSC_VER > 1600 || __clang__)
    typedef std::is_trivially_copyable<key_type> IS_TRIVIALLY_COPYABLE;
#else
    typedef std::is_pod<key_type> IS_TRIVIALLY_COPYABLE;
#endif
    struct key_getter {
        HRD_ALWAYS_INLINE static const key_type& get_key(const value_type& r) noexcept {
            return r;
        }
    };

public:
    typedef typename iterator_base<this_type>::iterator iterator;
    typedef typename iterator_base<this_type>::const_iterator const_iterator;

    hash_set() {
        ctor_empty();
    }

    hash_set(const this_type& r) :
        _hf(r._hf),
        _eql(r._eql)
    {
        ctor_copy(r);
    }

    hash_set(this_type&& r) noexcept :
        _hf(std::move(r._hf)),
        _eql(std::move(r._eql))
    {
        ctor_move(std::move(r));
    }

    hash_set(size_type hint_size, const hasher& hf = hasher(), const key_equal& eql = key_equal()) :
        _hf(hf),
        _eql(eql)
    {
        // |1 to prevent 0-usage (produces _capacity = 0 finally)
        ctor_pow2(roundup((hint_size | 1) * 2), sizeof(storage_type));
    }

    template<typename Iter>
    hash_set(Iter first, Iter last, const hasher& hf = hasher(), const key_equal& eql = key_equal()) :
        _hf(hf),
        _eql(eql)
    {
        ctor_iters(first, last, *this, Iter::iterator_category());
    }

#if (__cplusplus >= 201402L || _MSC_VER > 1600 || __clang__)
    hash_set(std::initializer_list<value_type> lst, const hasher& hf = hasher(), const key_equal& eql = key_equal()) :
        _hf(hf),
        _eql(eql)
    {
        ctor_init_list(lst, *this);
    }
#endif

    ~hash_set() {
        clear();
    }

    iterator begin() noexcept
    {
        auto pm = reinterpret_cast<storage_type*>(_elements);

        if (auto cnt = _size) {
            --cnt;
            for (;; ++pm) {
                if (HRD_UNLIKELY(pm->mark >= ACTIVE_MARK))
                    return iterator(pm, cnt);
            }
        }
        return iterator();
    }

    const_iterator begin() const noexcept {
        return cbegin();
    }

    const_iterator cbegin() const noexcept {
        return const_cast<this_type*>(this)->begin();
    }

    iterator end() noexcept {
        return iterator();
    }

    const_iterator end() const noexcept {
        return cend();
    }

    const_iterator cend() const noexcept {
        return const_iterator();
    }

    HRD_ALWAYS_INLINE void reserve(size_type hint)
    {
        hint *= 2;
        if (HRD_LIKELY(hint > _capacity))
            resize_pow2<this_type>(roundup(hint));
    }

    HRD_ALWAYS_INLINE void clear() noexcept {
        hash_base::clear<this_type>(IS_TRIVIALLY_COPYABLE());
    }

    void swap(hash_set& r) noexcept
    {
        hash_base::swap(r);
        std::swap(_hf, r._hf);
        std::swap(_eql, r._eql);
    }

    /*! Can invalidate iterators. */
    HRD_ALWAYS_INLINE std::pair<iterator, bool> insert(const key_type& val) {
        return insert_(val, const_cast<this_type&>(*this));
    }

    /*! Can invalidate iterators. */
    template<class P>
    HRD_ALWAYS_INLINE std::pair<iterator, bool> insert(P&& val) {
        return insert_(std::forward<P>(val), const_cast<this_type&>(*this));
    }

#if (__cplusplus >= 201402L || _MSC_VER > 1600 || __clang__)
    /*! Can invalidate iterators. */
    void insert(std::initializer_list<value_type> lst)
    {
        for (auto i = lst.begin(), e = lst.end(); i != e; ++i)
            insert_(*i, *this);
    }
#endif

    /*! Can invalidate iterators. */
    template<class K>
    HRD_ALWAYS_INLINE std::pair<iterator, bool> emplace(K&& val) {
        return insert_(std::forward<K>(val), const_cast<this_type&>(*this));
    }

    HRD_ALWAYS_INLINE iterator find(const key_type& k) noexcept {
        return iterator(find_(k, *this), 0);
    }

    HRD_ALWAYS_INLINE const_iterator find(const key_type& k) const noexcept {
        return const_iterator(find_(k, *this), 0);
    }

    HRD_ALWAYS_INLINE size_type count(const key_type& k) const noexcept {
        return find_(k, *this) != nullptr;
    }

    /*! Can invalidate iterators.
    * \params it - Iterator pointing to a single element to be removed
    * \return an iterator pointing to the position immediately following of the element erased
    */
    inline iterator erase(const_iterator it) noexcept
    {
        auto ptr = it._ptr;
        if (HRD_LIKELY(!!ptr)) //valid
        {
            auto cnt = it._cnt;
            deleteElement<storage_type, value_type>(it._ptr);

            if (HRD_UNLIKELY(cnt--)) {
                for (;;) {
                    if (HRD_UNLIKELY((++ptr)->mark >= ACTIVE_MARK))
                        return iterator(ptr, cnt);
                }
            }
        }
        return iterator();
    }

    /*! Can invalidate iterators.
    * \params k - Key of the element to be erased
    * \return 1 - if element erased and zero otherwise
    */
    inline size_type erase(const key_type& k) noexcept
    {
        auto ptr = find_(k, *this);
        if (HRD_LIKELY(!!ptr)) {
            deleteElement<storage_type, value_type>(ptr);
            return 1;
        }
        return 0;
    }

    HRD_ALWAYS_INLINE void shrink_to_fit() {
        hash_base::shrink_to_fit<this_type>();
    }

    HRD_ALWAYS_INLINE hash_set& operator=(const hash_set& r) {
        this_type(r).swap(*this);
        return *this;
    }

    HRD_ALWAYS_INLINE hash_set& operator=(hash_set&& r) noexcept {
        swap(r);
        return *this;
    }

private:
    hash_set(size_type pow2, bool) {
        ctor_pow2(pow2, sizeof(storage_type));
    }

    hasher _hf;
    key_equal _eql;
};

//----------------------------------------- hash_map -----------------------------------------

template<class Key, class T, class Hash = hash_base::hash_<Key>, class Pred = std::equal_to<Key>>
class hash_map : public hash_base
{
public:
    typedef hash_map<Key, T, Hash, Pred>            this_type;
    typedef Key                                     key_type;
    typedef T                                       mapped_type;
    typedef Hash                                    hasher;
    typedef Pred                                    key_equal;
    typedef std::pair<const key_type, mapped_type>  value_type;
    typedef value_type& reference;
    typedef const value_type& const_reference;

private:
    friend iterator_base<this_type>;
    friend hash_base;
    typedef StorageItem<value_type> storage_type;
#if (__cplusplus >= 201402L || _MSC_VER > 1600 || __clang__)
    typedef std::integral_constant<bool, std::is_trivially_copyable<key_type>::value && std::is_trivially_copyable<mapped_type>::value> IS_TRIVIALLY_COPYABLE;
#else
    typedef std::integral_constant<bool, std::is_pod<key_type>::value && std::is_pod<mapped_type>::value> IS_TRIVIALLY_COPYABLE;
#endif

    struct key_getter {
        HRD_ALWAYS_INLINE static const key_type& get_key(const value_type& r) noexcept {
            return r.first;
        }
    };

public:
    typedef typename iterator_base<this_type>::iterator iterator;
    typedef typename iterator_base<this_type>::const_iterator const_iterator;

    hash_map() {
        ctor_empty();
    }

    hash_map(const this_type& r) :
        _hf(r._hf),
        _eql(r._eql)
    {
        ctor_copy(r);
    }

    hash_map(this_type&& r) noexcept :
        _hf(std::move(r._hf)),
        _eql(std::move(r._eql))
    {
        ctor_move(std::move(r));
    }

    hash_map(size_type hint_size, const hasher& hf = hasher(), const key_equal& eql = key_equal()) :
        _hf(hf),
        _eql(eql)
    {
        // |1 to prevent 0-usage (produces _capacity = 0 finally)
        ctor_pow2(roundup((hint_size | 1) * 2), sizeof(storage_type));
    }

    template<typename Iter>
    hash_map(Iter first, Iter last, const hasher& hf = hasher(), const key_equal& eql = key_equal()) :
        _hf(hf),
        _eql(eql)
    {
        ctor_iters(first, last, *this, Iter::iterator_category());
    }

#if (__cplusplus >= 201402L || _MSC_VER > 1600 || __clang__)
    hash_map(std::initializer_list<value_type> lst, const hasher& hf = hasher(), const key_equal& eql = key_equal()) :
        _hf(hf),
        _eql(eql)
    {
        ctor_init_list(lst, *this);
    }
#endif

    ~hash_map() {
        clear();
    }

    iterator begin() noexcept
    {
        auto pm = reinterpret_cast<storage_type*>(_elements);
        auto cnt = _size;
        if (HRD_LIKELY(cnt)) {
            --cnt;
            for (;; ++pm) {
                if (HRD_UNLIKELY(pm->mark >= ACTIVE_MARK))
                    return iterator(pm, cnt);
            }
        }
        return iterator();
    }

    const_iterator begin() const noexcept {
        return cbegin();
    }

    const_iterator cbegin() const noexcept {
        return const_cast<this_type*>(this)->begin();
    }

    iterator end() noexcept {
        return iterator();
    }

    const_iterator end() const noexcept {
        return cend();
    }

    const_iterator cend() const noexcept {
        return const_iterator();
    }

    HRD_ALWAYS_INLINE void reserve(size_type hint) {
        hint *= 2;
        if (HRD_LIKELY(hint > _capacity))
            resize_pow2<this_type>(roundup(hint));
    }

    void clear() noexcept {
        hash_base::clear<this_type>(IS_TRIVIALLY_COPYABLE());
    }

    void swap(this_type& r) noexcept
    {
        hash_base::swap(r);
        std::swap(_hf, r._hf);
        std::swap(_eql, r._eql);
    }

    /*! Can invalidate iterators. */
    HRD_ALWAYS_INLINE std::pair<iterator, bool> insert(const value_type& val) {
        return insert_(val, const_cast<this_type&>(*this));
    }

    /*! Can invalidate iterators. */
    template <class P>
    HRD_ALWAYS_INLINE std::pair<iterator, bool> insert(P&& val) {
        return insert_(std::forward<P>(val), const_cast<this_type&>(*this));
    }

#if (__cplusplus >= 201402L || _MSC_VER > 1600 || __clang__)
    /*! Can invalidate iterators. */
    void insert(std::initializer_list<value_type> lst)
    {
        for (auto i = lst.begin(), e = lst.end(); i != e; ++i)
            insert_(*i, *this);
    }

    /*! Can invalidate iterators. */
    template<class... Args>
    HRD_ALWAYS_INLINE std::pair<iterator, bool> emplace(const Key& key, Args&&... args) {
        return emplace_(key, std::forward<Args>(args)...);
    }

    /*! Can invalidate iterators. */
    template<class K, class... Args>
    HRD_ALWAYS_INLINE std::pair<iterator, bool> emplace(K&& key, Args&&... args) {
        return emplace_(std::forward<K>(key), std::forward<Args>(args)...);
    }
#else
    /*! Can invalidate iterators. */
    template<class Args>
    HRD_ALWAYS_INLINE std::pair<iterator, bool> emplace(const Key& key, Args&& args) {
        return emplace_(key, std::forward<Args>(args));
    }

    /*! Can invalidate iterators. */
    template<class K, class Args>
    HRD_ALWAYS_INLINE std::pair<iterator, bool> emplace(K&& key, Args&& args) {
        return emplace_(std::forward<K>(key), std::forward<Args>(args));
    }
#endif //C++-11 support

    HRD_ALWAYS_INLINE iterator find(const key_type& k) noexcept {
        return iterator(find_(k, *this), 0);
    }

    HRD_ALWAYS_INLINE const_iterator find(const key_type& k) const noexcept {
        return const_iterator(find_(k, *this), 0);
    }

    HRD_ALWAYS_INLINE size_type count(const key_type& k) const noexcept {
        return find_(k, *this) != nullptr;
    }

    /*! Can invalidate iterators.
    * \params it - Iterator pointing to a single element to be removed
    * \return return an iterator pointing to the position immediately following of the element erased
    */
    inline iterator erase(const_iterator it) noexcept
    {
        if (auto ptr = it._ptr) //valid
        {
            auto cnt = it._cnt;
            deleteElement<storage_type, value_type>(it._ptr);

            if (HRD_UNLIKELY(cnt--)) {
                for (;;) {
                    if (HRD_UNLIKELY((++ptr)->mark >= ACTIVE_MARK))
                        return iterator(ptr, cnt);
                }
            }
        }
        return iterator();
    }

    /*! Can invalidate iterators.
    * \params k - Key of the element to be erased
    * \return 1 - if element erased and zero otherwise
    */
    inline size_type erase(const key_type& k) noexcept
    {
        auto ptr = find_(k, *this);
        if (HRD_LIKELY(!!ptr)) {
            deleteElement<storage_type, value_type>(ptr);
            return 1;
        }
        return 0;
    }

    HRD_ALWAYS_INLINE void shrink_to_fit() {
        hash_base::shrink_to_fit<this_type>();
    }

    HRD_ALWAYS_INLINE hash_map& operator=(const hash_map& r) {
        this_type(r).swap(*this);
        return *this;
    }

    HRD_ALWAYS_INLINE hash_map& operator=(hash_map&& r) noexcept {
        swap(r);
        return *this;
    }

    HRD_ALWAYS_INLINE mapped_type& operator[](const key_type& k) {
        return find_insert(k);
    }

    HRD_ALWAYS_INLINE mapped_type& operator[](key_type&& k) {
        return find_insert(std::move(k));
    }

private:
    hash_map(size_type pow2, bool) {
        ctor_pow2(pow2, sizeof(storage_type));
    }

#if (__cplusplus >= 201402L || _MSC_VER > 1600 || __clang__)
    template<typename K, typename... Args>
    HRD_ALWAYS_INLINE std::pair<iterator, bool> emplace_(K&& k, Args&&... args)
#else
    template<typename K, typename Args>
    HRD_ALWAYS_INLINE std::pair<iterator, bool> emplace_(K&& k, Args&& args)
#endif //C++-11 support
    {
        size_t used = _erased + _size;
        if (HRD_UNLIKELY(_capacity - used <= used))
            resize_pow2<this_type>(2 * (_capacity + 1));

        storage_type* empty_spot = nullptr;
        uint32_t deleted_mark = DELETED_MARK;
        const uint32_t mark = make_mark(_hf(k));

        for (size_t i = mark;; ++i)
        {
            i &= _capacity;
            storage_type* r = reinterpret_cast<storage_type*>(_elements) + i;
            auto h = r->mark;
            if (!h)
            {
                if (HRD_UNLIKELY(!!empty_spot)) r = empty_spot;

                std::pair<iterator, bool> ret(r, true);
#if (__cplusplus >= 201402L || _MSC_VER > 1600 || __clang__)
                new ((void*)&r->data) value_type(std::piecewise_construct, std::forward_as_tuple(std::forward<K>(k)), std::forward_as_tuple(std::forward<Args>(args)...));
#else
                new ((void*)&r->data) value_type(std::forward<K>(k), std::forward<Args>(args));
#endif //C++-11 support
                r->mark = mark;
                _size++;
                if (HRD_UNLIKELY(!!empty_spot)) _erased--;
                return ret;
            }
            if (h == mark)
            {
                if (HRD_LIKELY(_eql(r->data.first, k))) //identical found
                    return std::pair<iterator, bool>(r, false);
            }
            else if (h == deleted_mark)
            {
                deleted_mark = 0; //prevent additional empty_spot == null_ptr comparison
                empty_spot = r;
            }
        }
    }

    template<typename V>
    HRD_ALWAYS_INLINE mapped_type& find_insert(V&& k)
    {
        size_t used = _erased + _size;
        if (HRD_UNLIKELY(_capacity - used <= used))
            resize_pow2<this_type>(2 * (_capacity + 1));

        storage_type* empty_spot = nullptr;
        uint32_t deleted_mark = DELETED_MARK;
        const uint32_t mark = make_mark(_hf(k));

        for (size_t i = mark;; ++i)
        {
            i &= _capacity;
            storage_type* r = reinterpret_cast<storage_type*>(_elements) + i;
            auto h = r->mark;
            if (!h)
            {
                if (HRD_UNLIKELY(!!empty_spot)) r = empty_spot;

                new ((void*)&r->data) value_type(std::forward<V>(k), mapped_type());
                r->mark = mark;
                _size++;
                if (HRD_UNLIKELY(!!empty_spot)) _erased--;
                return r->data.second;
            }
            if (h == mark)
            {
                if (HRD_LIKELY(_eql(r->data.first, k))) //identical found
                    return r->data.second;
            }
            else if (h == deleted_mark)
            {
                deleted_mark = 0; //prevent additional empty_spot == null_ptr comparison
                empty_spot = r;
            }
        }
    }

    hasher _hf;
    key_equal _eql;
};

} //namespace hrd

#endif //hrd_hash_set_h_
