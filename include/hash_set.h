#ifndef hash_set_h_
#define hash_set_h_

#include <functional>
#include <stdexcept>
#include <cstdint>

#ifdef _WIN32
#  include <pmmintrin.h>
#  define ALWAYS_INLINE __forceinline
#else
#  define ALWAYS_INLINE __attribute__((always_inline))
#  include <x86intrin.h>
#endif

class hash_base
{
public:
    typedef size_t size_type;

    static const uint32_t OFFSET_BASIS = 2166136261;

    template<size_t SIZE>
    static uint32_t hash_1(const void* ptr, uint32_t offset) noexcept {
        return hash_base::fnv_1a((const char*)ptr, SIZE, offset);
    }

    template<typename T>
    static uint32_t hash_1(const T& v, uint32_t offset) noexcept {
        return hash_1<sizeof(T)>(&v, offset);
    }

    template<typename T>
    static uint32_t hash(const T& v) noexcept {
        return hash_1<sizeof(T)>(&v, hash_base::OFFSET_BASIS);
    }
    template<size_t SIZE>
    static uint32_t hash(const void* ptr) noexcept {
        return hash_1<SIZE>(ptr, hash_base::OFFSET_BASIS);
    }

    template<typename T>
    struct hash_ : public std::hash<T> {
        ALWAYS_INLINE size_t operator()(const T& val) const noexcept {
            hash_base::hash(val);
        }
    };

    constexpr static ALWAYS_INLINE uint32_t fnv_1a(const char* key, size_t len, uint32_t hash32 = OFFSET_BASIS) noexcept
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

    size_type size() const noexcept { return _size; }
    size_type capacity() const noexcept { return _capacity; }

    static constexpr size_type max_size() noexcept {
        return (size_type(1) << (sizeof(size_type) * 8 - 2)) - 1;
    }

    bool empty() const noexcept { return !_size; }

protected:
    //2 bits used as data-marker
    enum { ACTIVE_MARK = 0x2, USED_MARK = 0x1 | ACTIVE_MARK, DELETED_MARK = (USED_MARK & ~ACTIVE_MARK) };

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
                    while ((++p)->first < base::ACTIVE_MARK)
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

            const typename base::value_type& operator*() const noexcept { return _ptr->second; }
            const typename base::value_type* operator->() const noexcept { return &_ptr->second; }

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

            typename base::value_type& operator*() noexcept { return const_iterator::_ptr->second; }
            typename base::value_type* operator->() noexcept { return &const_iterator::_ptr->second; }

        private:
            friend base;
            iterator(typename base::storage_type* p, typename base::size_type cnt) : const_iterator(p, cnt) {}
        };
        };

    constexpr static ALWAYS_INLINE uint32_t make_hash32(size_t h) noexcept {
        return static_cast<uint32_t>(h | USED_MARK); //2 bits uses as flag
    }

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
        //set DELETED_MARK only if next element not 0
        const uint32_t next_mark = (ptr != (reinterpret_cast<storage_type*>(_elements) + _capacity) ? ptr + 1 : reinterpret_cast<storage_type*>(_elements))->first;
        if (!next_mark)
            ptr->first = 0;
        else {
            ptr->first = DELETED_MARK;
            _erased++;
        }
        ptr->second.~data_type();
        _size--;
    }

    template<typename storage_type, typename data_type>
    ALWAYS_INLINE void clear() noexcept
    {
        if (!_capacity)
            return;
        if (auto cnt = _size)
        {
            for (storage_type* p = reinterpret_cast<storage_type*>(_elements);; ++p)
            {
                if (p->first >= ACTIVE_MARK) {
                    cnt--;
                    p->second.~data_type();
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
constexpr ALWAYS_INLINE uint32_t hash_base::hash_1<1>(const void* ptr, uint32_t offset) noexcept {
    uint32_t hash32 = (offset ^ (*(uint8_t*)ptr)) * 1607;
    return hash32 ^ (hash32 >> 16);
}

template<>
ALWAYS_INLINE uint32_t hash_base::hash_1<2>(const void* ptr, uint32_t offset) noexcept {
    uint32_t hash32 = (offset ^ (*(uint16_t*)ptr)) * 1607;
    return hash32 ^ (hash32 >> 16);
}

template<>
constexpr ALWAYS_INLINE uint32_t hash_base::hash_1<4>(const void* ptr, uint32_t offset) noexcept {
    uint32_t hash32 = (offset ^ (*(uint32_t*)ptr)) * 1607;
    return hash32 ^ (hash32 >> 16);
}

template<>
constexpr ALWAYS_INLINE uint32_t hash_base::hash_1<8>(const void* ptr, uint32_t offset) noexcept {
    uint32_t* key = (uint32_t*)ptr;
    uint32_t hash32 = (((offset ^ key[0]) * 1607) ^ key[1]) * 1607;
    return hash32 ^ (hash32 >> 16);
}

template<>
constexpr ALWAYS_INLINE uint32_t hash_base::hash_1<12>(const void* ptr, uint32_t offset) noexcept
{
    const uint32_t* key = reinterpret_cast<const uint32_t*>(ptr);

    const uint32_t PRIME = 1607;

    uint32_t hash32 = (offset ^ key[0]) * PRIME;
    hash32 = (hash32 ^ key[1]) * PRIME;
    hash32 = (hash32 ^ key[2]) * PRIME;

    return hash32 ^ (hash32 >> 16);
}

template<>
constexpr ALWAYS_INLINE uint32_t hash_base::hash_1<16>(const void* ptr, uint32_t offset) noexcept
{
    const uint32_t* key = reinterpret_cast<const uint32_t*>(ptr);

    const uint32_t PRIME = 1607;

    uint32_t hash32 = (offset ^ key[0]) * PRIME;
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
    typedef std::pair<uint32_t, key_type> storage_type;

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
                if (pm->first >= ACTIVE_MARK)
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
        hash_base::clear<storage_type, key_type>();
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
    ALWAYS_INLINE std::pair<iterator, bool> insert(key_type&& val) {
        return find_insert(std::move(val));
    }

    iterator find(const key_type& k) noexcept {
        const_iterator it = static_cast<const this_type*>(this)->find(k);
        return iterator(it._ptr, it._cnt);
    }

    const_iterator find(const key_type& k) const noexcept
    {
        size_t i = _hf(k);
        const uint32_t hash32 = make_hash32(i);

        for (;; ++i)
        {
            i &= _capacity;
            auto& r = reinterpret_cast<storage_type*>(_elements)[i];
            uint32_t h = r.first;
            if (h == hash32)
            {
                if (_eql(r.second, k)) //identical found
                    return const_iterator(&r, 0);
            }
            else if (!h)
                break;
        }
        return const_iterator();
    }

    size_type count(const key_type& k) const noexcept
    {
        size_t i = _hf(k);
        const uint32_t hash32 = make_hash32(i);

        for (;; ++i)
        {
            i &= _capacity;
            auto& r = reinterpret_cast<storage_type*>(_elements)[i];
            uint32_t h = r.first;
            if (h == hash32)
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
                    if ((++ptr)->first >= ACTIVE_MARK)
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
    hash_set(const hasher& hf, const key_equal& eql, size_type pow2) :
        _hf(hf),
        _eql(eql)
    {
        ctor_pow2(pow2, sizeof(storage_type));
    }

    template<typename V>
    std::pair<iterator, bool> find_insert(V&& val)
    {
        size_t i = _hf(val);
        const uint32_t hash32 = make_hash32(i);

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
            uint32_t h = r->first;
            if (!h)
            {
                if (empty_spot) r = empty_spot;

                new (&r->second) key_type(std::forward<V>(val));
                r->first = hash32;
                ret.first._ptr = r;
                ret.second = true;
                _size++;
                return ret;
            }
            if (h == hash32)
            {
                if (_eql(r->second, val)) //identical found
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
    void insert_unique(V&& val)
    {
        size_t i = _hf(val);
        const uint32_t hash32 = make_hash32(i);

        for (;; ++i)
        {
            i &= _capacity;
            auto& r = reinterpret_cast<storage_type*>(_elements)[i];
            if (!r.first) {
                new (&r.second) key_type(std::forward<V>(val));
                r.first = hash32;
                _size++;
                return;
            }
        }
    }

    void resize_pow2(size_type pow2)
    {
        this_type tmp(_hf, _eql, pow2);
        if (_size) //rehash
        {
            for (storage_type* p = reinterpret_cast<storage_type*>(_elements);; ++p)
            {
                if (p->first >= ACTIVE_MARK) {
                    key_type& r = p->second;
                    tmp.insert_unique(std::move(r));
                    r.~key_type();

                    //next 2 lines need to be to cover any exception that occurs during next tmp.insert_unique(std::move(r));
                    p->first = DELETED_MARK;
                    _erased++;

                    if (!--_size)
                        break;
                }
            }
        }
        swap(tmp);
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
            insert_unique(*i);
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
    typedef std::pair<uint32_t, value_type> storage_type;

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
                if (pm->first >= ACTIVE_MARK)
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

    __forceinline void reserve(size_type hint) {
        hint *= 2;
        if (hint > _capacity)
            resize_pow2(roundup(hint));
    }

    void clear() noexcept {
        hash_base::clear<storage_type, value_type>();
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
        return insert_(std::move(val));
    }

    iterator find(const key_type& k) noexcept {
        const_iterator it = static_cast<const this_type*>(this)->find(k);
        return iterator(it._ptr, it._cnt);
    }

    const_iterator find(const key_type& k) const noexcept
    {
        size_t i = _hf(k);
        const uint32_t hash32 = make_hash32(i);

        for (;; ++i)
        {
            i &= _capacity;
            auto& r = reinterpret_cast<storage_type*>(_elements)[i];
            uint32_t h = r.first;
            if (h == hash32)
            {
                if (_eql(r.second.first, k)) //identical found
                    return const_iterator(&r, 0);
            }
            else if (!h)
                break;
        }
        return const_iterator();
    }

    size_type count(const key_type& k) const noexcept
    {
        size_t i = _hf(k);
        const uint32_t hash32 = make_hash32(i);

        for (;; ++i)
        {
            i &= _capacity;
            auto& r = reinterpret_cast<storage_type*>(_elements)[i];
            uint32_t h = r.first;
            if (h == hash32)
            {
                if (_eql(r.second.first, k)) //identical found
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
                    if ((++ptr)->first >= ACTIVE_MARK)
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
    hash_map(const hasher& hf, const key_equal& eql, size_type pow2) :
        _hf(hf),
        _eql(eql)
    {
        ctor_pow2(pow2, sizeof(storage_type));
    }

    template<typename V>
    mapped_type& find_insert(V&& k)
    {
        size_t i = _hf(k);
        const uint32_t hash32 = make_hash32(i);

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
            uint32_t h = r->first;
            if (!h)
            {
                if (empty_spot) r = empty_spot;

                new (&r->second) value_type(std::forward<V>(k), mapped_type());
                r->first = hash32;
                _size++;
                return r->second.second;
            }
            if (h == hash32)
            {
                if (_eql(r->second.first, k)) //identical found
                    return r->second.second;
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
        size_t i = _hf(val.first);
        const uint32_t hash32 = make_hash32(i);

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
            uint32_t h = r->first;
            if (!h)
            {
                if (empty_spot) r = empty_spot;

                new (&r->second) value_type(std::forward<V>(val));
                r->first = hash32;
                ret.first._ptr = r;
                ret.second = true;
                _size++;
                return ret;
            }
            if (h == hash32)
            {
                if (_eql(r->second.first, val.first)) //identical found
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
    ALWAYS_INLINE void insert_unique(V&& val)
    {
        size_t i = _hf(val.first);
        uint32_t hash32 = make_hash32(i);

        for (;; ++i)
        {
            i &= _capacity;
            auto& r = reinterpret_cast<storage_type*>(_elements)[i];
            if (!r.first) {
                new (&r.second) value_type(std::forward<V>(val));
                r.first = hash32;
                _size++;
                return;
            }
        }
    }

    void resize_pow2(size_type pow2)
    {
        this_type tmp(_hf, _eql, pow2);
        if (_size) //rehash
        {
            for (storage_type* p = reinterpret_cast<storage_type*>(_elements);; ++p)
            {
                if (p->first >= ACTIVE_MARK) {
                    value_type& r = p->second;
                    tmp.insert_unique(std::move(r));
                    r.~value_type();

                    //next 2 lines need to be to cover any exception that occurs during next tmp.insert_unique(std::move(r));
                    p->first = DELETED_MARK;
                    _erased++;

                    if (!--_size)
                        break;
                }
            }
        }
        swap(tmp);
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
        for (auto i = r.begin(); cnt--; ++i)
            insert_unique(*i);
    }

    hasher _hf;
    key_equal _eql;
};

#endif //hash_set_h_
