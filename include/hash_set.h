#ifndef hordi_hash_set_h_
#define hordi_hash_set_h_

#include <functional>
#include <stdexcept>
#include <cstdint>
#include <intrin.h>

//version 1.1.0

#ifdef _WIN32
#  include <pmmintrin.h>
#  define ALWAYS_INLINE __forceinline
#else
#  define ALWAYS_INLINE __attribute__((always_inline))
#  include <x86intrin.h>
#endif

namespace hordi {

class hash_base
{
public:
    typedef size_t size_type;

    template<typename T>
    struct hash_ : public std::hash<T> {
        ALWAYS_INLINE size_t operator()(const T& val) const noexcept {
            return hash_base::hash(val);
        }
    };

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
        StorageItem(StorageItem&& r) :mark(r.mark), data(std::move(r.data)) {}
        StorageItem(const StorageItem& r) :mark(r.mark), data(r.data) {}

        uint32_t mark;
        T data;
    };
#pragma pack(pop)

    static const uint32_t OFFSET_BASIS = 2166136261;

    template<size_t SIZE>
    static uint32_t hash_1(const void* ptr) noexcept {
        return hash_base::fnv_1a((const char*)ptr, SIZE);
    }

    template<typename T>
    static uint32_t hash_1(const T& v) noexcept {
        return hash_1<sizeof(T)>(&v);
    }

    template<typename T>
    static uint32_t hash(const T& v) noexcept {
        return hash_1<sizeof(T)>(&v);
    }
    template<size_t SIZE>
    static uint32_t hash(const void* ptr) noexcept {
        return hash_1<SIZE>(ptr);
    }

    static ALWAYS_INLINE uint32_t fnv_1a(const char* key, size_t len, uint32_t hash32 = OFFSET_BASIS) noexcept
    {
        const uint32_t PRIME = 1607;

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

    ALWAYS_INLINE static uint32_t make_mark(size_t h) noexcept {
        return static_cast<uint32_t>(h | USED_MARK);
    }

    template<typename this_type>
    static void resize_pow2(size_t pow2, hash_base& ref, std::true_type /*all data is trivial*/)
    {
        typename this_type::storage_type* elements = (typename this_type::storage_type*)calloc(pow2--, sizeof(typename this_type::storage_type));
        if (!elements)
            throw_bad_alloc();

        if (size_t cnt = ref._size)
        {
            for (typename this_type::storage_type* p = reinterpret_cast<typename this_type::storage_type*>(ref._elements);; ++p)
            {
                if (p->mark >= ACTIVE_MARK)
                {
                    for (size_t i = p->mark;; ++i)
                    {
                        i &= pow2;
                        auto& r = elements[i];
                        if (!r.mark) {
                            memcpy(&r, p, sizeof(typename this_type::storage_type));
                            break;
                        }
                    }
                    if (!--cnt)
                        break;
                }
            }
        }

        if (ref._capacity)
            free(ref._elements);
        ref._capacity = pow2;
        ref._elements = elements;
        ref._erased = 0;
    }

    template<typename this_type>
    static void resize_pow2(size_t pow2, hash_base& ref, std::false_type /*all data is trivial*/)
    {
        this_type tmp(pow2, false);
        if (ref._size) //rehash
        {
            for (typename this_type::storage_type* p = reinterpret_cast<typename this_type::storage_type*>(ref._elements);; ++p)
            {
                if (p->mark >= ACTIVE_MARK) {
                    typedef typename this_type::value_type VT;

                    VT& r = p->data;
                    tmp.insert_unique(std::move(*p));
                    r.~VT();

                    //next 2 lines to cover any exception that occurs during next tmp.insert_unique(std::move(r));
                    p->mark = DELETED_MARK;
                    ref._erased++;

                    if (!--ref._size)
                        break;
                }
            }
        }
        ref.swap(tmp); //swap base members only
    }

    static ALWAYS_INLINE size_t roundup(size_t sz) noexcept
    {
#ifdef _WIN32
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

#ifdef _WIN32
    __declspec(noreturn, noinline)
#else
    __attribute__((noinline, noreturn))
#endif
        static void throw_bad_alloc() {
        throw std::bad_alloc();
    }

#ifdef _WIN32
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
            const_iterator() noexcept : _ptr(nullptr), _cnt(0) {}

            const_iterator& operator++() noexcept
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

            const_iterator operator++(int) noexcept
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
            const_iterator(typename base::storage_type* p, typename base::size_type cnt) noexcept : _ptr(p), _cnt(cnt) {}

            typename base::storage_type* _ptr;
            typename base::size_type _cnt;
        };

        class iterator : public const_iterator
        {
        public:
            using const_iterator::operator*;
            using const_iterator::operator->;

            iterator() noexcept {}

            typename base::value_type& operator*() noexcept { return const_iterator::_ptr->data; }
            typename base::value_type* operator->() noexcept { return &const_iterator::_ptr->data; }

        private:
            friend base;
            iterator(typename base::storage_type* p, typename base::size_type cnt) : const_iterator(p, cnt) {}
        };
    };

    ALWAYS_INLINE void ctor_pow2(size_t pow2, size_t element_size)
    {
        _size = 0;
        _capacity = pow2 - 1;
        _elements = calloc(pow2, element_size); //pos2-- for performance in lookup-function
        _erased = 0;
        if (!_elements)
            throw_bad_alloc();
    }

    template<typename storage_type, typename data_type>
    ALWAYS_INLINE void deleteElement(storage_type* ptr) noexcept
    {
        ptr->data.~data_type();
        _size--;

        //set DELETED_MARK only if next element not 0
        const uint32_t next_mark = (ptr != (reinterpret_cast<storage_type*>(_elements) + _capacity) ? ptr + 1 : reinterpret_cast<storage_type*>(_elements))->mark;
        if (!next_mark)
            ptr->mark = 0;
        else {
            ptr->mark = DELETED_MARK;
            _erased++;
        }
    }

    template<typename storage_type, typename data_type>
    ALWAYS_INLINE void clear(std::true_type) noexcept
    {
        if (_capacity) {
            free(_elements);
            ctor_empty();
        }
    }

    template<typename storage_type, typename data_type>
    ALWAYS_INLINE void clear(std::false_type) noexcept
    {
        if (!_capacity)
            return;
        if (auto cnt = _size)
        {
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

        free(_elements);
        ctor_empty();
    }

    ALWAYS_INLINE void ctor_empty() noexcept
    {
        _size = 0;
        _capacity = 0;
        _elements = &_size; //0-hash indicates empty element - use this trick to prevent redundant "is empty" check in find-function
        _erased = 0;
    }

    ALWAYS_INLINE void swap(hash_base& r)
    {
        __m128i mm0 = _mm_loadu_si128((__m128i*)this);
        __m128i r_mm0 = _mm_loadu_si128((__m128i*)&r);

#if defined(_WIN64) || defined(__LP64__)
        static_assert(sizeof(r) == 32, "must be sizeof(hash_base)==32");

        __m128i mm1 = _mm_loadu_si128((__m128i*)this + 1);
        __m128i r_mm1 = _mm_loadu_si128((__m128i*)&r + 1);

        _mm_storeu_si128((__m128i*)this, r_mm0);
        _mm_storeu_si128((__m128i*)this + 1, r_mm1);
        _mm_storeu_si128((__m128i*)&r, mm0);
        _mm_storeu_si128((__m128i*)&r + 1, mm1);
#else
        static_assert(sizeof(r) == 16, "must be sizeof(hash_base)==16");

        _mm_storeu_si128((__m128i*)this, r_mm0);
        _mm_storeu_si128((__m128i*)&r, mm0);
#endif

        if (!_capacity)
            _elements = &_size;
        if (!r._capacity)
            r._elements = &r._size;
    }

    size_type _size;
    size_type _capacity;
    void* _elements;
    size_type _erased;
};

template<>
ALWAYS_INLINE uint32_t hash_base::hash_1<1>(const void* ptr) noexcept {
    uint32_t hash32 = (OFFSET_BASIS ^ (*(uint8_t*)ptr)) * 1607;
    return hash32 ^ (hash32 >> 16);
}

template<>
ALWAYS_INLINE uint32_t hash_base::hash_1<2>(const void* ptr) noexcept {
    uint32_t hash32 = (OFFSET_BASIS ^ (*(uint16_t*)ptr)) * 1607;
    return hash32 ^ (hash32 >> 16);
}

template<>
ALWAYS_INLINE uint32_t hash_base::hash_1<4>(const void* ptr) noexcept {
    uint64_t h, l = _umul128(*(uint32_t*)ptr, 0xde5fb9d2630458e9ull, &h);
    return static_cast<uint32_t>(h + l);
/*
    uint32_t hash32 = (OFFSET_BASIS ^ (*(uint32_t*)ptr)) * 1607;
    return hash32 ^ (hash32 >> 16);
*/
}

template<>
ALWAYS_INLINE uint32_t hash_base::hash_1<8>(const void* ptr) noexcept {
    uint64_t h;
    uint64_t l = _umul128(*(uint64_t*)ptr, 0xde5fb9d2630458e9ull, &h);
    return static_cast<uint32_t>(h + l);

/*
    uint32_t* key = (uint32_t*)ptr;
    uint32_t hash32 = (((OFFSET_BASIS ^ key[0]) * 1607) ^ key[1]) * 1607;
    return hash32 ^ (hash32 >> 16);
*/
}

template<>
ALWAYS_INLINE uint32_t hash_base::hash_1<12>(const void* ptr) noexcept
{
    const uint32_t* key = reinterpret_cast<const uint32_t*>(ptr);

    const uint32_t PRIME = 1607;

    uint32_t hash32 = (OFFSET_BASIS ^ key[0]) * PRIME;
    hash32 = (hash32 ^ key[1]) * PRIME;
    hash32 = (hash32 ^ key[2]) * PRIME;

    return hash32 ^ (hash32 >> 16);
}

template<>
ALWAYS_INLINE uint32_t hash_base::hash_1<16>(const void* ptr) noexcept
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
    ALWAYS_INLINE size_t operator()(const std::string& val) const noexcept {
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
    typedef value_type&                 reference;
    typedef const value_type&           const_reference;

private:
    friend iterator_base<this_type>;
    friend hash_base;
    typedef StorageItem<key_type> storage_type;

public:
    typedef typename iterator_base<this_type>::iterator iterator;
    typedef typename iterator_base<this_type>::const_iterator const_iterator;

    hash_set() {
        ctor_empty();
    }

    hash_set(size_type hint_size, const hasher& hf = hasher(), const key_equal& eql = key_equal()) :
        _hf(hf),
        _eql(eql)
    {
        ctor_pow2(roundup(hint_size * 2), sizeof(storage_type));
    }

    hash_set(const this_type& r) :
        _hf(r._hf),
        _eql(r._eql)
    {
        if (r._size) {
            ctor_pow2(r._capacity + 1, sizeof(storage_type));
            ctor_assign(r);
        }
        else
            ctor_empty();
    }

    hash_set(this_type&& r) noexcept :
        _hf(std::move(r._hf)),
        _eql(std::move(r._eql))
    {
        if (r._size) {
            *(hash_base*)this = r;
            r.ctor_empty();
        }
        else
            ctor_empty();
    }

    ~hash_set()
    {
        clear();
    }

    iterator begin() noexcept
    {
        auto pm = reinterpret_cast<storage_type*>(_elements);

        if (auto cnt = _size) {
            --cnt;
            for (;; ++pm) {
                if (pm->mark >= ACTIVE_MARK)
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

    ALWAYS_INLINE void reserve(size_type hint)
    {
        hint *= 2;
        if (hint > _capacity)
            resize_pow2(roundup(hint));
    }

    void clear() noexcept {
        hash_base::clear<storage_type, key_type>(std::is_trivially_copyable<key_type>());
    }

    void swap(hash_set& r) noexcept
    {
        hash_base::swap(r);
        std::swap(_hf, r._hf);
        std::swap(_eql, r._eql);
    }

    /*! Can invalidate iterators. */
    ALWAYS_INLINE std::pair<iterator, bool> insert(const key_type& val) {
        return find_insert(val);
    }

    /*! Can invalidate iterators. */
    template<class P>
    ALWAYS_INLINE std::pair<iterator, bool> insert(P&& val) {
        return find_insert(std::forward<P>(val));
    }

    /*! Can invalidate iterators. */
    template<class K>
    ALWAYS_INLINE std::pair<iterator, bool> emplace(K&& val) {
        return find_insert(std::forward<K>(val));
    }

    iterator find(const key_type& k) noexcept {
        const_iterator it = static_cast<const this_type*>(this)->find(k);
        return iterator(it._ptr, it._cnt);
    }

    const_iterator find(const key_type& k) const noexcept
    {
        const uint32_t mark = make_mark(_hf(k));
        for (size_t i = mark;; ++i)
        {
            i &= _capacity;
            auto& r = reinterpret_cast<storage_type*>(_elements)[i];
            uint32_t h = r.mark;
            if (h == mark)
            {
                if (_eql(r.data, k)) //identical found
                    return const_iterator(&r, 0);
            }
            else if (!h)
                break;
        }
        return const_iterator();
    }

    size_type count(const key_type& k) const noexcept
    {
        const uint32_t mark = make_mark(_hf(k));
        for (size_t i = mark;; ++i)
        {
            i &= _capacity;
            auto& r = reinterpret_cast<storage_type*>(_elements)[i];
            uint32_t h = r.mark;
            if (h == mark)
            {
                if (_eql(r.second, k)) //identical found
                    return 1;
            }
            else if (!h)
                break;
        }
        return 0;
    }

    /*! Can invalidate iterators.
    * \params it - Iterator pointing to a single element to be removed
    * \return an iterator pointing to the position immediately following of the element erased
    */
    iterator erase(const_iterator it) noexcept
    {
        if (auto ptr = it._ptr) //valid
        {
            auto cnt = it._cnt;
            deleteElement<storage_type, value_type>(it._ptr);

            if (cnt--) {
                for (;;) {
                    if ((++ptr)->mark >= ACTIVE_MARK)
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
    size_type erase(const key_type& k) noexcept
    {
        iterator i = find(k);
        if (i._ptr) {
            deleteElement<storage_type, value_type>(i._ptr);
            return 1;
        }
        return 0;
    }

    hash_set& operator=(const hash_set& r)
    {
        if (this != &r)
        {
            clear();
            if (r._size)
            {
                ctor_pow2(r._capacity + 1, sizeof(storage_type));
                ctor_assign(r);
            }
        }
        return *this;
    }

    ALWAYS_INLINE hash_set& operator=(hash_set&& r) noexcept
    {
        swap(r);
        return *this;
    }

private:
    hash_set(size_type pow2, bool)
    {
        ctor_pow2(pow2, sizeof(storage_type));
    }

    template<typename V>
    std::pair<iterator, bool> find_insert(V&& val)
    {
        const uint32_t mark = make_mark(_hf(val));
        size_t i = mark;

        auto unused_cnt = _capacity - _size;
        if (unused_cnt <= _size)
            resize_next();
        else if (_erased > (unused_cnt / 2))
            resize_pow2(_capacity + 1);

        std::pair<iterator, bool> ret;
        storage_type* empty_spot = nullptr;
        uint32_t deleted_mark = DELETED_MARK;

        for (;; ++i)
        {
            i &= _capacity;
            storage_type* r = reinterpret_cast<storage_type*>(_elements) + i;
            uint32_t h = r->mark;
            if (!h)
            {
                if (empty_spot) r = empty_spot;

                new (&r->data) key_type(std::forward<V>(val));
                r->mark = mark;
                ret.first._ptr = r;
                ret.second = true;
                _size++;
                return ret;
            }
            if (h == mark)
            {
                if (_eql(r->data, val)) //identical found
                {
                    ret.first._ptr = r;
                    return ret;
                }
            }
            else if (h == deleted_mark)
            {
                empty_spot = r;
                deleted_mark = 0; //prevent additional empty_spot == null_ptr comparison
                _erased--;
            }
        }
    }

    //space must be allocated before
    template<typename V>
    ALWAYS_INLINE void insert_unique(V&& st)
    {
        for (size_t i = st.mark;; ++i)
        {
            i &= _capacity;
            auto& r = reinterpret_cast<storage_type*>(_elements)[i];
            if (!r.mark) {
                if
#ifdef __cpp_if_constexpr
                constexpr
#endif
                (std::is_trivially_copyable<key_type>::value) {
                    memcpy(&r, &st, sizeof(st));
                } else {
                    new (&r) storage_type(std::forward<V>(st));
                }
                _size++;
                return;
            }
        }
    }

    ALWAYS_INLINE void resize_pow2(size_type pow2)
    {
        hash_base::resize_pow2<this_type>(pow2, *this, std::is_trivially_copyable<key_type>());
    }

    ALWAYS_INLINE void resize_next()
    {
        size_t sz = (_capacity + 1) * 2;
        if (sz > _capacity)
            resize_pow2(sz);
        else
            throw_length_error();
    }

    //space must be allocated before
    ALWAYS_INLINE void ctor_assign(const this_type& r)
    {
        auto cnt = r._size;
        for (auto i = r.begin(); cnt--; ++i)
            insert_unique(*(i._ptr));
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
    typedef value_type&                             reference;
    typedef const value_type&                       const_reference;

private:
    friend iterator_base<this_type>;
    friend hash_base;
    typedef StorageItem<value_type> storage_type;

public:
    typedef typename iterator_base<this_type>::iterator iterator;
    typedef typename iterator_base<this_type>::const_iterator const_iterator;

    hash_map() {
        ctor_empty();
    }

    hash_map(size_type hint_size, const hasher& hf = hasher(), const key_equal& eql = key_equal()) :
        _hf(hf),
        _eql(eql)
    {
        ctor_pow2(roundup(hint_size * 2), sizeof(storage_type));
    }

    hash_map(const this_type& r) :
        _hf(r._hf),
        _eql(r._eql)
    {
        if (r._size) {
            ctor_pow2(r._capacity + 1, sizeof(storage_type));
            ctor_assign(r);
        }
        else
            ctor_empty();
    }

    hash_map(this_type&& r) noexcept :
        _hf(std::move(r._hf)),
        _eql(std::move(r._eql))
    {
        if (r._size) {
            *(hash_base*)this = r;
            r.ctor_empty();
        }
        else
            ctor_empty();
    }

    ~hash_map()
    {
        clear();
    }

    iterator begin() noexcept
    {
        auto pm = reinterpret_cast<storage_type*>(_elements);

        if (auto cnt = _size) {
            --cnt;
            for (;; ++pm) {
                if (pm->mark >= ACTIVE_MARK)
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

    ALWAYS_INLINE void reserve(size_type hint) {
        hint *= 2;
        if (hint > _capacity)
            resize_pow2(roundup(hint));
    }

    void clear() noexcept {
        hash_base::clear<storage_type, value_type>(std::integral_constant<bool, std::is_trivially_copyable<key_type>::value && std::is_trivially_copyable<mapped_type>::value>());
    }

    void swap(this_type& r) noexcept
    {
        hash_base::swap(r);
        std::swap(_hf, r._hf);
        std::swap(_eql, r._eql);
    }

    /*! Can invalidate iterators. */
    ALWAYS_INLINE std::pair<iterator, bool> insert(const value_type& val) {
        return insert_(val);
    }

    /*! Can invalidate iterators. */
    template <class P>
    ALWAYS_INLINE std::pair<iterator, bool> insert(P&& val) {
        return insert_(std::forward<P>(val));
    }

    /*! Can invalidate iterators. */
    template<class... Args>
    ALWAYS_INLINE std::pair<iterator, bool> emplace(const Key& key, Args&&... args) {
        return find_emplace(key, std::forward<Args>(args)...);
    }

    /*! Can invalidate iterators. */
    template<class K, class... Args>
    ALWAYS_INLINE std::pair<iterator, bool> emplace(K&& key, Args&&... args) {
        return find_emplace(std::forward<K>(key), std::forward<Args>(args)...);
    }

    iterator find(const key_type& k) noexcept {
        const_iterator it = static_cast<const this_type*>(this)->find(k);
        return iterator(it._ptr, it._cnt);
    }

    const_iterator find(const key_type& k) const noexcept
    {
        const uint32_t mark = make_mark(_hf(k));
        for (size_t i = mark;; ++i)
        {
            i &= _capacity;
            auto& r = reinterpret_cast<storage_type*>(_elements)[i];
            uint32_t h = r.mark;
            if (h == mark)
            {
                if (_eql(r.data.first, k)) //identical found
                    return const_iterator(&r, 0);
            }
            else if (!h)
                break;
        }
        return const_iterator();
    }

    size_type count(const key_type& k) const noexcept
    {
        const uint32_t mark = make_mark(_hf(k));
        for (size_t i = mark;; ++i)
        {
            i &= _capacity;
            auto& r = reinterpret_cast<storage_type*>(_elements)[i];
            uint32_t h = r.mark;
            if (h == mark)
            {
                if (_eql(r.data.first, k)) //identical found
                    return 1;
            }
            else if (!h)
                break;
        }
        return 0;
    }

    /*! Can invalidate iterators.
    * \params it - Iterator pointing to a single element to be removed
    * \return return an iterator pointing to the position immediately following of the element erased
    */
    iterator erase(const_iterator it) noexcept
    {
        if (auto ptr = it._ptr) //valid
        {
            auto cnt = it._cnt;
            deleteElement<storage_type, value_type>(it._ptr);

            if (cnt--) {
                for (;;) {
                    if ((++ptr)->mark >= ACTIVE_MARK)
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
    size_type erase(const key_type& k) noexcept
    {
        iterator i = find(k);
        if (i._ptr) {
            deleteElement<storage_type, value_type>(i._ptr);
            return 1;
        }
        return 0;
    }

    hash_map& operator=(const hash_map& r)
    {
        if (this != &r)
        {
            clear();
            if (r._size)
            {
                ctor_pow2(r._capacity + 1, sizeof(storage_type));
                ctor_assign(r);
            }
        }
        return *this;
    }

    hash_map& operator=(hash_map&& r) noexcept {
        swap(r);
        return *this;
    }

    ALWAYS_INLINE mapped_type& operator[](const key_type& k) {
        return find_insert(k);
    }

    ALWAYS_INLINE mapped_type& operator[](key_type&& k) {
        return find_insert(std::move(k));
    }

private:
    hash_map(size_type pow2, bool)
    {
        ctor_pow2(pow2, sizeof(storage_type));
    }

    template<typename K, typename... Args>
    std::pair<iterator, bool> find_emplace(K&& k, Args&&... args)
    {
        const uint32_t mark = make_mark(_hf(k));
        size_t i = mark;

        auto unused_cnt = _capacity - _size;
        if (unused_cnt <= _size)
            resize_next();
        else if (_erased > (unused_cnt / 2))
            resize_pow2(_capacity + 1);

        std::pair<iterator, bool> ret;
        storage_type* empty_spot = nullptr;
        uint32_t deleted_mark = DELETED_MARK;

        for (;; ++i)
        {
            i &= _capacity;
            storage_type* r = reinterpret_cast<storage_type*>(_elements) + i;
            uint32_t h = r->mark;
            if (!h)
            {
                if (empty_spot) r = empty_spot;

                new (&r->data) value_type(std::piecewise_construct, std::forward_as_tuple(std::forward<K>(k)), std::forward_as_tuple(std::forward<Args>(args)...));
                r->mark = mark;
                ret.first._ptr = r;
                ret.second = true;
                _size++;
                return ret;
            }
            if (h == mark)
            {
                if (_eql(r->data.first, k)) //identical found
                {
                    ret.first._ptr = r;
                    return ret;
                }
            }
            else if (h == deleted_mark)
            {
                empty_spot = r;
                deleted_mark = 0; //optimization to prevent additional first_found == null_ptr comparison
                _erased--;
            }
        }
    }

    template<typename V>
    mapped_type& find_insert(V&& k)
    {
        const uint32_t mark = make_mark(_hf(k));
        size_t i = mark;

        auto unused_cnt = _capacity - _size;
        if (unused_cnt <= _size)
            resize_next();
        else if (_erased > (unused_cnt / 2))
            resize_pow2(_capacity + 1);

        storage_type* empty_spot = nullptr;
        uint32_t deleted_mark = DELETED_MARK;

        for (;; ++i)
        {
            i &= _capacity;
            storage_type* r = reinterpret_cast<storage_type*>(_elements) + i;
            uint32_t h = r->mark;
            if (!h)
            {
                if (empty_spot) r = empty_spot;

                new (&r->data) value_type(std::forward<V>(k), mapped_type());
                r->mark = mark;
                _size++;
                return r->data.second;
            }
            if (h == mark)
            {
                if (_eql(r->data.first, k)) //identical found
                    return r->data.second;
            }
            else if (h == deleted_mark)
            {
                empty_spot = r;
                deleted_mark = 0; //optimization to prevent additional empty_spot == null_ptr comparison
                _erased--;
            }
        }
    }

    template<typename V>
    std::pair<iterator, bool> insert_(V&& val)
    {
        const uint32_t mark = make_mark(_hf(val.first));
        size_t i = mark;

        auto unused_cnt = _capacity - _size;
        if (unused_cnt <= _size)
            resize_next();
        else if (_erased > (unused_cnt / 2))
            resize_pow2(_capacity + 1);

        std::pair<iterator, bool> ret;
        storage_type* empty_spot = nullptr;
        uint32_t deleted_mark = DELETED_MARK;

        for (;; ++i)
        {
            i &= _capacity;
            storage_type* r = reinterpret_cast<storage_type*>(_elements) + i;
            uint32_t h = r->mark;
            if (!h)
            {
                if (empty_spot) r = empty_spot;

                new (&r->data) value_type(std::forward<V>(val));
                r->mark = mark;
                ret.first._ptr = r;
                ret.second = true;
                _size++;
                return ret;
            }
            if (h == mark)
            {
                if (_eql(r->data.first, val.first)) //identical found
                {
                    ret.first._ptr = r;
                    return ret;
                }
            }
            else if (h == deleted_mark)
            {
                empty_spot = r;
                deleted_mark = 0; //optimization to prevent additional first_found == null_ptr comparison
                _erased--;
            }
        }
    }

    template<typename V>
    ALWAYS_INLINE void insert_unique(V&& st)
    {
        for (size_t i = st.mark;; ++i)
        {
            i &= _capacity;
            auto& r = reinterpret_cast<storage_type*>(_elements)[i];
            if (!r.mark) {
                if
#ifdef __cpp_if_constexpr
                constexpr
#endif
                (std::is_trivially_copyable<key_type>::value && std::is_trivially_copyable<mapped_type>::value) {
                    memcpy(&r, &st, sizeof(st));
                } else {
                    new (&r) storage_type(std::forward<V>(st));
                }
                _size++;
                return;
            }
        }
    }

    ALWAYS_INLINE void resize_pow2(size_type pow2)
    {
        hash_base::resize_pow2<this_type>(pow2, *this, std::integral_constant<bool, std::is_trivially_copyable<key_type>::value && std::is_trivially_copyable<mapped_type>::value>());
    }

    ALWAYS_INLINE void resize_next()
    {
        size_type sz = (_capacity + 1) * 2;
        if (sz > _capacity)
            resize_pow2(sz);
        else
            throw_length_error();
    }

    //enough space must be allocated before
    ALWAYS_INLINE void ctor_assign(const this_type& r)
    {
        auto cnt = r._size;
        for (auto i = r.begin(); cnt--; ++i) {
            insert_unique(*(i._ptr));
        }
    }

    hasher _hf;
    key_equal _eql;
};

} //namespace hordi

#endif //hordi_hash_set_h_
