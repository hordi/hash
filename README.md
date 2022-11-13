# hash
Fast C++ flat (open addressing) hash set/map header only library. Requred C++11 (only because of "constexpr" and "noexcept" modifiers usage).

Drop in replacement (mostly, references invaildated if reallocation happen, allocator-type is absent) implementation of unordered hash-set and hash-map.
Default hash-functions use actual 32-bits hash-value, makes sense to use if amount of elements less than UINT_MAX/2 for good distribution. In other case - should be used 64-bits result hash-function (hash_set1.hpp supports full range of size_t).


EXAMPLES

Simplest set-map
```cpp
#include "hash_set1.h"
#include <iostream>

int main() {
    hrd1::hash_set<int> ss(1024);
	  hrd1::hash_map<int, int> ms(1024);
	  for (int i = 0; i != 1024; ++i) {
		    ss.insert(i);
		    ms[i] = i + 10;
	  }

    hrd1::hash_set<int> ss1(ss.begin(), ss.end());

	  size_t cnt = 0, cnt2 = 0, cnt3 = 0;
	  for (int i = 0; i != 1024 * 1024; ++i) {
		    cnt += ss.count(i);
		    cnt2 += ms.count(i);
        cnt3 += ss1.count(i);
	  }

	  std::cout << "found: " << cnt << ':' << cnt2 << ':' << cnt3 << '\n';
	  return 0;
}
```

Custom "complex" type with own hash-function and equal-operator
```cpp

#include "hash_set.h"
#include <iostream>

struct Key1 {
    Key1(uint64_t k, uint64_t d = 0) :key(k), data(d) {}

    bool operator==(const Key1& r) const noexcept {
        return key == r.key;
    }

    uint64_t key;
    uint64_t data;
};

template<>
struct hrd::hash_base::hash_<Key1> {
	  size_t operator()(const Key1& r) const noexcept {
		    return hrd::hash_base::hash_1<8>(&r.key);
	  }
};

int main() {
    hrd::hash_set<Key1> ss(1024);
    
    for (size_t i = 0; i != 1024; ++i)
      ss.insert(Key1(i, i + 10));

    size_t cnt = 0, cnt1 = 0;
    for (size_t i = 0; i != 1024 * 1024; ++i)
        cnt += ss.count(Key1(i));

    for (auto i = ss.begin(), e = ss.end(); i != e; ++i)
        cnt1++;

    std::cout << "found: " << cnt << ':' << cnt << '\n';

    return 0;
}
```
