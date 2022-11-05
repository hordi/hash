# hash
Fast C++ flat (open addressing) hash set, map header only library. Requred C++11 (only because of "constexpr" and "noexcept" modifiers usage).

Drop in replacement (mostly, allocator-type is absent) implementation of unordered hash-set and hash-map.
Used (defined) FNV-1a like hash-function for standards types (int, size_t, std::string). Default hash-function has 32-bits result (hash_set1.hpp supports full range of size_t), makes sense to use if amount of elements less than UINT_MAX/2 for good distribution. In other case - should be used 64-bits result hash-function.
