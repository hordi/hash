# hash
Fast C++ flat (open addressing) hash set, map

Drop in place (mostly, allocator-type is absent) implementation of unordered hash-set and hash-map.
Used (defined) FNV-1a like hash-function (32-bits, has sense to use if amount of elements less than UINT_MAX/2 for good distribution. In other case - should be used 64-bits result hash-function) for standards types (int, size_t, std::string).
