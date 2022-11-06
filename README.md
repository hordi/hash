# hash
Fast C++ flat (open addressing) hash set/map header only library. Requred C++11 (only because of "constexpr" and "noexcept" modifiers usage).

Drop in replacement (mostly, references invaildated if reallocation happen, allocator-type is absent) implementation of unordered hash-set and hash-map.
Default hash-functions use actual 32-bits hash-value, makes sense to use if amount of elements less than UINT_MAX/2 for good distribution. In other case - should be used 64-bits result hash-function (hash_set1.hpp supports full range of size_t).
